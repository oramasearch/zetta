#include "zetta/storage/parquet_store.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <utility>

#include "zetta/parquet/column_decoder.hpp"
#include "zetta/parquet/metadata.hpp"

namespace zetta::storage {
namespace {

constexpr std::string_view kMagic = "PAR1";

// A decoded row costs more in memory than on disk: JSON objects carry their keys
// and their allocator overhead. The budget is spent in *estimated* footprint, so
// it needs a defensible multiplier rather than a precise one — this is the
// conservative side of what a row group of scalar columns expands to.
constexpr std::size_t kJsonExpansion = 4;
constexpr std::size_t kPerRowOverhead = 64;

}  // namespace

// The pager: the body, its footer, and the bounded cache of decoded pages. One
// page is one Parquet row group.
class ParquetPager final : public RowPageSource,
                           public std::enable_shared_from_this<ParquetPager> {
public:
    ParquetPager(std::filesystem::path path, std::string bytes, std::string display_name,
                 ParquetStoreOptions options)
        : path_(std::move(path)),
          bytes_(std::move(bytes)),
          display_name_(std::move(display_name)),
          options_(std::move(options)) {}

    // Read the footer and the id column. Returns "" on success, else a reason.
    std::string open();

    // --- RowPageSource -----------------------------------------------------

    RowPageResult fetch_page(const RowKey& key) const override;

    // --- the store's queries ------------------------------------------------

    std::size_t rows() const { return ids_.size(); }
    std::size_t page_count() const { return pages_.size(); }
    const std::string& id_at(std::size_t row) const { return ids_[row]; }
    bool contains(const std::string& id) const { return id_index_.contains(id); }

    // The page holding `row`, and the row's index inside it.
    RowKey key_for(std::size_t row) const {
        RowKey key;
        for (std::size_t page = 0; page < pages_.size(); ++page) {
            if (row < pages_[page].first_row + pages_[page].rows) {
                key.page = static_cast<std::uint32_t>(page);
                key.index = row - pages_[page].first_row;
                return key;
            }
        }
        key.page = static_cast<std::uint32_t>(pages_.size());
        key.index = row;
        return key;
    }

    std::size_t resident_bytes() const {
        const std::lock_guard<std::mutex> guard(mutex_);
        return resident_bytes_;
    }
    std::size_t resident_pages() const {
        const std::lock_guard<std::mutex> guard(mutex_);
        return resident_pages_;
    }
    std::uint64_t decodes() const { return decodes_.load(std::memory_order_relaxed); }

    void reap() {
        const std::lock_guard<std::mutex> guard(mutex_);
        for (Slot& slot : slots_) slot.strong.reset();
        resident_bytes_ = 0;
        resident_pages_ = 0;
    }

    void refuse(const std::string& what) {
        const std::lock_guard<std::mutex> guard(mutex_);
        last_mutation_error_ = "parquet store '" + display_name_ + "': " + what +
                               " — the body is immutable; write a new one";
        ++rejected_mutations_;
        std::cerr << last_mutation_error_ << '\n';
    }
    std::string last_mutation_error() const {
        const std::lock_guard<std::mutex> guard(mutex_);
        return last_mutation_error_;
    }
    std::uint64_t rejected_mutations() const {
        const std::lock_guard<std::mutex> guard(mutex_);
        return rejected_mutations_;
    }

private:
    // One row group's place in the file.
    struct PageSpan {
        std::size_t first_row = 0;   // global index of this page's first row
        std::size_t rows = 0;
        std::size_t offset = 0;      // byte span covering every column chunk
        std::size_t length = 0;
        std::size_t footprint = 0;   // estimated decoded size, the budget unit
    };

    // A cache slot. `strong` is the cache's own reference — dropping it is the
    // whole of "reaping"; `weak` survives it so a page a handle still holds is
    // re-adopted instead of decoded again.
    //
    // The design calls for `std::atomic<std::shared_ptr<RowPage>>` so a reader
    // can take a resident page without a lock. **libc++ does not implement that
    // specialization** (P0718; the primary template static-asserts on trivially
    // copyable), and the C++20 `atomic_load(&shared_ptr)` free functions are
    // removed in C++26 — so the slots are guarded by `mutex_` instead. The
    // guarantee is identical (a reader sees the old page or the new one, never a
    // torn pointer); what is lost is the lock-free fast path, which becomes a
    // one-line change the day libc++ ships the specialization.
    struct Slot {
        RowPagePtr strong;                  // guarded by mutex_
        std::weak_ptr<const RowPage> weak;  // guarded by mutex_
        std::uint64_t last_used = 0;        // guarded by mutex_
    };

    std::string read_range(std::size_t offset, std::size_t length, std::string& out) const;
    std::size_t body_size() const;
    // Decode one row group into a page. Non-throwing; no cache involvement.
    RowPageResult decode_page(std::uint32_t page) const;
    // A metadata copy holding only `page`, with every chunk offset rebased to
    // the start of the buffer the caller read that span into.
    zetta::parquet::FileMetaData rebase(std::size_t page, std::size_t base) const;
    // Evict least-recently-used pages until the budget fits (never the page just
    // inserted). Caller holds the mutex.
    void evict_locked(std::size_t keep) const;

    std::filesystem::path path_;      // empty when the body is in memory
    std::string bytes_;               // the body, when in memory
    std::string display_name_;
    ParquetStoreOptions options_;

    zetta::parquet::FileMetaData metadata_;
    std::vector<zetta::parquet::LeafColumn> leaves_;
    std::size_t id_column_ = std::numeric_limits<std::size_t>::max();  // none
    std::vector<PageSpan> pages_;
    std::vector<std::string> ids_;
    std::unordered_map<std::string_view, std::size_t> id_index_;

    mutable std::mutex mutex_;
    mutable std::vector<Slot> slots_;
    mutable std::size_t resident_bytes_ = 0;
    mutable std::size_t resident_pages_ = 0;
    mutable std::atomic<std::uint64_t> decodes_{0};
    mutable std::uint64_t clock_ = 0;  // guarded by mutex_
    mutable std::string last_mutation_error_;
    mutable std::uint64_t rejected_mutations_ = 0;
};

// --- reading the body --------------------------------------------------------

std::size_t ParquetPager::body_size() const {
    if (path_.empty()) return bytes_.size();
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path_, ec);
    if (ec) return 0;
    return static_cast<std::size_t>(size);
}

