#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/parquet/writer.hpp"
#include "zetta/storage/parquet_store.hpp"

using zetta::parquet::WriterOptions;
using zetta::parquet::write_parquet_rows;
using zetta::storage::ParquetLazyPtr;
using zetta::storage::ParquetStore;
using zetta::storage::ParquetStoreOptions;
using zetta::storage::ParquetStoreResult;
using zetta::test::TempDir;

namespace {

// `count` rows of `{_zid, n, label}` — the shape a partition body has (the id as
// a column, the row's own fields beside it).
std::vector<nlohmann::json> sample_rows(std::size_t count) {
    std::vector<nlohmann::json> rows;
    rows.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        rows.push_back(nlohmann::json{{"_zid", "row-" + std::to_string(i)},
                                      {"n", static_cast<std::int64_t>(i)},
                                      {"label", "value-" + std::to_string(i)}});
    }
    return rows;
}

// Write rows to `path` as Parquet, `rows_per_group` rows per row group (= per
// page), and return the file's size.
std::size_t write_body(const std::filesystem::path& path,
                       const std::vector<nlohmann::json>& rows,
                       std::int64_t rows_per_group) {
    WriterOptions options;
    options.max_rows_per_row_group = rows_per_group;
    options.max_rows_per_page = rows_per_group;
    const auto written = write_parquet_rows(rows, options);
    EXPECT_TRUE(written.ok()) << written.error;
    std::ofstream out(path, std::ios::binary);
    out.write(written.data.data(), static_cast<std::streamsize>(written.data.size()));
    out.close();
    return written.data.size();
}

}  // namespace

// ---- the acceptance: a Parquet body read as a storage engine ----------------

TEST(ParquetStore, ReadsRowsIdsAndMembership) {
    TempDir tmp;
    const std::filesystem::path body = tmp.path() / "partition.parquet";
    write_body(body, sample_rows(10), /*rows_per_group=*/4);

    const ParquetStoreResult opened = ParquetStore::open(body);
    ASSERT_TRUE(opened.ok()) << opened.error;
    const ParquetStore& store = *opened.store;

    EXPECT_STREQ(store.name(), "parquet");
    EXPECT_EQ(store.size(), 10u);
    EXPECT_EQ(store.page_count(), 3u) << "10 rows at 4 per row group";

    for (std::size_t i = 0; i < store.size(); ++i) {
        const ParquetLazyPtr row = store.at(i);
        ASSERT_TRUE(row.ok()) << row.error();
        EXPECT_EQ(row.id(), "row-" + std::to_string(i));
        EXPECT_EQ(store.id_at(i), row.id());
        EXPECT_EQ((*row)["n"].get<std::int64_t>(), static_cast<std::int64_t>(i));
        EXPECT_EQ((*row)["label"].get<std::string>(), "value-" + std::to_string(i));
        // The id is the row's key, not a field of it.
        EXPECT_FALSE(row->contains("_zid"));
    }

    EXPECT_TRUE(store.contains("row-7"));
    EXPECT_FALSE(store.contains("row-77"));

    const ParquetLazyPtr past = store.at(10);
    EXPECT_FALSE(past.ok());
    EXPECT_NE(past.error().find("past the end"), std::string::npos) << past.error();
}

TEST(ParquetStore, KeepsTheIdColumnWhenAskedTo) {
    TempDir tmp;
    const std::filesystem::path body = tmp.path() / "with-id.parquet";
    write_body(body, sample_rows(3), /*rows_per_group=*/8);

    ParquetStoreOptions options;
    options.hide_id_column = false;
    const ParquetStoreResult opened = ParquetStore::open(body, options);
    ASSERT_TRUE(opened.ok()) << opened.error;
    const ParquetLazyPtr row = opened.store->at(1);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ((*row)["_zid"].get<std::string>(), "row-1");
}

TEST(ParquetStore, SynthesizesIdsForABodyWithoutAnIdColumn) {
    TempDir tmp;
    const std::filesystem::path body = tmp.path() / "foreign.parquet";
    std::vector<nlohmann::json> rows;
    for (int i = 0; i < 5; ++i) rows.push_back(nlohmann::json{{"n", i}});
    write_body(body, rows, /*rows_per_group=*/2);

    const ParquetStoreResult opened = ParquetStore::open(body);
    ASSERT_TRUE(opened.ok()) << opened.error;
    EXPECT_EQ(opened.store->size(), 5u);
    EXPECT_EQ(opened.store->id_at(0), "foreign.parquet#0");
    EXPECT_EQ(opened.store->id_at(4), "foreign.parquet#4");
    EXPECT_TRUE(opened.store->contains("foreign.parquet#3"));
    const ParquetLazyPtr row = opened.store->at(3);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ((*row)["n"].get<int>(), 3);
}

