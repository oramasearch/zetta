#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "zetta/tokenizer/bpe.hpp"
#include "zetta/tokenizer/byte_encoding.hpp"

namespace {

using zetta::tokenizer::BpeTokenizer;
using zetta::tokenizer::byte_to_unicode;
using zetta::tokenizer::utf8_encode;
using Vocab = std::unordered_map<std::string, int>;
using Merges = std::vector<std::pair<std::string, std::string>>;

// The byte-level string for one byte (the vocab key for that base symbol).
std::string byte_symbol(unsigned char byte) {
    std::string out;
    utf8_encode(byte_to_unicode()[byte], out);
    return out;
}

// A vocab covering all 256 base byte characters, ids 0..255. Extra tokens can be
// added on top.
Vocab base_vocab() {
    Vocab vocab;
    for (int byte = 0; byte < 256; ++byte) {
        vocab[byte_symbol(static_cast<unsigned char>(byte))] = byte;
    }
    return vocab;
}

// With no merges, every text round-trips exactly through the 256 base symbols —
// the byte-level guarantee, for arbitrary UTF-8.
TEST(Bpe, ByteLevelRoundTripWithoutMerges) {
    const std::optional<BpeTokenizer> tok =
        BpeTokenizer::from_vocab_and_merges(base_vocab(), {});
    ASSERT_TRUE(tok.has_value());

    for (const std::string& text :
         {std::string("Hello, world!"), std::string("café → π ≈ 3"),
          std::string("emoji 😀 and\ttabs\nnewlines"), std::string("")}) {
        const std::vector<int> ids = tok->encode(text);
        EXPECT_EQ(tok->decode(ids), text) << "text: " << text;
    }
}

// Merges combine symbols in rank order to build a single token for a word.
TEST(Bpe, MergesBuildAToken) {
    Vocab vocab = base_vocab();
    vocab["he"] = 256;
    vocab["ll"] = 257;
    vocab["hell"] = 258;
    vocab["hello"] = 259;
    const Merges merges = {{"h", "e"}, {"l", "l"}, {"he", "ll"}, {"hell", "o"}};

    const std::optional<BpeTokenizer> tok =
        BpeTokenizer::from_vocab_and_merges(vocab, merges);
    ASSERT_TRUE(tok.has_value());

    // "hello" merges all the way to the single token id 259.
    EXPECT_EQ(tok->encode("hello"), (std::vector<int>{259}));
    EXPECT_EQ(tok->decode({259}), "hello");
}

// Merges apply per pre-token; a leading space is its own byte symbol (Ġ) and can
// merge into a "Ġword" token like real BPE vocabularies have.
TEST(Bpe, LeadingSpaceMergesIntoWord) {
    Vocab vocab = base_vocab();
    const std::string g_the = byte_symbol(' ') + "the";  // "Ġthe"
    vocab[byte_symbol(' ') + "t"] = 256;                 // "Ġt"
    vocab["th"] = 257;
    vocab[byte_symbol(' ') + "th"] = 258;                // "Ġth"
    vocab[g_the] = 259;                                  // "Ġthe"
    const Merges merges = {
        {byte_symbol(' '), "t"},   // Ġ + t -> Ġt
        {"t", "h"},                // t + h -> th
        {byte_symbol(' ') + "t", "h"},  // Ġt + h -> Ġth
        {byte_symbol(' ') + "th", "e"}, // Ġth + e -> Ġthe
    };
    const std::optional<BpeTokenizer> tok =
        BpeTokenizer::from_vocab_and_merges(vocab, merges);
    ASSERT_TRUE(tok.has_value());

    // " the" -> ["Ġthe"] = [259]; still round-trips.
    const std::vector<int> ids = tok->encode(" the");
    EXPECT_EQ(ids, (std::vector<int>{259}));
    EXPECT_EQ(tok->decode(ids), " the");
}

// Lowest-rank pair wins first even when several pairs are mergeable.
TEST(Bpe, LowestRankPairMergesFirst) {
    Vocab vocab = base_vocab();
    vocab["ab"] = 256;
    vocab["bc"] = 257;
    // "ab" (rank 0) must win over "bc" (rank 1) on input "abc".
    const Merges merges = {{"a", "b"}, {"b", "c"}};
    const std::optional<BpeTokenizer> tok =
        BpeTokenizer::from_vocab_and_merges(vocab, merges);
    ASSERT_TRUE(tok.has_value());
    // "abc" -> "ab" + "c" = [256, 'c'].
    EXPECT_EQ(tok->encode("abc"), (std::vector<int>{256, 'c'}));
}

TEST(Bpe, AccessorsAndEmptyVocab) {
    const std::optional<BpeTokenizer> tok =
        BpeTokenizer::from_vocab_and_merges(base_vocab(), {});
    ASSERT_TRUE(tok.has_value());
    EXPECT_EQ(tok->vocab_size(), 256);
    EXPECT_EQ(tok->token_to_id(byte_symbol('A')), 'A');
    EXPECT_EQ(tok->id_to_token('A'), byte_symbol('A'));
    EXPECT_EQ(tok->token_to_id("no-such-token"), -1);

    std::string error;
    EXPECT_FALSE(BpeTokenizer::from_vocab_and_merges({}, {}, &error).has_value());
    EXPECT_FALSE(error.empty());
}

}  // namespace
