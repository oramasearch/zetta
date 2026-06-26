#include "zetta/compute/cuda_device.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <type_traits>

#include <cuda_runtime.h>
#include <thrust/device_ptr.h>
#include <thrust/execution_policy.h>
#include <thrust/functional.h>
#include <thrust/gather.h>
#include <thrust/reduce.h>
#include <thrust/scan.h>
#include <thrust/scatter.h>
#include <thrust/sort.h>
#include <thrust/transform.h>

// The CUDA backend of the ComputeDevice seam. Everything CUDA — <cuda_runtime.h>,
// Thrust, the __global__ kernels — is confined to this translation unit, the one
// file nvcc compiles into the zetta-compute static library. The public header
// (cuda_device.hpp) stays CUDA-free, so zetta-core links the backend without ever
// seeing a CUDA type (the same discipline that keeps SDL/ImGui out of zetta-core).
//
// The memory model is the CUDA runtime (cudaMalloc/cudaMemcpy/cudaFree). The
// operator set runs real device work: map/elementwise/axpy via thrust::transform
// with hand-written functors (no device lambdas, so no --extended-lambda), reduce/
// scan/sort/sort_by_key/gather/scatter via Thrust, and histogram/gemm as custom
// kernels. Each call dispatches on the buffer's runtime DataType to the matching
// element type, and is wrapped so a Thrust/CUDA failure becomes a ComputeStatus
// rather than an exception crossing the seam (zetta-core is no-throw at the seam).
namespace zetta::compute {

#if defined(ZETTA_WITH_CUDA)

namespace {

constexpr unsigned kBlockSize = 256;

// Map a CUDA runtime error to a ComputeStatus (out_of_memory for allocation
// failures, backend_error otherwise). cudaSuccess maps to success().
ComputeStatus from_cuda(cudaError_t error, std::string_view context) {
    if (error == cudaSuccess) return ComputeStatus::success();
    std::string reason =
        std::string{context} + ": " + cudaGetErrorString(error);
    if (error == cudaErrorMemoryAllocation) {
        return ComputeStatus::out_of_memory(std::move(reason));
    }
    return ComputeStatus::backend_error(std::move(reason));
}

// Check for a kernel launch / async error and synchronize so the status reflects
// the work actually completing (the operators surface failures eagerly rather
// than deferring every error to the caller's synchronize()).
ComputeStatus after_kernel(std::string_view context) {
    cudaError_t launch = cudaGetLastError();
    if (launch != cudaSuccess) return from_cuda(launch, context);
    return from_cuda(cudaDeviceSynchronize(), context);
}

// Cast a buffer's opaque handle to a typed device pointer. Read-only inputs cast
// away const (the bytes live on the device; Thrust only reads through it).
template <class Element>
thrust::device_ptr<Element> device_begin(DeviceBuffer& buffer) {
    return thrust::device_pointer_cast(static_cast<Element*>(buffer.handle()));
}
template <class Element>
thrust::device_ptr<Element> device_begin(const DeviceBuffer& buffer) {
    return thrust::device_pointer_cast(
        static_cast<Element*>(const_cast<void*>(buffer.handle())));
}

// Dispatch a generic callable over the runtime DataType to the matching C++
// element type. The element widths match byte_size(): Int64/UInt64 use the
// 8-byte long-long types so they line up with a buffer's raw bytes.
template <class Callable>
ComputeStatus dispatch_dtype(DataType dtype, Callable call) {
    switch (dtype) {
        case DataType::Float32: return call.template operator()<float>();
        case DataType::Float64: return call.template operator()<double>();
        case DataType::Int32:   return call.template operator()<std::int32_t>();
        case DataType::Int64:   return call.template operator()<long long>();
        case DataType::UInt32:  return call.template operator()<std::uint32_t>();
        case DataType::UInt64:  return call.template operator()<unsigned long long>();
        case DataType::UInt8:   return call.template operator()<unsigned char>();
    }
    return ComputeStatus::invalid_argument("dispatch: unknown data type");
}

// Dispatch over the integer DataTypes only — for index buffers (gather/scatter/
// sort_by_key) and histogram counters, where a float element type is a category
// error rather than a missing instantiation.
template <class Callable>
ComputeStatus dispatch_integer_dtype(DataType dtype, Callable call) {
    switch (dtype) {
        case DataType::Int32:  return call.template operator()<std::int32_t>();
        case DataType::Int64:  return call.template operator()<long long>();
        case DataType::UInt32: return call.template operator()<std::uint32_t>();
        case DataType::UInt64: return call.template operator()<unsigned long long>();
        case DataType::UInt8:  return call.template operator()<unsigned char>();
        case DataType::Float32:
        case DataType::Float64:
            return ComputeStatus::invalid_argument(
                "expected an integer buffer, got a floating-point one");
    }
    return ComputeStatus::invalid_argument("dispatch: unknown data type");
}

// --- Elementwise math, shared by host (functor construction) and device --------

template <class Element>
__host__ __device__ Element apply_unary(UnaryOp op, Element value) {
    switch (op) {
        case UnaryOp::Negate:
            if constexpr (std::is_unsigned_v<Element>) {
                return static_cast<Element>(static_cast<Element>(0) - value);
            } else {
                return static_cast<Element>(-value);
            }
        case UnaryOp::Absolute:
            if constexpr (std::is_unsigned_v<Element>) {
                return value;
            } else if constexpr (std::is_floating_point_v<Element>) {
                return static_cast<Element>(::fabs(static_cast<double>(value)));
            } else {
                return value < 0 ? static_cast<Element>(-value) : value;
            }
        case UnaryOp::Square:
            return static_cast<Element>(value * value);
        case UnaryOp::SquareRoot:
            return static_cast<Element>(::sqrt(static_cast<double>(value)));
        case UnaryOp::Exponential:
            return static_cast<Element>(::exp(static_cast<double>(value)));
        case UnaryOp::Logarithm:
            return static_cast<Element>(::log(static_cast<double>(value)));
    }
    return value;
}

template <class Element>
__host__ __device__ Element apply_binary(BinaryOp op, Element a, Element b) {
    switch (op) {
        case BinaryOp::Add:      return static_cast<Element>(a + b);
        case BinaryOp::Subtract: return static_cast<Element>(a - b);
        case BinaryOp::Multiply: return static_cast<Element>(a * b);
        case BinaryOp::Divide:   return static_cast<Element>(a / b);
        case BinaryOp::Maximum:  return a > b ? a : b;
        case BinaryOp::Minimum:  return a < b ? a : b;
    }
    return a;
}

template <class Element>
struct UnaryFunctor {
    UnaryOp op;
    __host__ __device__ Element operator()(Element value) const {
        return apply_unary<Element>(op, value);
    }
};

template <class Element>
struct BinaryFunctor {
    BinaryOp op;
    __host__ __device__ Element operator()(Element a, Element b) const {
        return apply_binary<Element>(op, a, b);
    }
};

template <class Element>
struct AxpyFunctor {
    double alpha;
    __host__ __device__ Element operator()(Element x, Element y) const {
        return static_cast<Element>(alpha * static_cast<double>(x) +
                                    static_cast<double>(y));
    }
};

// --- Custom kernels ----------------------------------------------------------

// Atomic increment specialized per counter width (CUDA's atomicAdd has int /
// unsigned / unsigned-long-long overloads; the 64-bit ints reinterpret to ULL).
__device__ inline void atomic_increment(std::int32_t* p) { atomicAdd(p, 1); }
__device__ inline void atomic_increment(std::uint32_t* p) { atomicAdd(p, 1u); }
__device__ inline void atomic_increment(long long* p) {
    atomicAdd(reinterpret_cast<unsigned long long*>(p), 1ull);
}
__device__ inline void atomic_increment(unsigned long long* p) {
    atomicAdd(p, 1ull);
}
__device__ inline void atomic_increment(unsigned char* p) {
    // No byte-wide atomicAdd; emulate via a 32-bit CAS on the enclosing word.
    auto* base = reinterpret_cast<unsigned int*>(
        reinterpret_cast<std::uintptr_t>(p) & ~std::uintptr_t{3});
    const unsigned shift =
        (reinterpret_cast<std::uintptr_t>(p) & 3u) * 8u;
    unsigned old = *base, assumed;
    do {
        assumed = old;
        unsigned byte = (assumed >> shift) & 0xffu;
        byte = (byte + 1u) & 0xffu;
        unsigned updated = (assumed & ~(0xffu << shift)) | (byte << shift);
        old = atomicCAS(base, assumed, updated);
    } while (assumed != old);
}

template <class Value, class Count>
__global__ void histogram_kernel(const Value* input, std::size_t count,
                                  Count* counts, std::size_t bin_count,
                                  double range_min, double range_max) {
    std::size_t i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= count) return;
    double value = static_cast<double>(input[i]);
    if (value < range_min || value >= range_max) return;
    double fraction = (value - range_min) / (range_max - range_min);
    long long bin = static_cast<long long>(fraction * static_cast<double>(bin_count));
    if (bin < 0) bin = 0;
    if (bin >= static_cast<long long>(bin_count)) bin = static_cast<long long>(bin_count) - 1;
    atomic_increment(&counts[bin]);
}

// Naive row-major GEMM: C = alpha * op(A) * op(B) + beta * C, accumulating in
// double for accuracy. op(A) is rows x inner, op(B) is inner x cols, C is
// rows x cols. The transpose flags pick the index into the row-major operands.
template <class Element>
__global__ void gemm_kernel(const Element* a, const Element* b, Element* c,
                            std::size_t rows, std::size_t inner, std::size_t cols,
                            double alpha, double beta, bool transpose_a,
                            bool transpose_b) {
    std::size_t row = static_cast<std::size_t>(blockIdx.y) * blockDim.y + threadIdx.y;
    std::size_t col = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (row >= rows || col >= cols) return;
    double accumulator = 0.0;
    for (std::size_t k = 0; k < inner; ++k) {
        double av = transpose_a ? static_cast<double>(a[k * rows + row])
                                : static_cast<double>(a[row * inner + k]);
        double bv = transpose_b ? static_cast<double>(b[col * inner + k])
                                : static_cast<double>(b[k * cols + col]);
        accumulator += av * bv;
    }
    double current = beta != 0.0 ? static_cast<double>(c[row * cols + col]) : 0.0;
    c[row * cols + col] = static_cast<Element>(alpha * accumulator + beta * current);
}

}  // namespace

