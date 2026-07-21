#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

// zetta's own reverse-mode autodiff (#555; docs/autograd/README.md) — native C++26,
// no foreign runtime (ADR-17). This is the engine the RLVR model is built on, and
// the reason we can write our own fused kernels instead of pulling in a framework.
//
// A `Node` is one value in the computation graph: its data, its shape, its
// accumulated gradient, its parents, and the backward closure that scatters this
// node's gradient to its parents. `Tensor` is a shared handle to a `Node`. You build
// the graph forward with the ops below, then call `backward()` on the (scalar) loss;
// it seeds the loss gradient to 1 and propagates through the graph in reverse
// topological order, accumulating each parameter's gradient.
//
// The reference kernels here are plain host loops, verified by gradient checking
// (analytic vs finite difference). The heavy op (matmul) can also route through the
// ComputeDevice seam, so the same graph runs on CpuDevice / CudaDevice / MlxDevice
// and one distributed model spans CPU + GPU + MLX nodes — the cluster uses what each
// node has.
namespace zetta::compute {
class ComputeDevice;
}

namespace zetta::autograd {

// Route the autograd's heavy op (matmul, forward and backward) onto a ComputeDevice.
// nullptr (the default) uses the built-in host reference kernels. Thread-local, so
// each cluster node / worker selects its own device; returns the previous device.
// The device is captured per-op at construction, so a graph backpropagates on the
// device it was built on.
compute::ComputeDevice* set_compute_device(compute::ComputeDevice* device);
compute::ComputeDevice* current_compute_device();

struct Node {
    std::vector<float> data;
    std::vector<int> shape;
    std::vector<float> grad;  // accumulated; zero at creation, sized like data
    std::vector<std::shared_ptr<Node>> parents;
    std::function<void(const std::vector<float>& out_grad)> backward;
    bool requires_grad = true;
};

class Tensor {
public:
    Tensor() = default;
    explicit Tensor(std::shared_ptr<Node> node) : node_(std::move(node)) {}

    // A leaf tensor (a parameter or an input). Its gradient starts at zero.
    static Tensor leaf(std::vector<float> data, std::vector<int> shape,
                       bool requires_grad = true);

    const std::vector<float>& data() const { return node_->data; }
    const std::vector<int>& shape() const { return node_->shape; }
    const std::vector<float>& grad() const { return node_->grad; }
    const std::shared_ptr<Node>& node() const { return node_; }
    bool valid() const { return node_ != nullptr; }

