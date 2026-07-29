/**
 * ConcurrentX GoogleTest integration suite (Step 6).
 *
 * Covers graceful shutdown under load, high-volume concurrent submission,
 * heavy reentrant workloads, and RAII / ownership cleanup under contention.
 * Memory safety is verified by tracking shared_ptr / weak_ptr lifetimes and
 * repeated pool create–destroy cycles (zero outstanding owned heap after scope).
 */

#include "concurrentx/exceptions.hpp"
#include "concurrentx/execution_context.hpp"
#include "concurrentx/log.hpp"
#include "concurrentx/reentrant_mutex.hpp"
#include "concurrentx/thread_pool.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

/** Heap payload tracked via weak_ptr so tests can prove destruction. */
struct TrackedPayload {
    explicit TrackedPayload(std::atomic<int>* live) : live_(live) {
        live_->fetch_add(1, std::memory_order_relaxed);
    }
    ~TrackedPayload() {
        live_->fetch_sub(1, std::memory_order_relaxed);
    }

    TrackedPayload(const TrackedPayload&) = delete;
    TrackedPayload& operator=(const TrackedPayload&) = delete;

    std::atomic<int>* live_;
    int value = 0;
};

std::uint64_t burn_cpu(std::size_t iterations) {
    volatile std::uint64_t sink = 0;
    for (std::size_t i = 0; i < iterations; ++i) {
        sink ^= static_cast<std::uint64_t>(i) * 0x9e3779b97f4a7c15ULL;
    }
    return sink;
}

/**
 * Nested submit + wait chain. Bounded depth keeps help-run stack usage safe
 * when called from a single in-flight outer (no flooded wait()-ing outers).
 */
std::uint64_t nested_reentrant(concurrentx::ThreadPool& pool,
                               std::size_t depth,
                               std::size_t work_iters,
                               std::size_t& max_depth) {
    max_depth =
        std::max(max_depth, concurrentx::ExecutionContext::nesting_depth());
    std::uint64_t acc = burn_cpu(work_iters);
    if (depth <= 1) {
        return acc;
    }
    auto inner = pool.submit([&pool, depth, work_iters, &max_depth] {
        return nested_reentrant(pool, depth - 1, work_iters, max_depth);
    });
    return acc + pool.wait(inner);
}

}  // namespace

class QuietLogsEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        concurrentx::Logger::set_level(concurrentx::LogLevel::Off);
    }
};

// =============================================================================
// Shutdown with pending / in-flight tasks
// =============================================================================

TEST(ShutdownPending, DestructorDrainsQueuedWork) {
    constexpr int kTasks = 500;
    std::atomic<int> completed{0};
    std::vector<std::future<void>> futures;
    futures.reserve(kTasks);

    auto gate = std::make_shared<std::promise<void>>();
    // shared_future is copyable; each task holds its own ref to the state so
    // the promise may be set and released without dangling references.
    std::shared_future<void> gate_fut = gate->get_future().share();

    {
        // One slow worker so a backlog forms before destruction.
        concurrentx::ThreadPool pool{1};

        futures.push_back(pool.submit([gate_fut, &completed] {
            gate_fut.wait();
            completed.fetch_add(1, std::memory_order_relaxed);
        }));

        for (int i = 1; i < kTasks; ++i) {
            futures.push_back(pool.submit([&completed] {
                completed.fetch_add(1, std::memory_order_relaxed);
            }));
        }

        EXPECT_GT(pool.queued_tasks(), 0u);
        gate->set_value();
        gate.reset();
        // Leave scope: ~ThreadPool stop() + join must drain the queue.
    }

    for (auto& f : futures) {
        EXPECT_NO_THROW(f.get());
    }
    EXPECT_EQ(completed.load(), kTasks);
}

