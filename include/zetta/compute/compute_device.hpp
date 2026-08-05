#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "zetta/compute/compute_status.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/compute/device_buffer.hpp"

// The ComputeDevice operator interface — the single seam every heavy op in zetta
// targets (ADR-27 / ARC-9, the native distributed accelerated-compute pass for
// UC-1 churn). It is the same shape as zetta's other seams (StepExecutor, Check,
// StorageEngine): a pure-virtual base class whose concrete implementations are
// the backends. A CPU reference backend (#360) and an optional CUDA backend
// (#361) implement this interface in their own issues; a device registry (#362)
// enumerates and selects between them. Consumers — the executor's step kernels,
// the churn featurize/train/explain ops — call this interface and stay unchanged
// when new hardware appears. New hardware = a new backend, nothing else.
//
// Two halves:
//   - a **memory model** — typed device Buffers (device_buffer.hpp) allocated by
//     the device, with explicit host<->device transfer. Memory is opaque: a CPU
//     device's buffers live in host RAM, a CUDA device's in GPU VRAM; only the
//     owning device interprets a buffer's handle.
//   - an **operator set** — declared at the largest grain that still lets the
//     cluster coordinator distribute the work: map/elementwise, reduce/scan,
//     histogram, gemm/axpy, sort, gather/scatter. These are the primitives the
//     churn pipeline (and later DDF / GBDT / SHAP ops, added by their own issues
//     against this interface) compose. Each returns a ComputeStatus rather than
//     throwing, and a backend that does not implement an op returns
//     ComputeStatus::unsupported so the coordinator can route elsewhere.
//
// No backend lives here, and no SDL/CUDA type appears in this header — it is pure
// zetta-core, like every other public seam.
namespace zetta::compute {

// What kind of hardware a device drives. Declared along the same "dial" idea as
// DeploymentKind: CPU is always available; CUDA is the first accelerator. The
// list grows as backends land (ROCm, Metal, …) — declaring a kind here does not
// imply a backend exists for it (mirrors ClusterRole / DeploymentKind).
enum class DeviceKind {
    Cpu,
    Cuda,
    Mlx,   // Apple-Silicon GPU via MLX (Metal); the default accelerator on macOS
};

// The lowercase canonical name of a device kind ("cpu", "cuda", "mlx").
constexpr std::string_view to_string(DeviceKind kind) {
    switch (kind) {
        case DeviceKind::Cpu:  return "cpu";
        case DeviceKind::Cuda: return "cuda";
        case DeviceKind::Mlx:  return "mlx";
    }
    return "unknown";
}

// A device's identity and coarse capability — what the registry/coordinator
// (#362) reads to enumerate devices and pick one for a piece of work. Kept
// deliberately coarse (the selection heuristic needs presence + rough capacity,
// not a full spec sheet); per-operator support is reported by each op's return
// status, not duplicated here.
struct DeviceInfo {
    DeviceKind kind = DeviceKind::Cpu;
    std::string name;                  // human label, e.g. "host-stub", "NVIDIA A100"
    std::size_t compute_units = 0;     // CPU cores or GPU streaming multiprocessors
    std::size_t memory_bytes = 0;      // total device memory (0 = host / unknown)
    bool unified_memory = false;       // device shares the host address space
};

// --- Operator parameters -----------------------------------------------------

// Unary elementwise transform applied by map(): output[i] = op(input[i]).
enum class UnaryOp {
    Negate,
    Absolute,
    Square,
    SquareRoot,
    Exponential,
    Logarithm,
};

// Binary elementwise combiner applied by elementwise(): out[i] = op(a[i], b[i]).
enum class BinaryOp {
    Add,
    Subtract,
    Multiply,
    Divide,
    Maximum,
    Minimum,
};

// Associative reduction operator for reduce() and scan().
enum class ReduceOp {
    Sum,
    Product,
    Minimum,
    Maximum,
};

// Whether a scan includes the current element (out[i] over input[0..i]) or
// excludes it (out[i] over input[0..i-1], with the identity at i=0).
enum class ScanInclusivity {
    Inclusive,
    Exclusive,
};

// Sort direction for sort() / sort_by_key().
enum class SortOrder {
    Ascending,
    Descending,
};

// Parameters for gemm(): C = alpha * op(A) * op(B) + beta * C, with the operand
// matrices row-major. Shapes are of the *post-transpose* operands, so A is
// rows x inner, B is inner x cols, and C is rows x cols.
struct GemmParams {
    std::size_t rows = 0;        // rows of op(A) and of C
    std::size_t inner = 0;       // cols of op(A) == rows of op(B)
    std::size_t cols = 0;        // cols of op(B) and of C
    double alpha = 1.0;          // scales the product
    double beta = 0.0;           // scales the incoming C (accumulate when != 0)
    bool transpose_a = false;    // use A^T as op(A)
    bool transpose_b = false;    // use B^T as op(B)
};

// --- The device --------------------------------------------------------------

class ComputeDevice {
public:
    virtual ~ComputeDevice() = default;

