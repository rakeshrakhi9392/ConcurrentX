/**
 * ConcurrentX benchmarking harness
 *
 * Profiles ThreadPool throughput, task latency, and scheduling overhead under
 * varying worker counts and workload intensities (light / medium / heavy /
 * reentrant). Emits CSV for scaling analysis.
 *
 * Example:
 *   concurrentx_bench --threads 1,2,4,8,16 --tasks 20000 --output results.csv
 */

#include "concurrentx/execution_context.hpp"
#include "concurrentx/thread_pool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Duration = Clock::duration;
using Microseconds = std::chrono::duration<double, std::micro>;
using Milliseconds = std::chrono::duration<double, std::milli>;

// ---------------------------------------------------------------------------
// CLI configuration
// ---------------------------------------------------------------------------

struct Config {
    std::vector<std::size_t> thread_counts{1, 2, 4, 8, 16};
    std::size_t task_count = 10000;
    std::size_t compute_iters = 1000;   // base loop iterations for "medium"
    std::size_t nested_depth = 2;       // outer + (depth-1) nested submits
    std::size_t warmup_runs = 1;
    std::vector<std::string> workloads{"light", "medium", "heavy", "reentrant"};
    std::string output_path;  // empty => stdout
};

void print_usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " [options]\n"
        << "\n"
        << "  --threads LIST     Comma-separated worker counts "
           "(default: 1,2,4,8,16)\n"
        << "  --tasks N          Tasks submitted per measurement "
           "(default: 10000)\n"
        << "  --compute-iters N  Base compute iterations for medium "
           "(default: 1000)\n"
        << "  --nested-depth D   Nesting levels for reentrant workload "
           "(default: 2)\n"
        << "  --warmup N         Discarded warmup runs per config "
           "(default: 1)\n"
        << "  --workload LIST    light,medium,heavy,reentrant or all "
           "(default: all)\n"
        << "  --output FILE      Write CSV to FILE (default: stdout)\n"
        << "  --help             Show this help\n";
}

std::vector<std::size_t> parse_size_list(const std::string& text) {
    std::vector<std::size_t> out;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.empty()) {
            continue;
        }
        const auto value = static_cast<std::size_t>(std::stoull(item));
        if (value == 0) {
            throw std::invalid_argument("thread/task counts must be > 0");
        }
        out.push_back(value);
    }
    if (out.empty()) {
        throw std::invalid_argument("expected a non-empty comma-separated list");
    }
    return out;
}

std::vector<std::string> parse_workload_list(const std::string& text) {
    if (text == "all") {
        return {"light", "medium", "heavy", "reentrant"};
    }
    std::vector<std::string> out;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.empty()) {
            continue;
        }
        if (item != "light" && item != "medium" && item != "heavy" &&
            item != "reentrant") {
            throw std::invalid_argument(
                "workload must be light|medium|heavy|reentrant|all");
        }
        out.push_back(item);
    }
    if (out.empty()) {
        throw std::invalid_argument("expected a non-empty workload list");
    }
    return out;
}

Config parse_args(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto require_value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument(std::string(name) +
                                            " requires a value");
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg == "--threads") {
            cfg.thread_counts = parse_size_list(require_value("--threads"));
        } else if (arg == "--tasks") {
            cfg.task_count =
                static_cast<std::size_t>(std::stoull(require_value("--tasks")));
            if (cfg.task_count == 0) {
                throw std::invalid_argument("--tasks must be > 0");
            }
        } else if (arg == "--compute-iters") {
            cfg.compute_iters = static_cast<std::size_t>(
                std::stoull(require_value("--compute-iters")));
        } else if (arg == "--nested-depth") {
            cfg.nested_depth = static_cast<std::size_t>(
                std::stoull(require_value("--nested-depth")));
            if (cfg.nested_depth < 1) {
                throw std::invalid_argument("--nested-depth must be >= 1");
            }
        } else if (arg == "--warmup") {
            cfg.warmup_runs = static_cast<std::size_t>(
                std::stoull(require_value("--warmup")));
        } else if (arg == "--workload") {
            cfg.workloads = parse_workload_list(require_value("--workload"));
        } else if (arg == "--output") {
            cfg.output_path = require_value("--output");
        } else {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }
    return cfg;
}

// ---------------------------------------------------------------------------
// Synthetic workloads
// ---------------------------------------------------------------------------

