#include "zetta/compute/cuda_transformer.hpp"

#include <cmath>
#include <string_view>

#include <cuda_runtime.h>

// Fused transformer kernels (#922). See the header for why these exist and the two rules they
// follow (operate on device buffers; fused and deterministic).
namespace zetta::compute {
namespace {

constexpr int kThreadsPerBlock = 256;

ComputeStatus from_cuda(cudaError_t error, std::string_view context) {
    if (error == cudaSuccess) return ComputeStatus::success();
    std::string reason = std::string{context} + ": " + cudaGetErrorString(error);
    if (error == cudaErrorMemoryAllocation) return ComputeStatus::out_of_memory(std::move(reason));
    return ComputeStatus::backend_error(std::move(reason));
}

// A launch is only "ok" once it has actually run: cudaGetLastError catches launch-time rejection,
// the synchronize surfaces faults raised inside the kernel. Reporting success before that turns a
// crashed kernel into silently wrong numbers.
ComputeStatus check_launch(std::string_view context) {
    const cudaError_t launch = cudaGetLastError();
    if (launch != cudaSuccess) return from_cuda(launch, context);
    return from_cuda(cudaDeviceSynchronize(), context);
}

const float* as_const_float(const DeviceBuffer& buffer) {
    return static_cast<const float*>(buffer.handle());
}
float* as_float(DeviceBuffer& buffer) { return static_cast<float*>(buffer.handle()); }

// Sum `value` across the block into thread 0's return, using shared memory. Deterministic for a
// fixed block size: the tree reduction always combines the same pairs in the same order.
__device__ float block_sum(float value, float* scratch) {
    const int lane = static_cast<int>(threadIdx.x);
    scratch[lane] = value;
    __syncthreads();
    for (int stride = kThreadsPerBlock / 2; stride > 0; stride >>= 1) {
        if (lane < stride) scratch[lane] += scratch[lane + stride];
        __syncthreads();
    }
    return scratch[0];
}

// out[r, c] = x[r, c] * rsqrt(mean(x[r, :]^2) + eps) * weight[c]
//
// One block per row. The row is read twice from L2/global (once to accumulate the sum of squares,
// once to scale) and written once — versus a composed implementation, which would materialize the
// squares, the row means, and the reciprocal as separate full-size tensors.
__global__ void rms_norm_kernel(const float* __restrict__ x, const float* __restrict__ weight,
                                float* __restrict__ out, int rows, int cols, float epsilon) {
    __shared__ float scratch[kThreadsPerBlock];
    const int row = static_cast<int>(blockIdx.x);
    if (row >= rows) return;
    const float* row_x = x + static_cast<long long>(row) * cols;
    float* row_out = out + static_cast<long long>(row) * cols;

    float local = 0.0F;
    for (int c = static_cast<int>(threadIdx.x); c < cols; c += kThreadsPerBlock) {
        const float value = row_x[c];
        local += value * value;
    }
    const float total = block_sum(local, scratch);
    const float scale = rsqrtf(total / static_cast<float>(cols) + epsilon);

    for (int c = static_cast<int>(threadIdx.x); c < cols; c += kThreadsPerBlock) {
        row_out[c] = row_x[c] * scale * weight[c];
    }
}

// grad_x, one block per row. Needs two row reductions (sum of squares for the scale, and
// sum(dout * x) for the projection term), both fused into this single pass over the row.
__global__ void rms_norm_grad_x_kernel(const float* __restrict__ x,
                                      const float* __restrict__ weight,
                                      const float* __restrict__ grad_out,
                                      float* __restrict__ grad_x, int rows, int cols,
                                      float epsilon) {
    __shared__ float scratch[kThreadsPerBlock];
    const int row = static_cast<int>(blockIdx.x);
    if (row >= rows) return;
    const long long offset = static_cast<long long>(row) * cols;
    const float* row_x = x + offset;
    const float* row_grad_out = grad_out + offset;
    float* row_grad_x = grad_x + offset;

    float local_square = 0.0F;
    for (int c = static_cast<int>(threadIdx.x); c < cols; c += kThreadsPerBlock) {
        const float value = row_x[c];
        local_square += value * value;
    }
    const float mean_square = block_sum(local_square, scratch) / static_cast<float>(cols);
    const float scale = rsqrtf(mean_square + epsilon);

    float local_dot = 0.0F;
    for (int c = static_cast<int>(threadIdx.x); c < cols; c += kThreadsPerBlock) {
        local_dot += row_grad_out[c] * weight[c] * row_x[c];
    }
    __syncthreads();  // block_sum reuses `scratch`, so the previous reduction must be finished
    const float mean_dot = block_sum(local_dot, scratch) / static_cast<float>(cols);

    for (int c = static_cast<int>(threadIdx.x); c < cols; c += kThreadsPerBlock) {
        const float dout = row_grad_out[c] * weight[c];
        row_grad_x[c] = scale * (dout - row_x[c] * scale * scale * mean_dot);
    }
}

// grad_weight[c] += sum_r grad_out[r, c] * x[r, c] * scale_r
//
// One thread per COLUMN, looping over rows in a fixed order. Deliberately not an atomicAdd from the
// per-row kernel: float atomics accumulate in whatever order blocks happen to retire, so the result
// varies in its last bits between identical runs — and #926 asserts CUDA reproduces the reference
// exactly. A deterministic gradient is worth one extra pass over the activation.
__global__ void rms_norm_grad_weight_kernel(const float* __restrict__ x,
                                            const float* __restrict__ grad_out,
                                            const float* __restrict__ row_scale,
                                            float* __restrict__ grad_weight, int rows, int cols) {
    const int col = static_cast<int>(blockIdx.x) * kThreadsPerBlock + static_cast<int>(threadIdx.x);
    if (col >= cols) return;
    float total = 0.0F;
    for (int row = 0; row < rows; ++row) {
        const long long offset = static_cast<long long>(row) * cols + col;
        total += grad_out[offset] * x[offset] * row_scale[row];
    }
    grad_weight[col] += total;  // accumulate, matching the autograd engine's leaf gradients
}

// The per-row 1/rms factor, so grad_weight does not recompute it per column.
__global__ void rms_norm_row_scale_kernel(const float* __restrict__ x, float* __restrict__ row_scale,
                                          int rows, int cols, float epsilon) {
    __shared__ float scratch[kThreadsPerBlock];
    const int row = static_cast<int>(blockIdx.x);
    if (row >= rows) return;
    const float* row_x = x + static_cast<long long>(row) * cols;
    float local = 0.0F;
    for (int c = static_cast<int>(threadIdx.x); c < cols; c += kThreadsPerBlock) {
        const float value = row_x[c];
        local += value * value;
    }
    const float total = block_sum(local, scratch);
    if (threadIdx.x == 0) {
        row_scale[row] = rsqrtf(total / static_cast<float>(cols) + epsilon);
    }
}


// --- RoPE ------------------------------------------------------------------------------------
// One thread per ROTATED PAIR: thread (row, i) owns elements i and i + half, so the rotation is a
// single read-modify-write of both halves with no synchronization. Writing per-element instead
// would make each thread recompute the same sin/cos its partner needs.
__global__ void rope_kernel(const float* __restrict__ x, float* __restrict__ out, int rows,
                            int head_dim, float base, int position_offset, bool inverse,
                            int rows_per_position) {
    const int half = head_dim / 2;
    const int pair = static_cast<int>(blockIdx.x) * kThreadsPerBlock + static_cast<int>(threadIdx.x);
    const int total = rows * half;
    if (pair >= total) return;
    const int row = pair / half;
    const int index = pair % half;

    const float exponent = -2.0F * static_cast<float>(index) / static_cast<float>(head_dim);
    const float inverse_frequency = powf(base, exponent);
    // Every head of a token shares that token's angle: with the packed [seq * n_heads, head_dim]
    // layout the row is `s * n_heads + h`, so the position is row / rows_per_position.
    const int position = row / rows_per_position + position_offset;
    const float angle = static_cast<float>(position) * inverse_frequency;
    const float cosine = cosf(angle);
    const float sine = sinf(angle);

    const long long offset = static_cast<long long>(row) * head_dim + index;
    const float low = x[offset];
    const float high = x[offset + half];
    if (inverse) {
        // The transpose of the rotation, which for an orthogonal matrix is its inverse.
        out[offset] = low * cosine + high * sine;
        out[offset + half] = -low * sine + high * cosine;
    } else {
        out[offset] = low * cosine - high * sine;
        out[offset + half] = low * sine + high * cosine;
    }
}

// --- SwiGLU ----------------------------------------------------------------------------------
__device__ inline float sigmoid_of(float value) { return 1.0F / (1.0F + expf(-value)); }

// out = silu(gate) * up. silu(gate) is never written to memory — that is the fusion.
__global__ void swiglu_kernel(const float* __restrict__ gate, const float* __restrict__ up,
                              float* __restrict__ out, long long elements) {
    const long long index =
        static_cast<long long>(blockIdx.x) * kThreadsPerBlock + threadIdx.x;
    if (index >= elements) return;
    const float g = gate[index];
    out[index] = g * sigmoid_of(g) * up[index];
}

// Both gradients in one pass: they share sigmoid(gate), and recomputing it is cheaper than saving
// it in the forward pass and reading it back.
__global__ void swiglu_backward_kernel(const float* __restrict__ gate,
                                       const float* __restrict__ up,
                                       const float* __restrict__ grad_out,
                                       float* __restrict__ grad_gate, float* __restrict__ grad_up,
                                       long long elements) {
    const long long index =
        static_cast<long long>(blockIdx.x) * kThreadsPerBlock + threadIdx.x;
    if (index >= elements) return;
    const float g = gate[index];
    const float s = sigmoid_of(g);
    const float silu = g * s;
    const float upstream = grad_out[index];
    grad_gate[index] = upstream * up[index] * s * (1.0F + g * (1.0F - s));
    grad_up[index] = upstream * silu;
}


// --- Causal self-attention -------------------------------------------------------------------
// One block per (head, query). Threads split the head dimension, so a dot product is a block
// reduction and the accumulator lives in registers/shared memory. Keys are streamed one at a time
// with the online-softmax recurrence, so the score row is never stored:
//
//   new_max = max(old_max, score)
//   sum    *= exp(old_max - new_max);  acc *= exp(old_max - new_max)
//   sum    += exp(score - new_max);    acc += exp(score - new_max) * v
//
// The rescaling by exp(old_max - new_max) is what keeps this numerically identical to a two-pass
// softmax rather than merely close to it.
__global__ void causal_attention_kernel(const float* __restrict__ q, const float* __restrict__ k,
                                        const float* __restrict__ v, float* __restrict__ out,
                                        float* __restrict__ softmax_lse, int seq, int n_heads,
                                        int n_kv_heads, int head_dim) {
    extern __shared__ float shared[];
    float* reduce = shared;                  // [kThreadsPerBlock] for dot-product reductions
    float* accumulator = shared + kThreadsPerBlock;  // [head_dim] the running weighted sum of V

    const int head = static_cast<int>(blockIdx.y);
    const int query = static_cast<int>(blockIdx.x);
    if (head >= n_heads || query >= seq) return;
    const int group = n_heads / n_kv_heads;  // GQA: how many query heads share one kv head
    const int kv_head = head / group;
    const int lane = static_cast<int>(threadIdx.x);

    const float* q_row = q + static_cast<long long>(query) * n_heads * head_dim +
                         static_cast<long long>(head) * head_dim;
    const float scale = rsqrtf(static_cast<float>(head_dim));

    for (int d = lane; d < head_dim; d += kThreadsPerBlock) accumulator[d] = 0.0F;
    __syncthreads();

    float running_max = -INFINITY;
    float running_sum = 0.0F;

    // Causal: only keys 0..query are visible, so the mask is a loop bound rather than a
    // materialized -inf matrix that then has to be read back.
    for (int key = 0; key <= query; ++key) {
        const float* k_row = k + static_cast<long long>(key) * n_kv_heads * head_dim +
                             static_cast<long long>(kv_head) * head_dim;
        float partial = 0.0F;
        for (int d = lane; d < head_dim; d += kThreadsPerBlock) partial += q_row[d] * k_row[d];
        const float score = block_sum(partial, reduce) * scale;

        const float new_max = fmaxf(running_max, score);
        const float rescale = (running_max == -INFINITY) ? 0.0F : __expf(running_max - new_max);
        const float weight = __expf(score - new_max);

        const float* v_row = v + static_cast<long long>(key) * n_kv_heads * head_dim +
                             static_cast<long long>(kv_head) * head_dim;
        __syncthreads();  // every thread needs the reduced score before touching the accumulator
        for (int d = lane; d < head_dim; d += kThreadsPerBlock) {
            accumulator[d] = accumulator[d] * rescale + weight * v_row[d];
        }
        running_sum = running_sum * rescale + weight;
        running_max = new_max;
        __syncthreads();
    }

    const float inverse_sum = 1.0F / running_sum;
    float* out_row = out + static_cast<long long>(query) * n_heads * head_dim +
                     static_cast<long long>(head) * head_dim;
    for (int d = lane; d < head_dim; d += kThreadsPerBlock) out_row[d] = accumulator[d] * inverse_sum;
    if (lane == 0) {
        // log-sum-exp, so the backward can rebuild p = exp(score - lse) with no max pass.
        softmax_lse[static_cast<long long>(head) * seq + query] = running_max + __logf(running_sum);
    }
}

// row_dot[head, query] = sum_d grad_out[query, head, d] * out[query, head, d]
// Needed by every key block in the backward, so it is computed once here rather than repeatedly.
__global__ void attention_row_dot_kernel(const float* __restrict__ grad_out,
                                         const float* __restrict__ out, float* __restrict__ row_dot,
                                         int seq, int n_heads, int head_dim) {
    __shared__ float reduce[kThreadsPerBlock];
    const int head = static_cast<int>(blockIdx.y);
    const int query = static_cast<int>(blockIdx.x);
    if (head >= n_heads || query >= seq) return;
    const long long offset = static_cast<long long>(query) * n_heads * head_dim +
                             static_cast<long long>(head) * head_dim;
    float partial = 0.0F;
    for (int d = static_cast<int>(threadIdx.x); d < head_dim; d += kThreadsPerBlock) {
        partial += grad_out[offset + d] * out[offset + d];
    }
    const float total = block_sum(partial, reduce);
    if (threadIdx.x == 0) row_dot[static_cast<long long>(head) * seq + query] = total;
}

// grad_q: one block per (head, QUERY), accumulating over that query's visible keys. Owning the
// query index means no other block writes this row, so no atomic is needed.
__global__ void attention_grad_q_kernel(const float* __restrict__ q, const float* __restrict__ k,
                                        const float* __restrict__ v,
                                        const float* __restrict__ grad_out,
                                        const float* __restrict__ softmax_lse,
                                        const float* __restrict__ row_dot,
                                        float* __restrict__ grad_q, int seq, int n_heads,
                                        int n_kv_heads, int head_dim) {
    extern __shared__ float shared[];
    float* reduce = shared;
    float* accumulator = shared + kThreadsPerBlock;

    const int head = static_cast<int>(blockIdx.y);
    const int query = static_cast<int>(blockIdx.x);
    if (head >= n_heads || query >= seq) return;
    const int kv_head = head / (n_heads / n_kv_heads);
    const int lane = static_cast<int>(threadIdx.x);
    const float scale = rsqrtf(static_cast<float>(head_dim));

    const long long q_offset = static_cast<long long>(query) * n_heads * head_dim +
                               static_cast<long long>(head) * head_dim;
    const float lse = softmax_lse[static_cast<long long>(head) * seq + query];
    const float dot = row_dot[static_cast<long long>(head) * seq + query];

    for (int d = lane; d < head_dim; d += kThreadsPerBlock) accumulator[d] = 0.0F;
    __syncthreads();

    for (int key = 0; key <= query; ++key) {
        const long long kv_offset = static_cast<long long>(key) * n_kv_heads * head_dim +
                                    static_cast<long long>(kv_head) * head_dim;
        float score_partial = 0.0F;
        float dv_partial = 0.0F;
        for (int d = lane; d < head_dim; d += kThreadsPerBlock) {
            score_partial += q[q_offset + d] * k[kv_offset + d];
            dv_partial += grad_out[q_offset + d] * v[kv_offset + d];
        }
        const float score = block_sum(score_partial, reduce) * scale;
        __syncthreads();
        const float grad_v_dot = block_sum(dv_partial, reduce);
        __syncthreads();

        // p = exp(score - lse) is the softmax probability; ds is the gradient wrt the score.
        const float probability = __expf(score - lse);
        const float grad_score = probability * (grad_v_dot - dot) * scale;
        for (int d = lane; d < head_dim; d += kThreadsPerBlock) {
            accumulator[d] += grad_score * k[kv_offset + d];
        }
        __syncthreads();
    }
    for (int d = lane; d < head_dim; d += kThreadsPerBlock) grad_q[q_offset + d] = accumulator[d];
}

// grad_k and grad_v: one block per (kv head, KEY), accumulating over the queries that can see this
// key (query >= key, by causality). Owning the key index is what makes this deterministic — the
// alternative is an atomicAdd from the per-query kernel, whose accumulation order varies per run.
// Under GQA a kv head serves several query heads, so this loops over the group too.
__global__ void attention_grad_kv_kernel(const float* __restrict__ q, const float* __restrict__ k,
                                         const float* __restrict__ v,
                                         const float* __restrict__ grad_out,
                                         const float* __restrict__ softmax_lse,
                                         const float* __restrict__ row_dot,
                                         float* __restrict__ grad_k, float* __restrict__ grad_v,
                                         int seq, int n_heads, int n_kv_heads, int head_dim) {
    extern __shared__ float shared[];
    float* reduce = shared;
    float* grad_k_acc = shared + kThreadsPerBlock;
    float* grad_v_acc = shared + kThreadsPerBlock + head_dim;

    const int kv_head = static_cast<int>(blockIdx.y);
    const int key = static_cast<int>(blockIdx.x);
    if (kv_head >= n_kv_heads || key >= seq) return;
    const int group = n_heads / n_kv_heads;
    const int lane = static_cast<int>(threadIdx.x);
    const float scale = rsqrtf(static_cast<float>(head_dim));

    const long long kv_offset = static_cast<long long>(key) * n_kv_heads * head_dim +
                                static_cast<long long>(kv_head) * head_dim;
    for (int d = lane; d < head_dim; d += kThreadsPerBlock) {
        grad_k_acc[d] = 0.0F;
        grad_v_acc[d] = 0.0F;
    }
    __syncthreads();

    for (int offset_in_group = 0; offset_in_group < group; ++offset_in_group) {
        const int head = kv_head * group + offset_in_group;
        for (int query = key; query < seq; ++query) {  // causal: only queries at or after the key
            const long long q_offset = static_cast<long long>(query) * n_heads * head_dim +
                                       static_cast<long long>(head) * head_dim;
            float score_partial = 0.0F;
            float dv_partial = 0.0F;
            for (int d = lane; d < head_dim; d += kThreadsPerBlock) {
                score_partial += q[q_offset + d] * k[kv_offset + d];
                dv_partial += grad_out[q_offset + d] * v[kv_offset + d];
            }
            const float score = block_sum(score_partial, reduce) * scale;
            __syncthreads();
            const float grad_v_dot = block_sum(dv_partial, reduce);
            __syncthreads();

            const float lse = softmax_lse[static_cast<long long>(head) * seq + query];
            const float dot = row_dot[static_cast<long long>(head) * seq + query];
            const float probability = __expf(score - lse);
            const float grad_score = probability * (grad_v_dot - dot) * scale;
            for (int d = lane; d < head_dim; d += kThreadsPerBlock) {
                grad_k_acc[d] += grad_score * q[q_offset + d];
                grad_v_acc[d] += probability * grad_out[q_offset + d];
            }
            __syncthreads();
        }
    }
    for (int d = lane; d < head_dim; d += kThreadsPerBlock) {
        grad_k[kv_offset + d] = grad_k_acc[d];
        grad_v[kv_offset + d] = grad_v_acc[d];
    }
}

// One thread per output element. An out-of-range id writes zeros rather than reading out of
// bounds: a corrupt token id should not become a segfault or, worse, silently valid garbage.
__global__ void embedding_kernel(const float* __restrict__ table, const int* __restrict__ ids,
                                 float* __restrict__ out, int seq, int d_model, int vocab_size) {
    const long long index = static_cast<long long>(blockIdx.x) * kThreadsPerBlock + threadIdx.x;
    const long long total = static_cast<long long>(seq) * d_model;
    if (index >= total) return;
    const int token = static_cast<int>(index / d_model);
    const int dim = static_cast<int>(index % d_model);
    const int id = ids[token];
    out[index] = (id < 0 || id >= vocab_size)
                     ? 0.0F
                     : table[static_cast<long long>(id) * d_model + dim];
}

// One block per row: max, then sum of exp, both block reductions, so the [seq, vocab] softmax is
// never written to memory.
__global__ void cross_entropy_row_kernel(const float* __restrict__ logits,
                                         const int* __restrict__ targets,
                                         float* __restrict__ row_loss, int seq, int vocab_size) {
    __shared__ float scratch[kThreadsPerBlock];
    const int row = static_cast<int>(blockIdx.x);
    if (row >= seq) return;
    const float* x = logits + static_cast<long long>(row) * vocab_size;

    float local_max = -INFINITY;
    for (int c = static_cast<int>(threadIdx.x); c < vocab_size; c += kThreadsPerBlock) {
        local_max = fmaxf(local_max, x[c]);
    }
    scratch[threadIdx.x] = local_max;
    __syncthreads();
    for (int stride = kThreadsPerBlock / 2; stride > 0; stride >>= 1) {
        if (threadIdx.x < stride) {
            scratch[threadIdx.x] = fmaxf(scratch[threadIdx.x], scratch[threadIdx.x + stride]);
        }
        __syncthreads();
    }
    const float max_value = scratch[0];
    __syncthreads();

    float local_sum = 0.0F;
    for (int c = static_cast<int>(threadIdx.x); c < vocab_size; c += kThreadsPerBlock) {
        local_sum += __expf(x[c] - max_value);
    }
    const float sum_exp = block_sum(local_sum, scratch);
    if (threadIdx.x == 0) {
        const int target = targets[row];
        const float target_logit = (target >= 0 && target < vocab_size) ? x[target] : 0.0F;
        row_loss[row] = __logf(sum_exp) + max_value - target_logit;
    }
}

// Mean over rows in ONE block, in a fixed tree order. Deterministic by construction — an atomic
// accumulation would vary in its last bits between runs, and the loss is the number every training
// comparison is judged on.
__global__ void mean_kernel(const float* __restrict__ values, float* __restrict__ out, int count) {
    __shared__ float scratch[kThreadsPerBlock];
    float local = 0.0F;
    for (int i = static_cast<int>(threadIdx.x); i < count; i += kThreadsPerBlock) local += values[i];
    const float total = block_sum(local, scratch);
    if (threadIdx.x == 0) out[0] = total / static_cast<float>(count);
}

// grad = (softmax - onehot) / seq, recomputing the softmax from the logits.
__global__ void cross_entropy_backward_kernel(const float* __restrict__ logits,
                                              const int* __restrict__ targets,
                                              float* __restrict__ grad, int seq, int vocab_size) {
    __shared__ float scratch[kThreadsPerBlock];
    const int row = static_cast<int>(blockIdx.x);
    if (row >= seq) return;
    const float* x = logits + static_cast<long long>(row) * vocab_size;
    float* g = grad + static_cast<long long>(row) * vocab_size;

    float local_max = -INFINITY;
    for (int c = static_cast<int>(threadIdx.x); c < vocab_size; c += kThreadsPerBlock) {
        local_max = fmaxf(local_max, x[c]);
    }
    scratch[threadIdx.x] = local_max;
    __syncthreads();
    for (int stride = kThreadsPerBlock / 2; stride > 0; stride >>= 1) {
        if (threadIdx.x < stride) {
            scratch[threadIdx.x] = fmaxf(scratch[threadIdx.x], scratch[threadIdx.x + stride]);
        }
        __syncthreads();
    }
    const float max_value = scratch[0];
    __syncthreads();

    float local_sum = 0.0F;
    for (int c = static_cast<int>(threadIdx.x); c < vocab_size; c += kThreadsPerBlock) {
        local_sum += __expf(x[c] - max_value);
    }
    const float sum_exp = block_sum(local_sum, scratch);
    const float inverse_rows = 1.0F / static_cast<float>(seq);
    const int target = targets[row];
    for (int c = static_cast<int>(threadIdx.x); c < vocab_size; c += kThreadsPerBlock) {
        const float probability = __expf(x[c] - max_value) / sum_exp;
        g[c] = (probability - (c == target ? 1.0F : 0.0F)) * inverse_rows;
    }
}

__global__ void embedding_backward_kernel(const float* __restrict__ grad_out,
                                          const int* __restrict__ ids,
                                          float* __restrict__ grad_table, int seq, int d_model,
                                          int vocab_size) {
    const int dim = static_cast<int>(blockIdx.x) * kThreadsPerBlock + static_cast<int>(threadIdx.x);
    if (dim >= d_model) return;
    // This thread owns column `dim` of the whole table, so every accumulation into it is serial
    // and ordered — no atomics, no run-to-run variation even with repeated token ids.
    for (int token = 0; token < seq; ++token) {
        const int id = ids[token];
        if (id < 0 || id >= vocab_size) continue;
        grad_table[static_cast<long long>(id) * d_model + dim] +=
            grad_out[static_cast<long long>(token) * d_model + dim];
    }
}

__global__ void adamw_kernel(float* __restrict__ parameter, const float* __restrict__ gradient,
                             float* __restrict__ moment1, float* __restrict__ moment2,
                             long long elements, float learning_rate, float beta1, float beta2,
                             float epsilon, float weight_decay, float bias_correction1,
                             float bias_correction2) {
    const long long index =
        static_cast<long long>(blockIdx.x) * kThreadsPerBlock + threadIdx.x;
    if (index >= elements) return;
    const float g = gradient[index];
    const float m = beta1 * moment1[index] + (1.0F - beta1) * g;
    const float v = beta2 * moment2[index] + (1.0F - beta2) * g * g;
    moment1[index] = m;
    moment2[index] = v;
    const float m_hat = m / bias_correction1;
    const float v_hat = v / bias_correction2;
    // Decoupled weight decay: applied to the weight, NOT folded into the gradient. That is the
    // whole difference between AdamW and Adam-with-L2, and folding it in would change what the
    // adaptive term normalizes.
    parameter[index] -= learning_rate * (m_hat / (sqrtf(v_hat) + epsilon) +
                                         weight_decay * parameter[index]);
}

// In-place causal mask + row softmax over [n_heads * seq, seq], one block per row. Fused so the
// masked scores are never written before being exponentiated, and the log-sum-exp is emitted for
// the backward in the same pass.
__global__ void causal_softmax_kernel(float* __restrict__ scores, float* __restrict__ lse, int seq,
                                      float scale) {
    __shared__ float scratch[kThreadsPerBlock];
    const int row = static_cast<int>(blockIdx.x);
    const int query = row % seq;
    float* line = scores + static_cast<long long>(row) * seq;

    // Causal: only keys <= query are visible. Masking by bounding the loop beats writing -inf and
    // reading it back.
    const int visible = query + 1;
    float local_max = -INFINITY;
    for (int c = static_cast<int>(threadIdx.x); c < visible; c += kThreadsPerBlock) {
        local_max = fmaxf(local_max, line[c] * scale);
    }
    scratch[threadIdx.x] = local_max;
    __syncthreads();
    for (int stride = kThreadsPerBlock / 2; stride > 0; stride >>= 1) {
        if (threadIdx.x < stride) {
            scratch[threadIdx.x] = fmaxf(scratch[threadIdx.x], scratch[threadIdx.x + stride]);
        }
        __syncthreads();
    }
    const float max_value = scratch[0];
    __syncthreads();

    float local_sum = 0.0F;
    for (int c = static_cast<int>(threadIdx.x); c < visible; c += kThreadsPerBlock) {
        const float e = __expf(line[c] * scale - max_value);
        line[c] = e;
        local_sum += e;
    }
    const float sum_exp = block_sum(local_sum, scratch);
    const float inverse = 1.0F / sum_exp;
    for (int c = static_cast<int>(threadIdx.x); c < visible; c += kThreadsPerBlock) {
        line[c] *= inverse;
    }
    // Masked-out positions must be ZERO, not stale: the PV matmul reads the whole row.
    for (int c = visible + static_cast<int>(threadIdx.x); c < seq; c += kThreadsPerBlock) {
        line[c] = 0.0F;
    }
    if (threadIdx.x == 0) lse[row] = max_value + __logf(sum_exp);
}

// [seq, source_heads * head_dim] -> [n_heads, seq, head_dim]. When source_heads < n_heads the head
// is BROADCAST across its GQA group, so the batched GEMM sees a uniform batch and K/V need no
// special casing downstream.
__global__ void pack_heads_kernel(const float* __restrict__ in, float* __restrict__ out, int seq,
                                  int n_heads, int source_heads, int head_dim) {
    const long long index =
        static_cast<long long>(blockIdx.x) * kThreadsPerBlock + threadIdx.x;
    const long long total = static_cast<long long>(n_heads) * seq * head_dim;
    if (index >= total) return;
    const int dim = static_cast<int>(index % head_dim);
    const int token = static_cast<int>((index / head_dim) % seq);
    const int head = static_cast<int>(index / (static_cast<long long>(head_dim) * seq));
    const int source_head = head / (n_heads / source_heads);
    out[index] = in[static_cast<long long>(token) * source_heads * head_dim +
                    static_cast<long long>(source_head) * head_dim + dim];
}

// The inverse, back to the interleaved [seq, n_heads * head_dim] the o_proj expects.
__global__ void unpack_heads_kernel(const float* __restrict__ in, float* __restrict__ out, int seq,
                                    int n_heads, int head_dim) {
    const long long index =
        static_cast<long long>(blockIdx.x) * kThreadsPerBlock + threadIdx.x;
    const long long total = static_cast<long long>(n_heads) * seq * head_dim;
    if (index >= total) return;
    const int dim = static_cast<int>(index % head_dim);
    const int token = static_cast<int>((index / head_dim) % seq);
    const int head = static_cast<int>(index / (static_cast<long long>(head_dim) * seq));
    out[static_cast<long long>(token) * n_heads * head_dim +
        static_cast<long long>(head) * head_dim + dim] = in[index];
}

// Shapes are validated at the boundary: a kernel launched with mismatched extents reads out of
// bounds, which shows up much later as corrupt numbers rather than as an error.
ComputeStatus validate(const DeviceBuffer& x, const DeviceBuffer& weight, int rows, int cols) {
    if (rows <= 0 || cols <= 0) {
        return ComputeStatus::invalid_argument("rms_norm: rows and cols must be positive");
    }
    if (x.empty() || weight.empty()) {
        return ComputeStatus::invalid_argument("rms_norm: x and weight must be allocated");
    }
    if (x.element_count() < static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols)) {
        return ComputeStatus::invalid_argument("rms_norm: x is smaller than rows * cols");
    }
    if (weight.element_count() < static_cast<std::size_t>(cols)) {
        return ComputeStatus::invalid_argument("rms_norm: weight is smaller than cols");
    }
    return ComputeStatus::success();
}

}  // namespace