CudaDevice::CudaDevice(int device_index) noexcept : device_index_(device_index) {}

DeviceInfo CudaDevice::info() const {
    DeviceInfo information;
    information.kind = DeviceKind::Cuda;
    cudaDeviceProp properties{};
    if (cudaGetDeviceProperties(&properties, device_index_) == cudaSuccess) {
        information.name = properties.name;
        information.compute_units =
            static_cast<std::size_t>(properties.multiProcessorCount);
        information.memory_bytes = properties.totalGlobalMem;
        information.unified_memory = properties.unifiedAddressing != 0;
    } else {
        information.name = "cuda";
    }
    return information;
}

DeviceBuffer CudaDevice::allocate(DataType dtype, std::size_t element_count) {
    if (cudaSetDevice(device_index_) != cudaSuccess) return DeviceBuffer{};
    const std::size_t byte_count = element_count * byte_size(dtype);
    void* storage = nullptr;
    if (cudaMalloc(&storage, byte_count) != cudaSuccess || storage == nullptr) {
        return DeviceBuffer{};
    }
    return make_buffer(storage, dtype, element_count);
}

ComputeStatus CudaDevice::copy_from_host(const void* host_source,
                                         DeviceBuffer& destination,
                                         std::size_t byte_count) {
    if (byte_count == 0) return ComputeStatus::success();
    if (host_source == nullptr || destination.handle() == nullptr) {
        return ComputeStatus::invalid_argument("copy_from_host: null buffer");
    }
    if (byte_count > destination.size_bytes()) {
        return ComputeStatus::invalid_argument(
            "copy_from_host: byte_count exceeds destination size");
    }
    return from_cuda(cudaMemcpy(destination.handle(), host_source, byte_count,
                                cudaMemcpyHostToDevice),
                     "copy_from_host");
}

