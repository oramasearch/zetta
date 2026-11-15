// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

// The byte-level foundation of zetta's BPE tokenizer (#555; docs/tokenizer/).
// Byte-level BPE (GPT-2 / Qwen family) works not on raw bytes but on a reversible
// mapping of each byte to a *printable* Unicode code point, so whitespace and
// control bytes become visible characters the merge algorithm can operate on and
// that round-trip exactly. These are the UTF-8 codec + that byte<->code-point map;
// no foreign runtime.
namespace zetta::tokenizer {

// Decode a UTF-8 string into Unicode code points. Malformed bytes decode to the
// replacement character U+FFFD — never throws (a non-throwing boundary).
std::vector<char32_t> utf8_decode(std::string_view text);

// Append one code point's UTF-8 encoding to `out`.
void utf8_encode(char32_t code_point, std::string& out);

// The GPT-2 / Qwen reversible byte<->unicode map: each of the 256 byte values maps
// to a printable Unicode code point (bytes that are already printable map to
// themselves; the rest — control/space bytes — map to code points at U+0100 and
// up). byte_to_unicode()[b] is the code point standing in for byte b. Space (0x20)
// maps to U+0120 'Ġ', the marker BPE vocabularies use for a leading space.
const std::array<char32_t, 256>& byte_to_unicode();

// The inverse map: the byte a code point stands for, or -1 if the code point is
// not one of the 256 in the map.
int unicode_to_byte(char32_t code_point);

}  // namespace zetta::tokenizer
