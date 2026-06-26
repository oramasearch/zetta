#include "zetta/compute/cpu_device.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <new>
#include <numeric>
#include <thread>
#include <type_traits>
#include <vector>

#include "zetta/runtime.hpp"

// The CPU reference backend. "Device memory" is host RAM (new[]/memcpy, like
// HostStubDevice); the difference is that every operator is implemented here. The
// data-parallel operators fan their per-element work across the zetta::runtime
// thread pool via runtime::parallel_for; the few that don't parallelize cleanly
// (comparison sort, and scatter, whose duplicate indices would race on write)
// run sequentially and say so. Correctness is the contract: these results are the
// numerical reference the CUDA backend (#361) is checked against.
namespace zetta::compute {

namespace {

// --- dtype classification ----------------------------------------------------

constexpr bool is_floating(DataType dtype) {
    return dtype == DataType::Float32 || dtype == DataType::Float64;
}

constexpr bool is_integer(DataType dtype) { return !is_floating(dtype); }

// --- type dispatch -----------------------------------------------------------
// A buffer's element type is a runtime DataType tag; these turn it into a static
// C++ type for a templated visitor. The visitor is `[]<class T>() -> ComputeStatus`,
// so the kernels stay one templated body each instead of seven copies.

template <class Visitor>
ComputeStatus visit_numeric(DataType dtype, Visitor&& visitor) {
    switch (dtype) {
        case DataType::Float32: return visitor.template operator()<float>();
        case DataType::Float64: return visitor.template operator()<double>();
        case DataType::Int32:   return visitor.template operator()<std::int32_t>();
        case DataType::Int64:   return visitor.template operator()<std::int64_t>();
        case DataType::UInt32:  return visitor.template operator()<std::uint32_t>();
        case DataType::UInt64:  return visitor.template operator()<std::uint64_t>();
        case DataType::UInt8:   return visitor.template operator()<std::uint8_t>();
    }
    return ComputeStatus::invalid_argument("unknown data type");
}

template <class Visitor>
ComputeStatus visit_integer(DataType dtype, Visitor&& visitor) {
    switch (dtype) {
        case DataType::Int32:  return visitor.template operator()<std::int32_t>();
        case DataType::Int64:  return visitor.template operator()<std::int64_t>();
        case DataType::UInt32: return visitor.template operator()<std::uint32_t>();
        case DataType::UInt64: return visitor.template operator()<std::uint64_t>();
        case DataType::UInt8:  return visitor.template operator()<std::uint8_t>();
        default: break;
    }
    return ComputeStatus::invalid_argument("operator requires an integer dtype");
}

template <class Visitor>
ComputeStatus visit_float(DataType dtype, Visitor&& visitor) {
    switch (dtype) {
        case DataType::Float32: return visitor.template operator()<float>();
        case DataType::Float64: return visitor.template operator()<double>();
        default: break;
    }
    return ComputeStatus::invalid_argument(
        "operator requires a floating-point dtype");
}

// --- scalar kernels ----------------------------------------------------------

template <class T>
T absolute_value(T value) {
    if constexpr (std::is_unsigned_v<T>) {
        return value;
    } else if constexpr (std::is_floating_point_v<T>) {
        return std::fabs(value);
    } else {
        return value < 0 ? static_cast<T>(-value) : value;
    }
}

template <class T>
T apply_unary(UnaryOp op, T value) {
    switch (op) {
        case UnaryOp::Negate:   return static_cast<T>(T{0} - value);
        case UnaryOp::Absolute: return absolute_value(value);
        case UnaryOp::Square:   return static_cast<T>(value * value);
        case UnaryOp::SquareRoot:
            if constexpr (std::is_floating_point_v<T>)
                return static_cast<T>(std::sqrt(value));
            else
                return T{0};  // guarded out at the op level for integers
        case UnaryOp::Exponential:
            if constexpr (std::is_floating_point_v<T>)
                return static_cast<T>(std::exp(value));
            else
                return T{0};
        case UnaryOp::Logarithm:
            if constexpr (std::is_floating_point_v<T>)
                return static_cast<T>(std::log(value));
            else
                return T{0};
    }
    return value;
}

// Math ops only defined over the reals — integer dtypes are rejected up front.
bool unary_requires_float(UnaryOp op) {
    return op == UnaryOp::SquareRoot || op == UnaryOp::Exponential ||
           op == UnaryOp::Logarithm;
}

template <class T>
T apply_binary(BinaryOp op, T lhs, T rhs) {
    switch (op) {
        case BinaryOp::Add:      return static_cast<T>(lhs + rhs);
        case BinaryOp::Subtract: return static_cast<T>(lhs - rhs);
        case BinaryOp::Multiply: return static_cast<T>(lhs * rhs);
        case BinaryOp::Divide:
            if constexpr (std::is_integral_v<T>) {
                // Integer divide-by-zero is UB on hardware; the reference
                // returns 0 so a stray zero divisor can't crash the node.
                return rhs == 0 ? T{0} : static_cast<T>(lhs / rhs);
            } else {
                return static_cast<T>(lhs / rhs);  // IEEE: yields inf / nan
            }
        case BinaryOp::Maximum: return std::max(lhs, rhs);
        case BinaryOp::Minimum: return std::min(lhs, rhs);
    }
    return lhs;
}

template <class T>
T reduce_identity(ReduceOp op) {
    switch (op) {
        case ReduceOp::Sum:     return T{0};
        case ReduceOp::Product: return T{1};
        case ReduceOp::Minimum: return std::numeric_limits<T>::max();
        case ReduceOp::Maximum: return std::numeric_limits<T>::lowest();
    }
    return T{0};
}

template <class T>
T reduce_combine(ReduceOp op, T lhs, T rhs) {
    switch (op) {
        case ReduceOp::Sum:     return static_cast<T>(lhs + rhs);
        case ReduceOp::Product: return static_cast<T>(lhs * rhs);
        case ReduceOp::Minimum: return std::min(lhs, rhs);
        case ReduceOp::Maximum: return std::max(lhs, rhs);
    }
    return lhs;
}

// Scan one contiguous range [begin, end) of `input` into `output`, seeded with
// `seed` (the reduction of everything before `begin`).
template <class T>
void scan_range(ReduceOp op, ScanInclusivity inclusivity, const T* input,
                T* output, std::size_t begin, std::size_t end, T seed) {
    T running = seed;
    if (inclusivity == ScanInclusivity::Inclusive) {
        for (std::size_t i = begin; i < end; ++i) {
            running = reduce_combine(op, running, input[i]);
            output[i] = running;
        }
    } else {
        for (std::size_t i = begin; i < end; ++i) {
            output[i] = running;
            running = reduce_combine(op, running, input[i]);
        }
    }
}

// --- validation helpers ------------------------------------------------------

ComputeStatus require(bool condition, const char* reason) {
    return condition ? ComputeStatus::success()
                     : ComputeStatus::invalid_argument(reason);
}

}  // namespace

// --- device info + memory model ----------------------------------------------

DeviceInfo CpuDevice::info() const {
    DeviceInfo information;
    information.kind = DeviceKind::Cpu;
    information.name = "cpu";
    information.compute_units = std::thread::hardware_concurrency();
    information.memory_bytes = 0;       // host RAM; not separately budgeted
    information.unified_memory = true;  // the "device" is the host address space
    return information;
}

DeviceBuffer CpuDevice::allocate(DataType dtype, std::size_t element_count) {
    const std::size_t byte_count = element_count * byte_size(dtype);
    // new[] throws std::bad_alloc; nothrow turns failure into an empty buffer so
    // the seam stays exception-free for callers.
    std::byte* storage = new (std::nothrow) std::byte[byte_count];
    if (storage == nullptr) return DeviceBuffer{};
    return make_buffer(storage, dtype, element_count);
}

ComputeStatus CpuDevice::copy_from_host(const void* host_source,
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
    std::memcpy(destination.handle(), host_source, byte_count);
    return ComputeStatus::success();
}

ComputeStatus CpuDevice::copy_to_host(const DeviceBuffer& source,
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
    std::memcpy(host_destination, source.handle(), byte_count);
    return ComputeStatus::success();
}

ComputeStatus CpuDevice::synchronize() {
    // parallel_for joins before returning, so all issued work is already done.
    return ComputeStatus::success();
}

// --- map / elementwise -------------------------------------------------------

ComputeStatus CpuDevice::map(UnaryOp op, const DeviceBuffer& input,
                             DeviceBuffer& output) {
    if (auto status = require(!input.empty() && !output.empty(),
                              "map: empty buffer");
        !status.ok()) {
        return status;
    }
    if (auto status = require(input.dtype() == output.dtype(),
                              "map: input/output dtype mismatch");
        !status.ok()) {
        return status;
    }
    if (auto status = require(input.element_count() == output.element_count(),
                              "map: input/output element count mismatch");
        !status.ok()) {
        return status;
    }
    if (unary_requires_float(op) && !is_floating(input.dtype())) {
        return ComputeStatus::invalid_argument(
            "map: sqrt/exp/log require a floating-point dtype");
    }

    return visit_numeric(input.dtype(), [&]<class T>() -> ComputeStatus {
        const T* in = static_cast<const T*>(input.handle());
        T* out = static_cast<T*>(output.handle());
        runtime::parallel_for(
            input.element_count(),
            [&](std::size_t, std::size_t begin, std::size_t end) {
                for (std::size_t i = begin; i < end; ++i)
                    out[i] = apply_unary<T>(op, in[i]);
            });
        return ComputeStatus::success();
    });
}

ComputeStatus CpuDevice::elementwise(BinaryOp op, const DeviceBuffer& lhs,
                                     const DeviceBuffer& rhs,
                                     DeviceBuffer& output) {
    if (auto status = require(!lhs.empty() && !rhs.empty() && !output.empty(),
                              "elementwise: empty buffer");
        !status.ok()) {
        return status;
    }
    if (auto status = require(lhs.dtype() == rhs.dtype() &&
                                  lhs.dtype() == output.dtype(),
                              "elementwise: dtype mismatch");
        !status.ok()) {
        return status;
    }
    if (auto status = require(lhs.element_count() == rhs.element_count() &&
                                  lhs.element_count() == output.element_count(),
                              "elementwise: element count mismatch");
        !status.ok()) {
        return status;
    }

    return visit_numeric(lhs.dtype(), [&]<class T>() -> ComputeStatus {
        const T* a = static_cast<const T*>(lhs.handle());
        const T* b = static_cast<const T*>(rhs.handle());
        T* out = static_cast<T*>(output.handle());
        runtime::parallel_for(
            lhs.element_count(),
            [&](std::size_t, std::size_t begin, std::size_t end) {
                for (std::size_t i = begin; i < end; ++i)
                    out[i] = apply_binary<T>(op, a[i], b[i]);
            });
        return ComputeStatus::success();
    });
}

// --- reduce / scan -----------------------------------------------------------

ComputeStatus CpuDevice::reduce(ReduceOp op, const DeviceBuffer& input,
                                DeviceBuffer& output) {
    if (auto status = require(!input.empty() && !output.empty(),
                              "reduce: empty buffer");
        !status.ok()) {
        return status;
    }
    if (auto status = require(input.dtype() == output.dtype(),
                              "reduce: input/output dtype mismatch");
        !status.ok()) {
        return status;
    }
    if (auto status = require(output.element_count() >= 1,
                              "reduce: output must hold at least one element");
        !status.ok()) {
        return status;
    }

    return visit_numeric(input.dtype(), [&]<class T>() -> ComputeStatus {
        const T* in = static_cast<const T*>(input.handle());
        const T identity = reduce_identity<T>(op);
        std::mutex merge_mutex;
        T total = identity;
        runtime::parallel_for(
            input.element_count(),
            [&](std::size_t, std::size_t begin, std::size_t end) {
                T local = identity;
                for (std::size_t i = begin; i < end; ++i)
                    local = reduce_combine(op, local, in[i]);
                std::lock_guard<std::mutex> lock(merge_mutex);
                total = reduce_combine(op, total, local);
            });
        *static_cast<T*>(output.handle()) = total;
        return ComputeStatus::success();
    });
}

ComputeStatus CpuDevice::scan(ReduceOp op, ScanInclusivity inclusivity,
                              const DeviceBuffer& input, DeviceBuffer& output) {
    if (auto status = require(!input.empty() && !output.empty(),
                              "scan: empty buffer");
        !status.ok()) {
        return status;
    }
    if (auto status = require(input.dtype() == output.dtype(),
                              "scan: input/output dtype mismatch");
        !status.ok()) {
        return status;
    }
    if (auto status = require(input.element_count() == output.element_count(),
                              "scan: input/output element count mismatch");
        !status.ok()) {
        return status;
    }

    const std::size_t count = input.element_count();
    return visit_numeric(input.dtype(), [&]<class T>() -> ComputeStatus {
        const T* in = static_cast<const T*>(input.handle());
        T* out = static_cast<T*>(output.handle());
        const T identity = reduce_identity<T>(op);

        const std::size_t chunks = runtime::parallel_chunks(count);
        if (chunks <= 1) {
            scan_range<T>(op, inclusivity, in, out, 0, count, identity);
            return ComputeStatus::success();
        }

        // Parallel three-phase prefix scan. parallel_for tiles [0, count) the
        // same way in both passes (its tiling is a pure function of count), so a
        // chunk index maps to the same [begin, end) in phase 1 and phase 3.
        std::vector<T> chunk_total(chunks, identity);
        runtime::parallel_for(
            count,
            [&](std::size_t chunk, std::size_t begin, std::size_t end) {
                T local = identity;
                for (std::size_t i = begin; i < end; ++i)
                    local = reduce_combine(op, local, in[i]);
                chunk_total[chunk] = local;
            });

        std::vector<T> offset(chunks);
        T running = identity;
        for (std::size_t chunk = 0; chunk < chunks; ++chunk) {
            offset[chunk] = running;
            running = reduce_combine(op, running, chunk_total[chunk]);
        }

        runtime::parallel_for(
            count,
            [&](std::size_t chunk, std::size_t begin, std::size_t end) {
                scan_range<T>(op, inclusivity, in, out, begin, end,
                              offset[chunk]);
            });
        return ComputeStatus::success();
    });
}

// --- histogram ---------------------------------------------------------------

ComputeStatus CpuDevice::histogram(const DeviceBuffer& input,
                                   std::size_t bin_count, double range_min,
                                   double range_max,
                                   DeviceBuffer& output_counts) {
    if (auto status = require(!input.empty() && !output_counts.empty(),
                              "histogram: empty buffer");
        !status.ok()) {
        return status;
    }
    if (auto status = require(bin_count > 0, "histogram: bin_count must be > 0");
        !status.ok()) {
        return status;
    }
    if (auto status = require(range_max > range_min,
                              "histogram: range_max must exceed range_min");
        !status.ok()) {
        return status;
    }
    if (auto status =
            require(output_counts.element_count() == bin_count,
                    "histogram: output_counts size must equal bin_count");
        !status.ok()) {
        return status;
    }
    if (auto status = require(is_integer(output_counts.dtype()),
                              "histogram: output_counts must be an integer dtype");
        !status.ok()) {
        return status;
    }

    const double span = range_max - range_min;
    std::vector<std::uint64_t> counts(bin_count, 0);
    std::mutex merge_mutex;

    const ComputeStatus accumulate =
        visit_numeric(input.dtype(), [&]<class T>() -> ComputeStatus {
            const T* in = static_cast<const T*>(input.handle());
            runtime::parallel_for(
                input.element_count(),
                [&](std::size_t, std::size_t begin, std::size_t end) {
                    std::vector<std::uint64_t> local(bin_count, 0);
                    for (std::size_t i = begin; i < end; ++i) {
                        const double value = static_cast<double>(in[i]);
                        if (value < range_min || value >= range_max) continue;
                        std::size_t bin = static_cast<std::size_t>(
                            (value - range_min) / span *
                            static_cast<double>(bin_count));
                        if (bin >= bin_count) bin = bin_count - 1;  // fp edge
                        ++local[bin];
                    }
                    std::lock_guard<std::mutex> lock(merge_mutex);
                    for (std::size_t b = 0; b < bin_count; ++b)
                        counts[b] += local[b];
                });
            return ComputeStatus::success();
        });
    if (!accumulate.ok()) return accumulate;

    return visit_integer(output_counts.dtype(), [&]<class Count>() -> ComputeStatus {
        Count* out = static_cast<Count*>(output_counts.handle());
        for (std::size_t b = 0; b < bin_count; ++b)
            out[b] = static_cast<Count>(counts[b]);
        return ComputeStatus::success();
    });
}

// --- gemm / axpy -------------------------------------------------------------

ComputeStatus CpuDevice::gemm(const GemmParams& params, const DeviceBuffer& a,
                              const DeviceBuffer& b, DeviceBuffer& c) {
    if (auto status = require(!a.empty() && !b.empty() && !c.empty(),
                              "gemm: empty buffer");
        !status.ok()) {
        return status;
    }
    if (auto status =
            require(a.dtype() == b.dtype() && a.dtype() == c.dtype(),
                    "gemm: operand dtype mismatch");
        !status.ok()) {
        return status;
    }
    if (auto status = require(is_floating(c.dtype()),
                              "gemm: requires a floating-point dtype");
        !status.ok()) {
        return status;
    }
    const std::size_t rows = params.rows;
    const std::size_t inner = params.inner;
    const std::size_t cols = params.cols;
    if (auto status = require(a.element_count() == rows * inner,
                              "gemm: A size must equal rows*inner");
        !status.ok()) {
        return status;
    }
    if (auto status = require(b.element_count() == inner * cols,
                              "gemm: B size must equal inner*cols");
        !status.ok()) {
        return status;
    }
    if (auto status = require(c.element_count() == rows * cols,
                              "gemm: C size must equal rows*cols");
        !status.ok()) {
        return status;
    }

    return visit_float(c.dtype(), [&]<class T>() -> ComputeStatus {
        const T* matrix_a = static_cast<const T*>(a.handle());
        const T* matrix_b = static_cast<const T*>(b.handle());
        T* matrix_c = static_cast<T*>(c.handle());
        const T alpha = static_cast<T>(params.alpha);
        const T beta = static_cast<T>(params.beta);
        const bool beta_is_zero = params.beta == 0.0;

        // Parallelize over the rows of C; each row is an independent dot-product
        // sweep, so no cross-thread coordination is needed.
        runtime::parallel_for(
            rows, [&](std::size_t, std::size_t row_begin, std::size_t row_end) {
                for (std::size_t row = row_begin; row < row_end; ++row) {
                    for (std::size_t col = 0; col < cols; ++col) {
                        T accumulator = T{0};
                        for (std::size_t k = 0; k < inner; ++k) {
                            const T av = params.transpose_a
                                             ? matrix_a[k * rows + row]
                                             : matrix_a[row * inner + k];
                            const T bv = params.transpose_b
                                             ? matrix_b[col * inner + k]
                                             : matrix_b[k * cols + col];
                            accumulator =
                                static_cast<T>(accumulator + av * bv);
                        }
                        T& target = matrix_c[row * cols + col];
                        // beta == 0 means "do not read C" (BLAS convention), so
                        // an uninitialized / NaN C does not poison the result.
                        target = beta_is_zero
                                     ? static_cast<T>(alpha * accumulator)
                                     : static_cast<T>(alpha * accumulator +
                                                      beta * target);
                    }
                }
            });
        return ComputeStatus::success();
    });
}

ComputeStatus CpuDevice::axpy(double alpha, const DeviceBuffer& x,
                              DeviceBuffer& y) {
    if (auto status = require(!x.empty() && !y.empty(), "axpy: empty buffer");
        !status.ok()) {
        return status;
    }
    if (auto status = require(x.dtype() == y.dtype(), "axpy: dtype mismatch");
        !status.ok()) {
        return status;
    }
    if (auto status = require(x.element_count() == y.element_count(),
                              "axpy: element count mismatch");
        !status.ok()) {
        return status;
    }
    if (auto status = require(is_floating(x.dtype()),
                              "axpy: requires a floating-point dtype");
        !status.ok()) {
        return status;
    }

    return visit_float(x.dtype(), [&]<class T>() -> ComputeStatus {
        const T* vector_x = static_cast<const T*>(x.handle());
        T* vector_y = static_cast<T*>(y.handle());
        const T scale = static_cast<T>(alpha);
        runtime::parallel_for(
            x.element_count(),
            [&](std::size_t, std::size_t begin, std::size_t end) {
                for (std::size_t i = begin; i < end; ++i)
                    vector_y[i] =
                        static_cast<T>(scale * vector_x[i] + vector_y[i]);
            });
        return ComputeStatus::success();
    });
}

// --- sort / sort_by_key ------------------------------------------------------
// Comparison sort over the whole array; runs sequentially (a correct parallel
// merge sort is a future optimization — the reference's job is the right answer).

ComputeStatus CpuDevice::sort(DeviceBuffer& keys, SortOrder order) {
    if (auto status = require(!keys.empty(), "sort: empty buffer");
        !status.ok()) {
        return status;
    }
    return visit_numeric(keys.dtype(), [&]<class T>() -> ComputeStatus {
        T* data = static_cast<T*>(keys.handle());
        const std::size_t count = keys.element_count();
        if (order == SortOrder::Ascending)
            std::sort(data, data + count);
        else
            std::sort(data, data + count, std::greater<T>{});
        return ComputeStatus::success();
    });
}

ComputeStatus CpuDevice::sort_by_key(DeviceBuffer& keys, DeviceBuffer& values,
                                     SortOrder order) {
    if (auto status = require(!keys.empty() && !values.empty(),
                              "sort_by_key: empty buffer");
        !status.ok()) {
        return status;
    }
    if (auto status = require(keys.element_count() == values.element_count(),
                              "sort_by_key: key/value count mismatch");
        !status.ok()) {
        return status;
    }

    const std::size_t count = keys.element_count();
    std::vector<std::size_t> permutation(count);
    std::iota(permutation.begin(), permutation.end(), std::size_t{0});

    // Sort an index permutation by key (stable, so equal keys keep input order),
    // then reorder keys (typed) and values (by raw element size) through it.
    const ComputeStatus order_keys =
        visit_numeric(keys.dtype(), [&]<class T>() -> ComputeStatus {
            const T* key_data = static_cast<const T*>(keys.handle());
            std::stable_sort(permutation.begin(), permutation.end(),
                             [&](std::size_t lhs, std::size_t rhs) {
                                 return order == SortOrder::Ascending
                                            ? key_data[lhs] < key_data[rhs]
                                            : key_data[lhs] > key_data[rhs];
                             });
            std::vector<T> sorted(count);
            for (std::size_t i = 0; i < count; ++i)
                sorted[i] = key_data[permutation[i]];
            std::memcpy(keys.handle(), sorted.data(), count * sizeof(T));
            return ComputeStatus::success();
        });
    if (!order_keys.ok()) return order_keys;

    const std::size_t element_size = byte_size(values.dtype());
    std::byte* value_bytes = static_cast<std::byte*>(values.handle());
    std::vector<std::byte> reordered(count * element_size);
    for (std::size_t i = 0; i < count; ++i) {
        std::memcpy(reordered.data() + i * element_size,
                    value_bytes + permutation[i] * element_size, element_size);
    }
    std::memcpy(value_bytes, reordered.data(), count * element_size);
    return ComputeStatus::success();
}

// --- gather / scatter --------------------------------------------------------

ComputeStatus CpuDevice::gather(const DeviceBuffer& input,
                                const DeviceBuffer& indices,
                                DeviceBuffer& output) {
    if (auto status =
            require(!input.empty() && !indices.empty() && !output.empty(),
                    "gather: empty buffer");
        !status.ok()) {
        return status;
    }
    if (auto status = require(input.dtype() == output.dtype(),
                              "gather: input/output dtype mismatch");
        !status.ok()) {
        return status;
    }
    if (auto status = require(indices.element_count() == output.element_count(),
                              "gather: output/indices count mismatch");
        !status.ok()) {
        return status;
    }

    const std::size_t element_size = byte_size(input.dtype());
    const std::size_t input_count = input.element_count();
    const std::byte* source = static_cast<const std::byte*>(input.handle());
    std::byte* destination = static_cast<std::byte*>(output.handle());
    std::atomic<bool> out_of_range{false};

    const ComputeStatus status =
        visit_integer(indices.dtype(), [&]<class Index>() -> ComputeStatus {
            const Index* index = static_cast<const Index*>(indices.handle());
            runtime::parallel_for(
                indices.element_count(),
                [&](std::size_t, std::size_t begin, std::size_t end) {
                    for (std::size_t i = begin; i < end; ++i) {
                        const Index value = index[i];
                        if constexpr (std::is_signed_v<Index>) {
                            if (value < 0) {
                                out_of_range.store(true);
                                continue;
                            }
                        }
                        if (static_cast<std::size_t>(value) >= input_count) {
                            out_of_range.store(true);
                            continue;
                        }
                        std::memcpy(
                            destination + i * element_size,
                            source + static_cast<std::size_t>(value) *
                                         element_size,
                            element_size);
                    }
                });
            return ComputeStatus::success();
        });
    if (!status.ok()) return status;
    if (out_of_range.load()) {
        return ComputeStatus::invalid_argument("gather: index out of range");
    }
    return ComputeStatus::success();
}

ComputeStatus CpuDevice::scatter(const DeviceBuffer& input,
                                 const DeviceBuffer& indices,
                                 DeviceBuffer& output) {
    if (auto status =
            require(!input.empty() && !indices.empty() && !output.empty(),
                    "scatter: empty buffer");
        !status.ok()) {
        return status;
    }
    if (auto status = require(input.dtype() == output.dtype(),
                              "scatter: input/output dtype mismatch");
        !status.ok()) {
        return status;
    }
    if (auto status = require(indices.element_count() == input.element_count(),
                              "scatter: input/indices count mismatch");
        !status.ok()) {
        return status;
    }

    const std::size_t element_size = byte_size(input.dtype());
    const std::size_t output_count = output.element_count();
    const std::byte* source = static_cast<const std::byte*>(input.handle());
    std::byte* destination = static_cast<std::byte*>(output.handle());
    bool out_of_range = false;

    // Sequential on purpose: duplicate indices would be a write race in
    // parallel. Running in input order makes "last writer wins" deterministic.
    const ComputeStatus status =
        visit_integer(indices.dtype(), [&]<class Index>() -> ComputeStatus {
            const Index* index = static_cast<const Index*>(indices.handle());
            for (std::size_t i = 0; i < input.element_count(); ++i) {
                const Index value = index[i];
                if constexpr (std::is_signed_v<Index>) {
                    if (value < 0) {
                        out_of_range = true;
                        continue;
                    }
                }
                if (static_cast<std::size_t>(value) >= output_count) {
                    out_of_range = true;
                    continue;
                }
                std::memcpy(
                    destination + static_cast<std::size_t>(value) * element_size,
                    source + i * element_size, element_size);
            }
            return ComputeStatus::success();
        });
    if (!status.ok()) return status;
    if (out_of_range) {
        return ComputeStatus::invalid_argument("scatter: index out of range");
    }
    return ComputeStatus::success();
}

void CpuDevice::release_buffer(void* handle) noexcept {
    delete[] static_cast<std::byte*>(handle);
}

}  // namespace zetta::compute
