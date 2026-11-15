// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// The element-type tag for device buffers (see device_buffer.hpp).
//
// The ComputeDevice seam (compute_device.hpp) is a runtime-polymorphic base
// class, so its memory + operator methods cannot be templated. A device buffer
// therefore carries its element type as a runtime `DataType` rather than a
// template parameter: a buffer is *typed* (it knows what it holds and how wide
// each element is) without forcing template instantiations across the virtual
// interface. Backends (CPU, CUDA) dispatch their kernels on this tag, and the
// registry/coordinator can reason about a buffer's type without seeing C++ types.
//
// `data_type_of<Element>` (below) bridges the two worlds: the typed convenience
// helpers on ComputeDevice (`upload<float>`, `download<float>`) use it to attach
// the right tag, giving call sites compile-time type safety over the runtime-
// typed core.
namespace zetta::compute {

// The numeric element types a device buffer can hold. This is the closed set the
// declared operators (map/elementwise, reduce/scan, histogram, gemm/axpy, sort,
// gather/scatter) work over — floats for features / gradients, signed ints for
// indices and bin ids, unsigned for counts / masks. New widths are added here
// when a real operator needs them, not speculatively.
enum class DataType {
    Float32,
    Float64,
    Int32,
    Int64,
    UInt32,
    UInt64,
    UInt8,
};

// The width of one element of `type`, in bytes. A buffer's byte size is its
// element count times this.
constexpr std::size_t byte_size(DataType type) {
    switch (type) {
        case DataType::Float32: return 4;
        case DataType::Float64: return 8;
        case DataType::Int32:   return 4;
        case DataType::Int64:   return 8;
        case DataType::UInt32:  return 4;
        case DataType::UInt64:  return 8;
        case DataType::UInt8:   return 1;
    }
    return 0;
}

// The lowercase canonical name of a data type ("float32", "int64", …). Used for
// logs, error messages, and (later) the registry's device-capability reporting.
constexpr std::string_view to_string(DataType type) {
    switch (type) {
        case DataType::Float32: return "float32";
        case DataType::Float64: return "float64";
        case DataType::Int32:   return "int32";
        case DataType::Int64:   return "int64";
        case DataType::UInt32:  return "uint32";
        case DataType::UInt64:  return "uint64";
        case DataType::UInt8:   return "uint8";
    }
    return "unknown";
}

// Map a C++ element type to its DataType tag. Used by the typed transfer helpers
// (ComputeDevice::upload / download) so call sites stay type-safe over the
// runtime-typed buffer. Only the supported element types specialize this; using
// any other type is a compile error (the primary template is left undefined on
// purpose — there is no silent fallback).
template <class Element>
struct data_type_traits;

template <> struct data_type_traits<float>         { static constexpr DataType value = DataType::Float32; };
template <> struct data_type_traits<double>        { static constexpr DataType value = DataType::Float64; };
template <> struct data_type_traits<std::int32_t>  { static constexpr DataType value = DataType::Int32; };
template <> struct data_type_traits<std::int64_t>  { static constexpr DataType value = DataType::Int64; };
template <> struct data_type_traits<std::uint32_t> { static constexpr DataType value = DataType::UInt32; };
template <> struct data_type_traits<std::uint64_t> { static constexpr DataType value = DataType::UInt64; };
template <> struct data_type_traits<std::uint8_t>  { static constexpr DataType value = DataType::UInt8; };

// The DataType tag for a C++ element type, e.g. `data_type_of<float>` ==
// DataType::Float32.
template <class Element>
inline constexpr DataType data_type_of = data_type_traits<Element>::value;

}  // namespace zetta::compute
