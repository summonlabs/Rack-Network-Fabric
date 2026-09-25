// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "tests/support/harness.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <random>
#include <system_error>
#include <utility>

namespace rnf::test {
namespace {

struct Case {
    std::string suite;
    std::string name;
    TestFunction function;
};

std::vector<Case>& cases() {
    static std::vector<Case> registry;
    return registry;
}

struct RunState {
    std::size_t failures = 0;
    std::string current;
    std::uint64_t seed = 0;
    bool verbose = false;
};

RunState& state() {
    static RunState run;
    return run;
}

std::vector<std::filesystem::path>& temp_directories() {
    static std::vector<std::filesystem::path> directories;
    return directories;
}

std::vector<std::function<void()>>& cleanups() {
    static std::vector<std::function<void()>> actions;
    return actions;
}

struct AbortTest {};

}  // namespace

bool register_test(std::string_view suite, std::string_view name, TestFunction function) {
    cases().push_back(Case{std::string(suite), std::string(name), function});
    return true;
}

void report_failure(std::string_view file, int line, const std::string& message) {
    ++state().failures;
    std::cout << "FAIL " << state().current << " (" << file << ":" << line << ")\n"
              << "     " << message << "\n"
              << "     seed=" << state().seed << "\n";
    std::cout.flush();
}

void abort_test(std::string_view file, int line, const std::string& message) {
    report_failure(file, line, message);
    throw AbortTest{};
}

std::uint64_t current_seed() noexcept {
    return state().seed;
}

bool verbose() noexcept {
    return state().verbose;
}

std::filesystem::path make_temp_directory(std::string_view label) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    static std::atomic<unsigned> counter{0};
    const unsigned index = counter.fetch_add(1, std::memory_order_relaxed);
    std::filesystem::path base = std::filesystem::temp_directory_path();
    std::filesystem::path path = base / ("rnf-test-" + std::string(label) + "-" +
                                         std::to_string(now) + "-" + std::to_string(index));
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path, ec);
    temp_directories().push_back(path);
    return path;
}

void cleanup_temp_directories() {
    for (const std::filesystem::path& path : temp_directories()) {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    temp_directories().clear();
}

void defer_cleanup(std::function<void()> action) {
    cleanups().push_back(std::move(action));
}

void run_deferred_cleanups() {
    std::vector<std::function<void()>> actions;
    actions.swap(cleanups());
    for (auto it = actions.rbegin(); it != actions.rend(); ++it) {
        try {
            (*it)();
        } catch (const std::exception& error) {
            report_failure(__FILE__, __LINE__,
                           std::string("cleanup threw: ") + error.what());
        }
    }
}

int run_all(int argc, char** argv) {
    std::string suite_filter;
    std::string name_filter;
    std::uint64_t seed = 0x5EED1234ULL;
    if (const char* env = std::getenv("RNF_TEST_SEED")) {
        seed = std::strtoull(env, nullptr, 10);
    }
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        auto next = [&]() -> std::string {
            return i + 1 < argc ? std::string(argv[++i]) : std::string();
        };
        if (argument == "--suite") {
            suite_filter = next();
        } else if (argument == "--test") {
            name_filter = next();
        } else if (argument == "--seed") {
            seed = std::strtoull(next().c_str(), nullptr, 10);
        } else if (argument == "--verbose" || argument == "-v") {
            state().verbose = true;
        } else if (argument == "--list") {
            for (const Case& entry : cases()) {
                std::cout << entry.suite << "." << entry.name << "\n";
            }
            return 0;
        } else {
            std::cout << "unknown argument: " << argument << "\n";
            return 2;
        }
    }
    state().seed = seed;

    std::vector<Case> selected;
    for (const Case& entry : cases()) {
        if (!suite_filter.empty() && entry.suite != suite_filter) {
            continue;
        }
        if (!name_filter.empty() && entry.name != name_filter) {
            continue;
        }
        selected.push_back(entry);
    }
    std::sort(selected.begin(), selected.end(), [](const Case& a, const Case& b) {
        if (a.suite != b.suite) {
            return a.suite < b.suite;
        }
        return a.name < b.name;
    });

    std::size_t passed = 0;
    std::size_t failed = 0;
    for (const Case& entry : selected) {
        state().current = entry.suite + "." + entry.name;
        const std::size_t before = state().failures;
        if (state().verbose) {
            std::cout << "RUN  " << state().current << "\n";
        }
        try {
            entry.function();
        } catch (const AbortTest&) {
            // already reported
        } catch (const std::exception& error) {
            report_failure(__FILE__, __LINE__,
                           std::string("uncaught exception: ") + error.what());
        } catch (...) {
            report_failure(__FILE__, __LINE__, "uncaught non-standard exception");
        }
        run_deferred_cleanups();
        if (state().failures == before) {
            ++passed;
        } else {
            ++failed;
        }
    }
    cleanup_temp_directories();

    std::cout << "\n" << passed << " passed, " << failed << " failed, " << selected.size()
              << " selected, seed=" << seed << "\n";
    std::cout.flush();
    return failed == 0 ? 0 : 1;
}

}  // namespace rnf::test

int main(int argc, char** argv) {
    return ::rnf::test::run_all(argc, argv);
}