ComputeStatus cuda_rms_norm(const DeviceBuffer& x, const DeviceBuffer& weight, DeviceBuffer& out,
                            int rows, int cols, float epsilon) {
    if (const ComputeStatus valid = validate(x, weight, rows, cols); !valid.ok()) return valid;
    if (out.element_count() < static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols)) {
        return ComputeStatus::invalid_argument("rms_norm: out is smaller than rows * cols");
    }
    rms_norm_kernel<<<rows, kThreadsPerBlock>>>(as_const_float(x), as_const_float(weight),
                                                as_float(out), rows, cols, epsilon);
    return check_launch("rms_norm");
}

ComputeStatus cuda_rms_norm_backward(const DeviceBuffer& x, const DeviceBuffer& weight,
                                     const DeviceBuffer& grad_out, DeviceBuffer& grad_x,
                                     DeviceBuffer& grad_weight, int rows, int cols,
                                     float epsilon) {
    if (const ComputeStatus valid = validate(x, weight, rows, cols); !valid.ok()) return valid;
    const std::size_t elements = static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols);
    if (grad_out.element_count() < elements || grad_x.element_count() < elements) {
        return ComputeStatus::invalid_argument(
            "rms_norm_backward: grad_out and grad_x must hold rows * cols");
    }
    if (grad_weight.element_count() < static_cast<std::size_t>(cols)) {
        return ComputeStatus::invalid_argument("rms_norm_backward: grad_weight must hold cols");
    }

    rms_norm_grad_x_kernel<<<rows, kThreadsPerBlock>>>(
        as_const_float(x), as_const_float(weight), as_const_float(grad_out), as_float(grad_x), rows,
        cols, epsilon);
    if (const ComputeStatus launched = check_launch("rms_norm_backward.grad_x"); !launched.ok()) {
        return launched;
    }

    // grad_weight reduces over rows, so it needs the per-row scale. Allocated through the owning
    // device rather than raw cudaMalloc, so it frees itself and is accounted for like any buffer.
    ComputeDevice* device = x.device();
    if (device == nullptr) {
        return ComputeStatus::invalid_argument("rms_norm_backward: x has no owning device");
    }
    DeviceBuffer row_scale =
        device->allocate(DataType::Float32, static_cast<std::size_t>(rows));
    if (row_scale.empty()) {
        return ComputeStatus::out_of_memory("rms_norm_backward: could not allocate row scales");
    }
    rms_norm_row_scale_kernel<<<rows, kThreadsPerBlock>>>(as_const_float(x), as_float(row_scale),
                                                          rows, cols, epsilon);
    if (const ComputeStatus launched = check_launch("rms_norm_backward.row_scale");
        !launched.ok()) {
        return launched;
    }

    const int column_blocks = (cols + kThreadsPerBlock - 1) / kThreadsPerBlock;
    rms_norm_grad_weight_kernel<<<column_blocks, kThreadsPerBlock>>>(
        as_const_float(x), as_const_float(grad_out), as_const_float(row_scale),
        as_float(grad_weight), rows, cols);
    return check_launch("rms_norm_backward.grad_weight");
}

