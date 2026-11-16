// SPDX-License-Identifier: MIT

#include "zetta/compute/cuda_device.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <type_traits>

#include <mutex>
#include <unordered_map>
#include <vector>
#include <cublas_v2.h>
#include <cuda_runtime.h>
#include <thrust/device_ptr.h>
#include <thrust/execution_policy.h>
#include <thrust/functional.h>
#include <thrust/reduce.h>
#include <thrust/scan.h>
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
// with hand-written functors (no device lambdas, so no --extended-lambda),
// reduce/scan/sort/sort_by_key via Thrust, and histogram/gemm/gather/scatter as
// custom __global__ kernels. Each call dispatches on the buffer's runtime DataType
// to the matching element type, and is wrapped so a Thrust/CUDA failure becomes a
// ComputeStatus rather than an exception crossing the seam (zetta-core is no-throw
// at the seam).
//
// The CPU reference backend (CpuDevice, #360) defines the contract these kernels
// reproduce: the same per-operator preconditions (empty-buffer / dtype / size
// guards, float-only math for sqrt/exp/log/gemm/axpy, integer-divide-by-zero
// safety) and the same edge-case behavior (out-of-range gather/scatter rejected
// with InvalidArgument; duplicate-index scatter resolved last-writer-wins by input
// order). tests/test_cuda_device.cpp checks every operator against CpuDevice on
// identical inputs — the "matches the CPU reference within tolerance" acceptance
// criterion of #361.
namespace zetta::compute {

#if defined(ZETTA_WITH_CUDA)

namespace {

constexpr unsigned kBlockSize = 256;

// dtype classification, mirroring CpuDevice so the two backends accept/reject the
// same inputs (the seam's whole point is interchangeability — see #361).
constexpr bool is_floating(DataType dtype) {
    return dtype == DataType::Float32 || dtype == DataType::Float64;
}

// Math defined only over the reals — integer dtypes are rejected up front, exactly
// as CpuDevice::map / gemm / axpy do.
constexpr bool unary_requires_float(UnaryOp op) {
    return op == UnaryOp::SquareRoot || op == UnaryOp::Exponential ||
           op == UnaryOp::Logarithm;
}

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
        case BinaryOp::Divide:
            if constexpr (std::is_integral_v<Element>) {
                // Integer divide-by-zero is UB on the device; the CPU reference
                // returns 0 so a stray zero divisor can't fault a kernel. Match it.
                return b == Element{0} ? Element{0} : static_cast<Element>(a / b);
            } else {
                return static_cast<Element>(a / b);  // IEEE: yields inf / nan
            }
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

// Is `raw` a valid 0-based index into `bound` elements? Signed negatives and any
// value >= bound are out of range — the same test CpuDevice's gather/scatter run.
template <class Index>
__host__ __device__ inline bool index_in_range(Index raw, std::size_t bound) {
    if constexpr (std::is_signed_v<Index>) {
        if (raw < 0) return false;
    }
    return static_cast<unsigned long long>(raw) < bound;
}

// gather: output[i] = input[indices[i]]. Bounds-checks each index against
// input_count and flags (rather than reads out of bounds) on a violation, so an
// out-of-range index becomes an InvalidArgument like the CPU reference instead of
// undefined device memory access (thrust::gather does no bounds check).
template <class Element, class Index>
__global__ void gather_kernel(const Element* input, std::size_t input_count,
                              const Index* indices, std::size_t count,
                              Element* output, int* out_of_range) {
    std::size_t i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= count) return;
    const Index raw = indices[i];
    if (!index_in_range<Index>(raw, input_count)) {
        *out_of_range = 1;
        return;
    }
    output[i] = input[static_cast<std::size_t>(raw)];
}

// scatter pass 1: for each input i with a valid target indices[i], record the
// largest contributing i per output slot via atomicMax. "Largest i wins" makes a
// duplicate-index scatter resolve last-writer-wins by input order — deterministic,
// matching CpuDevice's sequential scatter (thrust::scatter races on duplicates).
// winner holds i+1 (0 means "no writer"); an out-of-range index flags instead.
template <class Index>
__global__ void scatter_winner_kernel(const Index* indices, std::size_t count,
                                      std::size_t output_count,
                                      unsigned long long* winner,
                                      int* out_of_range) {
    std::size_t i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= count) return;
    const Index raw = indices[i];
    if (!index_in_range<Index>(raw, output_count)) {
        *out_of_range = 1;
        return;
    }
    atomicMax(&winner[static_cast<std::size_t>(raw)],
              static_cast<unsigned long long>(i) + 1ull);
}