std::string ParquetPager::read_range(std::size_t offset, std::size_t length,
                                     std::string& out) const {
    out.clear();
    if (path_.empty()) {
        if (offset + length > bytes_.size()) {
            return "parquet store '" + display_name_ + "': range [" +
                   std::to_string(offset) + ", " + std::to_string(offset + length) +
                   ") runs past the body (" + std::to_string(bytes_.size()) + " bytes)";
        }
        out.assign(bytes_, offset, length);
        return {};
    }
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        return "parquet store '" + display_name_ + "': cannot open " + path_.string();
    }
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    out.resize(length);
    in.read(out.data(), static_cast<std::streamsize>(length));
    if (static_cast<std::size_t>(in.gcount()) != length) {
        out.clear();
        return "parquet store '" + display_name_ + "': short read of " +
               std::to_string(length) + " bytes at " + std::to_string(offset);
    }
    return {};
}

std::string ParquetPager::open() {
    const std::size_t size = body_size();
    constexpr std::size_t kTrailer = 8;  // 4-byte footer length + "PAR1"
    if (size < kMagic.size() + kTrailer) {
        return "parquet store '" + display_name_ + "': too small to be a Parquet file (" +
               std::to_string(size) + " bytes)";
    }

    std::string trailer;
    if (std::string error = read_range(size - kTrailer, kTrailer, trailer); !error.empty()) {
        return error;
    }
    if (std::string_view(trailer).substr(4) != kMagic) {
        return "parquet store '" + display_name_ + "': missing trailing PAR1 magic";
    }
    std::uint32_t footer_length = 0;
    std::memcpy(&footer_length, trailer.data(), sizeof(footer_length));
    if (footer_length == 0 || footer_length + kTrailer > size) {
        return "parquet store '" + display_name_ + "': footer length " +
               std::to_string(footer_length) + " does not fit the body";
    }

    // The footer parser needs a buffer whose *end* is the file's end (it walks
    // back from the trailing magic) and whose start carries the leading magic.
    // Reading the tail and prefixing the magic gives it exactly that without
    // reading the body — the whole point of opening a large file this way.
    std::string tail;
    if (std::string error = read_range(size - footer_length - kTrailer,
                                       footer_length + kTrailer, tail);
        !error.empty()) {
        return error;
    }
    const std::string footer_view = std::string(kMagic) + tail;
    const zetta::parquet::ParquetMetadataResult parsed =
        zetta::parquet::parse_parquet_metadata(footer_view);
    if (!parsed.ok()) {
        return "parquet store '" + display_name_ + "': " + parsed.error;
    }
    metadata_ = parsed.metadata;
    leaves_ = metadata_.leaf_columns();

    // Where each row group's bytes live: the span covering all of its column
    // chunks (they are contiguous inside a row group).
    std::size_t first_row = 0;
    for (const zetta::parquet::RowGroupMeta& group : metadata_.row_groups) {
        PageSpan span;
        span.first_row = first_row;
        span.rows = static_cast<std::size_t>(group.num_rows < 0 ? 0 : group.num_rows);
        std::size_t begin = std::numeric_limits<std::size_t>::max();
        std::size_t end = 0;
        std::size_t uncompressed = 0;
        for (const zetta::parquet::ColumnChunkMeta& chunk : group.columns) {
            const auto start = static_cast<std::size_t>(
                chunk.has_dictionary_page ? chunk.dictionary_page_offset
                                          : chunk.data_page_offset);
            const auto stop = start + static_cast<std::size_t>(chunk.total_compressed_size);
            begin = std::min(begin, start);
            end = std::max(end, stop);
            uncompressed += static_cast<std::size_t>(chunk.total_uncompressed_size);
        }
        if (begin == std::numeric_limits<std::size_t>::max() || end <= begin) {
            begin = 0;
            end = 0;
        }
        if (end > size) {
            return "parquet store '" + display_name_ + "': row group " +
                   std::to_string(pages_.size()) + " runs past the body";
        }
        span.offset = begin;
        span.length = end - begin;
        span.footprint = uncompressed * kJsonExpansion + span.rows * kPerRowOverhead;
        first_row += span.rows;
        pages_.push_back(span);
    }
    slots_.resize(pages_.size());

    // The id column, if the body carries one.
    for (std::size_t index = 0; index < leaves_.size(); ++index) {
        if (leaves_[index].path == options_.id_column) {
            if (leaves_[index].type != zetta::parquet::PhysicalType::ByteArray) {
                return "parquet store '" + display_name_ + "': id column '" +
                       options_.id_column + "' is " +
                       std::string(zetta::parquet::to_string(leaves_[index].type)) +
                       ", expected byte_array";
            }
            id_column_ = index;
            break;
        }
    }

    // Ids are resident for the store's lifetime: `contains(id)` and `id_at(i)`
    // are asked without wanting the row, and answering either from a page would
    // mean decoding row data to look up a key. Row *data* is what pages.
    ids_.reserve(first_row);
    for (std::size_t page = 0; page < pages_.size(); ++page) {
        if (id_column_ == std::numeric_limits<std::size_t>::max()) {
            for (std::size_t row = 0; row < pages_[page].rows; ++row) {
                ids_.push_back(display_name_ + "#" + std::to_string(ids_.size()));
            }
            continue;
        }
        std::string buffer;
        const zetta::parquet::ColumnChunkMeta& chunk =
            metadata_.row_groups[page].columns[id_column_];
        const auto chunk_start = static_cast<std::size_t>(
            chunk.has_dictionary_page ? chunk.dictionary_page_offset : chunk.data_page_offset);
        const auto chunk_length = static_cast<std::size_t>(chunk.total_compressed_size);
        if (std::string error = read_range(chunk_start, chunk_length, buffer); !error.empty()) {
            return error;
        }
        const zetta::parquet::FileMetaData rebased = rebase(page, chunk_start);
        const zetta::parquet::DecodedColumnResult decoded =
            zetta::parquet::decode_column_chunk(buffer, rebased, 0, id_column_);
        if (!decoded.ok()) {
            return "parquet store '" + display_name_ + "': id column of row group " +
                   std::to_string(page) + ": " + decoded.error;
        }
        const zetta::parquet::DecodedColumn& column = decoded.column;
        std::size_t next = 0;
        for (std::int64_t row = 0; row < column.num_rows; ++row) {
            if (!column.row_is_valid(row) || next >= column.values.byte_array_values.size()) {
                ids_.push_back(display_name_ + "#" + std::to_string(ids_.size()));
                if (column.row_is_valid(row)) ++next;
                continue;
            }
            ids_.push_back(column.values.byte_array_values[next]);
            ++next;
        }
    }
    if (ids_.size() != first_row) {
        return "parquet store '" + display_name_ + "': id column holds " +
               std::to_string(ids_.size()) + " values, the footer says " +
               std::to_string(first_row) + " rows";
    }
    id_index_.reserve(ids_.size());
    for (std::size_t row = 0; row < ids_.size(); ++row) {
        id_index_.emplace(std::string_view(ids_[row]), row);
    }
    return {};
}

