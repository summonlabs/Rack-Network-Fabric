// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Exclusive directory lock. The operating system releases the lock when the
// owning process dies, so a hard kill never leaves a lock that blocks the next
// start.

#include <utility>

#include "rnf/persist/store.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace rnf {

FileLock::~FileLock() {
    release();
}

FileLock::FileLock(FileLock&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
    if (this != &other) {
        release();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

bool FileLock::held() const noexcept {
    return handle_ != nullptr;
}

void FileLock::release() noexcept {
    if (handle_ == nullptr) {
        return;
    }
#ifdef _WIN32
    ::CloseHandle(static_cast<HANDLE>(handle_));
#else
    const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
    ::flock(fd, LOCK_UN);
    ::close(fd);
#endif
    handle_ = nullptr;
}

}  // namespace rnf