ComputeStatus CudaDevice::copy_to_host(const DeviceBuffer& source,
                                       void* host_destination,
                                       std::size_t byte_count) const {
    if (byte_count == 0) return ComputeStatus::success();
    if (host_destination == nullptr || source.handle() == nullptr) {
        return ComputeStatus::invalid_argument("copy_to_host: null buffer");
    }
    if (byte_count > source.size_bytes()) {
        return ComputeStatus::invalid_argument(
            "copy_to_host: byte_count exceeds source size");
    }
    return from_cuda(cudaMemcpy(host_destination, source.handle(), byte_count,
                                cudaMemcpyDeviceToHost),
                     "copy_to_host");
}

ComputeStatus CudaDevice::synchronize() {
    return from_cuda(cudaDeviceSynchronize(), "synchronize");
}

// --- Operators ---------------------------------------------------------------

ComputeStatus CudaDevice::map(UnaryOp op, const DeviceBuffer& input,
                              DeviceBuffer& output) {
    if (input.dtype() != output.dtype() ||
        input.element_count() != output.element_count()) {
        return ComputeStatus::invalid_argument(
            "map: input and output must share dtype and element count");
    }
    if (cudaSetDevice(device_index_) != cudaSuccess) {
        return ComputeStatus::backend_error("map: cudaSetDevice failed");
    }
    try {
        return dispatch_dtype(input.dtype(), [&]<class Element>() -> ComputeStatus {
            thrust::transform(thrust::device, device_begin<Element>(input),
                              device_begin<Element>(input) + input.element_count(),
                              device_begin<Element>(output),
                              UnaryFunctor<Element>{op});
            return after_kernel("map");
        });
    } catch (const std::exception& error) {
        return ComputeStatus::backend_error(std::string{"map: "} + error.what());
    }
}

