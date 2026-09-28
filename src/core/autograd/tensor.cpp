#include "zetta/autograd/tensor.hpp"

#include <cmath>
#include <cstddef>
#include <span>
#include <unordered_set>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/compute_status.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/compute/device_buffer.hpp"
#include "zetta/runtime.hpp"

namespace zetta::autograd {

namespace {
constexpr float kInvSqrt2 = 0.7071067811865476F;      // 1 / sqrt(2)
constexpr float kSqrt2OverPi = 0.7978845608028654F;   // sqrt(2 / pi)

float sigmoid(float x) { return 1.0F / (1.0F + std::exp(-x)); }

thread_local compute::ComputeDevice* g_compute_device = nullptr;

// C[rows, cols] = op(A) * op(B) on `device`, host floats in and out. `a` holds the
// stored operand for op(A) (rows*inner elements), `b` for op(B) (inner*cols).
// Returns empty on a device error.
std::vector<float> device_gemm(compute::ComputeDevice& device,
                               const std::vector<float>& a,
                               const std::vector<float>& b, int rows, int inner,
                               int cols, bool transpose_a, bool transpose_b) {
    compute::DeviceBuffer buffer_a =
        device.upload<float>(std::span<const float>(a));
    compute::DeviceBuffer buffer_b =
        device.upload<float>(std::span<const float>(b));
    compute::DeviceBuffer buffer_c = device.allocate(
        compute::DataType::Float32, static_cast<std::size_t>(rows) * cols);
    if (buffer_a.empty() || buffer_b.empty() || buffer_c.empty()) {
        return {};
    }
    compute::GemmParams params;
    params.rows = rows;
    params.inner = inner;
    params.cols = cols;
    params.alpha = 1.0;
    params.beta = 0.0;
    params.transpose_a = transpose_a;
    params.transpose_b = transpose_b;
    if (!device.gemm(params, buffer_a, buffer_b, buffer_c).ok()) {
        return {};
    }
    std::vector<float> result(static_cast<std::size_t>(rows) * cols, 0.0F);
    if (!device.download<float>(buffer_c, std::span<float>(result)).ok()) {
        return {};
    }
    return result;
}
}  // namespace

compute::ComputeDevice* set_compute_device(compute::ComputeDevice* device) {
    compute::ComputeDevice* previous = g_compute_device;
    g_compute_device = device;
    return previous;
}

compute::ComputeDevice* current_compute_device() { return g_compute_device; }

namespace {
// Grain for runtime::parallel_for over `count` items each doing `work_per_item`
// float ops: run inline (grain == count -> one chunk) when the total work is below
// a dispatch-worth threshold, else split across all workers (grain 1). This makes
// heavy-but-short matmuls (few tokens, wide projections) thread, while tiny ops
// (unit tests) stay inline — the default item-count grain never threads them.
std::size_t parallel_grain(std::size_t count, std::size_t work_per_item) {
    constexpr std::size_t kMinChunkWork = 1u << 17;  // ~131k float ops per chunk
    return count * work_per_item < kMinChunkWork ? count : 1;
}
}  // namespace

// Run `body(index)` over [0, count) across workers, with the same grain heuristic the matmul paths
// use so small tensors stay inline. Elementwise ops are per-index independent, so this is
// bit-identical to the serial loop — there is no cross-element reduction to reorder, which is what
// keeps the CUDA equivalence gate's exact-equality assertions valid.
template <class Body>
void parallel_elementwise(std::size_t count, Body body) {
    runtime::parallel_for(count, parallel_grain(count, 4),
                          [&](std::size_t, std::size_t begin, std::size_t end) {
                              for (std::size_t i = begin; i < end; ++i) body(i);
                          });
}

std::shared_ptr<Node> make_node(std::vector<float> data,
                                std::vector<int> shape) {
    auto node = std::make_shared<Node>();
    node->grad.assign(data.size(), 0.0F);
    node->data = std::move(data);
    node->shape = std::move(shape);
    return node;
}

Tensor Tensor::leaf(std::vector<float> data, std::vector<int> shape,
                    bool requires_grad) {
    auto node = make_node(std::move(data), std::move(shape));
    node->requires_grad = requires_grad;
    return Tensor{node};
}

void Tensor::backward() {
    // Topological order of the graph rooted at this node (parents before children).
    std::vector<std::shared_ptr<Node>> order;
    std::unordered_set<const Node*> visited;
    const std::function<void(const std::shared_ptr<Node>&)> build =
        [&](const std::shared_ptr<Node>& current) {
            if (current == nullptr || visited.count(current.get())) {
                return;
            }
            visited.insert(current.get());
            for (const auto& parent : current->parents) {
                build(parent);
            }
            order.push_back(current);
        };
    build(node_);

    // Seed: the loss is a scalar, so d(loss)/d(loss) = 1.
    node_->grad.assign(node_->data.size(), 0.0F);
    if (node_->data.size() == 1) {
        node_->grad[0] = 1.0F;
    }

    // Propagate in reverse topological order, accumulating into parents.
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if ((*it)->backward) {
            (*it)->backward((*it)->grad);
        }
    }
}

