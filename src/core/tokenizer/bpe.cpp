// SPDX-License-Identifier: MIT

#include "zetta/tokenizer/bpe.hpp"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "zetta/tokenizer/byte_encoding.hpp"
#include "zetta/tokenizer/pre_tokenizer.hpp"

namespace zetta::tokenizer {

namespace {
// The largest token id a vocabulary of `tokens` may use: its size plus generous room for added
// and reserved tokens, and never more than 2^24 (#1431).
std::size_t max_token_id(std::size_t tokens) {
    return std::min<std::size_t>(tokens + (std::size_t{1} << 20), std::size_t{1} << 24);
}
}  // namespace


std::size_t BpeTokenizer::resident_bytes() const {
    std::size_t bytes = sizeof(BpeTokenizer);
    for (const auto& [token, id] : vocab_) {
        (void)id;
        bytes += sizeof(std::pair<const std::string, int>) + token.capacity();
    }
    for (const auto& token : id_to_token_) {
        bytes += sizeof(std::string) + token.capacity();
    }
    bytes += merges_.size() *
             sizeof(std::pair<const std::uint64_t, Merge>);
    for (const auto& [token, id] : special_tokens_) {
        (void)id;
        bytes += sizeof(std::pair<const std::string, int>) + token.capacity();
    }
    return bytes;
}

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
    // The table is sized by the largest id, which the vocabulary file chooses (#1431):
    // real vocabularies number their tokens densely, so an id far past the vocabulary's size is
    // refused instead of allocating a table for it.
    if (static_cast<std::size_t>(max_id) >= max_token_id(tokenizer.vocab_.size())) {
        if (error != nullptr) {
            *error = "bpe: token id " + std::to_string(max_id) +
                     " is far beyond the vocabulary's " + std::to_string(tokenizer.vocab_.size()) +
                     " tokens";
        }
        return std::nullopt;
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
        // An added token's id is bounded the same way (#1431).
        if (content.empty() || id < 0 ||
            static_cast<std::size_t>(id) >= max_token_id(vocab_.size() + specials.size())) {
            continue;
        }
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

std::optional<BpeTokenizer> BpeTokenizer::from_tokenizer_json(
    const std::string& path, std::string* error) {
    const auto fail = [&](std::string reason) -> std::optional<BpeTokenizer> {
        if (error != nullptr) *error = std::move(reason);
        return std::nullopt;
    };

    std::ifstream in(path, std::ios::binary);
    if (!in) return fail("bpe: cannot open " + path);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const nlohmann::json root =
        nlohmann::json::parse(buffer.str(), nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object() || !root.contains("model") ||
        !root.at("model").is_object()) {
        return fail("bpe: " + path + " has no 'model' object");
    }
    const nlohmann::json& model = root.at("model");
    if (!model.contains("vocab") || !model.at("vocab").is_object() ||
        !model.contains("merges") || !model.at("merges").is_array()) {
        return fail("bpe: " + path + " model lacks vocab/merges");
    }

    std::unordered_map<std::string, int> vocab;
    for (auto entry = model.at("vocab").begin(); entry != model.at("vocab").end();
         ++entry) {
        if (entry.value().is_number_integer()) {
            vocab.emplace(entry.key(), entry.value().get<int>());
        }
    }

    std::vector<std::pair<std::string, std::string>> merges;
    for (const auto& merge : model.at("merges")) {
        if (merge.is_string()) {  // older format: "first second"
            const std::string joined = merge.get<std::string>();
            const std::size_t space = joined.find(' ');
            if (space != std::string::npos) {
                merges.emplace_back(joined.substr(0, space), joined.substr(space + 1));
            }
        } else if (merge.is_array() && merge.size() == 2 && merge[0].is_string() &&
                   merge[1].is_string()) {  // newer format: ["first", "second"]
            merges.emplace_back(merge[0].get<std::string>(),
                                merge[1].get<std::string>());
        }
    }

    std::optional<BpeTokenizer> tokenizer =
        from_vocab_and_merges(std::move(vocab), merges, error);
    if (!tokenizer.has_value()) return std::nullopt;

    // added_tokens (e.g. <|endoftext|>, <|im_start|>) → special tokens.
    if (root.contains("added_tokens") && root.at("added_tokens").is_array()) {
        std::vector<std::pair<std::string, int>> specials;
        for (const auto& token : root.at("added_tokens")) {
            if (token.is_object() && token.contains("content") &&
                token.at("content").is_string() && token.contains("id") &&
                token.at("id").is_number_integer()) {
                specials.emplace_back(token.at("content").get<std::string>(),
                                      token.at("id").get<int>());
            }
        }
        tokenizer->add_special_tokens(specials);
    }
    return tokenizer;
}

std::optional<BpeTokenizer> BpeTokenizer::from_directory(
    const std::string& directory, std::string* error) {
    const std::filesystem::path root(directory);
    if (std::filesystem::exists(root / "tokenizer.json")) {
        return from_tokenizer_json((root / "tokenizer.json").string(), error);
    }
    if (std::filesystem::exists(root / "vocab.json") &&
        std::filesystem::exists(root / "merges.txt")) {
        return from_gpt2_files((root / "vocab.json").string(),
                               (root / "merges.txt").string(), error);
    }
    if (error != nullptr) {
        *error = "bpe: no tokenizer.json or vocab.json+merges.txt in " + directory;
    }
    return std::nullopt;
}

}  // namespace zetta::tokenizer
