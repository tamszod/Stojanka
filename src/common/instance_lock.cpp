#include "common/instance_lock.h"

#include <filesystem>

#include "common/utf8_path.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace strojanka {

InstanceLock::InstanceLock(const std::string& path) : path_(path) {
    std::error_code ec;
    const auto p = utf8_path(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
#if defined(_WIN32)
    // No sharing at all: a second process cannot open the file while we hold it. The OS releases
    // the handle if the process dies, so a crash never leaves a stale lock behind.
    HANDLE h = CreateFileW(p.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        handle_ = h;
        acquired_ = true;
    }
#else
    fd_ = open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd_ >= 0 && flock(fd_, LOCK_EX | LOCK_NB) == 0) acquired_ = true;
#endif
}

InstanceLock::~InstanceLock() {
#if defined(_WIN32)
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
#else
    if (fd_ >= 0) {
        if (acquired_) {
            std::error_code ec;
            std::filesystem::remove(utf8_path(path_), ec);
        }
        close(fd_);
    }
#endif
}

}  // namespace strojanka