ComputeStatus CudaDevice::elementwise(BinaryOp op, const DeviceBuffer& lhs,
                                      const DeviceBuffer& rhs,
                                      DeviceBuffer& output) {
    if (lhs.dtype() != rhs.dtype() || lhs.dtype() != output.dtype() ||
        lhs.element_count() != rhs.element_count() ||
        lhs.element_count() != output.element_count()) {
        return ComputeStatus::invalid_argument(
            "elementwise: operands and output must share dtype and element count");
    }
    if (cudaSetDevice(device_index_) != cudaSuccess) {
        return ComputeStatus::backend_error("elementwise: cudaSetDevice failed");
    }
    try {
        return dispatch_dtype(lhs.dtype(), [&]<class Element>() -> ComputeStatus {
            thrust::transform(thrust::device, device_begin<Element>(lhs),
                              device_begin<Element>(lhs) + lhs.element_count(),
                              device_begin<Element>(rhs),
                              device_begin<Element>(output),
                              BinaryFunctor<Element>{op});
            return after_kernel("elementwise");
        });
    } catch (const std::exception& error) {
        return ComputeStatus::backend_error(std::string{"elementwise: "} +
                                            error.what());
    }
}

ComputeStatus CudaDevice::reduce(ReduceOp op, const DeviceBuffer& input,
                                 DeviceBuffer& output) {
    if (input.dtype() != output.dtype() || output.element_count() < 1) {
        return ComputeStatus::invalid_argument(
            "reduce: output must share dtype and hold at least one element");
    }
    if (cudaSetDevice(device_index_) != cudaSuccess) {
        return ComputeStatus::backend_error("reduce: cudaSetDevice failed");
    }
    try {
        return dispatch_dtype(input.dtype(), [&]<class Element>() -> ComputeStatus {
            auto first = device_begin<Element>(input);
            auto last = first + input.element_count();
            Element result{};
            switch (op) {
                case ReduceOp::Sum:
                    result = thrust::reduce(thrust::device, first, last,
                                            Element{0}, thrust::plus<Element>());
                    break;
                case ReduceOp::Product:
                    result = thrust::reduce(thrust::device, first, last,
                                            Element{1}, thrust::multiplies<Element>());
                    break;
                case ReduceOp::Minimum:
                    result = thrust::reduce(thrust::device, first, last,
                                            std::numeric_limits<Element>::max(),
                                            thrust::minimum<Element>());
                    break;
                case ReduceOp::Maximum:
                    result = thrust::reduce(thrust::device, first, last,
                                            std::numeric_limits<Element>::lowest(),
                                            thrust::maximum<Element>());
                    break;
            }
            return from_cuda(cudaMemcpy(output.handle(), &result, sizeof(Element),
                                        cudaMemcpyHostToDevice),
                             "reduce: store result");
        });
    } catch (const std::exception& error) {
        return ComputeStatus::backend_error(std::string{"reduce: "} + error.what());
    }
}