namespace {

ComputeStatus validate_rope(const DeviceBuffer& in, const DeviceBuffer& out, int rows,
                            int head_dim) {
    if (rows <= 0 || head_dim <= 0) {
        return ComputeStatus::invalid_argument("rope: rows and head_dim must be positive");
    }
    if (head_dim % 2 != 0) {
        // rotate_half pairs (i, i + head_dim/2); an odd head_dim has no such pairing, and
        // silently truncating would rotate some elements and leave others untouched.
        return ComputeStatus::invalid_argument("rope: head_dim must be even (rotate_half pairs)");
    }
    const std::size_t elements =
        static_cast<std::size_t>(rows) * static_cast<std::size_t>(head_dim);
    if (in.empty() || in.element_count() < elements || out.element_count() < elements) {
        return ComputeStatus::invalid_argument("rope: buffers must hold rows * head_dim");
    }
    return ComputeStatus::success();
}

ComputeStatus launch_rope(const DeviceBuffer& x, DeviceBuffer& out, int rows, int head_dim,
                          float base, int position_offset, bool inverse, int rows_per_position,
                          const char* context) {
    if (const ComputeStatus valid = validate_rope(x, out, rows, head_dim); !valid.ok()) return valid;
    if (rows_per_position <= 0 || rows % rows_per_position != 0) {
        // A ragged grouping would silently assign some heads to the wrong token's angle.
        return ComputeStatus::invalid_argument(
            "rope: rows must be a positive multiple of rows_per_position");
    }
    const int pairs = rows * (head_dim / 2);
    const int blocks = (pairs + kThreadsPerBlock - 1) / kThreadsPerBlock;
    rope_kernel<<<blocks, kThreadsPerBlock>>>(as_const_float(x), as_float(out), rows, head_dim,
                                              base, position_offset, inverse, rows_per_position);
    return check_launch(context);
}

ComputeStatus validate_elementwise(const DeviceBuffer& a, const DeviceBuffer& b,
                                   std::size_t elements, const char* op) {
    if (elements == 0) {
        return ComputeStatus::invalid_argument(std::string(op) + ": elements must be positive");
    }
    if (a.element_count() < elements || b.element_count() < elements) {
        return ComputeStatus::invalid_argument(std::string(op) + ": buffers are too small");
    }
    return ComputeStatus::success();
}

}  // namespace

