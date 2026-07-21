#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/storage/json_store.hpp"
#include "zetta/storage/jsonl_store.hpp"
#include "zetta/storage/lazy_row.hpp"

using zetta::storage::JsonlStore;
using zetta::storage::JsonStore;
using zetta::storage::ParquetLazyPtr;
using zetta::storage::RowKey;
using zetta::storage::RowPage;
using zetta::storage::RowPagePtr;
using zetta::storage::RowPageResult;
using zetta::storage::RowPageSource;
using zetta::test::TempDir;

namespace {

// A page source standing in for a paging engine: it "decodes" a page of rows on
// demand, counts how many times it did, keeps the store's own shared_ptr per page
// (which `reap()` drops, exactly like a cache eviction), and keeps a weak_ptr so a
// later fetch can re-adopt a page a handle is still holding alive.
class CountingSource final : public RowPageSource,
                             public std::enable_shared_from_this<CountingSource> {
public:
    explicit CountingSource(std::size_t pages, std::size_t rows_per_page)
        : rows_per_page_(rows_per_page), strong_(pages), weak_(pages) {}

    RowPageResult fetch_page(const RowKey& key) const override {
        RowPageResult out;
        if (key.page >= strong_.size()) {
            out.error = "counting source: no page " + std::to_string(key.page);
            return out;
        }
        if (fail_) {
            out.error = "counting source: page " + std::to_string(key.page) +
                        " is corrupt";
            return out;
        }
        if (const RowPagePtr resident = strong_[key.page]) {
            out.page = resident;
            return out;
        }
        if (const RowPagePtr adopted = weak_[key.page].lock()) {
            strong_[key.page] = adopted;  // re-adopt rather than decode a second copy
            out.page = adopted;
            return out;
        }
        ++decodes_;
        auto page = std::make_shared<RowPage>();
        for (std::size_t i = 0; i < rows_per_page_; ++i) {
            const std::size_t global = key.page * rows_per_page_ + i;
            page->ids.push_back("row-" + std::to_string(global));
            page->rows.push_back(nlohmann::json{{"n", static_cast<int>(global)}});
        }
        page->resident_bytes = rows_per_page_ * 32;
        strong_[key.page] = page;
        weak_[key.page] = page;
        out.page = std::move(page);
        return out;
    }

    // The store dropping its own references — the whole of "reaping".
    void reap() const {
        for (RowPagePtr& page : strong_) page.reset();
    }
    std::size_t decodes() const { return decodes_; }
    bool page_is_alive(std::size_t page) const { return !weak_[page].expired(); }
    void fail_next_reads(bool fail) const { fail_ = fail; }

    ParquetLazyPtr row(std::size_t page, std::size_t index) const {
        const RowKey key{.blob = 0,
                         .page = static_cast<std::uint32_t>(page),
                         .index = index};
        RowPageResult fetched = fetch_page(key);
        if (!fetched.ok()) return ParquetLazyPtr::failed(fetched.error);
        return ParquetLazyPtr(key, std::move(fetched.page),
                              std::weak_ptr<const RowPageSource>(shared_from_this()));
    }

private:
    std::size_t rows_per_page_;
    mutable std::vector<RowPagePtr> strong_;
    mutable std::vector<std::weak_ptr<const RowPage>> weak_;
    mutable std::size_t decodes_ = 0;
    mutable bool fail_ = false;
};

}  // namespace

// ---- the always-resident state costs nothing --------------------------------

TEST(LazyRow, JsonlStoreHandsBackBorrowedRows) {
    TempDir tmp;
    JsonlStore store{tmp.path()};
    store.add("id-a", nlohmann::json{{"a", 1}});
    store.add("id-b", nlohmann::json{{"b", 2}});

    const ParquetLazyPtr first = store.at(0);
    ASSERT_TRUE(first.ok());
    EXPECT_TRUE(static_cast<bool>(first));
    EXPECT_TRUE(first.error().empty());
    EXPECT_EQ(first.id(), "id-a");
    EXPECT_EQ((*first)["a"].get<int>(), 1);
    EXPECT_EQ(first->at("a").get<int>(), 1);

    // Borrowed, not paged: no source to re-read through, so it is never lazy and
    // release() cannot take the row away.
    EXPECT_FALSE(first.lazy());
    ParquetLazyPtr copy = first;
    copy.release();
    EXPECT_TRUE(copy.ok());
    EXPECT_EQ(copy.id(), "id-a");

    // The row is borrowed from the store, not copied out of it: an update through
    // the store is visible through a handle taken afterwards.
    store.update("id-a", nlohmann::json{{"a", 9}});
    EXPECT_EQ(store.at(0)->at("a").get<int>(), 9);
}

TEST(LazyRow, InMemoryStoreHandsBackBorrowedRows) {
    JsonStore store;
    store.add("id-a", nlohmann::json{{"a", 1}});
    const ParquetLazyPtr row = store.at(0);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row.id(), "id-a");
    EXPECT_EQ(row->at("a").get<int>(), 1);
}

TEST(LazyRow, PastTheEndIsAFailedHandleNotACrash) {
    JsonStore store;
    store.add("only", nlohmann::json{{"a", 1}});

    const ParquetLazyPtr past = store.at(7);
    EXPECT_FALSE(past.ok());
    EXPECT_FALSE(static_cast<bool>(past));
    EXPECT_NE(past.error().find("past the end"), std::string::npos) << past.error();

    // A defaulted handle is empty: not dereferenceable, and no error to report.
    const ParquetLazyPtr empty;
    EXPECT_FALSE(empty.ok());
    EXPECT_TRUE(empty.error().empty());
    static const nlohmann::json fallback = nlohmann::json::object();
    EXPECT_TRUE(empty.value_or(fallback).empty());
}