ComputeStatus CudaDevice::scan(ReduceOp op, ScanInclusivity inclusivity,
                               const DeviceBuffer& input, DeviceBuffer& output) {
    if (input.dtype() != output.dtype() ||
        input.element_count() != output.element_count()) {
        return ComputeStatus::invalid_argument(
            "scan: input and output must share dtype and element count");
    }
    if (cudaSetDevice(device_index_) != cudaSuccess) {
        return ComputeStatus::backend_error("scan: cudaSetDevice failed");
    }
    try {
        return dispatch_dtype(input.dtype(), [&]<class Element>() -> ComputeStatus {
            auto first = device_begin<Element>(input);
            auto last = first + input.element_count();
            auto out = device_begin<Element>(output);
            const bool inclusive = inclusivity == ScanInclusivity::Inclusive;
            switch (op) {
                case ReduceOp::Sum:
                    if (inclusive) {
                        thrust::inclusive_scan(thrust::device, first, last, out,
                                               thrust::plus<Element>());
                    } else {
                        thrust::exclusive_scan(thrust::device, first, last, out,
                                               Element{0}, thrust::plus<Element>());
                    }
                    break;
                case ReduceOp::Product:
                    if (inclusive) {
                        thrust::inclusive_scan(thrust::device, first, last, out,
                                               thrust::multiplies<Element>());
                    } else {
                        thrust::exclusive_scan(thrust::device, first, last, out,
                                               Element{1}, thrust::multiplies<Element>());
                    }
                    break;
                case ReduceOp::Minimum:
                    if (inclusive) {
                        thrust::inclusive_scan(thrust::device, first, last, out,
                                               thrust::minimum<Element>());
                    } else {
                        thrust::exclusive_scan(thrust::device, first, last, out,
                                               std::numeric_limits<Element>::max(),
                                               thrust::minimum<Element>());
                    }
                    break;
                case ReduceOp::Maximum:
                    if (inclusive) {
                        thrust::inclusive_scan(thrust::device, first, last, out,
                                               thrust::maximum<Element>());
                    } else {
                        thrust::exclusive_scan(thrust::device, first, last, out,
                                               std::numeric_limits<Element>::lowest(),
                                               thrust::maximum<Element>());
                    }
                    break;
            }
            return after_kernel("scan");
        });
    } catch (const std::exception& error) {
        return ComputeStatus::backend_error(std::string{"scan: "} + error.what());
    }
}