ComputeStatus cuda_rope(const DeviceBuffer& x, DeviceBuffer& out, int rows, int head_dim,
                        float base, int position_offset, int rows_per_position) {
    return launch_rope(x, out, rows, head_dim, base, position_offset, /*inverse=*/false,
                       rows_per_position, "rope");
}

ComputeStatus cuda_rope_backward(const DeviceBuffer& grad_out, DeviceBuffer& grad_x, int rows,
                                 int head_dim, float base, int position_offset,
                                 int rows_per_position) {
    return launch_rope(grad_out, grad_x, rows, head_dim, base, position_offset, /*inverse=*/true,
                       rows_per_position, "rope_backward");
}

ComputeStatus cuda_swiglu(const DeviceBuffer& gate, const DeviceBuffer& up, DeviceBuffer& out,
                          std::size_t elements) {
    if (const ComputeStatus valid = validate_elementwise(gate, up, elements, "swiglu");
        !valid.ok()) {
        return valid;
    }
    if (out.element_count() < elements) {
        return ComputeStatus::invalid_argument("swiglu: out is too small");
    }
    const long long count = static_cast<long long>(elements);
    const int blocks = static_cast<int>((count + kThreadsPerBlock - 1) / kThreadsPerBlock);
    swiglu_kernel<<<blocks, kThreadsPerBlock>>>(as_const_float(gate), as_const_float(up),
                                                as_float(out), count);
    return check_launch("swiglu");
}

