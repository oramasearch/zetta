// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "zetta/compute/compute_device.hpp"
#include "zetta/ddf/data_frame.hpp"

// DDF DataFrame operators (ARC-12, #385) — the first ops over the DDF substrate
// (#380): **sort** (order a frame by a key column), **concat** (union / row-wise
// concatenation of frames), and **slice** (a contiguous row range). They build on
// the [`ComputeDevice`](../compute/compute_device.hpp) operator set — the actual
// compare sort runs through `sort_by_key` on the frame's device, so a CUDA-backed
// frame sorts on the GPU — and are the local building blocks the **distributed
// sort** (distributed_sort.hpp) composes across the cluster.
//
// Every op is **non-throwing**: it returns a `DataFrameResult` whose `status`
// explains any failure (a missing key column, mismatched schemas, a device error),
// matching the loader and the no-exceptions convention of zetta-core. The input
// frame(s) are read, never mutated — each op produces a fresh, move-only frame.
//
// See docs/ddf/ddf-ops.md.
namespace zetta::ddf {

// Sort `frame` by the `key_column` column, ascending by default. Returns a new
// frame with the same schema and chunk layout, its rows globally ordered by the
// key. The sort is **stable** (equal keys keep their input order). InvalidArgument
// if the frame is empty (no device) with rows to sort, or `key_column` is absent;
// a 0/1-row frame round-trips unchanged. The key column may be any numeric dtype.
DataFrameResult sort(const DataFrame& frame, std::string_view key_column,
                     compute::SortOrder order = compute::SortOrder::Ascending);

// Row-wise concatenation (union / "bag" union — no dedup) of `frames`, in order:
// the result's rows is the sum, each input's rows appended after the previous.
// Every frame must share the same schema (column names + dtypes, same order). The
// result is built on the first frame's device with its chunk layout.
// InvalidArgument if `frames` is empty or any frame disagrees on schema.
DataFrameResult concat(const std::vector<const DataFrame*>& frames);

// The contiguous row range `[row_begin, row_end)` of `frame` (clamped to the
// frame's row count), as a new frame with the same schema. An empty range yields a
// 0-row frame. The natural way to cut a sorted frame into range partitions.
DataFrameResult slice(const DataFrame& frame, std::size_t row_begin,
                      std::size_t row_end);

}  // namespace zetta::ddf
