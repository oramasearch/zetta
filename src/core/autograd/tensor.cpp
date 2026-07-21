#include "zetta/autograd/tensor.hpp"

#include <cmath>
#include <cstddef>
#include <unordered_set>

namespace zetta::autograd {

namespace {
constexpr float kInvSqrt2 = 0.7071067811865476F;      // 1 / sqrt(2)
constexpr float kSqrt2OverPi = 0.7978845608028654F;   // sqrt(2 / pi)

float sigmoid(float x) { return 1.0F / (1.0F + std::exp(-x)); }
}  // namespace

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

    std::vector<float> out(static_cast<std::size_t>(m) * n, 0.0F);
    for (int row = 0; row < m; ++row) {
        for (int inner = 0; inner < k; ++inner) {
            const float a_value = a.data()[row * k + inner];
            for (int col = 0; col < n; ++col) {
                out[row * n + col] += a_value * b.data()[inner * n + col];
            }
        }
    }

    auto node = make_node(std::move(out), {m, n});
    node->parents = {a.node(), b.node()};
    const std::shared_ptr<Node> a_node = a.node();
    const std::shared_ptr<Node> b_node = b.node();
    node->backward = [a_node, b_node, m, k, n](const std::vector<float>& g) {
        // dA[row, inner] += sum_col g[row, col] * B[inner, col].
        for (int row = 0; row < m; ++row) {
            for (int inner = 0; inner < k; ++inner) {
                float accumulated = 0.0F;
                for (int col = 0; col < n; ++col) {
                    accumulated += g[row * n + col] * b_node->data[inner * n + col];
                }
                a_node->grad[row * k + inner] += accumulated;
            }
        }
        // dB[inner, col] += sum_row A[row, inner] * g[row, col].
        for (int inner = 0; inner < k; ++inner) {
            for (int col = 0; col < n; ++col) {
                float accumulated = 0.0F;
                for (int row = 0; row < m; ++row) {
                    accumulated += a_node->data[row * k + inner] * g[row * n + col];
                }
                b_node->grad[inner * n + col] += accumulated;
            }
        }
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
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = a.data()[i] + b.data()[i];
    }
    auto node = make_node(std::move(out), a.shape());
    node->parents = {a.node(), b.node()};
    const std::shared_ptr<Node> a_node = a.node();
    const std::shared_ptr<Node> b_node = b.node();
    node->backward = [a_node, b_node](const std::vector<float>& g) {
        for (std::size_t i = 0; i < g.size(); ++i) {
            a_node->grad[i] += g[i];
            b_node->grad[i] += g[i];
        }
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
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = a.data()[i] * factor;
    }
    auto node = make_node(std::move(out), a.shape());
    node->parents = {a.node()};
    const std::shared_ptr<Node> a_node = a.node();
    node->backward = [a_node, factor](const std::vector<float>& g) {
        for (std::size_t i = 0; i < g.size(); ++i) {
            a_node->grad[i] += g[i] * factor;
        }
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
    for (int row = 0; row < rows; ++row) {
        const float* x = a.data().data() + row * cols;
        float max_value = x[0];
        for (int col = 1; col < cols; ++col) {
            if (x[col] > max_value) max_value = x[col];
        }
        float total = 0.0F;
        for (int col = 0; col < cols; ++col) {
            const float e = std::exp(x[col] - max_value);
            out[row * cols + col] = e;
            total += e;
        }
        for (int col = 0; col < cols; ++col) {
            out[row * cols + col] /= total;
        }
    }

    auto node = make_node(std::move(out), a.shape());
    node->parents = {a.node()};
    const std::shared_ptr<Node> a_node = a.node();
    // Capture the output (softmax result) for the backward — Jacobian-vector product
    // per row: dX_j = Y_j * (dOut_j - sum_k dOut_k * Y_k).
    const std::vector<float> y = node->data;
    node->backward = [a_node, y, rows, cols](const std::vector<float>& g) {
        for (int row = 0; row < rows; ++row) {
            float dot = 0.0F;
            for (int col = 0; col < cols; ++col) {
                dot += g[row * cols + col] * y[row * cols + col];
            }
            for (int col = 0; col < cols; ++col) {
                a_node->grad[row * cols + col] +=
                    y[row * cols + col] * (g[row * cols + col] - dot);
            }
        }
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

Tensor cross_entropy(const Tensor& logits, const std::vector<int>& targets) {
    const int rows = logits.shape()[0];
    const int vocab = logits.shape()[1];
    std::vector<float> softmax(static_cast<std::size_t>(rows) * vocab, 0.0F);
    float total = 0.0F;

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
        // loss_row = log(sum_exp) + max − x[target] = −log(softmax[target]).
        total += std::log(sum_exp) + max_value - x[targets[row]];
    }
    total /= static_cast<float>(rows);

    auto node = make_node({total}, {1});
    node->parents = {logits.node()};
    const std::shared_ptr<Node> logits_node = logits.node();
    node->backward = [logits_node, softmax, targets, rows,
                      vocab](const std::vector<float>& g) {
        const float scale_factor = g[0] / static_cast<float>(rows);
        for (int row = 0; row < rows; ++row) {
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
