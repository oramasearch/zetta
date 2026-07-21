#include "zetta/tokenizer/bpe.hpp"

#include <climits>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "zetta/tokenizer/byte_encoding.hpp"
#include "zetta/tokenizer/pre_tokenizer.hpp"

namespace zetta::tokenizer {

std::optional<BpeTokenizer> BpeTokenizer::from_vocab_and_merges(
    std::unordered_map<std::string, int> vocab,
    const std::vector<std::pair<std::string, std::string>>& merges,
    std::string* error) {
    if (vocab.empty()) {
        if (error != nullptr) *error = "bpe: empty vocab";
        return std::nullopt;
    }

    BpeTokenizer tokenizer;
    tokenizer.vocab_ = std::move(vocab);

    // Inverse table id → token.
    int max_id = 0;
    for (const auto& [token, id] : tokenizer.vocab_) {
        if (id > max_id) max_id = id;
    }
    tokenizer.id_to_token_.assign(static_cast<std::size_t>(max_id) + 1, std::string{});
    for (const auto& [token, id] : tokenizer.vocab_) {
        if (id >= 0) tokenizer.id_to_token_[static_cast<std::size_t>(id)] = token;
    }

    // Base symbol id for each of the 256 byte characters.
    for (int byte = 0; byte < 256; ++byte) {
        std::string symbol;
        utf8_encode(byte_to_unicode()[static_cast<std::size_t>(byte)], symbol);
        const auto found = tokenizer.vocab_.find(symbol);
        tokenizer.byte_to_id_[static_cast<std::size_t>(byte)] =
            found == tokenizer.vocab_.end() ? -1 : found->second;
    }

    // Merge table: (id_first, id_second) → {rank, id_of(first+second)}. Skip a
    // merge whose tokens or result aren't in the vocab (a malformed entry).
    int rank = 0;
    for (const auto& [first, second] : merges) {
        const auto first_id = tokenizer.vocab_.find(first);
        const auto second_id = tokenizer.vocab_.find(second);
        const auto merged_id = tokenizer.vocab_.find(first + second);
        if (first_id != tokenizer.vocab_.end() &&
            second_id != tokenizer.vocab_.end() &&
            merged_id != tokenizer.vocab_.end()) {
            tokenizer.merges_.emplace(
                pair_key(first_id->second, second_id->second),
                Merge{rank, merged_id->second});
        }
        ++rank;
    }

    return tokenizer;
}

void BpeTokenizer::apply_merges(std::vector<int>& symbols) const {
    while (symbols.size() >= 2) {
        // Lowest-rank adjacent mergeable pair.
        int best_rank = INT_MAX;
        std::size_t best_index = symbols.size();
        int best_merged = -1;
        for (std::size_t i = 0; i + 1 < symbols.size(); ++i) {
            const auto found = merges_.find(pair_key(symbols[i], symbols[i + 1]));
            if (found != merges_.end() && found->second.rank < best_rank) {
                best_rank = found->second.rank;
                best_index = i;
                best_merged = found->second.merged_id;
            }
        }
        if (best_index == symbols.size()) break;  // nothing left to merge

        // Merge every occurrence of that exact pair in one pass.
        const int first = symbols[best_index];
        const int second = symbols[best_index + 1];
        std::vector<int> merged;
        merged.reserve(symbols.size());
        std::size_t i = 0;
        while (i < symbols.size()) {
            if (i + 1 < symbols.size() && symbols[i] == first &&
                symbols[i + 1] == second) {
                merged.push_back(best_merged);
                i += 2;
            } else {
                merged.push_back(symbols[i]);
                ++i;
            }
        }
        symbols.swap(merged);
    }
}

void BpeTokenizer::encode_ordinary(std::string_view text,
                                   std::vector<int>& out) const {
    for (const std::string& piece : pre_tokenize(text)) {
        std::vector<int> symbols;
        symbols.reserve(piece.size());
        for (const char byte : piece) {
            const int id = byte_to_id_[static_cast<unsigned char>(byte)];
            if (id >= 0) symbols.push_back(id);  // base vocab covers all 256 bytes
        }
        apply_merges(symbols);
        out.insert(out.end(), symbols.begin(), symbols.end());
    }
}

std::vector<int> BpeTokenizer::encode(std::string_view text) const {
    std::vector<int> result;
    if (special_tokens_.empty()) {
        encode_ordinary(text, result);
        return result;
    }
    // Split on special tokens (longest match at each position wins), BPE-encoding
    // the ordinary spans between them.
    std::size_t segment_start = 0;
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        std::size_t best_len = 0;
        int best_id = -1;
        for (const auto& [content, id] : special_tokens_) {
            const std::size_t length = content.size();
            if (length > best_len && i + length <= n &&
                text.compare(i, length, content) == 0) {
                best_len = length;
                best_id = id;
            }
        }
        if (best_len > 0) {
            encode_ordinary(text.substr(segment_start, i - segment_start), result);
            result.push_back(best_id);
            i += best_len;
            segment_start = i;
        } else {
            ++i;
        }
    }
    encode_ordinary(text.substr(segment_start), result);
    return result;
}

