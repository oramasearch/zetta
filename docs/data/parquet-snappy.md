# Parquet Snappy page codec

Parquet's default page codec is **SNAPPY**: each data page is stored as a Snappy
*raw block*, so the reader must reverse it to get the page's bytes back and the
writer must produce it. `zetta::parquet` carries a small, self-contained native
codec for exactly that block form — `snappy_decompress` came first with the
native Parquet reader ([ADR-32](https://github.com/oramasearch/zetta/issues/481):
no Arrow / parquet-cpp dependency; issue #507), and `snappy_compress` is the
write side the native Parquet writer emits pages with (#833).

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
| `snappy_compress(input)` | `SnappyResult` | Compress `input` into one Snappy raw block. `snappy_decompress(snappy_compress(x).data) == x` for every `x`; the only failure is an input longer than the 32-bit length preamble can describe. |
| `snappy_compress_literals(input)` | `SnappyResult` | The literals-only **reference** encoder — see [Two encoders](#two-encoders-a-reference-and-a-compressor). |

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

## Two encoders: a reference and a compressor

The compressor ships as **two layers**, and both stay in the build:

| Function | What it emits | Why it exists |
|---|---|---|
| `snappy_compress_literals` | The preamble plus **one literal element** per input — no back-references at all. Output is always slightly *larger* than the input. | The **correctness baseline**. A block of pure literals is spec-valid Snappy, and the encoder is short enough to read for correctness by eye. |
| `snappy_compress` | The same literal framing **plus** LZ77 back-references found through a hash table. | The shipped encoder — what actually makes a partition body smaller than the rows it holds. |

Both are checked by the **same** round-trip property over the same inputs, which
is the point of keeping the reference: a failure both encoders show is a framing
or decoder bug, while a failure only the compressor shows is in the match search.
This is the relationship `CpuDevice` has to `CudaDevice` — a slow, obviously
correct implementation kept as the numerical reference for a fast one.

> Status: the literals-only layer is in; `snappy_compress` currently delegates to
> it, and the hash-match search lands behind the same function and the same
> round-trip tests.

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

The **encoder** is tested by the round-trip property, run over both encoders so
they are differentially compared:

- **Every literal-length boundary** the tag encoding switches on — sizes
  0/1/59/60/61/255/256/257/65535/65536/65537 and more — across five input shapes
  (zeros, one repeated byte, deterministic pseudo-random noise, a repetitive
  phrase with an embedded NUL, and every byte value cycled).
- **A property-style sweep** — 400 generated inputs of varying length and
  varying alphabet size, so matches appear at every density from "incompressible"
  to "one byte repeated".
- The declared preamble length is asserted to equal the input length in every
  case, so a page header's `uncompressed_page_size` can be trusted.

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
