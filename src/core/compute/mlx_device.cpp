#include "zetta/compute/mlx_device.hpp"

#if defined(ZETTA_WITH_MLX)

#include <cstdlib>
#include <cstring>
#include <vector>

#include <mlx/mlx.h>

// The MLX backend. Apple Silicon is unified memory, so a device buffer is a plain
// host allocation the GPU reads directly; the operators wrap those bytes as MLX
// arrays, run on Metal (MLX's default device), and evaluate the result back into
// the destination buffer. Only float32 is implemented for the numeric operators —
// the tensor type the model runtime uses; other dtypes report InvalidArgument, and
// operators not yet implemented report Unsupported so the coordinator can route
// elsewhere (compute_status.hpp).
namespace zetta::compute {

namespace mx = mlx::core;

namespace {

// A device buffer's float payload as an MLX array, copied so MLX owns its own
// memory and never touches the caller's buffer.
mx::array as_float_array(const DeviceBuffer& buffer, mx::Shape shape) {
    return mx::array(static_cast<const float*>(buffer.handle()), std::move(shape),
                     mx::float32);
}

}  // namespace

DeviceInfo MlxDevice::info() const {
    DeviceInfo information;
    information.kind = DeviceKind::Mlx;
    information.name = "Apple MLX";
    information.compute_units = 0;   // MLX does not expose a core count
    information.memory_bytes = 0;    // unified memory: no separate device pool
    information.unified_memory = true;
    return information;
}

DeviceBuffer MlxDevice::allocate(DataType dtype, std::size_t element_count) {
    const std::size_t bytes = element_count * byte_size(dtype);
    void* handle = std::malloc(bytes == 0 ? 1 : bytes);
    if (handle == nullptr) {
        return DeviceBuffer{};
    }
    return make_buffer(handle, dtype, element_count);
}

ComputeStatus MlxDevice::copy_from_host(const void* host_source,
                                        DeviceBuffer& destination,
                                        std::size_t byte_count) {
    if (byte_count > destination.size_bytes()) {
        return ComputeStatus::invalid_argument(
            "mlx copy_from_host: byte_count exceeds destination size");
    }
    std::memcpy(destination.handle(), host_source, byte_count);
    return ComputeStatus::success();
}

ComputeStatus MlxDevice::copy_to_host(const DeviceBuffer& source,
                                      void* host_destination,
                                      std::size_t byte_count) const {
    if (byte_count > source.size_bytes()) {
        return ComputeStatus::invalid_argument(
            "mlx copy_to_host: byte_count exceeds source size");
    }
    std::memcpy(host_destination, source.handle(), byte_count);
    return ComputeStatus::success();
}

ComputeStatus MlxDevice::synchronize() {
    // The operators evaluate their result eagerly (mx::eval) before returning, so
    // there is no outstanding asynchronous work to wait on.
    return ComputeStatus::success();
}

ComputeStatus MlxDevice::gemm(const GemmParams& params, const DeviceBuffer& a,
                              const DeviceBuffer& b, DeviceBuffer& c) {
    if (a.dtype() != DataType::Float32 || b.dtype() != DataType::Float32 ||
        c.dtype() != DataType::Float32) {
        return ComputeStatus::invalid_argument(
            "mlx gemm: only float32 operands are supported");
    }
    if (a.element_count() != params.rows * params.inner ||
        b.element_count() != params.inner * params.cols ||
        c.element_count() != params.rows * params.cols) {
        return ComputeStatus::invalid_argument("mlx gemm: operand size mismatch");
    }

    const auto rows = static_cast<mx::ShapeElem>(params.rows);
    const auto inner = static_cast<mx::ShapeElem>(params.inner);
    const auto cols = static_cast<mx::ShapeElem>(params.cols);

    // op(A) is rows x inner; a stores A^T (inner x rows) when transpose_a.
    mx::array operand_a =
        params.transpose_a
            ? mx::transpose(as_float_array(a, mx::Shape{inner, rows}))
            : as_float_array(a, mx::Shape{rows, inner});
    // op(B) is inner x cols; b stores B^T (cols x inner) when transpose_b.
    mx::array operand_b =
        params.transpose_b
            ? mx::transpose(as_float_array(b, mx::Shape{cols, inner}))
            : as_float_array(b, mx::Shape{inner, cols});

    mx::array result = mx::multiply(mx::matmul(operand_a, operand_b),
                                    mx::array(static_cast<float>(params.alpha)));
    if (params.beta != 0.0) {
        result = mx::add(result, mx::multiply(
                                     as_float_array(c, mx::Shape{rows, cols}),
                                     mx::array(static_cast<float>(params.beta))));
    }

    mx::eval(result);
    std::memcpy(c.handle(), result.data<float>(),
                sizeof(float) * params.rows * params.cols);
    return ComputeStatus::success();
}

ComputeStatus MlxDevice::axpy(double alpha, const DeviceBuffer& x,
                              DeviceBuffer& y) {
    if (x.dtype() != DataType::Float32 || y.dtype() != DataType::Float32) {
        return ComputeStatus::invalid_argument(
            "mlx axpy: only float32 operands are supported");
    }
    if (x.element_count() != y.element_count()) {
        return ComputeStatus::invalid_argument("mlx axpy: size mismatch");
    }
    const auto count = static_cast<mx::ShapeElem>(x.element_count());
    mx::array result =
        mx::add(mx::multiply(as_float_array(x, mx::Shape{count}),
                             mx::array(static_cast<float>(alpha))),
                as_float_array(y, mx::Shape{count}));
    mx::eval(result);
    std::memcpy(y.handle(), result.data<float>(),
                sizeof(float) * y.element_count());
    return ComputeStatus::success();
}

// --- Operators not yet implemented on this backend --------------------------

ComputeStatus MlxDevice::map(UnaryOp, const DeviceBuffer&, DeviceBuffer&) {
    return ComputeStatus::unsupported("mlx map");
}
ComputeStatus MlxDevice::elementwise(BinaryOp, const DeviceBuffer&,
                                     const DeviceBuffer&, DeviceBuffer&) {
    return ComputeStatus::unsupported("mlx elementwise");
}
ComputeStatus MlxDevice::reduce(ReduceOp, const DeviceBuffer&, DeviceBuffer&) {
    return ComputeStatus::unsupported("mlx reduce");
}
ComputeStatus MlxDevice::scan(ReduceOp, ScanInclusivity, const DeviceBuffer&,
                              DeviceBuffer&) {
    return ComputeStatus::unsupported("mlx scan");
}
ComputeStatus MlxDevice::histogram(const DeviceBuffer&, std::size_t, double,
                                   double, DeviceBuffer&) {
    return ComputeStatus::unsupported("mlx histogram");
}
ComputeStatus MlxDevice::sort(DeviceBuffer&, SortOrder) {
    return ComputeStatus::unsupported("mlx sort");
}
ComputeStatus MlxDevice::sort_by_key(DeviceBuffer&, DeviceBuffer&, SortOrder) {
    return ComputeStatus::unsupported("mlx sort_by_key");
}
ComputeStatus MlxDevice::gather(const DeviceBuffer&, const DeviceBuffer&,
                                DeviceBuffer&) {
    return ComputeStatus::unsupported("mlx gather");
}
ComputeStatus MlxDevice::scatter(const DeviceBuffer&, const DeviceBuffer&,
                                 DeviceBuffer&) {
    return ComputeStatus::unsupported("mlx scatter");
}

void MlxDevice::release_buffer(void* handle) noexcept { std::free(handle); }

}  // namespace zetta::compute

#endif  // ZETTA_WITH_MLX