// scatter pass 2: each written output slot takes the value of its winning input.
// Slots with no writer keep their prior contents, exactly as the CPU reference
// leaves unscattered positions untouched.
template <class Element>
__global__ void scatter_apply_kernel(const Element* input,
                                     const unsigned long long* winner,
                                     Element* output, std::size_t output_count) {
    std::size_t slot = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (slot >= output_count) return;
    const unsigned long long w = winner[slot];
    if (w != 0ull) output[slot] = input[static_cast<std::size_t>(w - 1ull)];
}

}  // namespace

// The allocation pool. cudaMalloc/cudaFree synchronize the device, and an iterative workload
// allocates constantly — one autograd training step issues ~1,700 allocate/free pairs, which
// serializes the GPU against the host and showed up as ~11% GPU utilization while training (#928).
// Reusing exact-size blocks removes that churn without changing any interface.
struct CudaDevice::Pool {
    mutable std::mutex mutex;                                  // the sanctioned adapter-level lock
    // One cuBLAS handle per device, created lazily and reused. Creating a handle per gemm call
    // costs milliseconds and would reintroduce the per-call setup this pool exists to remove.
    cublasHandle_t blas = nullptr;
    bool blas_ready = false;
    bool allow_tf32 = true;
    std::unordered_map<std::size_t, std::vector<void*>> free_blocks;  // keyed by exact byte size
    std::unordered_map<void*, std::size_t> live_sizes;         // release_buffer only gets a handle
    std::size_t limit_bytes = 2ULL * 1024 * 1024 * 1024;       // 2 GiB reserve by default
    std::size_t held_bytes = 0;
    std::size_t hits = 0;

    // A block of exactly `bytes`, from the reserve when one is waiting. Exact-size matching, not
    // best-fit: a training step reuses the same shapes every iteration, so exact hits are the
    // common case, and best-fit would fragment the reserve into unusable odd sizes.
    void* take(std::size_t bytes) {
        std::lock_guard<std::mutex> guard(mutex);
        const auto found = free_blocks.find(bytes);
        if (found == free_blocks.end() || found->second.empty()) return nullptr;
        void* storage = found->second.back();
        found->second.pop_back();
        held_bytes -= bytes;
        ++hits;
        live_sizes[storage] = bytes;
        return storage;
    }

    void record_live(void* storage, std::size_t bytes) {
        std::lock_guard<std::mutex> guard(mutex);
        live_sizes[storage] = bytes;
    }

    // Keep the block for reuse, unless that would push the reserve past its limit — in which case
    // hand it back to CUDA. Without the cap the reserve would grow to the high-water mark of every
    // shape ever seen and quietly consume the memory ceiling #924 sets.
    bool keep(void* storage) {
        std::lock_guard<std::mutex> guard(mutex);
        const auto found = live_sizes.find(storage);
        if (found == live_sizes.end()) return false;  // not ours: caller frees it
        const std::size_t bytes = found->second;
        live_sizes.erase(found);
        if (limit_bytes == 0 || held_bytes + bytes > limit_bytes) return false;
        free_blocks[bytes].push_back(storage);
        held_bytes += bytes;
        return true;
    }

    // The cuBLAS handle, created on first use. TF32 is enabled by default: A100 tensor cores run
    // TF32 matmuls ~8x faster than fp32 CUDA cores, and TF32 keeps fp32's exponent range while
    // reducing the mantissa to 10 bits — a precision change that must be a deliberate, reported
    // choice rather than a silent one, which is why it is switchable.
    cublasHandle_t blas_handle() {
        std::lock_guard<std::mutex> guard(mutex);
        if (!blas_ready) {
            if (cublasCreate(&blas) != CUBLAS_STATUS_SUCCESS) {
                blas = nullptr;
            } else {
                cublasSetMathMode(blas, allow_tf32 ? CUBLAS_TF32_TENSOR_OP_MATH
                                                   : CUBLAS_DEFAULT_MATH);
            }
            blas_ready = true;
        }
        return blas;
    }