void BpeTokenizer::add_special_tokens(
    const std::vector<std::pair<std::string, int>>& specials) {
    for (const auto& [content, id] : specials) {
        if (content.empty() || id < 0) continue;
        special_tokens_[content] = id;
        // Store the byte-level form in the vocab / inverse so decode's uniform
        // byte path reproduces the content and token_to_id/vocab_size include it.
        std::string byte_level;
        for (const char byte : content) {
            utf8_encode(byte_to_unicode()[static_cast<unsigned char>(byte)],
                        byte_level);
        }
        vocab_[byte_level] = id;
        if (static_cast<std::size_t>(id) >= id_to_token_.size()) {
            id_to_token_.resize(static_cast<std::size_t>(id) + 1);
        }
        id_to_token_[static_cast<std::size_t>(id)] = byte_level;
    }
}

std::string BpeTokenizer::decode(const std::vector<int>& ids) const {
    std::string byte_level;
    for (const int id : ids) {
        if (id >= 0 && static_cast<std::size_t>(id) < id_to_token_.size()) {
            byte_level += id_to_token_[static_cast<std::size_t>(id)];
        }
    }
    std::string out;
    for (const char32_t code_point : utf8_decode(byte_level)) {
        const int byte = unicode_to_byte(code_point);
        if (byte >= 0) out.push_back(static_cast<char>(byte));
    }
    return out;
}

int BpeTokenizer::token_to_id(const std::string& token) const {
    const auto found = vocab_.find(token);
    return found == vocab_.end() ? -1 : found->second;
}

std::string BpeTokenizer::id_to_token(int id) const {
    if (id < 0 || static_cast<std::size_t>(id) >= id_to_token_.size()) {
        return std::string{};
    }
    return id_to_token_[static_cast<std::size_t>(id)];
}

std::optional<BpeTokenizer> BpeTokenizer::from_gpt2_files(
    const std::string& vocab_json_path, const std::string& merges_txt_path,
    std::string* error) {
    const auto fail = [&](std::string reason) -> std::optional<BpeTokenizer> {
        if (error != nullptr) *error = std::move(reason);
        return std::nullopt;
    };

    std::ifstream vocab_in(vocab_json_path, std::ios::binary);
    if (!vocab_in) return fail("bpe: cannot open " + vocab_json_path);
    std::ostringstream vocab_buffer;
    vocab_buffer << vocab_in.rdbuf();
    const nlohmann::json vocab_json =
        nlohmann::json::parse(vocab_buffer.str(), nullptr, /*allow_exceptions=*/false);
    if (vocab_json.is_discarded() || !vocab_json.is_object()) {
        return fail("bpe: " + vocab_json_path + " is not a JSON object");
    }
    std::unordered_map<std::string, int> vocab;
    for (auto entry = vocab_json.begin(); entry != vocab_json.end(); ++entry) {
        if (!entry.value().is_number_integer()) {
            return fail("bpe: vocab entry '" + entry.key() + "' is not an integer id");
        }
        vocab.emplace(entry.key(), entry.value().get<int>());
    }

    std::ifstream merges_in(merges_txt_path, std::ios::binary);
    if (!merges_in) return fail("bpe: cannot open " + merges_txt_path);
    std::vector<std::pair<std::string, std::string>> merges;
    std::string line;
    while (std::getline(merges_in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF
        if (line.empty() || line[0] == '#') continue;               // blank / header
        const std::size_t space = line.find(' ');
        if (space == std::string::npos) continue;
        merges.emplace_back(line.substr(0, space), line.substr(space + 1));
    }

    return from_vocab_and_merges(std::move(vocab), merges, error);
}

}  // namespace zetta::tokenizer
