// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/dataset_path.hpp"

namespace zetta {
class Node;
}

namespace zetta::pipeline {
class Run;
}

// M-11 document intelligence (#1050): native PDF text recovery, image-line
// recognition with a TrOCR encoder-decoder, field extraction, review routing,
// and field-level evaluation. See docs/ml/document_extraction.md.
namespace zetta::ml {

struct DocumentBox {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    bool operator==(const DocumentBox&) const = default;
};

struct DocumentRegion {
    std::string text;
    double confidence = 0.0;
    std::size_t page = 1;
    DocumentBox box;
};

struct PdfTextResult {
    std::vector<DocumentRegion> regions;
    std::string error;

    bool ok() const {
        return error.empty();
    }
};

// Read literal strings from a PDF content stream without a PDF dependency.
// This first slice supports unencrypted PDFs whose text layer uses Tj/TJ inside
// BT/ET blocks. Unsupported or image-only files return no regions rather than
// inventing text.
PdfTextResult read_pdf_text_layer(std::string_view bytes);

struct DocumentImage {
    std::size_t width = 0;
    std::size_t height = 0;
    std::size_t channels = 1;
    std::vector<float> pixels;

    bool valid() const;
};

// Parse the portable graymap formats P2 and P5. This keeps the image boundary
// native and gives tests and simple connectors a dependency-free scan format.
std::optional<DocumentImage> parse_pgm(std::string_view bytes,
                                       std::string* error = nullptr);

DocumentImage resize_and_normalize(const DocumentImage& image,
                                   std::size_t width, std::size_t height);

struct ImageLine {
    DocumentImage image;
    DocumentBox box;
};

// Segment dark horizontal bands into line crops. Coordinates remain in source
// image pixels so every OCR result can retain auditable evidence.
std::vector<ImageLine> segment_text_lines(const DocumentImage& image,
                                          float ink_threshold = 0.80F);

struct TrocrConfig {
    std::size_t image_width = 32;
    std::size_t image_height = 16;
    std::size_t patch_size = 4;
    std::size_t channels = 1;
    std::size_t encoder_hidden_size = 16;
    std::size_t decoder_hidden_size = 16;
    std::size_t vocabulary_size = 32;
    std::size_t encoder_layers = 1;
    std::size_t decoder_layers = 1;
    std::size_t maximum_tokens = 16;
    int decoder_start_token = 0;
    int end_token = 1;
};

struct TrocrRecognition {
    std::string text;
    double confidence = 0.0;
    std::vector<int> tokens;
};

class TrocrModel {
  public:
    static TrocrModel random(TrocrConfig config, std::uint64_t seed);

    // Load config.json, model.safetensors, and vocab.json from one pinned Hub
    // snapshot directory. The loader is non-throwing and reads tensors by the
    // native safetensors reader.
    static std::optional<TrocrModel>
    from_directory(const std::filesystem::path& directory,
                   std::string* error = nullptr);

    const TrocrConfig& config() const {
        return config_;
    }
    TrocrRecognition recognize(const DocumentImage& image) const;

  private:
    TrocrConfig config_;
    std::vector<float> patch_projection_;
    std::vector<float> patch_bias_;
    std::vector<std::vector<float>> encoder_weights_;
    std::vector<float> encoder_to_decoder_;
    std::vector<float> token_embedding_;
    std::vector<std::vector<float>> decoder_weights_;
    std::vector<float> output_projection_;
    std::vector<std::string> vocabulary_;
};

struct DocumentExtractionParams {
    std::vector<std::string> fields;
    std::string document_column = "document";
    std::string document_id_column = "document_id";
    std::string labels_column = "labels";
    double review_threshold = 0.80;
    std::uint64_t seed = 0;
    std::string project;
    std::string graph;
    std::string model_name = "document_extraction";
    std::filesystem::path trocr_directory;
};

struct ExtractedField {
    std::string document;
    std::string field;
    std::string value;
    double confidence = 0.0;
    std::size_t page = 1;
    DocumentBox box;
    bool review = false;
};

struct DocumentExtractionMetrics {
    std::size_t true_positives = 0;
    std::size_t false_positives = 0;
    std::size_t false_negatives = 0;
    std::size_t extracted = 0;
    std::size_t reviewed = 0;

    double precision() const;
    double recall() const;
    double f1() const;
    double review_share() const;
};

struct DocumentExtractionResult {
    std::vector<ExtractedField> fields;
    DocumentExtractionMetrics metrics;
    std::string fields_table;
    std::string review_table;
    std::string model_id;
    std::string error;

    bool ok() const {
        return error.empty();
    }
};

// Extract declared fields from regions. Exact key-value matches outrank layout
// proximity; near keys receive a lower confidence. Every result is assigned a
// confidence and anything below the threshold is marked for review.
std::vector<ExtractedField>
extract_document_fields(std::string_view document,
                        const std::vector<DocumentRegion>& regions,
                        const DocumentExtractionParams& params);

DocumentExtractionMetrics
evaluate_document_fields(const std::vector<ExtractedField>& fields,
                         const std::vector<nlohmann::json>& labelled_rows,
                         const DocumentExtractionParams& params);

// Phase-A runner contract used by the registry adapter: a resolved table and
// params go in; replicated result tables, model metrics, and run lineage come
// out. Node-owned reads and writes are routed through Node::run_on_owner.
DocumentExtractionResult
run_document_extraction(Node& node, const zetta::data::ResolvedDataset& dataset,
                        const DocumentExtractionParams& params,
                        const std::shared_ptr<zetta::pipeline::Run>& run = {});

class AsyncDocumentExtractionRun {
  public:
    static std::shared_ptr<AsyncDocumentExtractionRun>
    start(Node& node, zetta::data::ResolvedDataset dataset,
          DocumentExtractionParams params);
    ~AsyncDocumentExtractionRun();

    AsyncDocumentExtractionRun(const AsyncDocumentExtractionRun&) = delete;
    AsyncDocumentExtractionRun&
    operator=(const AsyncDocumentExtractionRun&) = delete;

    bool done() const {
        return done_.load(std::memory_order_acquire);
    }
    const std::shared_ptr<zetta::pipeline::Run>& run() const {
        return run_;
    }
    const DocumentExtractionResult& result() const {
        return result_;
    }

  private:
    AsyncDocumentExtractionRun(Node& node, zetta::data::ResolvedDataset dataset,
                               DocumentExtractionParams params);

    Node& node_;
    zetta::data::ResolvedDataset dataset_;
    DocumentExtractionParams params_;
    std::shared_ptr<zetta::pipeline::Run> run_;
    std::atomic<bool> done_{false};
    DocumentExtractionResult result_;
    std::thread worker_;
};

} // namespace zetta::ml