zetta::parquet::FileMetaData ParquetPager::rebase(std::size_t page, std::size_t base) const {
    zetta::parquet::FileMetaData copy = metadata_;
    copy.row_groups.clear();
    zetta::parquet::RowGroupMeta group = metadata_.row_groups[page];
    for (zetta::parquet::ColumnChunkMeta& chunk : group.columns) {
        chunk.data_page_offset -= static_cast<std::int64_t>(base);
        chunk.file_offset -= static_cast<std::int64_t>(base);
        if (chunk.has_dictionary_page) {
            chunk.dictionary_page_offset -= static_cast<std::int64_t>(base);
        }
    }
    copy.num_rows = group.num_rows;
    copy.row_groups.push_back(std::move(group));
    return copy;
}

RowPageResult ParquetPager::decode_page(std::uint32_t page) const {
    RowPageResult out;
    const PageSpan& span = pages_[page];

    std::string buffer;
    if (std::string error = read_range(span.offset, span.length, buffer); !error.empty()) {
        out.error = error;
        return out;
    }
    const zetta::parquet::FileMetaData rebased = rebase(page, span.offset);

    auto decoded = std::make_shared<RowPage>();
    decoded->rows.assign(span.rows, nlohmann::json::object());
    decoded->ids.assign(ids_.begin() + static_cast<std::ptrdiff_t>(span.first_row),
                        ids_.begin() + static_cast<std::ptrdiff_t>(span.first_row + span.rows));
    decoded->resident_bytes = span.footprint;

    for (std::size_t index = 0; index < leaves_.size(); ++index) {
        // The id column is not row data — it is the key, already resident.
        if (options_.hide_id_column && index == id_column_) continue;

        const zetta::parquet::DecodedColumnResult column_result =
            zetta::parquet::decode_column_chunk(buffer, rebased, 0, index);
        if (!column_result.ok()) {
            out.error = "parquet store '" + display_name_ + "': row group " +
                        std::to_string(page) + ", column " + leaves_[index].path + ": " +
                        column_result.error;
            return out;
        }
        const zetta::parquet::DecodedColumn& column = column_result.column;
        if (static_cast<std::size_t>(column.num_rows) != span.rows) {
            out.error = "parquet store '" + display_name_ + "': row group " +
                        std::to_string(page) + ", column " + column.path + " holds " +
                        std::to_string(column.num_rows) + " rows, the footer says " +
                        std::to_string(span.rows);
            return out;
        }

        // Transpose the typed column back into one JSON value per row — the same
        // mapping read_parquet_rows applies, a row group at a time.
        std::size_t next = 0;
        const zetta::parquet::ColumnValues& values = column.values;
        for (std::size_t row = 0; row < span.rows; ++row) {
            nlohmann::json& cell = decoded->rows[row][column.path];
            if (!column.row_is_valid(static_cast<std::int64_t>(row))) {
                cell = nullptr;
                continue;
            }
            switch (column.type) {
                case zetta::parquet::PhysicalType::Boolean:
                    cell = values.bool_values[next] != 0;
                    break;
                case zetta::parquet::PhysicalType::Int32:
                    cell = values.int32_values[next];
                    break;
                case zetta::parquet::PhysicalType::Int64:
                    cell = values.int64_values[next];
                    break;
                case zetta::parquet::PhysicalType::Float:
                    cell = values.float_values[next];
                    break;
                case zetta::parquet::PhysicalType::Double:
                    cell = values.double_values[next];
                    break;
                case zetta::parquet::PhysicalType::ByteArray:
                case zetta::parquet::PhysicalType::FixedLenByteArray:
                    cell = values.byte_array_values[next];
                    break;
                default:
                    out.error = "parquet store '" + display_name_ + "': column " +
                                column.path + " has an unsupported physical type";
                    return out;
            }
            ++next;
        }
    }

    decodes_.fetch_add(1, std::memory_order_relaxed);
    out.page = std::move(decoded);
    return out;
}

