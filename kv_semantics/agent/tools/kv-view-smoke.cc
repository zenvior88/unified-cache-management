/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include "KVClientAgent.h"

namespace {
volatile std::sig_atomic_t stop_requested = 0;
void stop(int) { stop_requested = 1; }

struct Options {
    agent::KVClientAgentConfig config;
    std::string_view ns;
    const char* log_dir = nullptr;
    int runtime = 0;
    int print_interval_ms = 250;
};

bool parse_options(int argc, char** argv, Options& opts)
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view key = argv[i];
        if (++i == argc || std::string_view(argv[i]).empty()) { return false; }
        const std::string_view value = argv[i];
        if (key == "--mon-host") {
            opts.config.mon_host = value;
        } else if (key == "--client-name") {
            opts.config.client_name = value;
        } else if (key == "--client-key") {
            opts.config.client_key = value;
        } else if (key == "--ns") {
            opts.ns = value;
        } else if (key == "--log-dir") {
            opts.log_dir = argv[i];
        } else {
            int* field = nullptr;
            if (key == "--runtime") {
                field = &opts.runtime;
            } else if (key == "--print-interval-ms") {
                field = &opts.print_interval_ms;
            } else if (key == "--view-refresh-interval") {
                field = &opts.config.view_refresh_interval;
            } else if (key == "--view-fetch-timeout") {
                field = &opts.config.view_fetch_timeout;
            } else if (key == "--view-fallback-delay") {
                field = &opts.config.view_fallback_delay;
            } else {
                return false;
            }
            const auto end = value.data() + value.size();
            const auto [ptr, ec] = std::from_chars(value.data(), end, *field);
            if (ec != std::errc{} || ptr != end || *field < 0) { return false; }
        }
    }
    return opts.print_interval_ms > 0 && !opts.ns.empty() && !opts.config.mon_host.empty() &&
           !opts.config.client_name.empty() && !opts.config.client_key.empty();
}

using View = std::map<int32_t, std::vector<std::string>>;

void print_ids(const char* label, const std::vector<int32_t>& ids)
{
    std::cout << ' ' << label << "=[";
    const char* sep = "";
    for (const auto id : ids) {
        std::cout << sep << id;
        sep = ",";
    }
    std::cout << ']';
}

void print_view(std::string_view ns, uint64_t epoch, const View& view, const View& previous,
                bool initial)
{
    std::cout << "[view] ns=" << ns << " epoch=" << epoch << " up_in=" << view.size();
    if (!initial && view == previous) {
        std::cout << " unchanged\n";
        return;
    }
    if (!initial) {
        std::vector<int32_t> added, removed, changed;
        for (const auto& [id, addrs] : view) {
            const auto it = previous.find(id);
            if (it == previous.end()) {
                added.push_back(id);
            } else if (it->second != addrs) {
                changed.push_back(id);
            }
        }
        for (const auto& [id, addrs] : previous) {
            if (!view.contains(id)) { removed.push_back(id); }
        }
        print_ids("added", added);
        print_ids("removed", removed);
        print_ids("addresses_changed", changed);
    }
    std::cout << '\n';
    if (view.empty()) { return; }
    std::cout << "  OSD  UP  IN  ADDRESSES\n";
    for (const auto& [id, addrs] : view) {
        std::cout << "  " << id << "  Y   Y   ";
        const char* sep = "";
        for (const auto& addr : addrs) {
            std::cout << sep << addr;
            sep = ", ";
        }
        if (addrs.empty()) { std::cout << '-'; }
        std::cout << '\n';
    }
}
}  // namespace

int main(int argc, char** argv)
{
    Options opts;
    if (!parse_options(argc, argv, opts)) {
        std::cerr << "Usage: kv-view-smoke --mon-host <hosts> --client-name <name> "
                     "--client-key <key> --ns <name> [--runtime <seconds; 0=continuous>] "
                     "[--print-interval-ms <positive milliseconds; default 250>] "
                     "[--view-refresh-interval <seconds>] [--view-fetch-timeout <seconds>] "
                     "[--view-fallback-delay <seconds>] [--log-dir <directory>]\n";
        return EXIT_FAILURE;
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    const auto& config = opts.config;
    std::cout << "[start] ns=" << opts.ns << " runtime=";
    if (opts.runtime == 0) {
        std::cout << "continuous";
    } else {
        std::cout << opts.runtime << 's';
    }
    std::cout << " refresh="
              << (config.view_refresh_interval ? config.view_refresh_interval
                                               : KV_VIEW_REFRESH_INTERVAL_DEFAULT)
              << "s fetch_timeout="
              << (config.view_fetch_timeout ? config.view_fetch_timeout
                                            : KV_VIEW_FETCH_TIMEOUT_DEFAULT)
              << "s fallback="
              << (config.view_fallback_delay ? config.view_fallback_delay
                                             : KV_VIEW_FALLBACK_DELAY_DEFAULT)
              << "s print_interval=" << opts.print_interval_ms << "ms" << std::endl;

    agent::KVClientAgent client;
    if (const int rc = client.init(config, opts.log_dir); rc != 0) {
        std::cerr << "[error] agent init failed rc=" << rc << '\n';
        return EXIT_FAILURE;
    }
    if (const int rc = client.start(); rc != 0) {
        std::cerr << "[error] agent start failed rc=" << rc << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "[agent] started; observing initial OSDMap and background refresh" << std::endl;

    using namespace std::chrono;
    const auto deadline = steady_clock::now() + seconds(opts.runtime);
    const std::string ns(opts.ns);
    uint64_t last_epoch = 0;
    uint64_t epoch_advances = 0;
    View previous;
    while (!stop_requested) {
        const auto epoch = client.get_epoch();
        if (epoch != 0) {
            View view;
            for (auto& node : client.get_up_in_by_ns(ns)) {
                std::sort(node.addrs.begin(), node.addrs.end());
                view.emplace(node.osd_id, std::move(node.addrs));
            }
            // Skip samples that straddle a refresh of the Agent's cached map.
            if (client.get_epoch() == epoch && (epoch != last_epoch || view != previous)) {
                if (last_epoch == 0) {
                    std::cout << "[ready] initial OSDMap received epoch=" << epoch << '\n';
                } else if (epoch > last_epoch) {
                    ++epoch_advances;
                }
                print_view(ns, epoch, view, previous, last_epoch == 0);
                std::cout << std::flush;
                previous = std::move(view);
                last_epoch = epoch;
            }
        }
        auto next = steady_clock::now() + milliseconds(opts.print_interval_ms);
        if (opts.runtime > 0) {
            if (steady_clock::now() >= deadline) { break; }
            next = std::min(next, deadline);
        }
        // Keep signal handling responsive even with a long print interval.
        while (!stop_requested) {
            const auto now = steady_clock::now();
            if (now >= next) { break; }
            std::this_thread::sleep_until(std::min(next, now + milliseconds(250)));
        }
    }
    std::cout << "[exit] reason=" << (stop_requested ? "signal" : "runtime_elapsed")
              << " initial_map=" << (last_epoch ? "yes" : "no") << " last_epoch=" << last_epoch
              << " epoch_advances=" << epoch_advances << std::endl;
    if (last_epoch == 0) {
        std::cerr << "[error] no initial OSDMap observed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