ComputeStatus cuda_swiglu_backward(const DeviceBuffer& gate, const DeviceBuffer& up,
                                   const DeviceBuffer& grad_out, DeviceBuffer& grad_gate,
                                   DeviceBuffer& grad_up, std::size_t elements) {
    if (const ComputeStatus valid = validate_elementwise(gate, up, elements, "swiglu_backward");
        !valid.ok()) {
        return valid;
    }
    if (grad_out.element_count() < elements || grad_gate.element_count() < elements ||
        grad_up.element_count() < elements) {
        return ComputeStatus::invalid_argument("swiglu_backward: gradient buffers are too small");
    }
    const long long count = static_cast<long long>(elements);
    const int blocks = static_cast<int>((count + kThreadsPerBlock - 1) / kThreadsPerBlock);
    swiglu_backward_kernel<<<blocks, kThreadsPerBlock>>>(
        as_const_float(gate), as_const_float(up), as_const_float(grad_out), as_float(grad_gate),
        as_float(grad_up), count);
    return check_launch("swiglu_backward");
}

namespace {

ComputeStatus validate_attention(int seq, int n_heads, int n_kv_heads, int head_dim) {
    if (seq <= 0 || n_heads <= 0 || n_kv_heads <= 0 || head_dim <= 0) {
        return ComputeStatus::invalid_argument("attention: all extents must be positive");
    }
    if (n_heads % n_kv_heads != 0) {
        // GQA maps query head h to kv head h / (n_heads / n_kv_heads); a non-integer group size
        // has no such mapping, and rounding it would silently pair heads with the wrong keys.
        return ComputeStatus::invalid_argument("attention: n_heads must be a multiple of n_kv_heads");
    }
    return ComputeStatus::success();
}

}  // namespace