RowPageResult ParquetPager::fetch_page(const RowKey& key) const {
    RowPageResult out;
    if (key.page >= pages_.size()) {
        out.error = "parquet store '" + display_name_ + "': no page " +
                    std::to_string(key.page) + " (" + std::to_string(pages_.size()) +
                    " row groups)";
        return out;
    }

    const std::lock_guard<std::mutex> guard(mutex_);
    Slot& slot = slots_[key.page];

    // Still cached.
    if (slot.strong) {
        slot.last_used = ++clock_;
        out.page = slot.strong;
        return out;
    }
    // Reaped, but a handle somewhere still holds it: re-adopt rather than decode
    // a second copy of the same bytes.
    if (RowPagePtr adopted = slot.weak.lock()) {
        slot.strong = adopted;
        slot.last_used = ++clock_;
        resident_bytes_ += adopted->resident_bytes;
        ++resident_pages_;
        evict_locked(key.page);
        out.page = std::move(adopted);
        return out;
    }

    RowPageResult decoded = decode_page(key.page);
    if (!decoded.ok()) return decoded;
    slot.strong = decoded.page;
    slot.weak = decoded.page;
    slot.last_used = ++clock_;
    resident_bytes_ += decoded.page->resident_bytes;
    ++resident_pages_;
    evict_locked(key.page);
    return decoded;
}