// ---- the point of the design: a handle outlives the store's reference --------

TEST(LazyRow, HandleSurvivesAReap) {
    const auto source = std::make_shared<CountingSource>(/*pages=*/2, /*rows=*/4);

    const ParquetLazyPtr held = source->row(/*page=*/0, /*index=*/1);
    ASSERT_TRUE(held.ok());
    EXPECT_EQ(held.id(), "row-1");
    EXPECT_EQ(source->decodes(), 1u);

    // The store reclaims the page: it drops its own shared_ptr, nothing else.
    source->reap();
    EXPECT_TRUE(source->page_is_alive(0)) << "the handle still holds the page alive";

    // The handle is still readable — the refcount, not a policy, decided.
    ASSERT_TRUE(held.ok());
    EXPECT_EQ((*held)["n"].get<int>(), 1);
    EXPECT_EQ(held.id(), "row-1");
}

TEST(LazyRow, ReapedPageFreesItselfWhenTheLastHandleGoes) {
    const auto source = std::make_shared<CountingSource>(/*pages=*/1, /*rows=*/4);
    {
        const ParquetLazyPtr held = source->row(0, 0);
        ASSERT_TRUE(held.ok());
        source->reap();
        EXPECT_TRUE(source->page_is_alive(0));
    }
    EXPECT_FALSE(source->page_is_alive(0))
        << "the last holder releasing frees the page";
}

TEST(LazyRow, ASecondRowOfALivePageReAdoptsItRatherThanDecodingTwice) {
    const auto source = std::make_shared<CountingSource>(/*pages=*/1, /*rows=*/4);

    const ParquetLazyPtr first = source->row(0, 0);
    ASSERT_TRUE(first.ok());
    source->reap();  // the cache lets go; `first` keeps the page alive

    const ParquetLazyPtr second = source->row(0, 2);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second.id(), "row-2");
    EXPECT_EQ(source->decodes(), 1u)
        << "a page a handle still holds is re-adopted through the weak_ptr";
    // Both handles read out of the same decoded page — the rows are neighbours in
    // one contiguous buffer, not two copies of it.
    EXPECT_LT(&(*first), &(*second));
    EXPECT_EQ((*first)["n"].get<int>(), 0);
    EXPECT_EQ((*second)["n"].get<int>(), 2);
}

// ---- lazy: give up residency, come back later -------------------------------

TEST(LazyRow, ReleaseThenMaterializeReReads) {
    const auto source = std::make_shared<CountingSource>(/*pages=*/1, /*rows=*/4);

    ParquetLazyPtr row = source->row(0, 3);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(source->decodes(), 1u);

    source->reap();
    row.release();
    EXPECT_FALSE(row.ok());
    EXPECT_TRUE(row.lazy());
    EXPECT_TRUE(row.error().empty()) << "released on purpose is not a failure";
    EXPECT_FALSE(source->page_is_alive(0)) << "nothing holds the page now";

    ASSERT_TRUE(row.materialize());
    EXPECT_TRUE(row.ok());
    EXPECT_EQ(row.id(), "row-3");
    EXPECT_EQ((*row)["n"].get<int>(), 3);
    EXPECT_EQ(source->decodes(), 2u) << "the page really was re-read";

    // Materializing an already-resident handle is a no-op.
    EXPECT_TRUE(row.materialize());
    EXPECT_EQ(source->decodes(), 2u);
}

TEST(LazyRow, AFailedReReadReportsAReasonInsteadOfNull) {
    const auto source = std::make_shared<CountingSource>(/*pages=*/1, /*rows=*/4);
    ParquetLazyPtr row = source->row(0, 0);
    ASSERT_TRUE(row.ok());

    source->reap();
    row.release();
    source->fail_next_reads(true);

    EXPECT_FALSE(row.materialize());
    EXPECT_FALSE(row.ok());
    EXPECT_NE(row.error().find("corrupt"), std::string::npos) << row.error();

    // The same handle recovers once the read does.
    source->fail_next_reads(false);
    EXPECT_TRUE(row.materialize());
    EXPECT_TRUE(row.error().empty());
    EXPECT_EQ(row.id(), "row-0");
}

TEST(LazyRow, ALazyHandleOutlivingItsStoreSaysSo) {
    ParquetLazyPtr row;
    {
        const auto source = std::make_shared<CountingSource>(/*pages=*/1, /*rows=*/2);
        row = source->row(0, 0);
        ASSERT_TRUE(row.ok());
        row.release();
        EXPECT_TRUE(row.lazy());
    }
    EXPECT_FALSE(row.lazy());
    EXPECT_FALSE(row.materialize());
    EXPECT_NE(row.error().find("is gone"), std::string::npos) << row.error();
}

TEST(LazyRow, AResidentHandleOutlivingItsStoreIsStillReadable) {
    ParquetLazyPtr row;
    {
        const auto source = std::make_shared<CountingSource>(/*pages=*/1, /*rows=*/2);
        row = source->row(0, 1);
        ASSERT_TRUE(row.ok());
    }
    ASSERT_TRUE(row.ok()) << "the page is refcounted, not owned by the store";
    EXPECT_EQ((*row)["n"].get<int>(), 1);
}

TEST(LazyRow, AnIndexPastThePageIsAFailedHandle) {
    const auto source = std::make_shared<CountingSource>(/*pages=*/1, /*rows=*/2);
    const ParquetLazyPtr row = source->row(0, /*index=*/9);
    EXPECT_FALSE(row.ok());
    EXPECT_NE(row.error().find("does not hold row"), std::string::npos) << row.error();
}