Tensor matmul(const Tensor& a, const Tensor& b) {
    const int m = a.shape()[0];
    const int k = a.shape()[1];
    const int n = b.shape()[1];
    // Capture the device at construction so the graph backpropagates on the device
    // it was built on (nullptr = the host reference kernels).
    compute::ComputeDevice* device = g_compute_device;

    std::vector<float> out;
    if (device != nullptr) {
        out = device_gemm(*device, a.data(), b.data(), m, k, n, false, false);
    } else {
        // Host reference kernel, spread across the shared runtime thread pool. Both
        // ikj-ordered (cache-friendly contiguous column sweeps); we parallelize
        // over whichever output axis is larger, because transformer inference has a
        // tiny row count (a few tokens) but a huge column count (projections up to
        // the 151936 vocab) — parallelizing over rows there gives no parallelism.
        out.assign(static_cast<std::size_t>(m) * n, 0.0F);
        const float* a_data = a.data().data();
        const float* b_data = b.data().data();
        float* out_data = out.data();
        if (m >= n) {
            runtime::parallel_for(
                static_cast<std::size_t>(m),
                parallel_grain(m, static_cast<std::size_t>(n) * k),
                [&](std::size_t, std::size_t row_begin, std::size_t row_end) {
                    for (std::size_t row = row_begin; row < row_end; ++row) {
                        float* out_row = out_data + row * n;
                        const float* a_row = a_data + row * k;
                        for (int inner = 0; inner < k; ++inner) {
                            const float a_value = a_row[inner];
                            const float* b_row = b_data + static_cast<std::size_t>(inner) * n;
                            for (int col = 0; col < n; ++col) {
                                out_row[col] += a_value * b_row[col];
                            }
                        }
                    }
                });
        } else {
            runtime::parallel_for(
                static_cast<std::size_t>(n),
                parallel_grain(n, static_cast<std::size_t>(m) * k),
                [&](std::size_t, std::size_t col_begin, std::size_t col_end) {
                    for (int row = 0; row < m; ++row) {
                        float* out_row = out_data + static_cast<std::size_t>(row) * n;
                        const float* a_row = a_data + static_cast<std::size_t>(row) * k;
                        for (int inner = 0; inner < k; ++inner) {
                            const float a_value = a_row[inner];
                            const float* b_row = b_data + static_cast<std::size_t>(inner) * n;
                            for (std::size_t col = col_begin; col < col_end; ++col) {
                                out_row[col] += a_value * b_row[col];
                            }
                        }
                    }
                });
        }
    }

    auto node = make_node(std::move(out), {m, n});
    node->parents = {a.node(), b.node()};
    const std::shared_ptr<Node> a_node = a.node();
    const std::shared_ptr<Node> b_node = b.node();
    node->backward = [a_node, b_node, m, k, n, device](const std::vector<float>& g) {
        if (device != nullptr) {
            // dA = dOut * B^T [m,k]; dB = A^T * dOut [k,n].
            const std::vector<float> da =
                device_gemm(*device, g, b_node->data, m, n, k, false, true);
            const std::vector<float> db =
                device_gemm(*device, a_node->data, g, k, m, n, true, false);
            for (std::size_t i = 0; i < da.size(); ++i) a_node->grad[i] += da[i];
            for (std::size_t i = 0; i < db.size(); ++i) b_node->grad[i] += db[i];
            return;
        }
        // dA[row, inner] += sum_col g[row, col] * B[inner, col]. Parallel over
        // rows — each thread writes a disjoint band of a_node->grad, no races.
        const float* g_data = g.data();
        const float* b_data = b_node->data.data();
        float* a_grad = a_node->grad.data();
        runtime::parallel_for(
            static_cast<std::size_t>(m),
            parallel_grain(m, static_cast<std::size_t>(k) * n),
            [&](std::size_t, std::size_t row_begin, std::size_t row_end) {
                for (std::size_t row = row_begin; row < row_end; ++row) {
                    const float* g_row = g_data + row * n;
                    for (int inner = 0; inner < k; ++inner) {
                        float accumulated = 0.0F;
                        const float* b_row = b_data + static_cast<std::size_t>(inner) * n;
                        for (int col = 0; col < n; ++col) {
                            accumulated += g_row[col] * b_row[col];
                        }
                        a_grad[row * k + inner] += accumulated;
                    }
                }
            });
        // dB[inner, col] += sum_row A[row, inner] * g[row, col]. Parallel over
        // inner — each thread writes a disjoint band of b_node->grad.
        const float* a_data = a_node->data.data();
        float* b_grad = b_node->grad.data();
        runtime::parallel_for(
            static_cast<std::size_t>(k),
            parallel_grain(k, static_cast<std::size_t>(m) * n),
            [&](std::size_t, std::size_t inner_begin, std::size_t inner_end) {
                for (std::size_t inner = inner_begin; inner < inner_end; ++inner) {
                    for (int col = 0; col < n; ++col) {
                        float accumulated = 0.0F;
                        for (int row = 0; row < m; ++row) {
                            accumulated += a_data[row * k + inner] * g_data[row * n + col];
                        }
                        b_grad[inner * n + col] += accumulated;
                    }
                }
            });
    };
    return Tensor{node};
}

