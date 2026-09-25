// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Child process control for the multiprocess proofs. Deliberately blocks rather
// than waiting with a deadline: if a child never reports readiness the read
// either sees end of file (the child died) or it blocks, and a block is a
// defect to diagnose rather than something to paper over.

#ifndef RNF_TEST_PROCESS_HPP
#define RNF_TEST_PROCESS_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "rnf/core/status.hpp"

namespace rnf::test {

struct SpawnOptions {
    std::string executable;
    std::vector<std::string> arguments;
    std::filesystem::path working_directory;
    bool capture_output = true;
};

/// A real operating system process. Every method is safe to call from one
/// thread; kill() may be called from another while a read is blocked only on
/// the same thread as the read.
class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&& other) noexcept;

    [[nodiscard]] static Result<ChildProcess> spawn(const SpawnOptions& options);

    [[nodiscard]] long pid() const noexcept { return pid_; }
    [[nodiscard]] bool valid() const noexcept { return process_ != nullptr; }
    [[nodiscard]] bool running() const;

    /// Read until a line starting with prefix is seen. Returns false at end of
    /// file, which is how a child that died before reporting readiness is
    /// detected.
    [[nodiscard]] bool wait_for_line(const std::string& prefix, std::string& line);

    /// Read whatever the child has already written, without blocking.
    [[nodiscard]] std::string read_available();

    /// Wait for exit and return the exit code, or -1 when the child was killed.
    [[nodiscard]] int wait();

    /// Immediately and unconditionally terminate the child. This is a hard
    /// kill: no destructors, no flush, no clean shutdown marker.
    void kill_hard();

    /// Everything the child has written so far, draining the pipe.
    [[nodiscard]] std::string drain();

private:
    void close_read_end();
    void close_process();

    void* process_ = nullptr;
    void* thread_ = nullptr;
    std::intptr_t read_fd_ = -1;
    long pid_ = 0;
    mutable std::string buffer_;
    int exit_code_ = -1;
    bool exited_ = false;
    bool killed_ = false;
};

/// Path of an executable built by this project, resolved from the path the test
/// binary was told about at configure time.
[[nodiscard]] std::string tool_path(std::string_view name);

}  // namespace rnf::test

#endif  // RNF_TEST_PROCESS_HPP
