#pragma once

#include <string>

namespace strojanka {

// Holds an exclusive lock on a file (normally "<database>.lock") for as long as it lives, so two
// copies of the bot never run against the same database (users would get every alert twice).
class InstanceLock {
public:
    explicit InstanceLock(const std::string& path);
    ~InstanceLock();
    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;

    bool acquired() const { return acquired_; }

private:
    std::string path_;
    bool acquired_ = false;
#if defined(_WIN32)
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

}  // namespace strojanka
