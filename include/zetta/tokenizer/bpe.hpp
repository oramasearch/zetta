#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// The byte-level BPE tokenizer (#555; docs/tokenizer/): text ⇄ token ids for the
// QwenModel. It composes the byte-level map ([byte_encoding.hpp]) and the
// pre-tokenizer ([pre_tokenizer.hpp]) with the BPE merge algorithm and a vocab —
// the GPT-2 / Qwen pipeline, native C++26, no foreign runtime and no regex engine.
// Encoding: pre-tokenize → map each piece's bytes to base symbol ids → merge
// adjacent symbols by learned rank until none remain → the surviving ids ARE the
// tokens. Decoding runs it backwards, and because the base vocab covers all 256
// byte characters, any text round-trips exactly.
//
// Speed: the merge loop works entirely on integer symbol ids, with each candidate
// pair looked up as a single packed-`uint64` hash key — no string allocation or
// concatenation in the hot path (the naive "join the two token strings and hash
// that" approach allocates on every pair of every merge step).
namespace zetta::tokenizer {

class BpeTokenizer {
public:
    // Build from a vocab (byte-level token string → id) and an ordered merge list
    // (each entry the two token strings that merge, in priority order — merges.txt
    // order). Returns std::nullopt (with a reason) if the vocab is empty. A
    // non-throwing boundary.
    static std::optional<BpeTokenizer> from_vocab_and_merges(
        std::unordered_map<std::string, int> vocab,
        const std::vector<std::pair<std::string, std::string>>& merges,
        std::string* error = nullptr);

    // Load from GPT-2 / Qwen-style files: a vocab.json ({token: id}) and a
    // merges.txt (an optional "#" comment header, then "first second" lines).
    // std::nullopt (with a reason) if a file is missing or malformed.
    static std::optional<BpeTokenizer> from_gpt2_files(
        const std::string& vocab_json_path, const std::string& merges_txt_path,
        std::string* error = nullptr);

    // Load from a Hugging Face tokenizer.json (what Qwen ships as canonical): its
    // model.vocab, model.merges (space-joined strings or [first, second] pairs),
    // and added_tokens (registered as special tokens). std::nullopt + a reason on a
    // missing/malformed file.
    static std::optional<BpeTokenizer> from_tokenizer_json(
        const std::string& path, std::string* error = nullptr);

    // Load a tokenizer from a checkpoint directory: prefer <dir>/tokenizer.json,
    // else <dir>/vocab.json + <dir>/merges.txt. THE entry point paired with the
    // model's load_qwen_model. std::nullopt + a reason if neither is present/valid.
    static std::optional<BpeTokenizer> from_directory(const std::string& directory,
                                                      std::string* error = nullptr);

    // Register special tokens (e.g. <|endoftext|>, <|im_start|>). Each is matched
    // as one atomic unit during encode — before pre-tokenization, longest match
    // wins — and emitted as its id; decode reproduces its content. Also added to
    // the vocab, so vocab_size and id lookups include it.
    void add_special_tokens(
        const std::vector<std::pair<std::string, int>>& specials);

    // Encode text to token ids (special tokens split off first, then each ordinary
    // span: pre-tokenize → base symbols → BPE merges → ids).
    std::vector<int> encode(std::string_view text) const;

    // Decode token ids back to text (ids → byte-level string → bytes → UTF-8).
    // Unknown ids contribute nothing.
    std::string decode(const std::vector<int>& ids) const;

    int vocab_size() const { return static_cast<int>(vocab_.size()); }
    std::size_t resident_bytes() const;

    // A byte-level token string → its id, or -1 if absent.
    int token_to_id(const std::string& token) const;
    // An id → its byte-level token string, or "" if out of range.
    std::string id_to_token(int id) const;

private:
    BpeTokenizer() = default;

    // One learned merge: its priority rank and the id of the merged token.
    struct Merge {
        int rank;
        int merged_id;
    };

    // Encode one span that contains no special tokens, appending ids to `out`.
    void encode_ordinary(std::string_view text, std::vector<int>& out) const;

    // Merge the base symbol ids of one pre-token in place, lowest-rank pair first.
    void apply_merges(std::vector<int>& symbols) const;

    // Pack an adjacent (first, second) id pair into one 64-bit hash key.
    static std::uint64_t pair_key(int first, int second) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(first)) << 32) |
               static_cast<std::uint32_t>(second);
    }

    std::unordered_map<std::string, int> vocab_;   // token → id
    std::vector<std::string> id_to_token_;         // id → token ("" if unset)
    std::array<int, 256> byte_to_id_{};            // byte → its base symbol id (-1 if none)
    std::unordered_map<std::uint64_t, Merge> merges_;  // packed id pair → merge
    std::unordered_map<std::string, int> special_tokens_;  // raw content → id
};

}  // namespace zetta::tokenizer