TEST(ParquetStore, ReadsABodyHeldInMemory) {
    const auto written = write_parquet_rows(sample_rows(4));
    ASSERT_TRUE(written.ok()) << written.error;
    const ParquetStoreResult opened =
        ParquetStore::open_bytes(written.data, "in-memory");
    ASSERT_TRUE(opened.ok()) << opened.error;
    EXPECT_EQ(opened.store->size(), 4u);
    EXPECT_EQ(opened.store->at(2)->at("n").get<std::int64_t>(), 2);
}

// ---- the point of ParquetLazyPtr: a handle outlives the page's reclamation ---

TEST(ParquetStore, AHandleStaysValidAcrossAReap) {
    TempDir tmp;
    const std::filesystem::path body = tmp.path() / "reaped.parquet";
    write_body(body, sample_rows(12), /*rows_per_group=*/4);

    const ParquetStoreResult opened = ParquetStore::open(body);
    ASSERT_TRUE(opened.ok()) << opened.error;
    ParquetStore& store = *opened.store;

    const ParquetLazyPtr held = store.at(5);
    ASSERT_TRUE(held.ok()) << held.error();
    EXPECT_EQ(store.resident_pages(), 1u);
    EXPECT_EQ(store.decode_count(), 1u);

    store.reap();  // the cache drops every reference it holds
    EXPECT_EQ(store.resident_pages(), 0u);
    EXPECT_EQ(store.resident_bytes(), 0u);

    // The page is still alive because this handle holds it — the refcount
    // decided, nothing arbitrated.
    ASSERT_TRUE(held.ok());
    EXPECT_EQ(held.id(), "row-5");
    EXPECT_EQ((*held)["n"].get<std::int64_t>(), 5);
    EXPECT_EQ((*held)["label"].get<std::string>(), "value-5");
    EXPECT_EQ(store.decode_count(), 1u) << "reading a held handle reads no disk";
}

TEST(ParquetStore, TwoRowsOfALivePageCostOneDecode) {
    TempDir tmp;
    const std::filesystem::path body = tmp.path() / "shared-page.parquet";
    write_body(body, sample_rows(8), /*rows_per_group=*/8);  // one page

    const ParquetStoreResult opened = ParquetStore::open(body);
    ASSERT_TRUE(opened.ok()) << opened.error;
    ParquetStore& store = *opened.store;

    const ParquetLazyPtr first = store.at(0);
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(store.decode_count(), 1u);

    // Reap so the cache no longer holds the page — only `first` does. The next
    // read must re-adopt it through the weak_ptr, not decode a second copy.
    store.reap();
    const ParquetLazyPtr second = store.at(6);
    ASSERT_TRUE(second.ok()) << second.error();
    EXPECT_EQ(second.id(), "row-6");
    EXPECT_EQ(store.decode_count(), 1u)
        << "a page a handle still holds is re-adopted, never decoded twice";

    // And a cache hit (no reap in between) is also one decode.
    const ParquetLazyPtr third = store.at(3);
    ASSERT_TRUE(third.ok());
    EXPECT_EQ(store.decode_count(), 1u);
}

TEST(ParquetStore, ALazyHandleReReadsAfterEveryoneLetsGo) {
    TempDir tmp;
    const std::filesystem::path body = tmp.path() / "lazy.parquet";
    write_body(body, sample_rows(6), /*rows_per_group=*/6);

    const ParquetStoreResult opened = ParquetStore::open(body);
    ASSERT_TRUE(opened.ok()) << opened.error;
    ParquetStore& store = *opened.store;

    ParquetLazyPtr row = store.at(4);
    ASSERT_TRUE(row.ok());
    store.reap();
    row.release();  // now nothing holds the page: it is really gone
    EXPECT_TRUE(row.lazy());

    ASSERT_TRUE(row.materialize()) << row.error();
    EXPECT_EQ(row.id(), "row-4");
    EXPECT_EQ((*row)["n"].get<std::int64_t>(), 4);
    EXPECT_EQ(store.decode_count(), 2u) << "the page was read off disk again";
}

// ---- memory is bounded ------------------------------------------------------