    // Identity + coarse capability for enumeration / selection.
    virtual DeviceInfo info() const = 0;

    // ---- Memory model -------------------------------------------------------

    // Allocate a buffer of `element_count` elements of `dtype` in device memory.
    // Returns an empty buffer (not an exception) on failure; check with
    // buffer.empty(). The returned buffer owns its storage and frees it on
    // destruction.
    virtual DeviceBuffer allocate(DataType dtype, std::size_t element_count) = 0;

    // Copy `byte_count` bytes from host memory at `host_source` into
    // `destination` (host -> device). `byte_count` must not exceed
    // destination.size_bytes().
    virtual ComputeStatus copy_from_host(const void* host_source,
                                         DeviceBuffer& destination,
                                         std::size_t byte_count) = 0;

    // Copy `byte_count` bytes from `source` into host memory at `host_destination`
    // (device -> host). `byte_count` must not exceed source.size_bytes().
    virtual ComputeStatus copy_to_host(const DeviceBuffer& source,
                                       void* host_destination,
                                       std::size_t byte_count) const = 0;

    // Block until all previously issued work on this device has completed.
    // Transfers and operators may execute asynchronously on a backend (e.g. a
    // CUDA stream); synchronize() is the barrier before reading results back. A
    // synchronous backend implements it as a no-op returning success.
    virtual ComputeStatus synchronize() = 0;

    // ---- Operators (the declared coarse-grained set) ------------------------
    // Every operator returns a ComputeStatus: Ok on success, Unsupported when
    // this backend does not implement it, InvalidArgument on a shape/dtype/size
    // mismatch. Buffers must already live on this device.

    // Unary elementwise: output[i] = op(input[i]). input and output have the
    // same element count.
    virtual ComputeStatus map(UnaryOp op, const DeviceBuffer& input,
                              DeviceBuffer& output) = 0;

    // Binary elementwise: output[i] = op(lhs[i], rhs[i]). All three have the same
    // element count.
    virtual ComputeStatus elementwise(BinaryOp op, const DeviceBuffer& lhs,
                                      const DeviceBuffer& rhs,
                                      DeviceBuffer& output) = 0;

    // Reduction: output[0] = op over every element of input. output holds one
    // element.
    virtual ComputeStatus reduce(ReduceOp op, const DeviceBuffer& input,
                                 DeviceBuffer& output) = 0;

    // Prefix scan: output[i] = op over input[0..i] (inclusive) or input[0..i-1]
    // (exclusive). input and output have the same element count.
    virtual ComputeStatus scan(ReduceOp op, ScanInclusivity inclusivity,
                               const DeviceBuffer& input,
                               DeviceBuffer& output) = 0;

