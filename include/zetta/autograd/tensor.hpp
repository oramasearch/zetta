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
// (analytic vs finite difference). Routing the heavy ops (matmul) through the
// ComputeDevice seam — so the same graph runs on CpuDevice / CudaDevice / MlxDevice
// and one distributed model spans them — is a later slice.
namespace zetta::autograd {

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

}  // namespace zetta::autograd