ComputeStatus cuda_causal_attention(const DeviceBuffer& q, const DeviceBuffer& k,
                                    const DeviceBuffer& v, DeviceBuffer& out,
                                    DeviceBuffer& softmax_lse, int seq, int n_heads,
                                    int n_kv_heads, int head_dim) {
    if (const ComputeStatus valid = validate_attention(seq, n_heads, n_kv_heads, head_dim);
        !valid.ok()) {
        return valid;
    }
    const std::size_t q_elements =
        static_cast<std::size_t>(seq) * n_heads * static_cast<std::size_t>(head_dim);
    const std::size_t kv_elements =
        static_cast<std::size_t>(seq) * n_kv_heads * static_cast<std::size_t>(head_dim);
    if (q.element_count() < q_elements || out.element_count() < q_elements ||
        k.element_count() < kv_elements || v.element_count() < kv_elements ||
        softmax_lse.element_count() <
            static_cast<std::size_t>(n_heads) * static_cast<std::size_t>(seq)) {
        return ComputeStatus::invalid_argument("attention: buffers do not match the given extents");
    }
    const dim3 grid(static_cast<unsigned>(seq), static_cast<unsigned>(n_heads));
    const std::size_t shared = (kThreadsPerBlock + head_dim) * sizeof(float);
    causal_attention_kernel<<<grid, kThreadsPerBlock, shared>>>(
        as_const_float(q), as_const_float(k), as_const_float(v), as_float(out),
        as_float(softmax_lse), seq, n_heads, n_kv_heads, head_dim);
    return check_launch("causal_attention");
}

