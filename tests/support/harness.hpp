// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// A deliberately small test harness: no external dependency, deterministic
// seeding, and no timeouts anywhere. A test that hangs is a defect to diagnose,
// not something to cut short.

#ifndef RNF_TEST_HARNESS_HPP
#define RNF_TEST_HARNESS_HPP

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "rnf/core/status.hpp"

namespace rnf::test {

using TestFunction = void (*)();

/// Uniform view of a failure, so that checks work whether the callee returns a
/// Status or a Result<T>.
struct StatusView {
    bool ok = true;
    StatusCode code = StatusCode::kOk;
    std::string detail;
};

[[nodiscard]] inline StatusView view(const Status& status) {
    return StatusView{status.ok(), status.code(), status.detail()};
}

template <class T>
[[nodiscard]] inline StatusView view(const Result<T>& result) {
    if (result.has_value()) {
        return StatusView{};
    }
    return StatusView{false, result.error().code(), result.error().detail()};
}

/// Register a test case. Called from a static initialiser.
bool register_test(std::string_view suite, std::string_view name, TestFunction function);

/// Record a failure without aborting the test.
void report_failure(std::string_view file, int line, const std::string& message);

/// Abort the running test after recording a failure.
[[noreturn]] void abort_test(std::string_view file, int line, const std::string& message);

/// Run every registered test that matches the filters. Returns the number of
/// failing tests.
int run_all(int argc, char** argv);

/// Seed for the current test: --seed on the command line, or the value of
/// RNF_TEST_SEED, or a fixed default. Printed on every failure.
[[nodiscard]] std::uint64_t current_seed() noexcept;

/// True when the environment asks for verbose output.
[[nodiscard]] bool verbose() noexcept;

/// Create a unique temporary directory that is removed when the process exits.
[[nodiscard]] std::filesystem::path make_temp_directory(std::string_view label);

/// Remove every temporary directory created by make_temp_directory.
void cleanup_temp_directories();

/// Register a cleanup action that runs after the test that registered it.
void defer_cleanup(std::function<void()> action);

/// Run and clear the deferred cleanups. Called by the harness after each test.
void run_deferred_cleanups();

}  // namespace rnf::test

#define RNF_TEST(suite, name)                                                          \
    static void suite##_##name##_body();                                               \
    namespace {                                                                        \
    const bool suite##_##name##_registered =                                           \
        ::rnf::test::register_test(#suite, #name, &suite##_##name##_body);             \
    }                                                                                  \
    static void suite##_##name##_body()

#define RNF_CHECK(expr)                                                                 \
    do {                                                                                \
        if (!(expr)) {                                                                  \
            ::rnf::test::report_failure(__FILE__, __LINE__, "CHECK failed: " #expr);     \
        }                                                                               \
    } while (false)

#define RNF_CHECK_EQ(a, b)                                                              \
    do {                                                                                \
        const auto& rnf_a = (a);                                                        \
        const auto& rnf_b = (b);                                                        \
        if (!(rnf_a == rnf_b)) {                                                        \
            ::rnf::test::report_failure(__FILE__, __LINE__,                             \
                                        std::string("CHECK_EQ failed: " #a " == " #b)); \
        }                                                                               \
    } while (false)

#define RNF_REQUIRE(expr)                                                               \
    do {                                                                                \
        if (!(expr)) {                                                                  \
            ::rnf::test::abort_test(__FILE__, __LINE__, "REQUIRE failed: " #expr);       \
        }                                                                               \
    } while (false)

/// Require that a Status is ok, printing the detailed message when it is not.
#define RNF_REQUIRE_OK(status)                                                          \
    do {                                                                                \
        const ::rnf::Status rnf_status = (status);                                      \
        if (!rnf_status.ok()) {                                                         \
            ::rnf::test::abort_test(__FILE__, __LINE__,                                 \
                                    std::string("expected ok, got ") +                  \
                                        std::string(rnf_status.describe()));            \
        }                                                                               \
    } while (false)

/// Require that a Result carries a value, aborting with the error otherwise.
#define RNF_REQUIRE_VALUE(dest, expr)                                                   \
    auto dest##_harness = (expr);                                                       \
    if (!dest##_harness.has_value()) {                                                  \
        ::rnf::test::abort_test(__FILE__, __LINE__,                                     \
                                std::string("unexpected failure: ") +                   \
                                    dest##_harness.error().describe());                 \
    }                                                                                   \
    auto& dest = *dest##_harness

/// Require that an expression fails with exactly the expected status code.
#define RNF_REQUIRE_CODE(expr, expected)                                                \
    do {                                                                                \
        const ::rnf::test::StatusView rnf_view = ::rnf::test::view((expr));             \
        if (rnf_view.ok) {                                                              \
            ::rnf::test::abort_test(__FILE__, __LINE__,                                 \
                                    "expected failure " #expected ", got ok");          \
        }                                                                               \
        if (rnf_view.code != (expected)) {                                              \
            ::rnf::test::abort_test(__FILE__, __LINE__,                                 \
                                    std::string("expected " #expected ", got ") +       \
                                        std::string(::rnf::to_string(rnf_view.code)) +  \
                                        ": " + rnf_view.detail);                        \
        }                                                                               \
    } while (false)

#endif  // RNF_TEST_HARNESS_HPP