/** Busy-loop compute; intensity scales with iterations. Volatile sink prevents
 *  the optimizer from eliding the work under -O2/-O3. */
std::uint64_t burn_cpu(std::size_t iterations) {
    volatile std::uint64_t sink = 0;
    for (std::size_t i = 0; i < iterations; ++i) {
        sink = sink + static_cast<std::uint64_t>(i) * 0x9e3779b97f4a7c15ULL;
        sink ^= (sink << 7) | (sink >> 57);
    }
    return sink;
}

std::size_t iterations_for(const std::string& workload,
                           std::size_t compute_iters) {
    if (workload == "light") {
        return 0;  // near-noop: measures queueing / wake / join overhead
    }
    if (workload == "medium") {
        return compute_iters;
    }
    if (workload == "heavy") {
        return compute_iters * 16;
    }
    // reentrant: light compute on each nesting level
    return compute_iters / 4;
}

/**
 * Recursively submit nested work and wait via ThreadPool::wait so a single
 * worker can help-run without self-deadlock. Tracks max nesting depth seen.
 */
std::uint64_t reentrant_work(concurrentx::ThreadPool& pool,
                             std::size_t remaining_depth,
                             std::size_t iters_per_level,
                             std::size_t& max_depth_seen) {
    max_depth_seen =
        std::max(max_depth_seen, concurrentx::ExecutionContext::nesting_depth());

    std::uint64_t acc = burn_cpu(iters_per_level);
    if (remaining_depth <= 1) {
        return acc;
    }

    auto nested = pool.submit([&pool, remaining_depth, iters_per_level,
                               &max_depth_seen] {
        return reentrant_work(pool, remaining_depth - 1, iters_per_level,
                              max_depth_seen);
    });
    return acc + pool.wait(nested);
}

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

struct LatencyStats {
    double mean_us = 0.0;
    double p50_us = 0.0;
    double p95_us = 0.0;
    double p99_us = 0.0;
    double min_us = 0.0;
    double max_us = 0.0;
};

LatencyStats summarize_latencies(std::vector<double> samples_us) {
    LatencyStats stats;
    if (samples_us.empty()) {
        return stats;
    }
    std::sort(samples_us.begin(), samples_us.end());
    stats.min_us = samples_us.front();
    stats.max_us = samples_us.back();
    stats.mean_us =
        std::accumulate(samples_us.begin(), samples_us.end(), 0.0) /
        static_cast<double>(samples_us.size());

    auto percentile = [&](double p) {
        const double idx =
            p * static_cast<double>(samples_us.size() - 1);
        const auto lo = static_cast<std::size_t>(std::floor(idx));
        const auto hi = static_cast<std::size_t>(std::ceil(idx));
        if (lo == hi) {
            return samples_us[lo];
        }
        const double frac = idx - static_cast<double>(lo);
        return samples_us[lo] * (1.0 - frac) + samples_us[hi] * frac;
    };

    stats.p50_us = percentile(0.50);
    stats.p95_us = percentile(0.95);
    stats.p99_us = percentile(0.99);
    return stats;
}

struct BenchResult {
    std::string workload;
    std::size_t thread_count = 0;
    std::size_t task_count = 0;
    double total_ms = 0.0;
    double throughput_tps = 0.0;
    LatencyStats latency;
    /** Wall time attributed to scheduling / wakeups per task (light noop
     *  proxy, or residual after subtracting calibrated serial compute). */
    double overhead_us_per_task = 0.0;
    /** throughput(n) / (throughput(1) * n); 1.0 = ideal linear scaling. */
    double scaling_efficiency = 0.0;
    std::size_t max_nesting_depth = 0;
};

/**
 * Calibrate pure serial compute cost for one task body (no pool). Used to
 * isolate scheduling / context-switch overhead from useful work.
 */
double calibrate_serial_us(const std::string& workload,
                           const Config& cfg,
                           std::size_t samples = 64) {
    const std::size_t iters = iterations_for(workload, cfg.compute_iters);
    if (workload == "reentrant" || iters == 0) {
        return 0.0;
    }

    // Warm the instruction cache once.
    (void)burn_cpu(iters);

    const auto start = Clock::now();
    for (std::size_t i = 0; i < samples; ++i) {
        (void)burn_cpu(iters);
    }
    const auto elapsed = Clock::now() - start;
    return Microseconds(elapsed).count() / static_cast<double>(samples);
}

