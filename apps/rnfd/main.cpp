// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// rnfd - the rack authority daemon.
//
// The daemon owns one rack. It binds a loopback TCP port, serves the protocol
// in rnf/net/protocol.hpp, and keeps its state in a versioned, checksummed
// write-ahead log plus checkpoints. It transmits nothing to any endpoint other
// than the loopback port it prints at start-up.

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "rnf/core/log.hpp"
#include "rnf/net/socket.hpp"
#include "rnf/runtime/daemon.hpp"
#include "rnf/runtime/server.hpp"
#include "rnf/version.hpp"

namespace {

std::atomic<bool> g_stop{false};

void handle_signal(int) {
    g_stop.store(true, std::memory_order_release);
}

void print_usage() {
    std::cout <<
        "rnfd " << rnf::kVersionString << " - rack authority daemon\n"
        "\n"
        "Usage: rnfd --rack <id> [options]\n"
        "\n"
        "Required:\n"
        "  --rack <decimal id>        rack identity this daemon is authoritative for\n"
        "\n"
        "Options:\n"
        "  --name <text>              human readable rack name (default: rnfd)\n"
        "  --store <directory>        durable state directory (default: none, memory only)\n"
        "  --port <0-65535>           loopback port, 0 asks the OS (default: 0)\n"
        "  --host <address>           loopback address (default: 127.0.0.1)\n"
        "  --headroom <units>         capacity held back from grants (default: 0)\n"
        "  --max-connections <n>      concurrent connection bound (default: 32)\n"
        "  --max-frame <bytes>        frame payload bound (default: 1048576)\n"
        "  --checkpoint-every <n>     log records between checkpoints, 0 disables (default: 4096)\n"
        "  --default-ttl-ms <n>       lease lifetime applied when a request asks for 0\n"
        "  --auto-compose             recompose after every accepted evidence batch\n"
        "  --no-fsync                 keep the log in the page cache only (benchmarks only)\n"
        "  --log-level <level>        trace, debug, info, warn, error, off\n"
        "  --help                     print this message\n";
}

struct Options {
    rnf::DaemonConfig config;
    rnf::ServerConfig server;
    bool valid = true;
    std::string error;
};

bool parse_u64(std::string_view text, std::uint64_t& out) {
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (UINT64_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    out = value;
    return true;
}

Options parse_options(int argc, char** argv) {
    Options options;
    bool rack_set = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag = argv[i];
        const bool has_value = i + 1 < argc;
        auto need = [&](std::string_view name) -> std::string_view {
            if (!has_value) {
                options.valid = false;
                options.error = std::string(name) + " requires a value";
                return {};
            }
            ++i;
            return argv[i];
        };
        std::uint64_t number = 0;
        if (flag == "--help" || flag == "-h") {
            print_usage();
            std::exit(0);
        } else if (flag == "--rack") {
            if (!parse_u64(need("--rack"), number) || number == 0) {
                options.valid = false;
                options.error = "--rack needs a positive decimal identity";
                break;
            }
            options.config.rack.rack = rnf::RackId(number);
            rack_set = true;
        } else if (flag == "--name") {
            options.config.rack.name = std::string(need("--name"));
        } else if (flag == "--store") {
            options.config.store_directory = std::string(need("--store"));
        } else if (flag == "--port") {
            if (!parse_u64(need("--port"), number) || number > 65535) {
                options.valid = false;
                options.error = "--port needs a number between 0 and 65535";
                break;
            }
            options.config.port = static_cast<std::uint16_t>(number);
        } else if (flag == "--host") {
            options.config.bind_host = std::string(need("--host"));
        } else if (flag == "--headroom") {
            if (!parse_u64(need("--headroom"), number)) {
                options.valid = false;
                options.error = "--headroom needs a number";
                break;
            }
            options.config.rack.headroom_floor = rnf::Capacity{number};
        } else if (flag == "--max-connections") {
            if (!parse_u64(need("--max-connections"), number) || number == 0) {
                options.valid = false;
                options.error = "--max-connections needs a positive number";
                break;
            }
            options.server.max_connections = static_cast<std::size_t>(number);
            options.config.max_connections = static_cast<std::size_t>(number);
        } else if (flag == "--max-frame") {
            if (!parse_u64(need("--max-frame"), number) || number == 0 ||
                number > rnf::kHardMaxFramePayload) {
                options.valid = false;
                options.error = "--max-frame is out of range";
                break;
            }
            options.server.max_frame_payload = static_cast<std::uint32_t>(number);
            options.config.max_frame_payload = static_cast<std::uint32_t>(number);
        } else if (flag == "--checkpoint-every") {
            if (!parse_u64(need("--checkpoint-every"), number)) {
                options.valid = false;
                options.error = "--checkpoint-every needs a number";
                break;
            }
            options.config.checkpoint_every_records = number;
        } else if (flag == "--default-ttl-ms") {
            if (!parse_u64(need("--default-ttl-ms"), number) || number == 0) {
                options.valid = false;
                options.error = "--default-ttl-ms needs a positive number";
                break;
            }
            options.config.default_ttl_ms = number;
        } else if (flag == "--auto-compose") {
            options.config.auto_compose = true;
        } else if (flag == "--no-fsync") {
            options.config.fsync_on_append = false;
        } else if (flag == "--log-level") {
            rnf::LogLevel level = rnf::LogLevel::kInfo;
            const std::string_view text = need("--log-level");
            if (!rnf::parse_log_level(text, level)) {
                options.valid = false;
                options.error = "unknown log level";
                break;
            }
            rnf::Logger::instance().set_level(level);
        } else {
            options.valid = false;
            options.error = "unknown option " + std::string(flag);
            break;
        }
    }
    if (options.valid && !rack_set) {
        options.valid = false;
        options.error = "--rack is required";
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    rnf::ensure_socket_runtime_noexcept();
    const Options options = parse_options(argc, argv);
    if (!options.valid) {
        std::cerr << "rnfd: " << options.error << "\n";
        print_usage();
        return 2;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    rnf::DaemonCore core(options.config);
    const rnf::Status started = core.start();
    if (!started.ok()) {
        std::cerr << "rnfd: cannot start: " << started.describe() << "\n";
        return 1;
    }

    rnf::DaemonServer server(core, options.server);
    const rnf::Status bound = server.bind(options.config.bind_host, options.config.port);
    if (!bound.ok()) {
        std::cerr << "rnfd: cannot bind: " << bound.describe() << "\n";
        return 1;
    }

    const rnf::StateResponse state_view = [&]() {
        const rnf::Result<rnf::StateResponse> state = core.query_state();
        return state.has_value() ? *state : rnf::StateResponse{};
    }();
    const rnf::ControllerIncarnation incarnation = core.incarnation();

    std::cout << "RNFD READY port=" << server.port() << " rack="
              << options.config.rack.rack.to_hex() << " incarnation=" << incarnation.value
              << " generation=" << state_view.generation.value << " epoch="
              << state_view.epoch.value << " lifecycle="
              << rnf::to_string(state_view.lifecycle) << " pid=";
    std::cout.flush();
#ifdef _WIN32
    std::cout << static_cast<unsigned long>(::GetCurrentProcessId());
#else
    std::cout << static_cast<unsigned long>(::getpid());
#endif
    std::cout << "\n";
    std::cout.flush();

    const rnf::Status served = server.run(&g_stop);
    const rnf::Status stopped = core.stop(static_cast<rnf::TimestampMs>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count()));
    if (!served.ok()) {
        std::cerr << "rnfd: server stopped with " << served.describe() << "\n";
        return 1;
    }
    if (!stopped.ok()) {
        std::cerr << "rnfd: shutdown failed: " << stopped.describe() << "\n";
        return 1;
    }
    std::cout << "RNFD STOPPED\n";
    return 0;
}