Tensor matmul_transposed_b(const Tensor& a, const Tensor& b) {
    const int m = a.shape()[0];
    const int k = a.shape()[1];
    const int n = b.shape()[0];  // b is [n, k]; out is [m, n]
    // Route the heavy op onto a ComputeDevice (MLX GPU / CUDA) when one is active,
    // else the host thread-pool kernels. Captured per-op so backprop runs on the
    // same device the graph was built on.
    compute::ComputeDevice* device = g_compute_device;

    std::vector<float> out;
    if (device != nullptr) {
        // a[m,k] · (b[n,k])ᵀ  — b is the transposed operand.
        out = device_gemm(*device, a.data(), b.data(), m, k, n, false, true);
    } else {
        out.assign(static_cast<std::size_t>(m) * n, 0.0F);
        const float* a_data = a.data().data();
        const float* b_data = b.data().data();
        float* out_data = out.data();
        // out[i, j] = dot(a_row_i, b_row_j) — two contiguous rows. Parallelize over
        // the larger output axis (inference: few rows, huge n -> over n).
        const auto compute_row = [&](std::size_t row) {
            const float* a_row = a_data + row * k;
            float* out_row = out_data + row * n;
            for (int j = 0; j < n; ++j) {
                const float* b_row = b_data + static_cast<std::size_t>(j) * k;
                float accumulated = 0.0F;
                for (int l = 0; l < k; ++l) accumulated += a_row[l] * b_row[l];
                out_row[j] = accumulated;
            }
        };
        if (m >= n) {
            runtime::parallel_for(
                static_cast<std::size_t>(m), parallel_grain(m, static_cast<std::size_t>(n) * k),
                [&](std::size_t, std::size_t row_begin, std::size_t row_end) {
                    for (std::size_t row = row_begin; row < row_end; ++row) compute_row(row);
                });
        } else {
            runtime::parallel_for(
                static_cast<std::size_t>(n), parallel_grain(n, static_cast<std::size_t>(m) * k),
                [&](std::size_t, std::size_t col_begin, std::size_t col_end) {
                    for (int row = 0; row < m; ++row) {
                        const float* a_row = a_data + static_cast<std::size_t>(row) * k;
                        float* out_row = out_data + static_cast<std::size_t>(row) * n;
                        for (std::size_t j = col_begin; j < col_end; ++j) {
                            const float* b_row = b_data + j * k;
                            float accumulated = 0.0F;
                            for (int l = 0; l < k; ++l) accumulated += a_row[l] * b_row[l];
                            out_row[j] = accumulated;
                        }
                    }
                });
        }
    }

    auto node = make_node(std::move(out), {m, n});
    node->parents = {a.node(), b.node()};
    const std::shared_ptr<Node> a_node = a.node();
    const std::shared_ptr<Node> b_node = b.node();
    node->backward = [a_node, b_node, m, k, n, device](const std::vector<float>& g) {
        if (device != nullptr) {
            // dA = dOut · B [m,k]; dB = dOutᵀ · A [n,k].
            const std::vector<float> da =
                device_gemm(*device, g, b_node->data, m, n, k, false, false);
            const std::vector<float> db =
                device_gemm(*device, g, a_node->data, n, m, k, true, false);
            for (std::size_t i = 0; i < da.size(); ++i) a_node->grad[i] += da[i];
            for (std::size_t i = 0; i < db.size(); ++i) b_node->grad[i] += db[i];
            return;
        }
        const float* g_data = g.data();
        const float* a_data2 = a_node->data.data();
        const float* b_data2 = b_node->data.data();
        float* a_grad = a_node->grad.data();
        float* b_grad = b_node->grad.data();
        // dA[i,:] += sum_j g[i,j] * B[j,:] — axpy over contiguous rows, over rows i.
        runtime::parallel_for(
            static_cast<std::size_t>(m), parallel_grain(m, static_cast<std::size_t>(n) * k),
            [&](std::size_t, std::size_t row_begin, std::size_t row_end) {
                for (std::size_t i = row_begin; i < row_end; ++i) {
                    float* da_row = a_grad + i * k;
                    const float* g_row = g_data + i * n;
                    for (int j = 0; j < n; ++j) {
                        const float d = g_row[j];
                        const float* b_row = b_data2 + static_cast<std::size_t>(j) * k;
                        for (int l = 0; l < k; ++l) da_row[l] += d * b_row[l];
                    }
                }
            });
        // dB[j,:] += sum_i g[i,j] * A[i,:] — axpy over contiguous rows, over j (n).
        runtime::parallel_for(
            static_cast<std::size_t>(n), parallel_grain(n, static_cast<std::size_t>(m) * k),
            [&](std::size_t, std::size_t col_begin, std::size_t col_end) {
                for (std::size_t j = col_begin; j < col_end; ++j) {
                    float* db_row = b_grad + j * k;
                    for (int i = 0; i < m; ++i) {
                        const float d = g_data[static_cast<std::size_t>(i) * n + j];
                        const float* a_row = a_data2 + static_cast<std::size_t>(i) * k;
                        for (int l = 0; l < k; ++l) db_row[l] += d * a_row[l];
                    }
                }
            });
    };
    return Tensor{node};
}

