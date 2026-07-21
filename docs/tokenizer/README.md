# tokenizer — byte-level BPE (text ⇄ token ids)

`zetta::tokenizer` is the native **byte-level BPE tokenizer** (the GPT-2 / Qwen
family) that turns text into the token ids the `QwenModel` consumes and back again
(#555; the agent's text I/O). It is native C++26, no foreign runtime (ADR-17) — the
piece that lets a real open-weights checkpoint read and write natural language,
paired with the device-agnostic model in [`../autograd/`](../autograd/README.md).

## How byte-level BPE works

1. **Pre-tokenize** the raw text with the Qwen/GPT-2 regex into chunks (words,
   numbers, punctuation runs, whitespace) so merges never cross a word boundary.
2. **Byte-level map** each chunk's UTF-8 bytes through a reversible byte→code-point
   table, so every byte — including spaces and control bytes — becomes a printable
   character BPE can operate on and that round-trips exactly.
3. **Merge** adjacent symbols greedily by learned merge rank until no learned pair
   remains — the BPE algorithm.
4. **Look up** each resulting symbol in the vocab to get its id.

Decoding runs it backwards: ids → symbol strings → concatenate → code-points →
bytes → UTF-8 text.

## Pages

Built bottom-up (nibble-dev); shipped so far:

- **The byte foundation** — `byte_encoding.hpp` (below).

## The byte foundation (shipped)

- **`byte_encoding.hpp`** — `utf8_decode` / `utf8_encode` (a non-throwing UTF-8
  codec; malformed bytes → U+FFFD) and the GPT-2 reversible **`byte_to_unicode`**
  map (each of the 256 bytes → a printable code point; space → `Ġ`, the
  leading-space marker) with its inverse `unicode_to_byte`. Verified: UTF-8
  round-trips (ASCII + multibyte + emoji), the byte↔unicode map is a bijection, and
  the known GPT-2 anchors hold.
