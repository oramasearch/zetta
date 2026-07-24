#include <gtest/gtest.h>

#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "temp_dir.hpp"
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

// Special tokens are matched atomically before BPE, emitted as their id, and
// reproduced on decode.
TEST(Bpe, SpecialTokens) {
    std::optional<BpeTokenizer> tok =
        BpeTokenizer::from_vocab_and_merges(base_vocab(), {});
    ASSERT_TRUE(tok.has_value());
    tok->add_special_tokens({{"<|endoftext|>", 1000}, {"<|im_start|>", 1001}});

    const std::vector<int> ids = tok->encode("hi<|endoftext|>bye");
    // The special id appears between the two ordinary spans.
    ASSERT_GE(ids.size(), 3U);
    bool has_special = false;
    for (int id : ids) {
        if (id == 1000) has_special = true;
    }
    EXPECT_TRUE(has_special);
    EXPECT_EQ(tok->decode(ids), "hi<|endoftext|>bye");
    // A bare special token encodes to exactly its id, and round-trips.
    EXPECT_EQ(tok->encode("<|im_start|>"), (std::vector<int>{1001}));
    EXPECT_EQ(tok->decode({1001}), "<|im_start|>");
}

// Loading a GPT-2/Qwen-style vocab.json + merges.txt from disk.
TEST(Bpe, LoadsFromGpt2Files) {
    const zetta::test::TempDir temp;
    const std::string vocab_path = (temp.path() / "vocab.json").string();
    const std::string merges_path = (temp.path() / "merges.txt").string();

    // A small vocab: base symbols for the letters we use + merged tokens.
    std::ofstream vocab_out(vocab_path);
    vocab_out << "{";
    vocab_out << "\"h\":104,\"e\":101,\"l\":108,\"o\":111,";
    vocab_out << "\"he\":256,\"ll\":257,\"hell\":258,\"hello\":259";
    vocab_out << "}";
    vocab_out.close();

    std::ofstream merges_out(merges_path);
    merges_out << "#version: 0.2\n";
    merges_out << "h e\n";
    merges_out << "l l\n";
    merges_out << "he ll\n";
    merges_out << "hell o\n";
    merges_out.close();

    std::string error;
    const std::optional<BpeTokenizer> tok =
        BpeTokenizer::from_gpt2_files(vocab_path, merges_path, &error);
    ASSERT_TRUE(tok.has_value()) << error;
    EXPECT_EQ(tok->encode("hello"), (std::vector<int>{259}));
    EXPECT_EQ(tok->decode({259}), "hello");

    EXPECT_FALSE(
        BpeTokenizer::from_gpt2_files("/no/vocab.json", "/no/merges.txt", &error)
            .has_value());
}

// Load a Hugging Face tokenizer.json (model.vocab + model.merges + added_tokens).
TEST(Bpe, LoadsFromTokenizerJson) {
    const zetta::test::TempDir temp;
    const std::string path = (temp.path() / "tokenizer.json").string();
    std::ofstream out(path);
    out << R"({
      "added_tokens": [
        {"id": 1000, "content": "<|endoftext|>", "special": true}
      ],
      "model": {
        "type": "BPE",
        "vocab": {"h":104,"e":101,"l":108,"o":111,
                  "he":256,"ll":257,"hell":258,"hello":259},
        "merges": ["h e", "l l", "he ll", "hell o"]
      }
    })";
    out.close();

    std::string error;
    const std::optional<BpeTokenizer> tok =
        BpeTokenizer::from_tokenizer_json(path, &error);
    ASSERT_TRUE(tok.has_value()) << error;
    EXPECT_EQ(tok->encode("hello"), (std::vector<int>{259}));
    EXPECT_EQ(tok->encode("<|endoftext|>"), (std::vector<int>{1000}));
    EXPECT_EQ(tok->decode({259, 1000}), "hello<|endoftext|>");
}

// The newer merges format is an array of [first, second] pairs.
TEST(Bpe, LoadsTokenizerJsonWithPairMerges) {
    const zetta::test::TempDir temp;
    const std::string path = (temp.path() / "tokenizer.json").string();
    std::ofstream out(path);
    out << R"({
      "model": {
        "vocab": {"h":104,"i":105,"hi":256},
        "merges": [["h", "i"]]
      }
    })";
    out.close();

    std::string error;
    const std::optional<BpeTokenizer> tok =
        BpeTokenizer::from_tokenizer_json(path, &error);
    ASSERT_TRUE(tok.has_value()) << error;
    EXPECT_EQ(tok->encode("hi"), (std::vector<int>{256}));
}

// from_directory prefers tokenizer.json, falls back to vocab.json + merges.txt.
TEST(Bpe, FromDirectory) {
    const zetta::test::TempDir temp;
    std::ofstream(temp.path() / "tokenizer.json")
        << R"({"model":{"vocab":{"a":97,"b":98,"ab":256},"merges":["a b"]}})";
    std::string error;
    std::optional<BpeTokenizer> tok =
        BpeTokenizer::from_directory(temp.path().string(), &error);
    ASSERT_TRUE(tok.has_value()) << error;
    EXPECT_EQ(tok->encode("ab"), (std::vector<int>{256}));

    // An empty directory is rejected with a reason.
    const zetta::test::TempDir empty;
    EXPECT_FALSE(BpeTokenizer::from_directory(empty.path().string(), &error).has_value());
    EXPECT_FALSE(error.empty());
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