ComputeStatus CudaDevice::histogram(const DeviceBuffer& input,
                                    std::size_t bin_count, double range_min,
                                    double range_max, DeviceBuffer& output_counts) {
    if (bin_count == 0 || output_counts.element_count() < bin_count) {
        return ComputeStatus::invalid_argument(
            "histogram: output_counts must hold at least bin_count elements");
    }
    if (range_max <= range_min) {
        return ComputeStatus::invalid_argument(
            "histogram: range_max must exceed range_min");
    }
    if (cudaSetDevice(device_index_) != cudaSuccess) {
        return ComputeStatus::backend_error("histogram: cudaSetDevice failed");
    }
    cudaError_t cleared = cudaMemset(output_counts.handle(), 0,
                                     output_counts.size_bytes());
    if (cleared != cudaSuccess) return from_cuda(cleared, "histogram: clear counts");
    const std::size_t count = input.element_count();
    if (count == 0) return ComputeStatus::success();
    const unsigned blocks =
        static_cast<unsigned>((count + kBlockSize - 1) / kBlockSize);
    try {
        return dispatch_dtype(input.dtype(), [&]<class Value>() -> ComputeStatus {
            return dispatch_integer_dtype(
                output_counts.dtype(), [&]<class Count>() -> ComputeStatus {
                    histogram_kernel<Value, Count><<<blocks, kBlockSize>>>(
                        static_cast<const Value*>(input.handle()), count,
                        static_cast<Count*>(output_counts.handle()), bin_count,
                        range_min, range_max);
                    return after_kernel("histogram");
                });
        });
    } catch (const std::exception& error) {
        return ComputeStatus::backend_error(std::string{"histogram: "} +
                                            error.what());
    }
}

ComputeStatus CudaDevice::gemm(const GemmParams& params, const DeviceBuffer& a,
                               const DeviceBuffer& b, DeviceBuffer& c) {
    if (a.dtype() != b.dtype() || a.dtype() != c.dtype()) {
        return ComputeStatus::invalid_argument("gemm: operands must share dtype");
    }
    if (a.element_count() < params.rows * params.inner ||
        b.element_count() < params.inner * params.cols ||
        c.element_count() < params.rows * params.cols) {
        return ComputeStatus::invalid_argument(
            "gemm: a/b/c are too small for the requested shape");
    }
    if (cudaSetDevice(device_index_) != cudaSuccess) {
        return ComputeStatus::backend_error("gemm: cudaSetDevice failed");
    }
    if (params.rows == 0 || params.cols == 0) return ComputeStatus::success();
    const dim3 block(16, 16);
    const dim3 grid(static_cast<unsigned>((params.cols + 15) / 16),
                    static_cast<unsigned>((params.rows + 15) / 16));
    try {
        return dispatch_dtype(a.dtype(), [&]<class Element>() -> ComputeStatus {
            gemm_kernel<Element><<<grid, block>>>(
                static_cast<const Element*>(a.handle()),
                static_cast<const Element*>(b.handle()),
                static_cast<Element*>(c.handle()), params.rows, params.inner,
                params.cols, params.alpha, params.beta, params.transpose_a,
                params.transpose_b);
            return after_kernel("gemm");
        });
    } catch (const std::exception& error) {
        return ComputeStatus::backend_error(std::string{"gemm: "} + error.what());
    }
}

ComputeStatus CudaDevice::axpy(double alpha, const DeviceBuffer& x,
                               DeviceBuffer& y) {
    if (x.dtype() != y.dtype() || x.element_count() != y.element_count()) {
        return ComputeStatus::invalid_argument(
            "axpy: x and y must share dtype and element count");
    }
    if (cudaSetDevice(device_index_) != cudaSuccess) {
        return ComputeStatus::backend_error("axpy: cudaSetDevice failed");
    }
    try {
        return dispatch_dtype(x.dtype(), [&]<class Element>() -> ComputeStatus {
            thrust::transform(thrust::device, device_begin<Element>(x),
                              device_begin<Element>(x) + x.element_count(),
                              device_begin<Element>(y), device_begin<Element>(y),
                              AxpyFunctor<Element>{alpha});
            return after_kernel("axpy");
        });
    } catch (const std::exception& error) {
        return ComputeStatus::backend_error(std::string{"axpy: "} + error.what());
    }
}

ComputeStatus CudaDevice::sort(DeviceBuffer& keys, SortOrder order) {
    if (cudaSetDevice(device_index_) != cudaSuccess) {
        return ComputeStatus::backend_error("sort: cudaSetDevice failed");
    }
    try {
        return dispatch_dtype(keys.dtype(), [&]<class Key>() -> ComputeStatus {
            auto first = device_begin<Key>(keys);
            auto last = first + keys.element_count();
            if (order == SortOrder::Ascending) {
                thrust::sort(thrust::device, first, last);
            } else {
                thrust::sort(thrust::device, first, last, thrust::greater<Key>());
            }
            return after_kernel("sort");
        });
    } catch (const std::exception& error) {
        return ComputeStatus::backend_error(std::string{"sort: "} + error.what());
    }
}