Tensor sum(const Tensor& a) {
    float total = 0.0F;
    for (float value : a.data()) {
        total += value;
    }
    auto node = make_node({total}, {1});
    node->parents = {a.node()};
    const std::shared_ptr<Node> a_node = a.node();
    node->backward = [a_node](const std::vector<float>& g) {
        for (float& gradient : a_node->grad) {
            gradient += g[0];
        }
    };
    return Tensor{node};
}

Tensor add(const Tensor& a, const Tensor& b) {
    std::vector<float> out(a.data().size(), 0.0F);
    {
        const float* left = a.data().data();
        const float* right = b.data().data();
        float* destination = out.data();
        parallel_elementwise(out.size(),
                             [&](std::size_t i) { destination[i] = left[i] + right[i]; });
    }
    auto node = make_node(std::move(out), a.shape());
    node->parents = {a.node(), b.node()};
    const std::shared_ptr<Node> a_node = a.node();
    const std::shared_ptr<Node> b_node = b.node();
    node->backward = [a_node, b_node](const std::vector<float>& g) {
        parallel_elementwise(g.size(), [&](std::size_t i) {
            a_node->grad[i] += g[i];
            b_node->grad[i] += g[i];
        });
    };
    return Tensor{node};
}

Tensor mul(const Tensor& a, const Tensor& b) {
    std::vector<float> out(a.data().size(), 0.0F);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = a.data()[i] * b.data()[i];
    }
    auto node = make_node(std::move(out), a.shape());
    node->parents = {a.node(), b.node()};
    const std::shared_ptr<Node> a_node = a.node();
    const std::shared_ptr<Node> b_node = b.node();
    node->backward = [a_node, b_node](const std::vector<float>& g) {
        for (std::size_t i = 0; i < g.size(); ++i) {
            a_node->grad[i] += g[i] * b_node->data[i];
            b_node->grad[i] += g[i] * a_node->data[i];
        }
    };
    return Tensor{node};
}

Tensor scale(const Tensor& a, float factor) {
    std::vector<float> out(a.data().size(), 0.0F);
    {
        const float* input = a.data().data();
        float* destination = out.data();
        parallel_elementwise(out.size(),
                             [&](std::size_t i) { destination[i] = input[i] * factor; });
    }
    auto node = make_node(std::move(out), a.shape());
    node->parents = {a.node()};
    const std::shared_ptr<Node> a_node = a.node();
    node->backward = [a_node, factor](const std::vector<float>& g) {
        parallel_elementwise(g.size(), [&](std::size_t i) { a_node->grad[i] += g[i] * factor; });
    };
    return Tensor{node};
}

Tensor gelu(const Tensor& a) {
    std::vector<float> out(a.data().size(), 0.0F);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float x = a.data()[i];
        out[i] = 0.5F * x * (1.0F + std::erf(x * kInvSqrt2));
    }
    auto node = make_node(std::move(out), a.shape());
    node->parents = {a.node()};
    const std::shared_ptr<Node> a_node = a.node();
    node->backward = [a_node](const std::vector<float>& g) {
        for (std::size_t i = 0; i < g.size(); ++i) {
            const float x = a_node->data[i];
            const float derivative =
                0.5F * (1.0F + std::erf(x * kInvSqrt2)) +
                0.5F * x * kSqrt2OverPi * std::exp(-0.5F * x * x);
            a_node->grad[i] += g[i] * derivative;
        }
    };
    return Tensor{node};
}

Tensor silu(const Tensor& a) {
    std::vector<float> out(a.data().size(), 0.0F);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float x = a.data()[i];
        out[i] = x * sigmoid(x);
    }
    auto node = make_node(std::move(out), a.shape());
    node->parents = {a.node()};
    const std::shared_ptr<Node> a_node = a.node();
    node->backward = [a_node](const std::vector<float>& g) {
        for (std::size_t i = 0; i < g.size(); ++i) {
            const float x = a_node->data[i];
            const float s = sigmoid(x);
            a_node->grad[i] += g[i] * (s * (1.0F + x * (1.0F - s)));
        }
    };
    return Tensor{node};
}

