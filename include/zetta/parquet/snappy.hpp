#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Minimal, self-contained Snappy codec for Parquet data pages.
//
// Parquet's default page codec is SNAPPY, which stores each page as a Snappy
// *raw block* (the framing-less format from google/snappy's
// `format_description.txt` — NOT the streaming "framed" format with its stream
// identifier and CRC-32C checksums). This module reads and writes exactly that
// block form, so a compressed data page round-trips to the bytes the writer fed
// in.
//
// It is the first slice of the native Parquet reader (ADR-32 — no Arrow /
// parquet-cpp dependency); the footer parser (#505), column-chunk decoders
// (#506) and the reader → DDF wiring (#508) build on top of it. zstd is a
// deliberate follow-up (a separate codec, only if a target file needs it).
//
// The compressor (#833) is the write-side half: the native Parquet writer emits
// SNAPPY pages, so the bytes it produces must decode through `snappy_decompress`
// and through any conformant Snappy implementation. It is built in two layers,
// deliberately:
//
//   * `snappy_compress_literals` — a block of pure literal elements. Spec-valid
//     Snappy that never compresses, and simple enough to be read for
//     correctness. It is the *reference*: any round-trip failure in the real
//     encoder that this one also fails is a decoder or framing bug, not a
//     match-search bug (the same relationship `CpuDevice` has to `CudaDevice`).
//   * `snappy_compress` — the shipped encoder: the same literal framing plus
//     LZ77 back-references found through a hash table. Both satisfy the same
//     round-trip property against `snappy_decompress`.
//
// Decompression can fail on a truncated or malformed page, and zetta-core is
// compiled `-fexperimental-library` (nothing may unwind across its boundary), so
// every entry point is **non-throwing**: malformed input is rejected at the
// boundary with a clear reason rather than reading out of bounds or unwinding.
namespace zetta::parquet {

// The result of decompressing a Snappy raw block. On success `error` is empty
// and `data` holds the decompressed bytes; on failure `data` is empty and
// `error` explains why (so a caller can fail the page cleanly).
struct SnappyResult {
    std::string data;
    std::string error;

    bool ok() const { return error.empty(); }
};

// Decompress one Snappy raw block (a Parquet data-page body). Validates the
// preamble length, every tag, and every literal/back-reference against the
// input and output bounds; the produced output is required to be exactly the
// length the block's preamble declares (as a correct Snappy stream always is).
SnappyResult snappy_decompress(std::string_view compressed);

// Read just the declared uncompressed length from a Snappy raw block's leading
// varint, without decompressing — for cross-checking a page header's
// `uncompressed_page_size` before paying to decode. std::nullopt if the varint
// is truncated or overlong (more than 5 bytes for a 32-bit length).
std::optional<std::uint32_t> snappy_uncompressed_length(std::string_view compressed);

// Compress `input` into one Snappy raw block (the Parquet page-body form).
// `snappy_decompress(snappy_compress(input).data)` reproduces `input` exactly,
// for every input. The only failure is an input longer than the format's 32-bit
// length preamble can describe, which fails with a reason rather than
// truncating.
SnappyResult snappy_compress(std::string_view input);

// The literals-only reference encoder: a valid Snappy raw block that emits
// `input` as literal elements and never searches for a back-reference, so its
// output is always slightly *larger* than its input. It is the correctness
// baseline for `snappy_compress` — kept in the shipped build (not test-only) so
// the two can be differentially compared over the same inputs.
SnappyResult snappy_compress_literals(std::string_view input);

}  // namespace zetta::parquet