ComputeStatus cuda_causal_attention_backward(const DeviceBuffer& q, const DeviceBuffer& k,
                                             const DeviceBuffer& v, const DeviceBuffer& out,
                                             const DeviceBuffer& grad_out,
                                             const DeviceBuffer& softmax_lse, DeviceBuffer& row_dot,
                                             DeviceBuffer& grad_q, DeviceBuffer& grad_k,
                                             DeviceBuffer& grad_v, int seq, int n_heads,
                                             int n_kv_heads, int head_dim) {
    if (const ComputeStatus valid = validate_attention(seq, n_heads, n_kv_heads, head_dim);
        !valid.ok()) {
        return valid;
    }
    if (row_dot.element_count() <
        static_cast<std::size_t>(n_heads) * static_cast<std::size_t>(seq)) {
        return ComputeStatus::invalid_argument("attention_backward: row_dot must hold n_heads * seq");
    }

    const dim3 query_grid(static_cast<unsigned>(seq), static_cast<unsigned>(n_heads));
    attention_row_dot_kernel<<<query_grid, kThreadsPerBlock>>>(
        as_const_float(grad_out), as_const_float(out), as_float(row_dot), seq, n_heads, head_dim);
    if (const ComputeStatus launched = check_launch("attention_backward.row_dot"); !launched.ok()) {
        return launched;
    }

    const std::size_t query_shared = (kThreadsPerBlock + head_dim) * sizeof(float);
    attention_grad_q_kernel<<<query_grid, kThreadsPerBlock, query_shared>>>(
        as_const_float(q), as_const_float(k), as_const_float(v), as_const_float(grad_out),
        as_const_float(softmax_lse), as_const_float(row_dot), as_float(grad_q), seq, n_heads,
        n_kv_heads, head_dim);
    if (const ComputeStatus launched = check_launch("attention_backward.grad_q"); !launched.ok()) {
        return launched;
    }

    const dim3 key_grid(static_cast<unsigned>(seq), static_cast<unsigned>(n_kv_heads));
    const std::size_t key_shared = (kThreadsPerBlock + 2 * head_dim) * sizeof(float);
    attention_grad_kv_kernel<<<key_grid, kThreadsPerBlock, key_shared>>>(
        as_const_float(q), as_const_float(k), as_const_float(v), as_const_float(grad_out),
        as_const_float(softmax_lse), as_const_float(row_dot), as_float(grad_k), as_float(grad_v),
        seq, n_heads, n_kv_heads, head_dim);
    return check_launch("attention_backward.grad_kv");
}

ComputeStatus cuda_embedding(const DeviceBuffer& table, const DeviceBuffer& ids, DeviceBuffer& out,
                             int seq, int d_model, int vocab_size) {
    if (seq <= 0 || d_model <= 0 || vocab_size <= 0) {
        return ComputeStatus::invalid_argument("embedding: extents must be positive");
    }
    const std::size_t elements =
        static_cast<std::size_t>(seq) * static_cast<std::size_t>(d_model);
    if (table.element_count() <
            static_cast<std::size_t>(vocab_size) * static_cast<std::size_t>(d_model) ||
        ids.element_count() < static_cast<std::size_t>(seq) || out.element_count() < elements) {
        return ComputeStatus::invalid_argument("embedding: buffers do not match the extents");
    }
    const int blocks = static_cast<int>((elements + kThreadsPerBlock - 1) / kThreadsPerBlock);
    embedding_kernel<<<blocks, kThreadsPerBlock>>>(
        as_const_float(table), static_cast<const int*>(ids.handle()), as_float(out), seq, d_model,
        vocab_size);
    return check_launch("embedding");
}

ComputeStatus cuda_cross_entropy(const DeviceBuffer& logits, const DeviceBuffer& targets,
                                 DeviceBuffer& row_loss, DeviceBuffer& total, int seq,
                                 int vocab_size) {
    if (seq <= 0 || vocab_size <= 0) {
        return ComputeStatus::invalid_argument("cross_entropy: extents must be positive");
    }
    const std::size_t elements =
        static_cast<std::size_t>(seq) * static_cast<std::size_t>(vocab_size);
    if (logits.element_count() < elements ||
        targets.element_count() < static_cast<std::size_t>(seq) ||
        row_loss.element_count() < static_cast<std::size_t>(seq) || total.element_count() < 1) {
        return ComputeStatus::invalid_argument("cross_entropy: buffers do not match the extents");
    }
    cross_entropy_row_kernel<<<seq, kThreadsPerBlock>>>(
        as_const_float(logits), static_cast<const int*>(targets.handle()), as_float(row_loss), seq,
        vocab_size);
    if (const auto status = check_launch("cross_entropy.rows"); !status.ok()) return status;
    mean_kernel<<<1, kThreadsPerBlock>>>(as_const_float(row_loss), as_float(total), seq);
    return check_launch("cross_entropy.mean");
}

ComputeStatus cuda_cross_entropy_backward(const DeviceBuffer& logits, const DeviceBuffer& targets,
                                          DeviceBuffer& grad_logits, int seq, int vocab_size) {
    if (seq <= 0 || vocab_size <= 0) {
        return ComputeStatus::invalid_argument("cross_entropy_backward: extents must be positive");
    }
    const std::size_t elements =
        static_cast<std::size_t>(seq) * static_cast<std::size_t>(vocab_size);
    if (logits.element_count() < elements || grad_logits.element_count() < elements) {
        return ComputeStatus::invalid_argument("cross_entropy_backward: buffers are too small");
    }
    cross_entropy_backward_kernel<<<seq, kThreadsPerBlock>>>(
        as_const_float(logits), static_cast<const int*>(targets.handle()), as_float(grad_logits),
        seq, vocab_size);
    return check_launch("cross_entropy_backward");
}

