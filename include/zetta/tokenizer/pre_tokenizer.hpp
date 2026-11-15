// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>
#include <vector>

// The pre-tokenizer of zetta's BPE tokenizer (#555; docs/tokenizer/). Before BPE
// runs, text is split into pieces so a merge never crosses a word/number/
// punctuation boundary. This reproduces the Qwen2/Qwen3 (GPT-2 family) pattern
//   (?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}
//     | ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+
// as a hand-written code-point scanner — std::regex cannot express \p{L}/\p{N} or
// the trailing-whitespace lookahead. Contractions split off, letters clump (with an
// optional leading space/punctuation), each digit is its own piece, punctuation
// runs clump (optional leading space), and whitespace is handled so a single
// leading space attaches to the following word.
namespace zetta::tokenizer {

// Split raw UTF-8 text into pre-tokens; each is a byte-exact substring of the
// input, and concatenating them reproduces the input. Unicode letter/number
// classification is exact for ASCII and covers the common scripts (Latin, Greek,
// Cyrillic, CJK, kana, Hangul, common digits); full Unicode-category parity for
// rare scripts is a refinement (a complete UCD table) — see docs/tokenizer/.
std::vector<std::string> pre_tokenize(std::string_view text);

}  // namespace zetta::tokenizer
