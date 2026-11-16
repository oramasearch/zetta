// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "zetta/tokenizer/pre_tokenizer.hpp"

namespace {

using zetta::tokenizer::pre_tokenize;
using Pieces = std::vector<std::string>;

TEST(PreTokenizer, LeadingSpaceAttachesToWords) {
    EXPECT_EQ(pre_tokenize("hello world"), (Pieces{"hello", " world"}));
    EXPECT_EQ(pre_tokenize("the quick brown fox"),
              (Pieces{"the", " quick", " brown", " fox"}));
}

TEST(PreTokenizer, PunctuationSplitsOff) {
    // ',' before a space is its own piece.
    EXPECT_EQ(pre_tokenize("Hello, world!"),
              (Pieces{"Hello", ",", " world", "!"}));
    // But in Qwen's pattern a single non-alphanumeric char (not just a space) may
    // LEAD a word, so '.' attaches to the following "b".
    EXPECT_EQ(pre_tokenize("a.b"), (Pieces{"a", ".b"}));
}

TEST(PreTokenizer, Contractions) {
    EXPECT_EQ(pre_tokenize("don't"), (Pieces{"don", "'t"}));
    EXPECT_EQ(pre_tokenize("I'll go"), (Pieces{"I", "'ll", " go"}));
    EXPECT_EQ(pre_tokenize("we've"), (Pieces{"we", "'ve"}));
    // Case-insensitive contractions.
    EXPECT_EQ(pre_tokenize("DON'T"), (Pieces{"DON", "'T"}));
}

TEST(PreTokenizer, DigitsAreIndividual) {
    EXPECT_EQ(pre_tokenize("123"), (Pieces{"1", "2", "3"}));
    EXPECT_EQ(pre_tokenize("v2"), (Pieces{"v", "2"}));
    EXPECT_EQ(pre_tokenize(" 42"), (Pieces{" ", "4", "2"}));
}

TEST(PreTokenizer, WhitespaceHandling) {
    // A double space: the first is its own piece, the second attaches to the word.
    EXPECT_EQ(pre_tokenize("a  b"), (Pieces{"a", " ", " b"}));
    // Trailing whitespace at end of input is one piece.
    EXPECT_EQ(pre_tokenize("hi  "), (Pieces{"hi", "  "}));
    // A whitespace run ending in a newline clumps together.
    EXPECT_EQ(pre_tokenize("foo\n\nbar"), (Pieces{"foo", "\n\n", "bar"}));
    EXPECT_EQ(pre_tokenize("a \nb"), (Pieces{"a", " \n", "b"}));
}

TEST(PreTokenizer, LeadingSpaceOnPunctuation) {
    EXPECT_EQ(pre_tokenize("a ..."), (Pieces{"a", " ..."}));
    EXPECT_EQ(pre_tokenize("x := y"), (Pieces{"x", " :=", " y"}));
}

// A code sample exercises words, digits, punctuation, and spacing together. Note
// '(' leads "x" (Qwen's word rule), so "(x" is one piece.
TEST(PreTokenizer, CodeLikeInput) {
    EXPECT_EQ(pre_tokenize("fn(x) = x + 1"),
              (Pieces{"fn", "(x", ")", " =", " x", " +", " ", "1"}));
}

// Non-ASCII letters clump like ASCII (café is one word; the é is a letter).
TEST(PreTokenizer, NonAsciiLettersClump) {
    EXPECT_EQ(pre_tokenize("café"), (Pieces{"café"}));
    EXPECT_EQ(pre_tokenize("café latte"), (Pieces{"café", " latte"}));
}

// Concatenating the pieces always reproduces the input exactly (byte-exact).
TEST(PreTokenizer, PiecesConcatenateToTheInput) {
    for (const std::string& text :
         {std::string("Hello,  world!\n\tTabbed 123 and don't stop."),
          std::string("  leading and trailing  "),
          std::string("café → π ≈ 3")}) {
        std::string joined;
        for (const std::string& piece : pre_tokenize(text)) joined += piece;
        EXPECT_EQ(joined, text);
    }
}

}  // namespace