    // Reverse-mode backprop from this (scalar) node: seed grad = 1 and propagate
    // through the graph in reverse topological order, accumulating gradients.
    void backward();

private:
    std::shared_ptr<Node> node_;
};

// An intermediate node holding `data` of `shape`, with a zero-initialized gradient.
std::shared_ptr<Node> make_node(std::vector<float> data, std::vector<int> shape);

// --- Ops (forward + backward) ------------------------------------------------

// Matrix multiply: a [m, k] * b [k, n] -> [m, n] (row-major). Backward:
// dA += dOut * B^T, dB += A^T * dOut.
Tensor matmul(const Tensor& a, const Tensor& b);

// Sum all elements -> scalar [1]. Backward: each input element gets dOut.
Tensor sum(const Tensor& a);

// Elementwise add of two same-shaped tensors. Backward: dA += dOut, dB += dOut.
Tensor add(const Tensor& a, const Tensor& b);

// Elementwise (Hadamard) product of two same-shaped tensors. Backward:
// dA += dOut * B, dB += dOut * A.
Tensor mul(const Tensor& a, const Tensor& b);

// Multiply by a constant scalar. Backward: dA += dOut * factor.
Tensor scale(const Tensor& a, float factor);

// Add a per-column bias to each row: a [rows, cols] + bias [cols]. Backward:
// dA += dOut, dBias[col] += sum over rows of dOut[:, col].
Tensor add_bias(const Tensor& a, const Tensor& bias);

// GELU (exact erf form): 0.5*x*(1+erf(x/sqrt(2))), elementwise. The transformer MLP
// nonlinearity. Backward uses the analytic derivative.
Tensor gelu(const Tensor& a);

// SiLU / swish: x*sigmoid(x), elementwise — the SwiGLU activation. Backward:
// sigmoid(x)*(1 + x*(1-sigmoid(x))).
Tensor silu(const Tensor& a);

// Row-wise softmax of a [rows, cols] tensor (softmax over each row / the last axis)
// — the attention weight nonlinearity. Numerically stable (subtracts the row max).
// Backward per row: dX = Y ⊙ (dOut − sum(dOut ⊙ Y)).
Tensor row_softmax(const Tensor& a);

// RMSNorm: normalize each row of `a` [rows, cols] to unit RMS, then scale by
// `weight` [cols]. y[j] = a[j] / sqrt(mean(a²)+eps) * weight[j]. The transformer's
// normalization. Its backward couples every element of a row (the normalizer
// depends on all of them).
Tensor rms_norm(const Tensor& a, const Tensor& weight, float eps = 1e-5F);

// Transpose a 2-D tensor: [m, n] -> [n, m]. Needed for K^T in attention. Backward:
// dA = dOut^T.
Tensor transpose(const Tensor& a);

// Embedding lookup: gather rows of `table` [vocab, dim] at each id in `ids`,
// producing [ids.size(), dim]. The ids are plain indices (not differentiable);
// backward scatter-adds each row's gradient back into the selected table rows.
Tensor embedding(const Tensor& table, const std::vector<int>& ids);

// Mean cross-entropy of `logits` [rows, vocab] against integer `targets` [rows] —
// the language-model loss. Numerically stable (log-sum-exp), returns a scalar.
// Backward: dLogits[row, j] = (softmax[row, j] − 1[j == target]) / rows.
Tensor cross_entropy(const Tensor& logits, const std::vector<int>& targets);

// Select `count` contiguous rows starting at `start` from a [rows, cols] tensor ->
// [count, cols]. Used to align next-token predictions (logits[0..seq-2]) with their
// targets. Backward scatters each row's gradient back to its source row.
Tensor row_slice(const Tensor& a, int start, int count);

// The RLVR policy-gradient loss for one distribution: given `logits` [1, vocab] and
// a batch of sampled `actions` with their group-relative `advantages`,
// loss = -mean(advantage * log softmax(logits)[action]). A scalar. Backward:
// dLogits_j = -(1/G) * sum_i advantage_i * (1[j == action_i] - softmax_j).
Tensor policy_gradient_loss(const Tensor& logits,
                            const std::vector<int>& actions,
                            const std::vector<double>& advantages);

// The RLVR sequence policy-gradient (REINFORCE) loss for ONE completion: given
// per-step `logits` [T, vocab] (row t is the distribution that produced token t),
// the sampled `actions` [T], and a scalar group-relative `advantage`,
// loss = -advantage * sum_t log softmax(logits[t])[action_t]. A scalar [1]. A
// positive advantage pushes every chosen token's probability up, a negative one
// pushes it down — that is how a whole sequence's verifiable reward flows back
// into each token it emitted. Backward: dLogits[t, j] = -advantage * g *
// (1[j == action_t] - softmax(logits[t])[j]).
Tensor sequence_policy_gradient_loss(const Tensor& logits,
                                     const std::vector<int>& actions,
                                     double advantage);

// Rotary positional embedding (RoPE, the rotate_half convention Qwen/LLaMA use):
// rotate the pair (i, i + d/2) of each row [seq, d] by angle = position *
// base^(-2i/d). Applied to Q and K so attention sees relative positions. It is a
// rotation — preserves each row's norm and is the identity at position 0 — and its
// backward is the inverse rotation.
Tensor rope(const Tensor& x, float base = 10000.0F);

// Select `count` contiguous COLUMNS starting at `start` from [rows, cols] ->
// [rows, count] — used to split a projection into attention heads. Backward
// scatters each column's gradient back to its source column.
Tensor col_slice(const Tensor& a, int start, int count);

// Concatenate tensors along their columns: parts each [rows, cols_i] -> [rows,
// sum cols_i] — used to reassemble attention heads. Backward splits the gradient
// back to each part.
Tensor concat_cols(const std::vector<Tensor>& parts);

}  // namespace zetta::autograd
