// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "tests/support/process.hpp"

#include <cstring>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace rnf::test {
namespace {

#ifdef _WIN32
std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(),
                          size);
    return out;
}

std::string quote_argument(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '\"') {
            out += "\\\"";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\"');
    return out;
}
#endif

}  // namespace

ChildProcess::~ChildProcess() {
    if (valid() && running()) {
        kill_hard();
        (void)wait();
    }
    close_read_end();
    close_process();
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : process_(other.process_),
      thread_(other.thread_),
      read_fd_(other.read_fd_),
      pid_(other.pid_),
      buffer_(std::move(other.buffer_)),
      exit_code_(other.exit_code_),
      exited_(other.exited_),
      killed_(other.killed_) {
    other.process_ = nullptr;
    other.thread_ = nullptr;
    other.read_fd_ = -1;
    other.pid_ = 0;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
        if (valid() && running()) {
            kill_hard();
            (void)wait();
        }
        close_read_end();
        close_process();
        process_ = other.process_;
        thread_ = other.thread_;
        read_fd_ = other.read_fd_;
        pid_ = other.pid_;
        buffer_ = std::move(other.buffer_);
        exit_code_ = other.exit_code_;
        exited_ = other.exited_;
        killed_ = other.killed_;
        other.process_ = nullptr;
        other.thread_ = nullptr;
        other.read_fd_ = -1;
        other.pid_ = 0;
    }
    return *this;
}

void ChildProcess::close_read_end() {
    if (read_fd_ < 0) {
        return;
    }
#ifdef _WIN32
    ::CloseHandle(reinterpret_cast<HANDLE>(read_fd_));
#else
    ::close(static_cast<int>(read_fd_));
#endif
    read_fd_ = -1;
}

void ChildProcess::close_process() {
#ifdef _WIN32
    if (thread_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(thread_));
        thread_ = nullptr;
    }
    if (process_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(process_));
        process_ = nullptr;
    }
#endif
}

Result<ChildProcess> ChildProcess::spawn(const SpawnOptions& options) {
    ChildProcess child;
#ifdef _WIN32
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE read_handle = nullptr;
    HANDLE write_handle = nullptr;
    if (options.capture_output) {
        if (::CreatePipe(&read_handle, &write_handle, &attributes, 0) == 0) {
            return Status(StatusCode::kIo, "cannot create a pipe for the child process");
        }
        ::SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (options.capture_output) {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdOutput = write_handle;
        startup.hStdError = write_handle;
        startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION info{};

    std::string command = quote_argument(options.executable);
    for (const std::string& argument : options.arguments) {
        command.push_back(' ');
        command += quote_argument(argument);
    }
    std::wstring wide_command = widen(command);
    std::wstring wide_directory =
        options.working_directory.empty() ? std::wstring() : options.working_directory.wstring();

    const BOOL created =
        ::CreateProcessW(nullptr, wide_command.data(), nullptr, nullptr,
                         options.capture_output ? TRUE : FALSE, CREATE_NO_WINDOW, nullptr,
                         options.working_directory.empty() ? nullptr : wide_directory.c_str(),
                         &startup, &info);
    if (options.capture_output) {
        ::CloseHandle(write_handle);
    }
    if (created == 0) {
        if (options.capture_output) {
            ::CloseHandle(read_handle);
        }
        return Status(StatusCode::kIo, "cannot start " + options.executable);
    }
    child.process_ = info.hProcess;
    child.thread_ = info.hThread;
    child.pid_ = static_cast<long>(info.dwProcessId);
    child.read_fd_ = options.capture_output ? reinterpret_cast<std::intptr_t>(read_handle) : -1;
#else
    int fds[2] = {-1, -1};
    if (options.capture_output && ::pipe(fds) != 0) {
        return Status(StatusCode::kIo, "cannot create a pipe for the child process");
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        return Status(StatusCode::kIo, "cannot fork");
    }
    if (pid == 0) {
        if (options.capture_output) {
            ::close(fds[0]);
            ::dup2(fds[1], STDOUT_FILENO);
            ::dup2(fds[1], STDERR_FILENO);
            ::close(fds[1]);
        }
        if (!options.working_directory.empty()) {
            if (::chdir(options.working_directory.string().c_str()) != 0) {
                ::_exit(127);
            }
        }
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(options.executable.c_str()));
        for (const std::string& argument : options.arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(options.executable.c_str(), argv.data());
        ::_exit(127);
    }
    if (options.capture_output) {
        ::close(fds[1]);
        child.read_fd_ = fds[0];
    }
    child.pid_ = static_cast<long>(pid);
    child.process_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(pid));
#endif
    return child;
}

bool ChildProcess::running() const {
#ifdef _WIN32
    if (process_ == nullptr) {
        return false;
    }
    return ::WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT;
#else
    if (process_ == nullptr || exited_) {
        return false;
    }
    int status = 0;
    const pid_t result = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    return result == 0;
#endif
}

std::string ChildProcess::read_available() {
    if (read_fd_ < 0) {
        return {};
    }
    std::string out;
    char chunk[4096];
#ifdef _WIN32
    const HANDLE handle = reinterpret_cast<HANDLE>(read_fd_);
    DWORD available = 0;
    while (::PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr) != 0 &&
           available > 0) {
        DWORD read = 0;
        const DWORD request = available < sizeof(chunk) ? available : sizeof(chunk);
        if (::ReadFile(handle, chunk, request, &read, nullptr) == 0 || read == 0) {
            break;
        }
        out.append(chunk, read);
    }
#else
    for (;;) {
        const ssize_t count = ::read(static_cast<int>(read_fd_), chunk, sizeof(chunk));
        if (count <= 0) {
            break;
        }
        out.append(chunk, static_cast<std::size_t>(count));
    }
#endif
    return out;
}