void ParquetPager::evict_locked(std::size_t keep) const {
    while (resident_bytes_ > options_.page_cache_bytes && resident_pages_ > 0) {
        std::size_t victim = slots_.size();
        std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
        for (std::size_t page = 0; page < slots_.size(); ++page) {
            if (page == keep || !slots_[page].strong) continue;
            if (slots_[page].last_used < oldest) {
                oldest = slots_[page].last_used;
                victim = page;
            }
        }
        if (victim == slots_.size()) return;  // only the just-inserted page is left
        resident_bytes_ -= std::min(resident_bytes_, slots_[victim].strong->resident_bytes);
        slots_[victim].strong.reset();  // the cache lets go; the weak_ptr stays
        --resident_pages_;
    }
}

// --- the store ---------------------------------------------------------------

ParquetStore::ParquetStore(std::shared_ptr<ParquetPager> pager) : pager_(std::move(pager)) {}

ParquetStore::~ParquetStore() = default;

ParquetStoreResult ParquetStore::open(const std::filesystem::path& path,
                                      ParquetStoreOptions options) {
    ParquetStoreResult result;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        result.error = "parquet store: no such file: " + path.string();
        return result;
    }
    auto pager = std::make_shared<ParquetPager>(path, std::string{},
                                                path.filename().string(), std::move(options));
    if (std::string error = pager->open(); !error.empty()) {
        result.error = std::move(error);
        return result;
    }
    result.store = std::shared_ptr<ParquetStore>(new ParquetStore(std::move(pager)));
    return result;
}

ParquetStoreResult ParquetStore::open_bytes(std::string bytes, std::string name,
                                            ParquetStoreOptions options) {
    ParquetStoreResult result;
    auto pager = std::make_shared<ParquetPager>(std::filesystem::path{}, std::move(bytes),
                                                std::move(name), std::move(options));
    if (std::string error = pager->open(); !error.empty()) {
        result.error = std::move(error);
        return result;
    }
    result.store = std::shared_ptr<ParquetStore>(new ParquetStore(std::move(pager)));
    return result;
}

void ParquetStore::add(std::string id, nlohmann::json doc) {
    (void)doc;
    pager_->refuse("cannot add row '" + id + "'");
}

void ParquetStore::update(const std::string& id, nlohmann::json doc) {
    (void)doc;
    pager_->refuse("cannot update row '" + id + "'");
}

void ParquetStore::remove(const std::string& id) {
    pager_->refuse("cannot remove row '" + id + "'");
}

bool ParquetStore::contains(const std::string& id) const { return pager_->contains(id); }

std::size_t ParquetStore::size() const { return pager_->rows(); }

ParquetLazyPtr ParquetStore::at(std::size_t i) const {
    if (i >= pager_->rows()) {
        return ParquetLazyPtr::failed("parquet store: row " + std::to_string(i) +
                                      " is past the end (" +
                                      std::to_string(pager_->rows()) + " rows)");
    }
    const RowKey key = pager_->key_for(i);
    RowPageResult fetched = pager_->fetch_page(key);
    if (!fetched.ok()) return ParquetLazyPtr::failed(fetched.error);
    return ParquetLazyPtr(key, std::move(fetched.page),
                          std::weak_ptr<const RowPageSource>(pager_));
}

const std::string& ParquetStore::id_at(std::size_t i) const {
    static const std::string kNone;
    if (i >= pager_->rows()) return kNone;
    return pager_->id_at(i);
}

std::size_t ParquetStore::resident_bytes() const { return pager_->resident_bytes(); }
std::size_t ParquetStore::resident_pages() const { return pager_->resident_pages(); }
std::uint64_t ParquetStore::decode_count() const { return pager_->decodes(); }
std::size_t ParquetStore::page_count() const { return pager_->page_count(); }
void ParquetStore::reap() { pager_->reap(); }
std::string ParquetStore::last_mutation_error() const {
    return pager_->last_mutation_error();
}
std::uint64_t ParquetStore::rejected_mutations() const { return pager_->rejected_mutations(); }

}  // namespace zetta::storage
