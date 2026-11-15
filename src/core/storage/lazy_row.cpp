// SPDX-License-Identifier: MIT

#include "zetta/storage/lazy_row.hpp"

#include <utility>

namespace zetta::storage {

ParquetLazyPtr::ParquetLazyPtr(RowKey key, RowPagePtr page,
                               std::weak_ptr<const RowPageSource> source)
    : page_(std::move(page)), source_(std::move(source)), key_(key) {
    if (page_ && key_.index < page_->rows.size() &&
        key_.index < page_->ids.size()) {
        row_ = &page_->rows[static_cast<std::size_t>(key_.index)];
        id_ = &page_->ids[static_cast<std::size_t>(key_.index)];
        return;
    }
    // A page that does not contain the row it was fetched for is a decode bug or
    // a truncated body, not an absent row — say so rather than handing back a
    // silently empty handle.
    page_.reset();
    error_ = "lazy row: page " + std::to_string(key_.page) + " does not hold row " +
             std::to_string(key_.index);
}

ParquetLazyPtr::ParquetLazyPtr(RowKey key, std::weak_ptr<const RowPageSource> source)
    : source_(std::move(source)), key_(key) {}

ParquetLazyPtr ParquetLazyPtr::failed(std::string reason) {
    ParquetLazyPtr handle;
    handle.error_ = std::move(reason);
    return handle;
}

bool ParquetLazyPtr::materialize() const {
    if (row_ != nullptr) return true;

    const std::shared_ptr<const RowPageSource> source = source_.lock();
    if (!source) {
        error_ = "lazy row: the store that owns page " + std::to_string(key_.page) +
                 " is gone";
        return false;
    }

    RowPageResult fetched = source->fetch_page(key_);
    if (!fetched.ok()) {
        error_ = fetched.error.empty()
                     ? "lazy row: page " + std::to_string(key_.page) +
                           " could not be read"
                     : fetched.error;
        return false;
    }
    if (key_.index >= fetched.page->rows.size() ||
        key_.index >= fetched.page->ids.size()) {
        error_ = "lazy row: page " + std::to_string(key_.page) + " holds " +
                 std::to_string(fetched.page->rows.size()) + " rows, wanted row " +
                 std::to_string(key_.index);
        return false;
    }

    page_ = std::move(fetched.page);
    row_ = &page_->rows[static_cast<std::size_t>(key_.index)];
    id_ = &page_->ids[static_cast<std::size_t>(key_.index)];
    error_.clear();
    return true;
}

void ParquetLazyPtr::release() noexcept {
    if (source_.expired()) return;  // borrowed (or sourceless): nothing to re-read through
    row_ = nullptr;
    id_ = nullptr;
    page_.reset();
}

}  // namespace zetta::storage
