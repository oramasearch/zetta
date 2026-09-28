#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ml/document_extraction.hpp"
#include "zetta/node.hpp"

namespace {

std::string planted_pdf() {
    return "%PDF-1.4\n"
           "1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
           "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
           "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
           "/Contents 4 0 R >> endobj\n"
           "4 0 obj << /Length 180 >> stream\n"
           "BT /F1 12 Tf 72 720 Td (Invoice Number: INV-42) Tj ET\n"
           "BT /F1 12 Tf 72 700 Td (Date: 2026-09-27) Tj ET\n"
           "BT /F1 12 Tf 72 680 Td (Total: 149.95) Tj ET\n"
           "endstream endobj\n"
           "trailer << /Root 1 0 R >>\n%%EOF\n";
}

zetta::ml::DocumentExtractionParams invoice_params() {
    zetta::ml::DocumentExtractionParams params;
    params.fields = {"invoice_number", "date", "total"};
    params.review_threshold = 0.8;
    params.seed = 42;
    return params;
}

} // namespace

TEST(PdfDocumentExtraction, GeneratedTextLayerYieldsPlantedFields) {
    const zetta::ml::PdfTextResult text =
        zetta::ml::read_pdf_text_layer(planted_pdf());
    ASSERT_TRUE(text.ok()) << text.error;
    ASSERT_EQ(text.regions.size(), 3u);

    const std::vector<zetta::ml::ExtractedField> fields =
        zetta::ml::extract_document_fields("invoice-1", text.regions,
                                           invoice_params());
    ASSERT_EQ(fields.size(), 3u);
    EXPECT_EQ(fields[0].value, "INV-42");
    EXPECT_EQ(fields[1].value, "2026-09-27");
    EXPECT_EQ(fields[2].value, "149.95");
    for (const zetta::ml::ExtractedField& field : fields) {
        EXPECT_GT(field.confidence, 0.8);
        EXPECT_FALSE(field.review);
        EXPECT_EQ(field.page, 1u);
        EXPECT_GT(field.box.width, 0.0);
    }
}

TEST(OcrDocumentExtraction, TinyRandomTrocrIsDeterministicEndToEnd) {
    zetta::ml::TrocrConfig config;
    config.image_width = 16;
    config.image_height = 8;
    config.patch_size = 4;
    config.encoder_hidden_size = 8;
    config.decoder_hidden_size = 8;
    config.vocabulary_size = 12;
    config.maximum_tokens = 6;
    const zetta::ml::TrocrModel first =
        zetta::ml::TrocrModel::random(config, 73);
    const zetta::ml::TrocrModel second =
        zetta::ml::TrocrModel::random(config, 73);

    const std::string pgm =
        "P2\n16 8\n255\n"
        "255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255\n"
        "255 0 0 255 255 0 0 255 255 0 0 255 255 255 255 255\n"
        "255 0 255 255 255 0 255 255 255 0 255 255 255 255 255 255\n"
        "255 0 0 255 255 0 0 255 255 0 0 255 255 255 255 255\n"
        "255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255\n"
        "255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255\n"
        "255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255\n"
        "255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255\n";
    std::string error;
    const std::optional<zetta::ml::DocumentImage> image =
        zetta::ml::parse_pgm(pgm, &error);
    ASSERT_TRUE(image) << error;
    const std::vector<zetta::ml::ImageLine> lines =
        zetta::ml::segment_text_lines(*image);
    ASSERT_EQ(lines.size(), 1u);

    const zetta::ml::TrocrRecognition first_result =
        first.recognize(lines.front().image);
    const zetta::ml::TrocrRecognition second_result =
        second.recognize(lines.front().image);
    EXPECT_EQ(first_result.tokens, second_result.tokens);
    EXPECT_EQ(first_result.text, second_result.text);
    EXPECT_DOUBLE_EQ(first_result.confidence, second_result.confidence);
}

TEST(OcrDocumentExtraction, BinaryPgmKeepsWhitespaceValuedFirstPixel) {
    const std::string pgm =
        std::string("P5\n2 1\n255\n", 11) + std::string{"\x0a\xff", 2};
    std::string error;
    const std::optional<zetta::ml::DocumentImage> image =
        zetta::ml::parse_pgm(pgm, &error);
    ASSERT_TRUE(image) << error;
    ASSERT_EQ(image->pixels.size(), 2u);
    EXPECT_FLOAT_EQ(image->pixels[0], 10.0F / 255.0F);
    EXPECT_FLOAT_EQ(image->pixels[1], 1.0F);
}