void update_max_atomic(std::atomic<std::size_t>& target, std::size_t value) {
    auto cur = target.load(std::memory_order_relaxed);
    while (value > cur &&
           !target.compare_exchange_weak(cur, value,
                                         std::memory_order_relaxed)) {
    }
}

BenchResult run_once(const std::string& workload,
                     std::size_t thread_count,
                     const Config& cfg,
                     double serial_task_us) {
    concurrentx::ThreadPool pool{thread_count};
    const std::size_t n = cfg.task_count;
    const std::size_t iters = iterations_for(workload, cfg.compute_iters);
    const bool reentrant = (workload == "reentrant");

    std::vector<double> latencies_us;
    latencies_us.reserve(n);
    std::atomic<std::size_t> max_nesting{0};

    const auto wall_start = Clock::now();

    if (reentrant) {
        // Bound in-flight outers: wait() help-runs other queued tasks on the
        // same stack, so an unbounded flood can recurse to stack overflow.
        const std::size_t max_inflight = std::max<std::size_t>(
            1, std::min(pool.thread_count(), std::size_t{4}));

        struct Inflight {
            Clock::time_point submitted;
            std::future<std::uint64_t> future;
        };
        std::vector<Inflight> window;
        window.reserve(max_inflight);

        std::size_t submitted = 0;
        std::size_t completed = 0;
        while (completed < n) {
            while (submitted < n && window.size() < max_inflight) {
                Inflight slot;
                slot.submitted = Clock::now();
                slot.future = pool.submit([&pool, &cfg, &max_nesting, iters] {
                    std::size_t local_max = 0;
                    const auto v = reentrant_work(pool, cfg.nested_depth, iters,
                                                  local_max);
                    update_max_atomic(max_nesting, local_max);
                    return v;
                });
                window.push_back(std::move(slot));
                ++submitted;
            }

            // Reap any ready future; fall back to blocking on the oldest.
            bool progressed = false;
            for (std::size_t i = 0; i < window.size(); ++i) {
                if (window[i].future.wait_for(std::chrono::seconds(0)) ==
                    std::future_status::ready) {
                    (void)window[i].future.get();
                    latencies_us.push_back(
                        Microseconds(Clock::now() - window[i].submitted)
                            .count());
                    window.erase(window.begin() +
                                 static_cast<std::ptrdiff_t>(i));
                    ++completed;
                    progressed = true;
                    break;
                }
            }
            if (!progressed) {
                (void)window.front().future.get();
                latencies_us.push_back(
                    Microseconds(Clock::now() - window.front().submitted)
                        .count());
                window.erase(window.begin());
                ++completed;
            }
        }
    } else {
        std::vector<Clock::time_point> submit_times(n);
        std::vector<std::future<std::uint64_t>> futures;
        futures.reserve(n);

        for (std::size_t i = 0; i < n; ++i) {
            submit_times[i] = Clock::now();
            futures.push_back(pool.submit([iters] { return burn_cpu(iters); }));
        }
        for (std::size_t i = 0; i < n; ++i) {
            (void)futures[i].get();
            latencies_us.push_back(
                Microseconds(Clock::now() - submit_times[i]).count());
        }
    }

    const auto wall_end = Clock::now();
    const double total_ms = Milliseconds(wall_end - wall_start).count();
    const double total_us = Microseconds(wall_end - wall_start).count();

    BenchResult result;
    result.workload = workload;
    result.thread_count = pool.thread_count();
    result.task_count = n;
    result.total_ms = total_ms;
    result.throughput_tps =
        total_ms > 0.0 ? (static_cast<double>(n) * 1000.0 / total_ms) : 0.0;
    result.latency = summarize_latencies(std::move(latencies_us));
    result.max_nesting_depth = max_nesting.load(std::memory_order_relaxed);

    // Overhead proxy:
    //  - light (noop): entire wall time is scheduling / context-switch cost
    //  - compute: residual after subtracting calibrated serial work amortized
    //    across workers (serial_us * n / thread_count as ideal parallel cost)
    if (workload == "light" || serial_task_us <= 0.0) {
        result.overhead_us_per_task = total_us / static_cast<double>(n);
    } else {
        const double ideal_parallel_us =
            serial_task_us * static_cast<double>(n) /
            static_cast<double>(result.thread_count);
        const double residual = std::max(0.0, total_us - ideal_parallel_us);
        result.overhead_us_per_task = residual / static_cast<double>(n);
    }

    return result;
}

