#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/compute_status.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/ddf/data_frame.hpp"

// Internal host-staging substrate shared by the DDF ops (sort / concat / slice in
// data_frame_ops.cpp) and the distributed sort (distributed_sort.cpp). Not part of
// the public API — like src/core/catalog.hpp or src/core/base64.hpp.
//
// A DataFrame is move-only device memory chunked per column. The ops produce *new*
// frames (sorting reorders every row; concat appends; slice cuts a range), and the
// orchestration — building permutations, partitioning by splitters, shipping rows
// over the wire — is naturally host-side. So the ops stage a frame through a
// `HostFrame` (type-erased, row-major-by-column host bytes), do the data movement
// there, and bounce only the *compare sort* through the ComputeDevice (sort_by_key
// produces the permutation — the device does the actual O(n log n) compute, so a
// CUDA frame sorts on the GPU). Data movement (gather / concat / slice) is plain
// host memcpy: correctness over throughput, the same stance CpuDevice's own sort
// takes.
namespace zetta::ddf::internal {

// A host-resident, type-erased frame: one byte blob per column (element_count ==
// rows elements of the column's dtype), parallel to `names`/`dtypes`. Every column
// holds `rows` rows. This is the working representation the ops manipulate.
struct HostFrame {
    std::vector<std::string> names;
    std::vector<compute::DataType> dtypes;
    std::vector<std::vector<std::byte>> data;  // data[c] = column c's element bytes
    std::size_t rows = 0;

    std::size_t column_count() const noexcept { return names.size(); }

    // Index of column `name`, or std::nullopt.
    std::optional<std::size_t> column_index(std::string_view name) const;

    // The schema (names + dtypes) two frames must share to be row-compatible.
    bool same_schema(const HostFrame& other) const;
};

// Copy every column of `frame` off its device into a HostFrame (concatenating each
// column's chunks in row order). InvalidArgument if the frame has no device but
// non-zero columns; an empty (no-column) frame yields an empty HostFrame.
compute::ComputeStatus download_frame(const DataFrame& frame, HostFrame& out);

// Build a new DataFrame on `device` from `host`, chunked at `chunk_rows`. The
// inverse of download_frame. InvalidArgument on a device allocation/copy failure
// or chunk_rows == 0.
DataFrameResult upload_frame(compute::ComputeDevice& device, const HostFrame& host,
                             std::size_t chunk_rows);

// Reorder rows: out has the same schema, out_row[i] = host_row[permutation[i]].
// Every index must be < host.rows (callers build permutations that satisfy this).
HostFrame gather_rows(const HostFrame& host,
                      const std::vector<std::size_t>& permutation);

// The contiguous row range [begin, min(end, rows)) of `host` (begin clamped to
// rows), same schema.
HostFrame slice_rows(const HostFrame& host, std::size_t begin, std::size_t end);

// Row-wise concatenation of row-compatible frames (same names + dtypes, same
// order). nullopt if the list is empty or any frame disagrees on schema. The
// result's rows is the sum; column bytes are appended in list order.
std::optional<HostFrame> concat_host(const std::vector<const HostFrame*>& frames);

// Read column `name` as doubles (the monotonic projection used for sampling /
// splitter comparison — order-preserving for every supported dtype). nullopt if
// the column is absent.
std::optional<std::vector<double>> column_doubles(const HostFrame& host,
                                                  std::string_view name);

// Sort `host` by column `key_column` using `device` (the device computes the sort
// permutation via sort_by_key — the real compare work — and the rows are gathered
// through it on the host). Returns the sorted HostFrame. InvalidArgument if the key
// column is absent; a device error propagates. 0/1-row frames are returned as-is.
compute::ComputeStatus sort_host(compute::ComputeDevice& device,
                                 const HostFrame& host,
                                 std::string_view key_column,
                                 compute::SortOrder order, HostFrame& out);

}  // namespace zetta::ddf::internal