Tensor row_softmax(const Tensor& a) {
    const int rows = a.shape()[0];
    const int cols = a.shape()[1];
    std::vector<float> out(static_cast<std::size_t>(rows) * cols, 0.0F);
    // Rows are independent, so this threads with no reduction-order concern at all: each row's
    // max, sum and division touch only that row. That independence matters — a parallel reduction
    // ACROSS rows would combine in a nondeterministic order and break the exact-equality
    // assertions the CUDA equivalence gate relies on. Here there is nothing to combine.
    //
    // This is the hottest scalar loop in training. row_softmax runs on the [seq, seq] score matrix
    // once per head per layer, so a 28-layer Qwen3-0.6B at seq~1000 with 16 heads evaluates
    // ~448M std::exp calls per forward pass — and until now every one of them ran on a single core
    // while the other 29 sat idle.
    const float* input = a.data().data();
    runtime::parallel_for(
        static_cast<std::size_t>(rows),
        parallel_grain(static_cast<std::size_t>(rows), static_cast<std::size_t>(cols) * 4),
        [&](std::size_t, std::size_t row_begin, std::size_t row_end) {
            for (std::size_t row = row_begin; row < row_end; ++row) {
                const float* x = input + row * static_cast<std::size_t>(cols);
                float* destination = out.data() + row * static_cast<std::size_t>(cols);
                float max_value = x[0];
                for (int col = 1; col < cols; ++col) {
                    if (x[col] > max_value) max_value = x[col];
                }
                float total = 0.0F;
                for (int col = 0; col < cols; ++col) {
                    const float e = std::exp(x[col] - max_value);
                    destination[col] = e;
                    total += e;
                }
                const float inverse = 1.0F / total;  // one divide, then multiply
                for (int col = 0; col < cols; ++col) destination[col] *= inverse;
            }
        });

    auto node = make_node(std::move(out), a.shape());
    node->parents = {a.node()};
    const std::shared_ptr<Node> a_node = a.node();
    // Capture the output (softmax result) for the backward — Jacobian-vector product
    // per row: dX_j = Y_j * (dOut_j - sum_k dOut_k * Y_k).
    const std::vector<float> y = node->data;
    node->backward = [a_node, y, rows, cols](const std::vector<float>& g) {
        // Same row independence as the forward, and the same size — the backward is on the
        // [seq, seq] score matrix too, so leaving it serial would cap the win at half.
        runtime::parallel_for(
            static_cast<std::size_t>(rows),
            parallel_grain(static_cast<std::size_t>(rows), static_cast<std::size_t>(cols) * 4),
            [&](std::size_t, std::size_t row_begin, std::size_t row_end) {
                for (std::size_t row = row_begin; row < row_end; ++row) {
                    const std::size_t base = row * static_cast<std::size_t>(cols);
                    float dot = 0.0F;
                    for (int col = 0; col < cols; ++col) dot += g[base + col] * y[base + col];
                    for (int col = 0; col < cols; ++col) {
                        a_node->grad[base + col] += y[base + col] * (g[base + col] - dot);
                    }
                }
            });
    };
    return Tensor{node};
}

Tensor rms_norm(const Tensor& a, const Tensor& weight, float eps) {
    const int rows = a.shape()[0];
    const int cols = a.shape()[1];
    std::vector<float> out(static_cast<std::size_t>(rows) * cols, 0.0F);
    std::vector<float> inverse_rms(rows, 0.0F);  // 1 / sqrt(mean(x²)+eps) per row

    for (int row = 0; row < rows; ++row) {
        const float* x = a.data().data() + row * cols;
        float mean_square = 0.0F;
        for (int col = 0; col < cols; ++col) {
            mean_square += x[col] * x[col];
        }
        mean_square /= static_cast<float>(cols);
        const float inv = 1.0F / std::sqrt(mean_square + eps);
        inverse_rms[row] = inv;
        for (int col = 0; col < cols; ++col) {
            out[row * cols + col] = x[col] * inv * weight.data()[col];
        }
    }

    auto node = make_node(std::move(out), a.shape());
    node->parents = {a.node(), weight.node()};
    const std::shared_ptr<Node> a_node = a.node();
    const std::shared_ptr<Node> weight_node = weight.node();
    node->backward = [a_node, weight_node, inverse_rms, rows,
                      cols](const std::vector<float>& g) {
        for (int row = 0; row < rows; ++row) {
            const float* x = a_node->data.data() + row * cols;
            const float inv = inverse_rms[row];
            const float inv_cubed = inv * inv * inv;
            // dot = sum_j (dOut_j * weight_j) * x_j.
            float dot = 0.0F;
            for (int col = 0; col < cols; ++col) {
                dot += g[row * cols + col] * weight_node->data[col] * x[col];
            }
            for (int col = 0; col < cols; ++col) {
                const float gw = g[row * cols + col] * weight_node->data[col];
                a_node->grad[row * cols + col] +=
                    gw * inv - x[col] * dot * inv_cubed / static_cast<float>(cols);
                weight_node->grad[col] += g[row * cols + col] * x[col] * inv;
            }
        }
    };
    return Tensor{node};
}

Tensor transpose(const Tensor& a) {
    const int m = a.shape()[0];
    const int n = a.shape()[1];
    std::vector<float> out(static_cast<std::size_t>(m) * n, 0.0F);
    for (int row = 0; row < m; ++row) {
        for (int col = 0; col < n; ++col) {
            out[col * m + row] = a.data()[row * n + col];
        }
    }
    auto node = make_node(std::move(out), {n, m});
    node->parents = {a.node()};
    const std::shared_ptr<Node> a_node = a.node();
    node->backward = [a_node, m, n](const std::vector<float>& g) {
        for (int row = 0; row < m; ++row) {
            for (int col = 0; col < n; ++col) {
                a_node->grad[row * n + col] += g[col * m + row];
            }
        }
    };
    return Tensor{node};
}

Tensor embedding(const Tensor& table, const std::vector<int>& ids) {
    const int dim = table.shape()[1];
    const int count = static_cast<int>(ids.size());
    std::vector<float> out(static_cast<std::size_t>(count) * dim, 0.0F);
    for (int i = 0; i < count; ++i) {
        const int row = ids[i];
        for (int d = 0; d < dim; ++d) {
            out[i * dim + d] = table.data()[row * dim + d];
        }
    }
    auto node = make_node(std::move(out), {count, dim});
    node->parents = {table.node()};
    const std::shared_ptr<Node> table_node = table.node();
    node->backward = [table_node, ids, dim, count](const std::vector<float>& g) {
        for (int i = 0; i < count; ++i) {
            const int row = ids[i];
            for (int d = 0; d < dim; ++d) {
                table_node->grad[row * dim + d] += g[i * dim + d];
            }
        }
    };
    return Tensor{node};
}

