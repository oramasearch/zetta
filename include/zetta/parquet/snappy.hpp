#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Minimal, self-contained Snappy decompressor for Parquet data pages.
//
// Parquet's default page codec is SNAPPY, which stores each page as a Snappy
// *raw block* (the framing-less format from google/snappy's
// `format_description.txt` — NOT the streaming "framed" format with its stream
// identifier and CRC-32C checksums). This decoder reverses exactly that block
// form, so a compressed data page round-trips to the bytes the writer fed in.
//
// It is the first slice of the native Parquet reader (ADR-32 — no Arrow /
// parquet-cpp dependency); the footer parser (#505), column-chunk decoders
// (#506) and the reader → DDF wiring (#508) build on top of it. zstd is a
// deliberate follow-up (a separate codec, only if a target file needs it).
//
// Decompression can fail on a truncated or malformed page, and zetta-core is
// compiled `-fexperimental-library` (no exceptions across the seam), so every
// entry point is **non-throwing**: malformed input is rejected at the boundary
// with a clear reason rather than reading out of bounds or unwinding.
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

}  // namespace zetta::parquet