bool ChildProcess::wait_for_line(const std::string& prefix, std::string& line) {
    for (;;) {
        const std::size_t newline = buffer_.find('\n');
        if (newline != std::string::npos) {
            std::string candidate = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            if (!candidate.empty() && candidate.back() == '\r') {
                candidate.pop_back();
            }
            if (candidate.rfind(prefix, 0) == 0) {
                line = candidate;
                return true;
            }
            continue;
        }
        if (read_fd_ < 0) {
            return false;
        }
#ifdef _WIN32
        const HANDLE handle = reinterpret_cast<HANDLE>(read_fd_);
        char chunk[1024];
        DWORD read = 0;
        if (::ReadFile(handle, chunk, sizeof(chunk), &read, nullptr) == 0 || read == 0) {
            return false;
        }
        buffer_.append(chunk, read);
#else
        char chunk[1024];
        const ssize_t count = ::read(static_cast<int>(read_fd_), chunk, sizeof(chunk));
        if (count <= 0) {
            return false;
        }
        buffer_.append(chunk, static_cast<std::size_t>(count));
#endif
    }
}

std::string ChildProcess::drain() {
    std::string out = buffer_;
    buffer_.clear();
    out += read_available();
    return out;
}

int ChildProcess::wait() {
    if (exited_) {
        return exit_code_;
    }
#ifdef _WIN32
    if (process_ == nullptr) {
        return -1;
    }
    ::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
    exited_ = true;
    exit_code_ = killed_ ? -1 : static_cast<int>(code);
#else
    if (process_ == nullptr) {
        return -1;
    }
    int status = 0;
    ::waitpid(static_cast<pid_t>(pid_), &status, 0);
    exited_ = true;
    if (WIFSIGNALED(status)) {
        exit_code_ = -1;
    } else {
        exit_code_ = WEXITSTATUS(status);
    }
#endif
    close_read_end();
    return exit_code_;
}

void ChildProcess::kill_hard() {
    if (!valid()) {
        return;
    }
    killed_ = true;
#ifdef _WIN32
    ::TerminateProcess(static_cast<HANDLE>(process_), 0xDEADU);
#else
    ::kill(static_cast<pid_t>(pid_), SIGKILL);
#endif
}

std::string tool_path(std::string_view name) {
#ifdef RNF_TEST_TOOL_DIR
    std::filesystem::path directory = RNF_TEST_TOOL_DIR;
    std::filesystem::path candidate = directory / std::string(name);
    if (std::filesystem::exists(candidate)) {
        return candidate.string();
    }
#ifdef _WIN32
    candidate = directory / (std::string(name) + ".exe");
    if (std::filesystem::exists(candidate)) {
        return candidate.string();
    }
#endif
    return candidate.string();
#else
    return std::string(name);
#endif
}

}  // namespace rnf::test

