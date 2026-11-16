// SPDX-License-Identifier: MIT

#pragma once

#include <utility>

#include <unistd.h>

// Internal to src/core/data: one owned POSIX file descriptor, closed when replaced
// or destroyed. Used by the import and export roots (import_path.cpp,
// export_path.cpp), which walk a caller-named path one openat() at a time.
namespace zetta::data::detail {

class UniqueDescriptor {
public:
    explicit UniqueDescriptor(int value = -1) : value_(value) {}
    UniqueDescriptor(const UniqueDescriptor&) = delete;
    UniqueDescriptor& operator=(const UniqueDescriptor&) = delete;
    UniqueDescriptor(UniqueDescriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    UniqueDescriptor& operator=(UniqueDescriptor&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    ~UniqueDescriptor() { reset(); }

    int get() const { return value_; }
    bool valid() const { return value_ >= 0; }

private:
    void reset() {
        if (value_ >= 0) ::close(value_);
        value_ = -1;
    }
    int value_;
};

}  // namespace zetta::data::detail