TEST(ShutdownPending, ExplicitStopDrainsThenRejectsSubmit) {
    concurrentx::ThreadPool pool{2};
    constexpr int kTasks = 200;
    std::atomic<int> completed{0};
    std::vector<std::future<void>> futures;
    futures.reserve(kTasks);

    auto release = std::make_shared<std::promise<void>>();
    std::shared_future<void> release_fut = release->get_future().share();

    // Occupy both workers briefly so later tasks stay queued.
    for (int i = 0; i < 2; ++i) {
        futures.push_back(pool.submit([release_fut, &completed] {
            release_fut.wait();
            completed.fetch_add(1, std::memory_order_relaxed);
        }));
    }
    for (int i = 2; i < kTasks; ++i) {
        futures.push_back(pool.submit([&completed] {
            completed.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    release->set_value();
    pool.stop();
    EXPECT_TRUE(pool.is_stopped());

    EXPECT_THROW(
        (void)pool.submit([] { return 0; }),
        concurrentx::SchedulerStoppedException);

    for (auto& f : futures) {
        EXPECT_NO_THROW(f.get());
    }
    EXPECT_EQ(completed.load(), kTasks);
    EXPECT_EQ(pool.queued_tasks(), 0u);
}

TEST(ShutdownPending, StopUnderConcurrentProducers) {
    concurrentx::ThreadPool pool{4};
    std::atomic<int> accepted{0};
    std::atomic<int> rejected{0};
    std::atomic<int> executed{0};
    std::atomic<bool> stop_now{false};

    constexpr int kProducers = 8;
    constexpr int kPerProducer = 1000;
    std::vector<std::thread> producers;
    producers.reserve(kProducers);

    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            for (int i = 0; i < kPerProducer; ++i) {
                try {
                    auto fut = pool.submit([&executed] {
                        executed.fetch_add(1, std::memory_order_relaxed);
                    });
                    accepted.fetch_add(1, std::memory_order_relaxed);
                    // Detach completion into the future's shared state; we
                    // only need execute counts, not every handle.
                    (void)fut;
                } catch (const concurrentx::SchedulerStoppedException&) {
                    rejected.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                if (stop_now.load(std::memory_order_acquire) && (i & 0xf) == 0) {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::this_thread::sleep_for(5ms);
    stop_now.store(true, std::memory_order_release);
    pool.stop();

    for (auto& t : producers) {
        t.join();
    }

    // Give workers a moment to finish draining under stop.
    for (int spin = 0; spin < 200 && pool.queued_tasks() > 0; ++spin) {
        std::this_thread::sleep_for(1ms);
    }

    EXPECT_EQ(pool.queued_tasks(), 0u);
    EXPECT_GT(accepted.load(), 0);
    // Every accepted task must run (stop drains already-queued work).
    EXPECT_EQ(executed.load(), accepted.load());
    EXPECT_GE(rejected.load() + accepted.load(), 1);
}

TEST(ShutdownPending, DoubleStopIsIdempotent) {
    concurrentx::ThreadPool pool{2};
    auto fut = pool.submit([] { return 11; });
    pool.stop();
    pool.stop();
    EXPECT_TRUE(pool.is_stopped());
    EXPECT_EQ(fut.get(), 11);
    EXPECT_THROW((void)pool.submit([] { return 0; }),
                 concurrentx::SchedulerStoppedException);
}

// =============================================================================
// High-volume / high-contention submission
// =============================================================================

TEST(HighVolume, TensOfThousandsOfTasks) {
    constexpr int kTasks = 50000;
    concurrentx::ThreadPool pool{8};
    std::atomic<int> counter{0};
    std::vector<std::future<void>> futures;
    futures.reserve(kTasks);

    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(pool.submit([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        }));
    }
    for (auto& f : futures) {
        f.get();
    }
    EXPECT_EQ(counter.load(), kTasks);
    EXPECT_EQ(pool.queued_tasks(), 0u);
}

TEST(HighVolume, ManyProducerThreads) {
    constexpr int kProducers = 16;
    constexpr int kPerProducer = 2000;
    concurrentx::ThreadPool pool{8};
    std::atomic<int> counter{0};
    std::mutex futures_mu;
    std::vector<std::future<void>> futures;
    futures.reserve(static_cast<std::size_t>(kProducers * kPerProducer));

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            std::vector<std::future<void>> local;
            local.reserve(kPerProducer);
            for (int i = 0; i < kPerProducer; ++i) {
                local.push_back(pool.submit([&counter] {
                    counter.fetch_add(1, std::memory_order_relaxed);
                }));
            }
            std::lock_guard<std::mutex> lock(futures_mu);
            for (auto& f : local) {
                futures.push_back(std::move(f));
            }
        });
    }
    for (auto& t : producers) {
        t.join();
    }
    for (auto& f : futures) {
        f.get();
    }
    EXPECT_EQ(counter.load(), kProducers * kPerProducer);
}

TEST(HighVolume, MixedReturnTypesAndExceptions) {
    concurrentx::ThreadPool pool{4};
    std::vector<std::future<int>> oks;
    std::vector<std::future<int>> fails;
    constexpr int kEach = 500;
    oks.reserve(kEach);
    fails.reserve(kEach);

    for (int i = 0; i < kEach; ++i) {
        oks.push_back(pool.submit([i] { return i; }));
        fails.push_back(pool.submit([]() -> int {
            throw std::runtime_error("fail");
        }));
    }

    for (int i = 0; i < kEach; ++i) {
        EXPECT_EQ(oks[static_cast<std::size_t>(i)].get(), i);
        EXPECT_THROW((void)fails[static_cast<std::size_t>(i)].get(),
                     std::runtime_error);
    }
}

TEST(HighVolume, BoundedQueueContention) {
    // Small queue under bursty submit: either enqueue succeeds or overflow.
    concurrentx::ThreadPool pool{2, /*max_queue_size=*/32};
    std::atomic<int> overflowed{0};
    std::atomic<int> ran{0};

    auto hold = std::make_shared<std::promise<void>>();
    std::shared_future<void> hold_fut = hold->get_future().share();
    auto blocker1 = pool.submit([hold_fut] { hold_fut.wait(); });
    auto blocker2 = pool.submit([hold_fut] { hold_fut.wait(); });

    constexpr int kBurst = 200;
    std::vector<std::future<void>> accepted_futs;
    accepted_futs.reserve(32);

    for (int i = 0; i < kBurst; ++i) {
        try {
            accepted_futs.push_back(pool.submit([&ran] {
                ran.fetch_add(1, std::memory_order_relaxed);
            }));
        } catch (const concurrentx::TaskQueueOverflowException&) {
            overflowed.fetch_add(1, std::memory_order_relaxed);
        }
    }

    EXPECT_GT(overflowed.load(), 0);
    EXPECT_LE(accepted_futs.size(), 32u);
    hold->set_value();
    blocker1.get();
    blocker2.get();
    for (auto& f : accepted_futs) {
        f.get();
    }
    EXPECT_EQ(ran.load(), static_cast<int>(accepted_futs.size()));
}

// =============================================================================
// Heavy reentrant workloads
// =============================================================================

TEST(ReentrantHeavy, DeepNestingOnSingleWorker) {
    concurrentx::ThreadPool pool{1};
    constexpr std::size_t kDepth = 24;
    std::size_t max_depth = 0;

    auto fut = pool.submit([&] {
        return nested_reentrant(pool, kDepth, /*work_iters=*/8, max_depth);
    });

    EXPECT_NO_THROW((void)fut.get());
    // Help-run nesting: depth frames accumulate on the same TLS stack.
    EXPECT_GE(max_depth, kDepth);
    EXPECT_FALSE(concurrentx::ExecutionContext::in_worker());
}

TEST(ReentrantHeavy, ManySequentialReentrantChains) {
    concurrentx::ThreadPool pool{2};
    constexpr int kChains = 400;
    constexpr std::size_t kDepth = 6;
    std::atomic<int> done{0};
    std::atomic<std::size_t> max_seen{0};

    // One chain at a time per submission wave keeps help-run stacks bounded.
    for (int i = 0; i < kChains; ++i) {
        auto fut = pool.submit([&] {
            std::size_t local_max = 0;
            (void)nested_reentrant(pool, kDepth, 4, local_max);
            auto cur = max_seen.load(std::memory_order_relaxed);
            while (local_max > cur &&
                   !max_seen.compare_exchange_weak(cur, local_max,
                                                   std::memory_order_relaxed)) {
            }
            done.fetch_add(1, std::memory_order_relaxed);
        });
        fut.get();
    }

    EXPECT_EQ(done.load(), kChains);
    EXPECT_GE(max_seen.load(), kDepth);
}

TEST(ReentrantHeavy, FanOutWithWaitUnderLoad) {
    concurrentx::ThreadPool pool{4};
    constexpr int kOuters = 64;
    std::atomic<int> leaves{0};
    std::vector<std::future<int>> futures;
    futures.reserve(kOuters);

    for (int i = 0; i < kOuters; ++i) {
        futures.push_back(pool.submit([&pool, &leaves] {
            // Small fan-out; wait() may help-run siblings — keep fan modest.
            std::vector<std::future<int>> kids;
            kids.reserve(3);
            for (int k = 0; k < 3; ++k) {
                kids.push_back(pool.submit([&leaves] {
                    leaves.fetch_add(1, std::memory_order_relaxed);
                    return 1;
                }));
            }
            int sum = 0;
            for (auto& kid : kids) {
                sum += pool.wait(kid);
            }
            return sum;
        }));
    }

    int total = 0;
    for (auto& f : futures) {
        total += f.get();
    }
    EXPECT_EQ(total, kOuters * 3);
    EXPECT_EQ(leaves.load(), kOuters * 3);
}

TEST(ReentrantHeavy, ReentrantMutexUnderNestedHelpRunStorm) {
    concurrentx::ReentrantMutex mutex;
    int shared = 0;
    concurrentx::ThreadPool pool{1};
    constexpr int kIters = 100;

    for (int i = 0; i < kIters; ++i) {
        auto outer = pool.submit([&] {
            std::lock_guard<concurrentx::ReentrantMutex> outer_lock(mutex);
            ++shared;
            auto inner = pool.submit([&] {
                std::lock_guard<concurrentx::ReentrantMutex> inner_lock(mutex);
                ++shared;
                return 0;
            });
            (void)pool.wait(inner);
            ++shared;
        });
        outer.get();
    }
    EXPECT_EQ(shared, kIters * 3);
}

// =============================================================================
// Memory safety / resource cleanup under contention
// =============================================================================

TEST(MemorySafety, TaskOwnedSharedPtrReleasedAfterCompletion) {
    std::atomic<int> live{0};
    std::weak_ptr<TrackedPayload> weak;

    {
        concurrentx::ThreadPool pool{4};
        constexpr int kTasks = 2000;
        std::vector<std::future<int>> futures;
        futures.reserve(kTasks);

        for (int i = 0; i < kTasks; ++i) {
            auto payload = std::make_shared<TrackedPayload>(&live);
            if (i == 0) {
                weak = payload;
            }
            futures.push_back(pool.submit([payload] {
                payload->value += 1;
                return payload->value;
            }));
        }

        for (auto& f : futures) {
            EXPECT_GE(f.get(), 1);
        }
        // Last strong refs were in the packaged tasks / lambdas; after get(),
        // only this scope's payloads are gone — weak from task 0 should expire.
    }

    EXPECT_EQ(live.load(), 0);
    EXPECT_TRUE(weak.expired());
}

TEST(MemorySafety, PoolLifecycleDoesNotLeakWorkerOwnedState) {
    std::atomic<int> live{0};
    constexpr int kCycles = 50;
    constexpr int kTasksPerCycle = 200;

    for (int c = 0; c < kCycles; ++c) {
        concurrentx::ThreadPool pool{4};
        std::vector<std::future<void>> futures;
        futures.reserve(kTasksPerCycle);
        for (int i = 0; i < kTasksPerCycle; ++i) {
            auto payload = std::make_shared<TrackedPayload>(&live);
            futures.push_back(pool.submit([payload] {
                payload->value = 42;
            }));
        }
        for (auto& f : futures) {
            f.get();
        }
    }

    EXPECT_EQ(live.load(), 0);
}

TEST(MemorySafety, ShutdownAbandonsNoQueuedTaskResources) {
    std::atomic<int> live{0};
    std::vector<std::future<void>> futures;

    auto gate = std::make_shared<std::promise<void>>();
    std::shared_future<void> gate_fut = gate->get_future().share();

    {
        concurrentx::ThreadPool pool{2};

        futures.push_back(pool.submit([gate_fut] { gate_fut.wait(); }));

        for (int i = 0; i < 300; ++i) {
            auto payload = std::make_shared<TrackedPayload>(&live);
            futures.push_back(pool.submit([payload] {
                payload->value = 1;
            }));
        }

        EXPECT_GT(live.load(), 0);
        gate->set_value();
        // Destructor drains queue; all TrackedPayloads inside tasks must die.
    }

    for (auto& f : futures) {
        f.get();
    }
    EXPECT_EQ(live.load(), 0);
}

TEST(MemorySafety, UniquePtrMoveOnlyCallable) {
    concurrentx::ThreadPool pool{2};
    auto owned = std::make_unique<int>(7);
    auto fut = pool.submit([p = std::move(owned)]() mutable {
        EXPECT_NE(p, nullptr);
        const int v = *p;
        p.reset();
        return v;
    });
    EXPECT_EQ(fut.get(), 7);
    EXPECT_EQ(owned, nullptr);
}

TEST(MemorySafety, HighContentionSharedHeapWithReentrantMutex) {
    concurrentx::ReentrantMutex mutex;
    auto heap = std::make_shared<std::vector<int>>();
    heap->reserve(10000);
    std::weak_ptr<std::vector<int>> weak = heap;

    {
        concurrentx::ThreadPool pool{8};
        constexpr int kTasks = 5000;
        std::vector<std::future<void>> futures;
        futures.reserve(kTasks);

        for (int i = 0; i < kTasks; ++i) {
            futures.push_back(pool.submit([&mutex, heap, i] {
                std::lock_guard<concurrentx::ReentrantMutex> lock(mutex);
                heap->push_back(i);
            }));
        }
        for (auto& f : futures) {
            f.get();
        }
        EXPECT_EQ(heap->size(), static_cast<std::size_t>(kTasks));
    }

    heap.reset();
    EXPECT_TRUE(weak.expired());
}

TEST(MemorySafety, WaitGetAliasesDoNotLeakFutures) {
    concurrentx::ThreadPool pool{2};
    for (int i = 0; i < 1000; ++i) {
        auto outer = pool.submit([&pool] {
            auto inner = pool.submit([] { return 1; });
            return pool.get(inner);
        });
        EXPECT_EQ(outer.get(), 1);
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new QuietLogsEnvironment);
    return RUN_ALL_TESTS();
}