ComputeStatus cuda_embedding_backward(const DeviceBuffer& grad_out, const DeviceBuffer& ids,
                                      DeviceBuffer& grad_table, int seq, int d_model,
                                      int vocab_size) {
    if (seq <= 0 || d_model <= 0 || vocab_size <= 0) {
        return ComputeStatus::invalid_argument("embedding_backward: extents must be positive");
    }
    if (grad_table.element_count() <
        static_cast<std::size_t>(vocab_size) * static_cast<std::size_t>(d_model)) {
        return ComputeStatus::invalid_argument("embedding_backward: grad_table too small");
    }
    const int blocks = (d_model + kThreadsPerBlock - 1) / kThreadsPerBlock;
    embedding_backward_kernel<<<blocks, kThreadsPerBlock>>>(
        as_const_float(grad_out), static_cast<const int*>(ids.handle()), as_float(grad_table), seq,
        d_model, vocab_size);
    return check_launch("embedding_backward");
}

ComputeStatus cuda_adamw_step(DeviceBuffer& parameter, const DeviceBuffer& gradient,
                              DeviceBuffer& moment1, DeviceBuffer& moment2, std::size_t elements,
                              float learning_rate, float beta1, float beta2, float epsilon,
                              float weight_decay, int step) {
    if (elements == 0) return ComputeStatus::success();
    if (parameter.element_count() < elements || gradient.element_count() < elements ||
        moment1.element_count() < elements || moment2.element_count() < elements) {
        return ComputeStatus::invalid_argument("adamw: buffers are too small");
    }
    if (step < 1) return ComputeStatus::invalid_argument("adamw: step is 1-based");
    const float bias_correction1 = 1.0F - powf(beta1, static_cast<float>(step));
    const float bias_correction2 = 1.0F - powf(beta2, static_cast<float>(step));
    const long long count = static_cast<long long>(elements);
    const int blocks = static_cast<int>((count + kThreadsPerBlock - 1) / kThreadsPerBlock);
    adamw_kernel<<<blocks, kThreadsPerBlock>>>(as_float(parameter), as_const_float(gradient),
                                               as_float(moment1), as_float(moment2), count,
                                               learning_rate, beta1, beta2, epsilon, weight_decay,
                                               bias_correction1, bias_correction2);
    return check_launch("adamw");
}

ComputeStatus cuda_causal_attention_batched(ComputeDevice& device, const DeviceBuffer& q,
                                            const DeviceBuffer& k, const DeviceBuffer& v,
                                            DeviceBuffer& scores, DeviceBuffer& out,
                                            DeviceBuffer& softmax_lse, int seq, int n_heads,
                                            int n_kv_heads, int head_dim) {
    if (const ComputeStatus valid = validate_attention(seq, n_heads, n_kv_heads, head_dim);
        !valid.ok()) {
        return valid;
    }
    const std::size_t per_head = static_cast<std::size_t>(seq) * static_cast<std::size_t>(head_dim);
    const std::size_t packed_elements = per_head * static_cast<std::size_t>(n_heads);
    const std::size_t score_elements =
        static_cast<std::size_t>(n_heads) * static_cast<std::size_t>(seq) *
        static_cast<std::size_t>(seq);
    if (scores.element_count() < score_elements) {
        return ComputeStatus::invalid_argument("attention_batched: scores scratch too small");
    }

    // A batched GEMM needs each head to be a contiguous [seq, head_dim] matrix, but the projections
    // interleave heads: head h occupies columns [h*head_dim, (h+1)*head_dim) of a
    // [seq, n_heads*head_dim] tensor. Permute once into [n_heads, seq, head_dim]. The permute is
    // O(seq * d) against attention's O(seq^2 * head_dim), so it is noise — and it lets K and V be
    // BROADCAST across their GQA group in the same pass, which makes the batch uniform and removes
    // the per-head launch entirely.
    DeviceBuffer q_packed = device.allocate(DataType::Float32, packed_elements);
    DeviceBuffer k_packed = device.allocate(DataType::Float32, packed_elements);
    DeviceBuffer v_packed = device.allocate(DataType::Float32, packed_elements);
    DeviceBuffer context_packed = device.allocate(DataType::Float32, packed_elements);
    if (q_packed.empty() || k_packed.empty() || v_packed.empty() || context_packed.empty()) {
        return ComputeStatus::out_of_memory("attention_batched: packing buffers");
    }
    const int blocks = static_cast<int>((packed_elements + kThreadsPerBlock - 1) / kThreadsPerBlock);
    pack_heads_kernel<<<blocks, kThreadsPerBlock>>>(as_const_float(q), as_float(q_packed), seq,
                                                    n_heads, n_heads, head_dim);
    pack_heads_kernel<<<blocks, kThreadsPerBlock>>>(as_const_float(k), as_float(k_packed), seq,
                                                    n_heads, n_kv_heads, head_dim);
    pack_heads_kernel<<<blocks, kThreadsPerBlock>>>(as_const_float(v), as_float(v_packed), seq,
                                                    n_heads, n_kv_heads, head_dim);
    if (const auto status = check_launch("attention_batched.pack"); !status.ok()) return status;

    // scores = Q K^T, one matmul per head, batched.
    GemmParams qk;
    qk.rows = static_cast<std::size_t>(seq);
    qk.inner = static_cast<std::size_t>(head_dim);
    qk.cols = static_cast<std::size_t>(seq);
    qk.alpha = 1.0;
    qk.beta = 0.0;
    qk.transpose_a = false;
    qk.transpose_b = true;
    if (const auto status = device.gemm_batched(qk, q_packed, per_head, k_packed, per_head, scores,
                                                static_cast<std::size_t>(seq) * seq, n_heads);
        !status.ok()) {
        return status;
    }

    // Scale, causal-mask and softmax in place, emitting the log-sum-exp the backward needs.
    const float scale = 1.0F / std::sqrt(static_cast<float>(head_dim));
    causal_softmax_kernel<<<n_heads * seq, kThreadsPerBlock>>>(as_float(scores),
                                                               as_float(softmax_lse), seq, scale);
    if (const auto status = check_launch("attention_batched.softmax"); !status.ok()) return status;

    // context = P V, batched again.
    GemmParams pv;
    pv.rows = static_cast<std::size_t>(seq);
    pv.inner = static_cast<std::size_t>(seq);
    pv.cols = static_cast<std::size_t>(head_dim);
    pv.alpha = 1.0;
    pv.beta = 0.0;
    pv.transpose_a = false;
    pv.transpose_b = false;
    if (const auto status =
            device.gemm_batched(pv, scores, static_cast<std::size_t>(seq) * seq, v_packed, per_head,
                                context_packed, per_head, n_heads);
        !status.ok()) {
        return status;
    }

    unpack_heads_kernel<<<blocks, kThreadsPerBlock>>>(as_const_float(context_packed), as_float(out),
                                                      seq, n_heads, head_dim);
    return check_launch("attention_batched.unpack");
}

}  // namespace zetta::compute
