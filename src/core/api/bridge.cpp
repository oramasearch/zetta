#include "zetta/api/bridge.hpp"

#include <utility>
#include <vector>

namespace zetta::api {

runtime::task<> MainThreadBridge::run_on_owner(std::function<void()> work) {
    auto completed = std::make_shared<runtime::async_event>();
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push_back(PendingWork{std::move(work), completed});
    }
    co_await completed->wait();
}

void MainThreadBridge::drain() {
    std::vector<PendingWork> batch;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        batch.assign(std::make_move_iterator(queue_.begin()),
                     std::make_move_iterator(queue_.end()));
        queue_.clear();
    }
    for (PendingWork& pending_work : batch) {
        pending_work.work();
        pending_work.completed->set();
    }
}

std::size_t MainThreadBridge::pending() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return queue_.size();
}

}  // namespace zetta::api