Tensor patch_embedding(const Tensor& image, const Tensor& weight,
                       const Tensor& bias, int patch_size) {
    const int height = image.shape()[0];
    const int width = image.shape()[1];
    const int channels = image.shape()[2];
    const int hidden = weight.shape()[0];
    const int patch_rows = height / patch_size;
    const int patch_columns = width / patch_size;
    const int patch_count = patch_rows * patch_columns;
    const int patch_values = patch_size * patch_size * channels;
    std::vector<float> out(static_cast<std::size_t>(patch_count) * hidden,
                           0.0F);
    for (int patch_row = 0; patch_row < patch_rows; ++patch_row) {
        for (int patch_column = 0; patch_column < patch_columns;
             ++patch_column) {
            const int patch_index = patch_row * patch_columns + patch_column;
            for (int output = 0; output < hidden; ++output) {
                float value = bias.data()[output];
                int element = 0;
                for (int y = 0; y < patch_size; ++y) {
                    for (int x = 0; x < patch_size; ++x) {
                        for (int channel = 0; channel < channels; ++channel) {
                            const int image_index =
                                (((patch_row * patch_size + y) * width +
                                  patch_column * patch_size + x) *
                                     channels +
                                 channel);
                            value += image.data()[image_index] *
                                     weight.data()[output * patch_values +
                                                   element];
                            ++element;
                        }
                    }
                }
                out[patch_index * hidden + output] = value;
            }
        }
    }

    auto node = make_node(std::move(out), {patch_count, hidden});
    node->parents = {image.node(), weight.node(), bias.node()};
    const std::shared_ptr<Node> image_node = image.node();
    const std::shared_ptr<Node> weight_node = weight.node();
    const std::shared_ptr<Node> bias_node = bias.node();
    node->backward = [image_node, weight_node, bias_node, width, channels,
                      hidden, patch_size, patch_rows, patch_columns,
                      patch_values](const std::vector<float>& gradient) {
        for (int patch_row = 0; patch_row < patch_rows; ++patch_row) {
            for (int patch_column = 0; patch_column < patch_columns;
                 ++patch_column) {
                const int patch_index =
                    patch_row * patch_columns + patch_column;
                for (int output = 0; output < hidden; ++output) {
                    const float output_gradient =
                        gradient[patch_index * hidden + output];
                    bias_node->grad[output] += output_gradient;
                    int element = 0;
                    for (int y = 0; y < patch_size; ++y) {
                        for (int x = 0; x < patch_size; ++x) {
                            for (int channel = 0; channel < channels;
                                 ++channel) {
                                const int image_index =
                                    (((patch_row * patch_size + y) * width +
                                      patch_column * patch_size + x) *
                                         channels +
                                     channel);
                                const int weight_index =
                                    output * patch_values + element;
                                image_node->grad[image_index] +=
                                    output_gradient *
                                    weight_node->data[weight_index];
                                weight_node->grad[weight_index] +=
                                    output_gradient *
                                    image_node->data[image_index];
                                ++element;
                            }
                        }
                    }
                }
            }
        }
    };
    return Tensor{node};
}

Tensor cross_entropy(const Tensor& logits, const std::vector<int>& targets, int first_scored) {
    const int rows = logits.shape()[0];
    const int vocab = logits.shape()[1];
    // Clamp rather than reject: a boundary past the end would divide by zero, and a caller whose
    // prompt filled the whole window should train on nothing rather than produce a NaN loss.
    const int begin = first_scored < 0 ? 0 : (first_scored >= rows ? rows - 1 : first_scored);
    const int scored = rows - begin;
    std::vector<float> softmax(static_cast<std::size_t>(rows) * vocab, 0.0F);
    float total = 0.0F;

    for (int row = begin; row < rows; ++row) {  // masked rows keep softmax 0 and are skipped
        const float* x = logits.data().data() + row * vocab;
        float max_value = x[0];
        for (int j = 1; j < vocab; ++j) {
            if (x[j] > max_value) max_value = x[j];
        }
        float sum_exp = 0.0F;
        for (int j = 0; j < vocab; ++j) {
            const float e = std::exp(x[j] - max_value);
            softmax[row * vocab + j] = e;
            sum_exp += e;
        }
        for (int j = 0; j < vocab; ++j) {
            softmax[row * vocab + j] /= sum_exp;
        }
        // loss_row = log(sum_exp) + max − x[target] = −log(softmax[target]).
        total += std::log(sum_exp) + max_value - x[targets[row]];
    }
    total /= static_cast<float>(scored);

    auto node = make_node({total}, {1});
    node->parents = {logits.node()};
    const std::shared_ptr<Node> logits_node = logits.node();
    node->backward = [logits_node, softmax, targets, rows, begin, scored,
                      vocab](const std::vector<float>& g) {
        const float scale_factor = g[0] / static_cast<float>(scored);
        for (int row = begin; row < rows; ++row) {  // masked rows receive exactly zero gradient
            for (int j = 0; j < vocab; ++j) {
                const float indicator = (j == targets[row]) ? 1.0F : 0.0F;
                logits_node->grad[row * vocab + j] +=
                    (softmax[row * vocab + j] - indicator) * scale_factor;
            }
        }
    };
    return Tensor{node};
}