TEST(ParquetStore, AScanMuchLargerThanTheBudgetStaysWithinIt) {
    TempDir tmp;
    const std::filesystem::path body = tmp.path() / "big.parquet";
    constexpr std::size_t kRows = 4000;
    write_body(body, sample_rows(kRows), /*rows_per_group=*/50);  // 80 pages

    ParquetStoreOptions options;
    options.page_cache_bytes = 4096;  // far smaller than the whole body
    const ParquetStoreResult opened = ParquetStore::open(body, options);
    ASSERT_TRUE(opened.ok()) << opened.error;
    ParquetStore& store = *opened.store;
    ASSERT_EQ(store.size(), kRows);
    ASSERT_EQ(store.page_count(), 80u);

    std::int64_t sum = 0;
    std::size_t peak_pages = 0;
    std::size_t peak_bytes = 0;
    for (std::size_t i = 0; i < store.size(); ++i) {
        const ParquetLazyPtr row = store.at(i);
        ASSERT_TRUE(row.ok()) << row.error();
        sum += (*row)["n"].get<std::int64_t>();
        peak_pages = std::max(peak_pages, store.resident_pages());
        peak_bytes = std::max(peak_bytes, store.resident_bytes());
    }
    EXPECT_EQ(sum, static_cast<std::int64_t>(kRows * (kRows - 1) / 2));

    // The resident set is one page at a time, not the dataset: the budget cannot
    // hold two pages, so each insert evicts the previous one.
    EXPECT_EQ(peak_pages, 1u);
    EXPECT_LT(peak_bytes, 10u * options.page_cache_bytes);
    EXPECT_EQ(store.decode_count(), 80u) << "each page decoded exactly once";
}

// ---- mutations are refused, loudly ------------------------------------------

TEST(ParquetStore, RefusesMutationsInsteadOfNoOppingThem) {
    TempDir tmp;
    const std::filesystem::path body = tmp.path() / "immutable.parquet";
    write_body(body, sample_rows(3), /*rows_per_group=*/8);

    const ParquetStoreResult opened = ParquetStore::open(body);
    ASSERT_TRUE(opened.ok()) << opened.error;
    ParquetStore& store = *opened.store;
    EXPECT_EQ(store.rejected_mutations(), 0u);
    EXPECT_TRUE(store.last_mutation_error().empty());

    store.add("row-9", nlohmann::json{{"n", 9}});
    EXPECT_EQ(store.rejected_mutations(), 1u);
    EXPECT_NE(store.last_mutation_error().find("cannot add row 'row-9'"),
              std::string::npos)
        << store.last_mutation_error();

    store.update("row-1", nlohmann::json{{"n", 42}});
    store.remove("row-1");
    EXPECT_EQ(store.rejected_mutations(), 3u);

    // Nothing changed: the body is what it was.
    EXPECT_EQ(store.size(), 3u);
    EXPECT_TRUE(store.contains("row-1"));
    EXPECT_EQ(store.at(1)->at("n").get<std::int64_t>(), 1);
}

// ---- failures carry a reason ------------------------------------------------

TEST(ParquetStore, OpeningAMissingOrMalformedBodyFails) {
    TempDir tmp;
    const ParquetStoreResult missing = ParquetStore::open(tmp.path() / "nope.parquet");
    EXPECT_FALSE(missing.ok());
    EXPECT_NE(missing.error.find("no such file"), std::string::npos) << missing.error;

    const std::filesystem::path garbage = tmp.path() / "garbage.parquet";
    {
        std::ofstream out(garbage, std::ios::binary);
        out << "this is not a parquet file at all";
    }
    const ParquetStoreResult bad = ParquetStore::open(garbage);
    EXPECT_FALSE(bad.ok());
    EXPECT_FALSE(bad.error.empty());

    const ParquetStoreResult tiny = ParquetStore::open_bytes("PAR1", "tiny");
    EXPECT_FALSE(tiny.ok());
    EXPECT_NE(tiny.error.find("too small"), std::string::npos) << tiny.error;
}

TEST(ParquetStore, AnUnreadablePageIsAFailedHandleWithAReason) {
    TempDir tmp;
    const std::filesystem::path body = tmp.path() / "truncated.parquet";
    const std::size_t size = write_body(body, sample_rows(200), /*rows_per_group=*/50);

    const ParquetStoreResult opened = ParquetStore::open(body);
    ASSERT_TRUE(opened.ok()) << opened.error;
    ParquetStore& store = *opened.store;
    ASSERT_TRUE(store.at(0).ok());
    store.reap();

    // The body loses its tail after the store opened it — the pages it has not
    // read yet are gone.
    std::error_code ec;
    std::filesystem::resize_file(body, size / 4, ec);
    ASSERT_FALSE(ec) << ec.message();

    const ParquetLazyPtr row = store.at(199);
    EXPECT_FALSE(row.ok());
    EXPECT_FALSE(row.error().empty()) << "a data-loss read reports a reason, not null";
    // Absence and data loss stay distinguishable: the id is still known.
    EXPECT_EQ(store.id_at(199), "row-199");
}
