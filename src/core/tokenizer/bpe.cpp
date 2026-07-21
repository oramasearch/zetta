#include "zetta/tokenizer/bpe.hpp"

#include <climits>
#include <cstddef>
#include <utility>

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

std::vector<int> BpeTokenizer::encode(std::string_view text) const {
    std::vector<int> result;
    for (const std::string& piece : pre_tokenize(text)) {
        std::vector<int> symbols;
        symbols.reserve(piece.size());
        for (const char byte : piece) {
            const int id = byte_to_id_[static_cast<unsigned char>(byte)];
            if (id >= 0) symbols.push_back(id);  // base vocab covers all 256 bytes
        }
        apply_merges(symbols);
        result.insert(result.end(), symbols.begin(), symbols.end());
    }
    return result;
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

}  // namespace zetta::tokenizer