Tensor row_slice(const Tensor& a, int start, int count) {
    const int cols = a.shape()[1];
    std::vector<float> out(static_cast<std::size_t>(count) * cols, 0.0F);
    for (int k = 0; k < count; ++k) {
        for (int col = 0; col < cols; ++col) {
            out[k * cols + col] = a.data()[(start + k) * cols + col];
        }
    }
    auto node = make_node(std::move(out), {count, cols});
    node->parents = {a.node()};
    const std::shared_ptr<Node> a_node = a.node();
    node->backward = [a_node, start, count, cols](const std::vector<float>& g) {
        for (int k = 0; k < count; ++k) {
            for (int col = 0; col < cols; ++col) {
                a_node->grad[(start + k) * cols + col] += g[k * cols + col];
            }
        }
    };
    return Tensor{node};
}

Tensor policy_gradient_loss(const Tensor& logits,
                            const std::vector<int>& actions,
                            const std::vector<double>& advantages) {
    const int vocab = logits.shape()[1];
    const int group = static_cast<int>(actions.size());

    // softmax of the single logits row.
    const float* x = logits.data().data();
    float max_value = x[0];
    for (int j = 1; j < vocab; ++j) {
        if (x[j] > max_value) max_value = x[j];
    }
    std::vector<float> softmax(vocab, 0.0F);
    float sum_exp = 0.0F;
    for (int j = 0; j < vocab; ++j) {
        softmax[j] = std::exp(x[j] - max_value);
        sum_exp += softmax[j];
    }
    for (int j = 0; j < vocab; ++j) {
        softmax[j] /= sum_exp;
    }
    const float log_sum = std::log(sum_exp) + max_value;  // for log softmax

    float loss = 0.0F;
    for (int i = 0; i < group; ++i) {
        const float log_prob = x[actions[i]] - log_sum;
        loss += static_cast<float>(advantages[i]) * log_prob;
    }
    loss = group > 0 ? -loss / static_cast<float>(group) : 0.0F;

    auto node = make_node({loss}, {1});
    node->parents = {logits.node()};
    const std::shared_ptr<Node> logits_node = logits.node();
    node->backward = [logits_node, softmax, actions, advantages, group,
                      vocab](const std::vector<float>& g) {
        if (group == 0) {
            return;
        }
        const float scale_factor = g[0] / static_cast<float>(group);
        // Per-action advantage mass, and the total advantage.
        float total_advantage = 0.0F;
        std::vector<float> advantage_at(vocab, 0.0F);
        for (int i = 0; i < group; ++i) {
            advantage_at[actions[i]] += static_cast<float>(advantages[i]);
            total_advantage += static_cast<float>(advantages[i]);
        }
        for (int j = 0; j < vocab; ++j) {
            // dLoss/dLogit_j = -(1/G) * (advantage_at_j - softmax_j * total).
            logits_node->grad[j] +=
                -scale_factor * (advantage_at[j] - softmax[j] * total_advantage);
        }
    };
    return Tensor{node};
}

Tensor sequence_policy_gradient_loss(const Tensor& logits,
                                     const std::vector<int>& actions,
                                     double advantage) {
    const int rows = logits.shape()[0];
    const int vocab = logits.shape()[1];
    const float adv = static_cast<float>(advantage);

    std::vector<float> softmax(static_cast<std::size_t>(rows) * vocab, 0.0F);
    float log_prob_sum = 0.0F;
    for (int row = 0; row < rows; ++row) {
        const float* x = logits.data().data() + row * vocab;
        float max_value = x[0];
        for (int j = 1; j < vocab; ++j) {
            if (x[j] > max_value) max_value = x[j];
        }
        float sum_exp = 0.0F;
        for (int j = 0; j < vocab; ++j) {
            const float e = std::exp(x[j] - max_value);
            softmax[row * vocab + j] = e;
            sum_exp += e;
        }
        for (int j = 0; j < vocab; ++j) {
            softmax[row * vocab + j] /= sum_exp;
        }
        const float log_sum = std::log(sum_exp) + max_value;  // log softmax denom
        log_prob_sum += x[actions[row]] - log_sum;
    }
    const float loss = -adv * log_prob_sum;

    auto node = make_node({loss}, {1});
    node->parents = {logits.node()};
    const std::shared_ptr<Node> logits_node = logits.node();
    node->backward = [logits_node, softmax, actions, adv, rows,
                      vocab](const std::vector<float>& g) {
        const float scale_factor = -adv * g[0];
        for (int row = 0; row < rows; ++row) {
            for (int j = 0; j < vocab; ++j) {
                const float indicator = (j == actions[row]) ? 1.0F : 0.0F;
                logits_node->grad[row * vocab + j] +=
                    scale_factor * (indicator - softmax[row * vocab + j]);
            }
        }
    };
    return Tensor{node};
}