    void set_tf32(bool enabled) {
        std::lock_guard<std::mutex> guard(mutex);
        allow_tf32 = enabled;
        if (blas != nullptr) {
            cublasSetMathMode(blas, enabled ? CUBLAS_TF32_TENSOR_OP_MATH : CUBLAS_DEFAULT_MATH);
        }
    }

    void destroy_blas() {
        std::lock_guard<std::mutex> guard(mutex);
        if (blas != nullptr) cublasDestroy(blas);
        blas = nullptr;
        blas_ready = false;
    }

    void drain() {
        std::vector<void*> to_free;
        {
            std::lock_guard<std::mutex> guard(mutex);
            for (auto& [bytes, blocks] : free_blocks) {
                for (void* storage : blocks) to_free.push_back(storage);
                blocks.clear();
            }
            free_blocks.clear();
            held_bytes = 0;
        }
        for (void* storage : to_free) cudaFree(storage);  // outside the lock: cudaFree synchronizes
    }
};

CudaDevice::CudaDevice(int device_index) noexcept
    : device_index_(device_index), pool_(std::make_shared<Pool>()) {}

// Hand the reserve back to CUDA. Without this the pooled blocks would live until process exit,
// which on a box that also serves a node is a leak, not a cache.
CudaDevice::~CudaDevice() {
    if (pool_) {
        pool_->destroy_blas();
        pool_->drain();
    }
}

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
    if (byte_count == 0) return DeviceBuffer{};

    // Reuse a pooled block when one of exactly this size is waiting, so the common case of an
    // iterative workload re-allocating the same shapes costs no cudaMalloc at all.
    if (pool_) {
        if (void* reused = pool_->take(byte_count); reused != nullptr) {
            return make_buffer(reused, dtype, element_count);
        }
    }
    void* storage = nullptr;
    if (cudaMalloc(&storage, byte_count) != cudaSuccess || storage == nullptr) {
        // Out of memory with a reserve still held is recoverable: hand the reserve back and retry
        // once. Failing here while pooling megabytes would be an own goal.
        if (pool_ != nullptr && pool_->held_bytes > 0) {
            pool_->drain();
            if (cudaMalloc(&storage, byte_count) != cudaSuccess || storage == nullptr) {
                return DeviceBuffer{};
            }
        } else {
            return DeviceBuffer{};
        }
    }
    if (pool_) pool_->record_live(storage, byte_count);
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
    if (input.empty() || output.empty()) {
        return ComputeStatus::invalid_argument("map: empty buffer");
    }
    if (input.dtype() != output.dtype() ||
        input.element_count() != output.element_count()) {
        return ComputeStatus::invalid_argument(
            "map: input and output must share dtype and element count");
    }
    if (unary_requires_float(op) && !is_floating(input.dtype())) {
        return ComputeStatus::invalid_argument(
            "map: sqrt/exp/log require a floating-point dtype");
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
    if (lhs.empty() || rhs.empty() || output.empty()) {
        return ComputeStatus::invalid_argument("elementwise: empty buffer");
    }
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
    if (input.empty() || output.empty()) {
        return ComputeStatus::invalid_argument("reduce: empty buffer");
    }
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
    if (input.empty() || output.empty()) {
        return ComputeStatus::invalid_argument("scan: empty buffer");
    }
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
    if (input.empty() || output_counts.empty()) {
        return ComputeStatus::invalid_argument("histogram: empty buffer");
    }
    if (bin_count == 0) {
        return ComputeStatus::invalid_argument("histogram: bin_count must be > 0");
    }
    if (output_counts.element_count() != bin_count) {
        return ComputeStatus::invalid_argument(
            "histogram: output_counts size must equal bin_count");
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
    if (a.empty() || b.empty() || c.empty()) {
        return ComputeStatus::invalid_argument("gemm: empty buffer");
    }
    if (a.dtype() != b.dtype() || a.dtype() != c.dtype()) {
        return ComputeStatus::invalid_argument("gemm: operands must share dtype");
    }
    if (!is_floating(c.dtype())) {
        return ComputeStatus::invalid_argument(
            "gemm: requires a floating-point dtype");
    }
    if (a.element_count() != params.rows * params.inner ||
        b.element_count() != params.inner * params.cols ||
        c.element_count() != params.rows * params.cols) {
        return ComputeStatus::invalid_argument(
            "gemm: a/b/c sizes must match the requested shape");
    }
    if (cudaSetDevice(device_index_) != cudaSuccess) {
        return ComputeStatus::backend_error("gemm: cudaSetDevice failed");
    }
    if (params.rows == 0 || params.cols == 0) return ComputeStatus::success();

    // --- cuBLAS fast path (#929) ----------------------------------------------------------
    // The hand-written kernel below is a naive global-memory loop with an fp64 accumulator: no
    // tiling, no data reuse, no tensor cores. On an A100 it sustains ~0.3-0.5 TFLOPS against the
    // 110-130 TFLOPS cuBLAS reaches with TF32 — a 200-400x gap on the hottest op in training.
    // Keep it only as the fallback for dtypes cuBLAS does not cover, and as the reference the
    // conformance tests compare against.
    //
    // cuBLAS is COLUMN-major; our buffers are row-major. Rather than transpose anything, compute
    // the transpose of the result: a row-major C[m,n] occupies the same bytes as a column-major
    // C^T[n,m], and C^T = op_b(B)^T * op_a(A)^T. So the operands are passed in the opposite order
    // and each "no transpose" case is already transposed by the layout reinterpretation itself.
    if (a.dtype() == DataType::Float32 && pool_) {
        if (cublasHandle_t blas = pool_->blas_handle(); blas != nullptr) {
            const int m = static_cast<int>(params.rows);
            const int n = static_cast<int>(params.cols);
            const int k = static_cast<int>(params.inner);
            const float alpha = static_cast<float>(params.alpha);
            const float beta = static_cast<float>(params.beta);
            // Reading row-major memory as column-major already yields the transpose, so a caller
            // asking for NO transpose needs OP_N here, and one asking FOR a transpose needs OP_T.
            const cublasOperation_t op_a = params.transpose_a ? CUBLAS_OP_T : CUBLAS_OP_N;
            const cublasOperation_t op_b = params.transpose_b ? CUBLAS_OP_T : CUBLAS_OP_N;
            const int lda = params.transpose_a ? m : k;
            const int ldb = params.transpose_b ? k : n;
            const cublasStatus_t blas_status = cublasSgemm(
                blas, op_b, op_a, n, m, k, &alpha,
                static_cast<const float*>(b.handle()), ldb,
                static_cast<const float*>(a.handle()), lda, &beta,
                static_cast<float*>(c.handle()), n);
            if (blas_status == CUBLAS_STATUS_SUCCESS) {
                return after_kernel("gemm");
            }
            // Fall through to the reference kernel rather than failing: a cuBLAS refusal (an
            // unsupported shape, say) should degrade to slow-but-correct, not to an error.
        }
    }

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
    if (x.empty() || y.empty()) {
        return ComputeStatus::invalid_argument("axpy: empty buffer");
    }
    if (x.dtype() != y.dtype() || x.element_count() != y.element_count()) {
        return ComputeStatus::invalid_argument(
            "axpy: x and y must share dtype and element count");
    }
    if (!is_floating(x.dtype())) {
        return ComputeStatus::invalid_argument(
            "axpy: requires a floating-point dtype");
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
    if (keys.empty()) {
        return ComputeStatus::invalid_argument("sort: empty buffer");
    }
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
    if (keys.empty() || values.empty()) {
        return ComputeStatus::invalid_argument("sort_by_key: empty buffer");
    }
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
    if (input.empty() || indices.empty() || output.empty()) {
        return ComputeStatus::invalid_argument("gather: empty buffer");
    }
    if (input.dtype() != output.dtype() ||
        indices.element_count() != output.element_count()) {
        return ComputeStatus::invalid_argument(
            "gather: output must share input dtype and indices' element count");
    }
    if (cudaSetDevice(device_index_) != cudaSuccess) {
        return ComputeStatus::backend_error("gather: cudaSetDevice failed");
    }

    // A custom bounds-checking kernel (thrust::gather does none) flags an
    // out-of-range index in a one-int device cell, so it becomes InvalidArgument
    // like the CPU reference rather than an undefined out-of-bounds device read.
    const std::size_t count = indices.element_count();
    int* out_of_range = nullptr;
    if (ComputeStatus alloc =
            from_cuda(cudaMalloc(&out_of_range, sizeof(int)), "gather: alloc flag");
        !alloc.ok()) {
        return alloc;
    }
    ComputeStatus status =
        from_cuda(cudaMemset(out_of_range, 0, sizeof(int)), "gather: clear flag");
    if (status.ok()) {
        const unsigned blocks =
            static_cast<unsigned>((count + kBlockSize - 1) / kBlockSize);
        try {
            status =
                dispatch_dtype(input.dtype(), [&]<class Element>() -> ComputeStatus {
                    return dispatch_integer_dtype(
                        indices.dtype(), [&]<class Index>() -> ComputeStatus {
                            gather_kernel<Element, Index><<<blocks, kBlockSize>>>(
                                static_cast<const Element*>(input.handle()),
                                input.element_count(),
                                static_cast<const Index*>(indices.handle()), count,
                                static_cast<Element*>(output.handle()),
                                out_of_range);
                            return after_kernel("gather");
                        });
                });
        } catch (const std::exception& error) {
            status =
                ComputeStatus::backend_error(std::string{"gather: "} + error.what());
        }
    }
    if (status.ok()) {
        int flagged = 0;
        status = from_cuda(cudaMemcpy(&flagged, out_of_range, sizeof(int),
                                      cudaMemcpyDeviceToHost),
                           "gather: read flag");
        if (status.ok() && flagged != 0) {
            status = ComputeStatus::invalid_argument("gather: index out of range");
        }
    }
    cudaFree(out_of_range);
    return status;
}

ComputeStatus CudaDevice::scatter(const DeviceBuffer& input,
                                  const DeviceBuffer& indices,
                                  DeviceBuffer& output) {
    if (input.empty() || indices.empty() || output.empty()) {
        return ComputeStatus::invalid_argument("scatter: empty buffer");
    }
    if (input.dtype() != output.dtype() ||
        input.element_count() != indices.element_count()) {
        return ComputeStatus::invalid_argument(
            "scatter: input must share output dtype and indices' element count");
    }
    if (cudaSetDevice(device_index_) != cudaSuccess) {
        return ComputeStatus::backend_error("scatter: cudaSetDevice failed");
    }

    // thrust::scatter neither bounds-checks nor resolves duplicate indices
    // deterministically. A two-pass kernel does both: pass 1 records, per output
    // slot, the largest contributing input index via atomicMax (last-writer-wins
    // by input order, matching the CPU reference) and flags any out-of-range
    // index; pass 2 writes each winning value. winner holds i+1 (0 = no writer).
    const std::size_t count = input.element_count();
    const std::size_t output_count = output.element_count();
    unsigned long long* winner = nullptr;
    if (ComputeStatus alloc =
            from_cuda(cudaMalloc(&winner, output_count * sizeof(unsigned long long)),
                      "scatter: alloc winners");
        !alloc.ok()) {
        return alloc;
    }
    int* out_of_range = nullptr;
    ComputeStatus status =
        from_cuda(cudaMalloc(&out_of_range, sizeof(int)), "scatter: alloc flag");
    if (status.ok()) {
        status = from_cuda(
            cudaMemset(winner, 0, output_count * sizeof(unsigned long long)),
            "scatter: clear winners");
    }
    if (status.ok()) {
        status = from_cuda(cudaMemset(out_of_range, 0, sizeof(int)),
                           "scatter: clear flag");
    }
    if (status.ok()) {
        const unsigned in_blocks =
            static_cast<unsigned>((count + kBlockSize - 1) / kBlockSize);
        try {
            status = dispatch_integer_dtype(
                indices.dtype(), [&]<class Index>() -> ComputeStatus {
                    scatter_winner_kernel<Index><<<in_blocks, kBlockSize>>>(
                        static_cast<const Index*>(indices.handle()), count,
                        output_count, winner, out_of_range);
                    return after_kernel("scatter: index pass");
                });
        } catch (const std::exception& error) {
            status = ComputeStatus::backend_error(std::string{"scatter: "} +
                                                  error.what());
        }
    }
    if (status.ok()) {
        int flagged = 0;
        status = from_cuda(cudaMemcpy(&flagged, out_of_range, sizeof(int),
                                      cudaMemcpyDeviceToHost),
                           "scatter: read flag");
        if (status.ok() && flagged != 0) {
            status = ComputeStatus::invalid_argument("scatter: index out of range");
        }
    }
    if (status.ok()) {
        const unsigned out_blocks =
            static_cast<unsigned>((output_count + kBlockSize - 1) / kBlockSize);
        try {
            status =
                dispatch_dtype(input.dtype(), [&]<class Element>() -> ComputeStatus {
                    scatter_apply_kernel<Element><<<out_blocks, kBlockSize>>>(
                        static_cast<const Element*>(input.handle()), winner,
                        static_cast<Element*>(output.handle()), output_count);
                    return after_kernel("scatter: apply pass");
                });
        } catch (const std::exception& error) {
            status = ComputeStatus::backend_error(std::string{"scatter: "} +
                                                  error.what());
        }
    }
    cudaFree(winner);
    cudaFree(out_of_range);
    return status;
}

void CudaDevice::release_buffer(void* handle) noexcept {
    // Keep the block for reuse when the pool will take it; only fall through to cudaFree (which
    // synchronizes the device) when it will not.
    if (pool_ && pool_->keep(handle)) return;
    cudaFree(handle);
}

ComputeStatus CudaDevice::gemm_batched(const GemmParams& params, const DeviceBuffer& a,
                                       std::size_t stride_a, const DeviceBuffer& b,
                                       std::size_t stride_b, DeviceBuffer& c, std::size_t stride_c,
                                       int batch) {
    if (batch <= 0) return ComputeStatus::invalid_argument("gemm_batched: batch must be positive");
    if (a.dtype() != DataType::Float32 || b.dtype() != DataType::Float32 ||
        c.dtype() != DataType::Float32) {
        return ComputeStatus::unsupported("gemm_batched: float32 only");
    }
    if (cudaSetDevice(device_index_) != cudaSuccess) {
        return ComputeStatus::backend_error("gemm_batched: cudaSetDevice failed");
    }
    if (!pool_) return ComputeStatus::backend_error("gemm_batched: no device pool");
    cublasHandle_t blas = pool_->blas_handle();
    if (blas == nullptr) return ComputeStatus::backend_error("gemm_batched: no cuBLAS handle");

    // Same column-major inversion as `gemm`: compute C^T = op_b(B)^T * op_a(A)^T, so the operands
    // are passed in the opposite order and the layout reinterpretation supplies the transpose.
    const int m = static_cast<int>(params.rows);
    const int n = static_cast<int>(params.cols);
    const int k = static_cast<int>(params.inner);
    const float alpha = static_cast<float>(params.alpha);
    const float beta = static_cast<float>(params.beta);
    const cublasOperation_t op_a = params.transpose_a ? CUBLAS_OP_T : CUBLAS_OP_N;
    const cublasOperation_t op_b = params.transpose_b ? CUBLAS_OP_T : CUBLAS_OP_N;
    const int lda = params.transpose_a ? m : k;
    const int ldb = params.transpose_b ? k : n;
    const cublasStatus_t status = cublasSgemmStridedBatched(
        blas, op_b, op_a, n, m, k, &alpha, static_cast<const float*>(b.handle()), ldb,
        static_cast<long long>(stride_b), static_cast<const float*>(a.handle()), lda,
        static_cast<long long>(stride_a), &beta, static_cast<float*>(c.handle()), n,
        static_cast<long long>(stride_c), batch);
    if (status != CUBLAS_STATUS_SUCCESS) {
        return ComputeStatus::backend_error("gemm_batched: cublasSgemmStridedBatched failed");
    }
    return after_kernel("gemm_batched");
}

void CudaDevice::set_pool_limit(std::size_t bytes) {
    if (!pool_) return;
    {
        std::lock_guard<std::mutex> guard(pool_->mutex);
        pool_->limit_bytes = bytes;
    }
    if (bytes == 0) pool_->drain();  // disabling the pool must release what it already holds
}

std::size_t CudaDevice::pooled_bytes() const {
    if (!pool_) return 0;
    std::lock_guard<std::mutex> guard(pool_->mutex);
    return pool_->held_bytes;
}

std::size_t CudaDevice::pool_hits() const {
    if (!pool_) return 0;
    std::lock_guard<std::mutex> guard(pool_->mutex);
    return pool_->hits;
}

void CudaDevice::drain_pool() {
    if (pool_) pool_->drain();
}

#endif  // ZETTA_WITH_CUDA

}  // namespace zetta::compute
