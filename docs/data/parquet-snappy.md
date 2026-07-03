# Parquet Snappy page decompression

Parquet's default page codec is **SNAPPY**: each data page is stored as a Snappy
*raw block*, and the reader must reverse it to get the page's bytes back.
`zetta::parquet::snappy_decompress` is a small, self-contained native decoder for
exactly that block form — the first slice of the native Parquet reader
([ADR-32](https://github.com/oramasearch/zetta/issues/481): no Arrow /
parquet-cpp dependency; issue #507).

It decodes the **raw block** format (google/snappy's
[`format_description.txt`](https://github.com/google/snappy/blob/main/format_description.txt)),
**not** the streaming "framed" format — there is no stream identifier and no
CRC-32C, because Parquet frames its own pages and carries its own page CRCs.
zstd (the other common page codec) is a deliberate follow-up: a separate codec,
added only if a target file needs it.

## The API

`include/zetta/parquet/snappy.hpp` (namespace `zetta::parquet`):

| Function | Returns | Behavior |
|---|---|---|
| `snappy_decompress(compressed)` | `SnappyResult` | Decompress one Snappy raw block (a data-page body). On success `result.ok()` is true and `result.data` holds the bytes; on malformed input `result.error` explains why. |
| `snappy_uncompressed_length(compressed)` | `std::optional<std::uint32_t>` | Read just the block's declared uncompressed length from its leading varint, **without** decompressing — for cross-checking a page header's `uncompressed_page_size` before paying to decode. `std::nullopt` on a truncated / overlong varint. |

```cpp
#include "zetta/parquet/snappy.hpp"

const auto result = zetta::parquet::snappy_decompress(page_body);
if (!result.ok()) {
    // reject the page with result.error — never read a half-decoded buffer
}
const std::string& page_bytes = result.data;
```

Decompression can fail on a truncated or malformed page, and zetta-core is
compiled `-fexperimental-library` (no exceptions across the seam), so the decoder
is **non-throwing**: it returns a result + reason rather than throwing or reading
out of bounds. The compressed bytes arrive from disk or a peer, so the decoder
treats them as a **trust boundary** — every read (the preamble varint, each tag,
every literal length and back-reference) is bounds-checked, and the produced
output must be exactly the length the block's preamble declares (as a correct
Snappy stream always is). This is the binary-format analog of the JSON
schema-at-the-boundary rule.

## The block format

A Snappy raw block is a little-endian varint **preamble** (the uncompressed
length) followed by a sequence of **elements**. Each element starts with a tag
byte whose low 2 bits select the type:

| Low 2 bits | Element | Length | Offset |
|---|---|---|---|
| `00` | literal | `tag>>2`; if that is 60..63, the real length−1 is the next 1..4 LE bytes | — (raw bytes follow) |
| `01` | copy, 1-byte offset | `((tag>>2)&7)+4` → 4..11 | 11-bit: `((tag>>5)&7)<<8 \| next byte` |
| `10` | copy, 2-byte offset | `(tag>>2)+1` → 1..64 | next 2 LE bytes |
| `11` | copy, 4-byte offset | `(tag>>2)+1` → 1..64 | next 4 LE bytes |

A **copy** is an LZ77 back-reference into the already-produced output, `offset`
bytes before the current position. It may **overlap** the region being written
(`offset < length` → run-length expansion), so the decoder copies one byte at a
time, the source index trailing the write index.

## Testing — byte-identical decode

`tests/data/test_parquet_snappy.cpp` proves the acceptance ("snappy-compressed pages
decode byte-identical to an uncompressed golden") without a foreign Snappy
*compressor* in the build (ADR-17 forbids a foreign runtime), so the ground truth
is the published block format itself, made executable:

- **Canonical spec vectors** — compressed byte sequences hand-built per
  `format_description.txt` (each element type, the literal length-extension
  boundaries at 60/61/300 bytes, a non-overlapping copy, an overlapping RLE copy,
  the 2- and 4-byte offset forms) next to the exact bytes they must decode to.
  Passing them shows the decoder matches the on-disk format, not merely that it
  round-trips with some encoder of our own.
- **A page-shaped golden** — a 16-byte dictionary pattern repeated 64× (1024
  bytes), the way a Parquet snappy page packs repetitive column data, built as one
  literal plus a chain of real back-reference copies and decoded back to the
  byte-identical uncompressed golden.
- **Malformed inputs** — a truncated / overlong preamble, a literal that runs
  past the input, a copy whose offset points before the output start, a zero
  offset, and a stream shorter or longer than its declared length — all rejected
  with a reason, never a crash.

## How it fits

This is the page-codec leaf of the native Parquet reader. The
[footer/metadata parser (#505)](https://github.com/oramasearch/zetta/issues/505)
locates each column chunk's pages; the
[column-chunk decoders (#506)](https://github.com/oramasearch/zetta/issues/506)
call `snappy_decompress` on each compressed page before decoding PLAIN /
dictionary / RLE values; and the
[reader → DDF wiring (#508)](https://github.com/oramasearch/zetta/issues/508)
turns those into typed [DDF](../ddf/ddf.md) columns. zstd page decompression is the
sibling codec to add behind the same per-page seam when a target file needs it.