Tensor rope(const Tensor& x, float base) {
    const int seq = x.shape()[0];
    const int d = x.shape()[1];
    const int half = d / 2;
    std::vector<float> out(static_cast<std::size_t>(seq) * d, 0.0F);
    // Cache cos/sin per (position, pair) for the backward (the inverse rotation).
    std::vector<float> cos_cache(static_cast<std::size_t>(seq) * half, 0.0F);
    std::vector<float> sin_cache(static_cast<std::size_t>(seq) * half, 0.0F);

    for (int pos = 0; pos < seq; ++pos) {
        for (int i = 0; i < half; ++i) {
            const double inv_freq =
                std::pow(static_cast<double>(base), -2.0 * i / d);
            const double angle = pos * inv_freq;
            const float c = static_cast<float>(std::cos(angle));
            const float s = static_cast<float>(std::sin(angle));
            cos_cache[pos * half + i] = c;
            sin_cache[pos * half + i] = s;
            const float a = x.data()[pos * d + i];
            const float b = x.data()[pos * d + i + half];
            out[pos * d + i] = a * c - b * s;
            out[pos * d + i + half] = b * c + a * s;
        }
    }

    auto node = make_node(std::move(out), x.shape());
    node->parents = {x.node()};
    const std::shared_ptr<Node> a_node = x.node();
    node->backward = [a_node, cos_cache, sin_cache, seq, d,
                      half](const std::vector<float>& g) {
        for (int pos = 0; pos < seq; ++pos) {
            for (int i = 0; i < half; ++i) {
                const float c = cos_cache[pos * half + i];
                const float s = sin_cache[pos * half + i];
                const float g0 = g[pos * d + i];
                const float g1 = g[pos * d + i + half];
                a_node->grad[pos * d + i] += g0 * c + g1 * s;
                a_node->grad[pos * d + i + half] += -g0 * s + g1 * c;
            }
        }
    };
    return Tensor{node};
}

Tensor col_slice(const Tensor& a, int start, int count) {
    const int rows = a.shape()[0];
    const int cols = a.shape()[1];
    std::vector<float> out(static_cast<std::size_t>(rows) * count, 0.0F);
    for (int row = 0; row < rows; ++row) {
        for (int c = 0; c < count; ++c) {
            out[row * count + c] = a.data()[row * cols + start + c];
        }
    }
    auto node = make_node(std::move(out), {rows, count});
    node->parents = {a.node()};
    const std::shared_ptr<Node> a_node = a.node();
    node->backward = [a_node, start, count, rows, cols](const std::vector<float>& g) {
        for (int row = 0; row < rows; ++row) {
            for (int c = 0; c < count; ++c) {
                a_node->grad[row * cols + start + c] += g[row * count + c];
            }
        }
    };
    return Tensor{node};
}

Tensor concat_cols(const std::vector<Tensor>& parts) {
    const int rows = parts[0].shape()[0];
    int total_cols = 0;
    for (const Tensor& part : parts) {
        total_cols += part.shape()[1];
    }
    std::vector<float> out(static_cast<std::size_t>(rows) * total_cols, 0.0F);
    int offset = 0;
    for (const Tensor& part : parts) {
        const int width = part.shape()[1];
        for (int row = 0; row < rows; ++row) {
            for (int c = 0; c < width; ++c) {
                out[row * total_cols + offset + c] = part.data()[row * width + c];
            }
        }
        offset += width;
    }

    auto node = make_node(std::move(out), {rows, total_cols});
    for (const Tensor& part : parts) {
        node->parents.push_back(part.node());
    }
    std::vector<std::shared_ptr<Node>> part_nodes;
    std::vector<int> widths;
    for (const Tensor& part : parts) {
        part_nodes.push_back(part.node());
        widths.push_back(part.shape()[1]);
    }
    node->backward = [part_nodes, widths, rows,
                      total_cols](const std::vector<float>& g) {
        int offset = 0;
        for (std::size_t p = 0; p < part_nodes.size(); ++p) {
            const int width = widths[p];
            for (int row = 0; row < rows; ++row) {
                for (int c = 0; c < width; ++c) {
                    part_nodes[p]->grad[row * width + c] +=
                        g[row * total_cols + offset + c];
                }
            }
            offset += width;
        }
    };
    return Tensor{node};
}

Tensor add_bias(const Tensor& a, const Tensor& bias) {
    const int rows = a.shape()[0];
    const int cols = a.shape()[1];
    std::vector<float> out(static_cast<std::size_t>(rows) * cols, 0.0F);
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            out[row * cols + col] = a.data()[row * cols + col] + bias.data()[col];
        }
    }
    auto node = make_node(std::move(out), a.shape());
    node->parents = {a.node(), bias.node()};
    const std::shared_ptr<Node> a_node = a.node();
    const std::shared_ptr<Node> bias_node = bias.node();
    node->backward = [a_node, bias_node, rows, cols](const std::vector<float>& g) {
        for (int row = 0; row < rows; ++row) {
            for (int col = 0; col < cols; ++col) {
                a_node->grad[row * cols + col] += g[row * cols + col];
                bias_node->grad[col] += g[row * cols + col];
            }
        }
    };
    return Tensor{node};
}

}  // namespace zetta::autograd