    // Histogram: count input's values into `bin_count` equal-width bins spanning
    // [range_min, range_max). output_counts holds bin_count elements.
    virtual ComputeStatus histogram(const DeviceBuffer& input,
                                    std::size_t bin_count, double range_min,
                                    double range_max,
                                    DeviceBuffer& output_counts) = 0;

    // General matrix multiply: c = alpha * op(a) * op(b) + beta * c (see
    // GemmParams for shapes and transpose flags).
    virtual ComputeStatus gemm(const GemmParams& params, const DeviceBuffer& a,
                               const DeviceBuffer& b, DeviceBuffer& c) = 0;

    // AXPY: y[i] = alpha * x[i] + y[i] (updates y in place). x and y have the
    // same element count.
    virtual ComputeStatus axpy(double alpha, const DeviceBuffer& x,
                               DeviceBuffer& y) = 0;

    // Sort `keys` in place into `order`.
    virtual ComputeStatus sort(DeviceBuffer& keys, SortOrder order) = 0;

    // Sort `keys` into `order` and apply the same permutation to `values`
    // (key/value sort) — both updated in place, same element count. The
    // primitive ranking needs: sort entities by risk score, carrying their ids.
    virtual ComputeStatus sort_by_key(DeviceBuffer& keys, DeviceBuffer& values,
                                      SortOrder order) = 0;

    // Gather: output[i] = input[indices[i]]. indices is an integer buffer;
    // output has the same element count as indices.
    virtual ComputeStatus gather(const DeviceBuffer& input,
                                 const DeviceBuffer& indices,
                                 DeviceBuffer& output) = 0;

    // Scatter: output[indices[i]] = input[i] (the inverse of gather). indices
    // and input have the same element count.
    virtual ComputeStatus scatter(const DeviceBuffer& input,
                                  const DeviceBuffer& indices,
                                  DeviceBuffer& output) = 0;

    // ---- Typed transfer helpers (non-virtual) -------------------------------
    // Convenience over the byte-oriented transfer API: allocate + upload a typed
    // host span, or download a buffer into a typed host span. They attach the
    // right DataType tag (data_type_of<Element>) so call sites are type-safe
    // over the runtime-typed buffer, and validate dtype/size before transferring.

    // Allocate a device buffer and copy `host_data` into it. Returns an empty
    // buffer if allocation or the copy fails.
    template <class Element>
    DeviceBuffer upload(std::span<const Element> host_data) {
        DeviceBuffer buffer = allocate(data_type_of<Element>, host_data.size());
        if (buffer.empty()) return buffer;
        const ComputeStatus status =
            copy_from_host(host_data.data(), buffer, host_data.size_bytes());
        if (!status.ok()) return DeviceBuffer{};  // frees buffer, returns empty
        return buffer;
    }

    // Copy `buffer` into the host span `host_out`. Fails with InvalidArgument if
    // the element type or count does not match the buffer.
    template <class Element>
    ComputeStatus download(const DeviceBuffer& buffer,
                           std::span<Element> host_out) const {
        if (buffer.dtype() != data_type_of<Element>) {
            return ComputeStatus::invalid_argument(
                "download: host element type does not match buffer dtype");
        }
        if (host_out.size() != buffer.element_count()) {
            return ComputeStatus::invalid_argument(
                "download: host span size does not match buffer element count");
        }
        return copy_to_host(buffer, host_out.data(), host_out.size_bytes());
    }

protected:
    // Mint a buffer that owns `handle` (storage this device allocated). Backends
    // call this from allocate() — it is the only way to construct a non-empty
    // DeviceBuffer, so minting and freeing both stay inside the device.
    DeviceBuffer make_buffer(void* handle, DataType dtype,
                             std::size_t element_count) noexcept;

private:
    // Return `handle` (previously minted via make_buffer) to the device. Called
    // only by DeviceBuffer's destructor / reset — buffers free themselves
    // through their owning device, so freeing is polymorphic per backend.
    friend class DeviceBuffer;
    virtual void release_buffer(void* handle) noexcept = 0;
};

}  // namespace zetta::compute
