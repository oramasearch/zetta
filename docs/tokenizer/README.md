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
- **The pre-tokenizer** — `pre_tokenizer.hpp` (below).
- **The BPE tokenizer** — `bpe.hpp` (below).

## The byte foundation (shipped)

- **`byte_encoding.hpp`** — `utf8_decode` / `utf8_encode` (a non-throwing UTF-8
  codec; malformed bytes → U+FFFD) and the GPT-2 reversible **`byte_to_unicode`**
  map (each of the 256 bytes → a printable code point; space → `Ġ`, the
  leading-space marker) with its inverse `unicode_to_byte`. Verified: UTF-8
  round-trips (ASCII + multibyte + emoji), the byte↔unicode map is a bijection, and
  the known GPT-2 anchors hold.

## The pre-tokenizer (shipped)

- **`pre_tokenizer.hpp`** — `pre_tokenize(text)` reproduces the **Qwen2/Qwen3**
  pre-tokenization pattern as a hand-written code-point scanner (std::regex cannot
  express `\p{L}`/`\p{N}` or the trailing-whitespace lookahead). Each of the seven
  regex alternatives is a matcher, tried in order (Perl leftmost-first): apostrophe
  contractions, `[^\r\n\p{L}\p{N}]?\p{L}+` (a word with an optional single
  non-alphanumeric lead — Qwen attaches a leading `.`/`(`, not just a space), single
  digits, punctuation runs (optional leading space + trailing newlines), and three
  whitespace rules that leave a single space to attach to the next word. Unicode
  letter/number classification is exact for ASCII and covers the common scripts;
  full-UCD parity for rare scripts is a documented refinement. Verified on ASCII
  where the classification is exact — words, contractions (case-insensitive),
  digits, punctuation, whitespace runs, code, and that the pieces always
  concatenate back to the input byte-for-byte.

## The BPE tokenizer (shipped)

- **`bpe.hpp`** — `BpeTokenizer`: `from_vocab_and_merges` builds it from a
  byte-level vocab (token → id) and an ordered merge list; `encode` runs the full
  pipeline (pre-tokenize → base symbol ids → BPE merges → ids) and `decode` runs it
  backwards. Because the base vocab covers all 256 byte characters, **any** text
  round-trips exactly. **Fast by construction:** the merge loop works entirely on
  integer symbol ids, with each candidate pair looked up as a single packed-`uint64`
  hash key — no string concatenation/allocation in the hot path (the naive
  "join the token strings and hash that" allocates on every pair of every step).
  Verified: arbitrary UTF-8 round-trips with no merges (the byte-level guarantee);
  merges build a word into one token in rank order; a leading space merges into a
  `Ġword` token; the lowest-rank pair wins first; accessors + empty-vocab rejection.