BenchResult measure(const std::string& workload,
                    std::size_t thread_count,
                    const Config& cfg,
                    double serial_task_us) {
    for (std::size_t w = 0; w < cfg.warmup_runs; ++w) {
        (void)run_once(workload, thread_count, cfg, serial_task_us);
    }
    return run_once(workload, thread_count, cfg, serial_task_us);
}

// ---------------------------------------------------------------------------
// CSV output
// ---------------------------------------------------------------------------

void write_csv_header(std::ostream& out) {
    out << "workload,thread_count,task_count,total_ms,throughput_tasks_per_sec,"
           "latency_mean_us,latency_p50_us,latency_p95_us,latency_p99_us,"
           "latency_min_us,latency_max_us,overhead_us_per_task,"
           "scaling_efficiency,max_nesting_depth\n";
}

void write_csv_row(std::ostream& out, const BenchResult& r) {
    out << std::fixed << std::setprecision(3)
        << r.workload << ','
        << r.thread_count << ','
        << r.task_count << ','
        << r.total_ms << ','
        << r.throughput_tps << ','
        << r.latency.mean_us << ','
        << r.latency.p50_us << ','
        << r.latency.p95_us << ','
        << r.latency.p99_us << ','
        << r.latency.min_us << ','
        << r.latency.max_us << ','
        << r.overhead_us_per_task << ','
        << std::setprecision(4) << r.scaling_efficiency << ','
        << r.max_nesting_depth << '\n';
}

void print_summary(std::ostream& err, const BenchResult& r) {
    err << "  " << std::setw(10) << r.workload
        << "  threads=" << std::setw(2) << r.thread_count
        << "  total=" << std::setw(10) << std::fixed << std::setprecision(2)
        << r.total_ms << " ms"
        << "  thrpt=" << std::setw(12) << std::setprecision(1)
        << r.throughput_tps << " t/s"
        << "  p50=" << std::setw(10) << std::setprecision(1)
        << r.latency.p50_us << " us"
        << "  overhead/task=" << std::setw(8) << std::setprecision(2)
        << r.overhead_us_per_task << " us"
        << "  scale_eff=" << std::setprecision(3) << r.scaling_efficiency
        << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Config cfg = parse_args(argc, argv);

        std::ofstream file;
        std::ostream* out = &std::cout;
        if (!cfg.output_path.empty()) {
            file.open(cfg.output_path);
            if (!file) {
                std::cerr << "error: cannot open output file: "
                          << cfg.output_path << '\n';
                return 1;
            }
            out = &file;
        }

        std::cerr << "ConcurrentX benchmark harness\n"
                  << "  tasks=" << cfg.task_count
                  << "  compute_iters=" << cfg.compute_iters
                  << "  nested_depth=" << cfg.nested_depth
                  << "  warmup=" << cfg.warmup_runs << '\n';

        write_csv_header(*out);

        for (const auto& workload : cfg.workloads) {
            const double serial_us = calibrate_serial_us(workload, cfg);
            std::cerr << "\n[" << workload << "] serial_task≈"
                      << std::fixed << std::setprecision(2) << serial_us
                      << " us\n";

            double baseline_throughput = 0.0;

            for (std::size_t threads : cfg.thread_counts) {
                BenchResult result =
                    measure(workload, threads, cfg, serial_us);

                if (baseline_throughput <= 0.0) {
                    baseline_throughput = result.throughput_tps;
                    result.scaling_efficiency = 1.0;
                } else {
                    const double ideal =
                        baseline_throughput *
                        (static_cast<double>(result.thread_count) /
                         static_cast<double>(cfg.thread_counts.front()));
                    result.scaling_efficiency =
                        ideal > 0.0 ? result.throughput_tps / ideal : 0.0;
                }

                write_csv_row(*out, result);
                print_summary(std::cerr, result);
            }
        }

        if (!cfg.output_path.empty()) {
            std::cerr << "\nCSV written to " << cfg.output_path << '\n';
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << '\n';
        print_usage(argv[0]);
        return 1;
    }
}