ComputeStatus CudaDevice::sort_by_key(DeviceBuffer& keys, DeviceBuffer& values,
                                      SortOrder order) {
    if (keys.element_count() != values.element_count()) {
        return ComputeStatus::invalid_argument(
            "sort_by_key: keys and values must share element count");
    }
    if (cudaSetDevice(device_index_) != cudaSuccess) {
        return ComputeStatus::backend_error("sort_by_key: cudaSetDevice failed");
    }
    try {
        return dispatch_dtype(keys.dtype(), [&]<class Key>() -> ComputeStatus {
            return dispatch_dtype(values.dtype(), [&]<class Value>() -> ComputeStatus {
                auto keys_first = device_begin<Key>(keys);
                auto keys_last = keys_first + keys.element_count();
                auto values_first = device_begin<Value>(values);
                if (order == SortOrder::Ascending) {
                    thrust::sort_by_key(thrust::device, keys_first, keys_last,
                                        values_first);
                } else {
                    thrust::sort_by_key(thrust::device, keys_first, keys_last,
                                        values_first, thrust::greater<Key>());
                }
                return after_kernel("sort_by_key");
            });
        });
    } catch (const std::exception& error) {
        return ComputeStatus::backend_error(std::string{"sort_by_key: "} +
                                            error.what());
    }
}

ComputeStatus CudaDevice::gather(const DeviceBuffer& input,
                                 const DeviceBuffer& indices,
                                 DeviceBuffer& output) {
    if (input.dtype() != output.dtype() ||
        indices.element_count() != output.element_count()) {
        return ComputeStatus::invalid_argument(
            "gather: output must share input dtype and indices' element count");
    }
    if (cudaSetDevice(device_index_) != cudaSuccess) {
        return ComputeStatus::backend_error("gather: cudaSetDevice failed");
    }
    try {
        return dispatch_dtype(input.dtype(), [&]<class Element>() -> ComputeStatus {
            return dispatch_integer_dtype(
                indices.dtype(), [&]<class Index>() -> ComputeStatus {
                    auto map_first = device_begin<Index>(indices);
                    auto map_last = map_first + indices.element_count();
                    thrust::gather(thrust::device, map_first, map_last,
                                   device_begin<Element>(input),
                                   device_begin<Element>(output));
                    return after_kernel("gather");
                });
        });
    } catch (const std::exception& error) {
        return ComputeStatus::backend_error(std::string{"gather: "} + error.what());
    }
}

ComputeStatus CudaDevice::scatter(const DeviceBuffer& input,
                                  const DeviceBuffer& indices,
                                  DeviceBuffer& output) {
    if (input.dtype() != output.dtype() ||
        input.element_count() != indices.element_count()) {
        return ComputeStatus::invalid_argument(
            "scatter: input must share output dtype and indices' element count");
    }
    if (cudaSetDevice(device_index_) != cudaSuccess) {
        return ComputeStatus::backend_error("scatter: cudaSetDevice failed");
    }
    try {
        return dispatch_dtype(input.dtype(), [&]<class Element>() -> ComputeStatus {
            return dispatch_integer_dtype(
                indices.dtype(), [&]<class Index>() -> ComputeStatus {
                    auto input_first = device_begin<Element>(input);
                    auto input_last = input_first + input.element_count();
                    thrust::scatter(thrust::device, input_first, input_last,
                                    device_begin<Index>(indices),
                                    device_begin<Element>(output));
                    return after_kernel("scatter");
                });
        });
    } catch (const std::exception& error) {
        return ComputeStatus::backend_error(std::string{"scatter: "} +
                                            error.what());
    }
}

void CudaDevice::release_buffer(void* handle) noexcept { cudaFree(handle); }

#endif  // ZETTA_WITH_CUDA

}  // namespace zetta::compute