TEST(OcrDocumentExtraction, MalformedCheckpointConfigIsRejected) {
    zetta::test::TempDir directory;
    {
        std::ofstream config(directory.path() / "config.json");
        config << R"({"encoder":{"image_size":"large"}})";
    }
    std::string error;
    const std::optional<zetta::ml::TrocrModel> model =
        zetta::ml::TrocrModel::from_directory(directory.path(), &error);
    EXPECT_FALSE(model);
    EXPECT_NE(error.find("invalid TrOCR config"), std::string::npos);
}

TEST(DocumentExtraction, LowConfidenceFieldsAreRoutedToReview) {
    zetta::ml::DocumentExtractionParams params;
    params.fields = {"total"};
    params.review_threshold = 0.8;
    const std::vector<zetta::ml::DocumentRegion> regions = {
        {"Totl: 149.95", 0.90, 2, {10.0, 20.0, 80.0, 12.0}}};
    const std::vector<zetta::ml::ExtractedField> fields =
        zetta::ml::extract_document_fields("scan-1", regions, params);
    ASSERT_EQ(fields.size(), 1u);
    EXPECT_EQ(fields.front().value, "149.95");
    EXPECT_LT(fields.front().confidence, params.review_threshold);
    EXPECT_TRUE(fields.front().review);
    EXPECT_EQ(fields.front().page, 2u);
}

TEST(DocumentExtraction, NonFiniteConfidenceCannotBypassReview) {
    zetta::ml::DocumentExtractionParams params;
    params.fields = {"total"};
    params.review_threshold = 0.8;
    const std::vector<zetta::ml::DocumentRegion> regions = {
        {"Total: 149.95",
         std::numeric_limits<double>::quiet_NaN(),
         1,
         {10.0, 20.0, 80.0, 12.0}}};
    const std::vector<zetta::ml::ExtractedField> fields =
        zetta::ml::extract_document_fields("scan-1", regions, params);
    ASSERT_EQ(fields.size(), 1u);
    EXPECT_TRUE(std::isfinite(fields.front().confidence));
    EXPECT_EQ(fields.front().confidence, 0.0);
    EXPECT_TRUE(fields.front().review);
}

TEST(DocumentExtraction, RunnerWritesAuditableTablesAndFieldMetrics) {
    zetta::test::TempDir directory;
    zetta::Node node{{directory.path().string(), /*preflight=*/false}};
    ASSERT_TRUE(node.start());
    const std::shared_ptr<zetta::data::Database> database =
        node.create_database("documents");
    ASSERT_TRUE(database);
    const std::shared_ptr<zetta::data::Table> table =
        database->create_table("documents");
    ASSERT_TRUE(table);
    table->add_row({{"document_id", "invoice-1"},
                    {"document", planted_pdf()},
                    {"labels",
                     {{"invoice_number", "INV-42"},
                      {"date", "2026-09-27"},
                      {"total", "149.95"}}}});

    zetta::data::ResolvedDataset dataset;
    dataset.table_id = table->id();
    dataset.database = database->name();
    dataset.table = table->name();
    dataset.rows = table->row_count();
    zetta::ml::DocumentExtractionParams params = invoice_params();
    params.project = "project";
    params.graph = "graph-12345678";
    const zetta::ml::DocumentExtractionResult result =
        zetta::ml::run_document_extraction(node, dataset, params);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.fields.size(), 3u);
    EXPECT_DOUBLE_EQ(result.metrics.precision(), 1.0);
    EXPECT_DOUBLE_EQ(result.metrics.recall(), 1.0);
    EXPECT_DOUBLE_EQ(result.metrics.f1(), 1.0);
    EXPECT_FALSE(result.fields_table.empty());
    EXPECT_FALSE(result.review_table.empty());
    EXPECT_FALSE(result.model_id.empty());
    node.stop();
}

TEST(OcrDocumentExtraction, RealPinnedCheckpointRunsWhenConfigured) {
    const char* directory = std::getenv("ZETTA_TROCR_DIR");
    if (directory == nullptr || std::string(directory).empty()) {
        GTEST_SKIP() << "set ZETTA_TROCR_DIR to the pinned checkpoint snapshot";
    }
    std::string error;
    const std::optional<zetta::ml::TrocrModel> model =
        zetta::ml::TrocrModel::from_directory(directory, &error);
    ASSERT_TRUE(model) << error;
    zetta::ml::DocumentImage image;
    image.width = model->config().image_width;
    image.height = model->config().image_height;
    image.channels = model->config().channels;
    image.pixels.assign(image.width * image.height * image.channels, 1.0F);
    const zetta::ml::TrocrRecognition result = model->recognize(image);
    EXPECT_GE(result.confidence, 0.0);
    EXPECT_LE(result.confidence, 1.0);
}
