#include "zetta/api/background.hpp"

#include <algorithm>
#include <utility>

namespace zetta::api {

void BackgroundActions::adopt(std::shared_ptr<void> handle,
                              std::function<bool()> done) {
    if (!handle || !done) return;
    entries_.push_back(Entry{std::move(handle), std::move(done)});
}

void BackgroundActions::prune() {
    std::erase_if(entries_, [](const Entry& entry) { return entry.done(); });
}

}  // namespace zetta::api
