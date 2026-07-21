#pragma once

#include "zetta/autograd/tensor.hpp"

// The transformer, built on zetta's own autograd (#555; docs/autograd/README.md).
// These blocks compose the primitive ops (matmul, transpose, scale, add,
// row_softmax, gelu, rms_norm) into the pieces of a decoder, so training the model
// is just building the graph forward and calling backward() — no framework. The
// heavy op (matmul) routes through the ComputeDevice seam in a later slice, making
// the same model run on CpuDevice / CudaDevice / MlxDevice.
namespace zetta::autograd {

// Single-head causal self-attention over `x` [seq, d_model]. wq/wk/wv/wo are each
// [d_model, d_model]. Position t attends only to positions <= t (an additive causal
// mask before the softmax), so the block is a valid autoregressive decoder.
Tensor causal_self_attention(const Tensor& x, const Tensor& wq, const Tensor& wk,
                             const Tensor& wv, const Tensor& wo);

// Position-wise feed-forward: gelu(x @ w1) @ w2. x [seq, d_model],
// w1 [d_model, d_ff], w2 [d_ff, d_model].
Tensor feed_forward(const Tensor& x, const Tensor& w1, const Tensor& w2);

}  // namespace zetta::autograd
