// SPDX-License-Identifier: MIT

#include "zetta/tokenizer/pre_tokenizer.hpp"

#include <cstddef>

namespace zetta::tokenizer {

namespace {

bool is_newline(char32_t c) { return c == U'\n' || c == U'\r'; }

bool is_whitespace(char32_t c) {
    if (c == U' ' || c == U'\t' || c == U'\n' || c == U'\r' || c == 0x0B ||
        c == 0x0C) {
        return true;
    }
    if (c >= 0x2000 && c <= 0x200A) return true;  // en/em spaces
    switch (c) {
        case 0x00A0:  // no-break space
        case 0x1680:
        case 0x2028:
        case 0x2029:
        case 0x202F:
        case 0x205F:
        case 0x3000:  // ideographic space
        case 0xFEFF:
            return true;
        default:
            return false;
    }
}

// \p{L} — exact for ASCII, plus the common scripts (see the header note).
bool is_letter(char32_t c) {
    if ((c >= U'A' && c <= U'Z') || (c >= U'a' && c <= U'z')) return true;
    if (c < 0x80) return false;
    if (c == 0xAA || c == 0xB5 || c == 0xBA) return true;         // ª µ º
    if (c >= 0xC0 && c <= 0xFF && c != 0xD7 && c != 0xF7) return true;  // Latin-1
    if (c >= 0x100 && c <= 0x2AF) return true;                    // Latin ext + IPA
    if (c >= 0x370 && c <= 0x3FF && c != 0x37E && c != 0x384 && c != 0x385 &&
        c != 0x387) {
        return true;  // Greek
    }
    if (c >= 0x400 && c <= 0x4FF) return true;   // Cyrillic
    if (c >= 0x531 && c <= 0x58F) return true;   // Armenian
    if (c >= 0x5D0 && c <= 0x5EA) return true;   // Hebrew
    if (c >= 0x620 && c <= 0x64A) return true;   // Arabic
    if (c >= 0x3040 && c <= 0x30FF) return true; // Hiragana + Katakana
    if (c >= 0x3400 && c <= 0x4DBF) return true; // CJK ext A
    if (c >= 0x4E00 && c <= 0x9FFF) return true; // CJK unified
    if (c >= 0xAC00 && c <= 0xD7A3) return true; // Hangul syllables
    return false;
}

// \p{N} — ASCII digits plus common decimal-digit blocks.
bool is_number(char32_t c) {
    if (c >= U'0' && c <= U'9') return true;
    if (c >= 0x0660 && c <= 0x0669) return true;  // Arabic-Indic
    if (c >= 0x06F0 && c <= 0x06F9) return true;  // Extended Arabic-Indic
    if (c >= 0xFF10 && c <= 0xFF19) return true;  // Fullwidth
    return false;
}

char32_t to_lower_ascii(char32_t c) {
    return (c >= U'A' && c <= U'Z') ? c + 32 : c;
}

using CodePoints = std::vector<char32_t>;

// One matcher per regex alternative; each returns the number of code points it
// consumes at `pos`, or 0 if it does not apply. Tried in the pattern's order.

int match_contraction(const CodePoints& cp, std::size_t pos) {
    const std::size_t n = cp.size();
    if (pos >= n || cp[pos] != U'\'') return 0;
    if (pos + 2 < n) {
        const char32_t a = to_lower_ascii(cp[pos + 1]);
        const char32_t b = to_lower_ascii(cp[pos + 2]);
        if ((a == U'r' && b == U'e') || (a == U'v' && b == U'e') ||
            (a == U'l' && b == U'l')) {
            return 3;  // 're | 've | 'll
        }
    }
    if (pos + 1 < n) {
        const char32_t a = to_lower_ascii(cp[pos + 1]);
        if (a == U's' || a == U't' || a == U'm' || a == U'd') return 2;  // 's't'm'd
    }
    return 0;
}

// [^\r\n\p{L}\p{N}]?\p{L}+  — an optional single non-(newline/letter/number) lead,
// then one or more letters.
int match_word(const CodePoints& cp, std::size_t pos) {
    const std::size_t n = cp.size();
    std::size_t after_lead = pos;
    if (pos < n && !is_newline(cp[pos]) && !is_letter(cp[pos]) &&
        !is_number(cp[pos])) {
        after_lead = pos + 1;  // consume the optional lead
    }
    std::size_t j = after_lead;
    while (j < n && is_letter(cp[j])) ++j;
    if (j > after_lead) return static_cast<int>(j - pos);  // lead + letters
    if (after_lead != pos) {                               // lead but no letters:
        std::size_t k = pos;                               // retry without the lead
        while (k < n && is_letter(cp[k])) ++k;
        if (k > pos) return static_cast<int>(k - pos);
    }
    return 0;
}

int match_digit(const CodePoints& cp, std::size_t pos) {
    return (pos < cp.size() && is_number(cp[pos])) ? 1 : 0;  // \p{N}, one at a time
}

//  ?[^\s\p{L}\p{N}]+[\r\n]*  — optional single space, a punctuation/symbol run,
// then any trailing newlines.
int match_punct(const CodePoints& cp, std::size_t pos) {
    const std::size_t n = cp.size();
    std::size_t i = pos;
    if (i < n && cp[i] == U' ') ++i;  // optional literal space
    std::size_t j = i;
    while (j < n && !is_whitespace(cp[j]) && !is_letter(cp[j]) && !is_number(cp[j])) {
        ++j;
    }
    if (j == i) return 0;  // no punctuation followed the optional space
    while (j < n && is_newline(cp[j])) ++j;
    return static_cast<int>(j - pos);
}

// \s*[\r\n]+  — a whitespace run that ends at its last newline.
int match_ws_newline(const CodePoints& cp, std::size_t pos) {
    const std::size_t n = cp.size();
    std::size_t j = pos;
    while (j < n && is_whitespace(cp[j])) ++j;
    long last_newline = -1;
    for (std::size_t k = pos; k < j; ++k) {
        if (is_newline(cp[k])) last_newline = static_cast<long>(k);
    }
    if (last_newline < 0) return 0;
    return static_cast<int>(last_newline + 1 - static_cast<long>(pos));
}

// \s+(?!\S)  — trailing whitespace: the whole run if it ends the string, else all
// but the last space (which the following word claims).
int match_trailing_ws(const CodePoints& cp, std::size_t pos) {
    const std::size_t n = cp.size();
    std::size_t j = pos;
    while (j < n && is_whitespace(cp[j])) ++j;
    const std::size_t length = j - pos;
    if (length == 0) return 0;
    if (j == n) return static_cast<int>(length);          // run at end of input
    return length >= 1 ? static_cast<int>(length - 1) : 0;  // leave one for the word
}

// \s+  — the fallback whitespace run.
int match_ws(const CodePoints& cp, std::size_t pos) {
    const std::size_t n = cp.size();
    std::size_t j = pos;
    while (j < n && is_whitespace(cp[j])) ++j;
    return static_cast<int>(j - pos);
}

// Decode UTF-8, recording the byte offset where each code point began (plus a final
// entry = text.size()) so a matched code-point range slices the original bytes.
void decode_with_offsets(std::string_view text, CodePoints& code_points,
                         std::vector<std::size_t>& byte_offsets) {
    const std::size_t n = text.size();
    std::size_t i = 0;
    while (i < n) {
        const auto lead = static_cast<unsigned char>(text[i]);
        int extra = 0;
        char32_t code_point = 0;
        if (lead < 0x80) {
            code_point = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            code_point = lead & 0x1F;
            extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            code_point = lead & 0x0F;
            extra = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            code_point = lead & 0x07;
            extra = 3;
        } else {
            code_points.push_back(0xFFFD);
            byte_offsets.push_back(i);
            ++i;
            continue;
        }
        if (i + static_cast<std::size_t>(extra) >= n) {
            code_points.push_back(0xFFFD);
            byte_offsets.push_back(i);
            break;
        }
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            const auto cont = static_cast<unsigned char>(text[i + k]);
            if ((cont & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            code_point = (code_point << 6) | (cont & 0x3F);
        }
        if (!ok) {
            code_points.push_back(0xFFFD);
            byte_offsets.push_back(i);
            ++i;
            continue;
        }
        code_points.push_back(code_point);
        byte_offsets.push_back(i);
        i += static_cast<std::size_t>(extra) + 1;
    }
    byte_offsets.push_back(n);  // sentinel end offset
}

}  // namespace

std::vector<std::string> pre_tokenize(std::string_view text) {
    CodePoints code_points;
    std::vector<std::size_t> byte_offsets;
    decode_with_offsets(text, code_points, byte_offsets);

    std::vector<std::string> pieces;
    std::size_t pos = 0;
    const std::size_t n = code_points.size();
    while (pos < n) {
        int matched = match_contraction(code_points, pos);
        if (matched == 0) matched = match_word(code_points, pos);
        if (matched == 0) matched = match_digit(code_points, pos);
        if (matched == 0) matched = match_punct(code_points, pos);
        if (matched == 0) matched = match_ws_newline(code_points, pos);
        if (matched == 0) matched = match_trailing_ws(code_points, pos);
        if (matched == 0) matched = match_ws(code_points, pos);
        if (matched == 0) matched = 1;  // defensive: never stall

        const std::size_t start = byte_offsets[pos];
        const std::size_t end = byte_offsets[pos + static_cast<std::size_t>(matched)];
        pieces.emplace_back(text.substr(start, end - start));
        pos += static_cast<std::size_t>(matched);
    }
    return pieces;
}

}  // namespace zetta::tokenizer
