# ConcurrentX

A lightweight C++17 multithreaded task scheduler and thread pool library.

ConcurrentX focuses on deterministic resource management, thread-safe task submission, safe reentrancy, and clear exception propagation — the core concurrency patterns expected in systems-oriented C++ work.

## Features

- **Fixed-size thread pool** with RAII lifecycle — workers are joined on destruction; no thread leaks
- **Type-erased task queue** accepting arbitrary callables via `std::packaged_task`
- **`std::future<T>` results** — retrieve return values or exceptions from the calling thread
- **Mutex + condition variable** coordination — no busy-waiting, no lock held during task execution
- **Reentrancy-safe nesting** — TLS execution context and help-running wait APIs avoid self-deadlock on nested `submit()`
- **Bounded or unbounded queues** — optional capacity with `TaskQueueOverflowException` on overflow
- **Custom exception hierarchy** for scheduler failure modes
- **Benchmark harness** that emits CSV throughput and latency metrics across thread counts
- **Unit and integration tests** covering shutdown under load, reentrancy, overflow, and exception propagation

## Requirements

| Item | Version |
|------|---------|
| C++ standard | C++17 |
| CMake | ≥ 3.15 |
| Compiler | MSVC, GCC, or Clang with standard multithreading support |
| Dependencies (core) | None |
| Dependencies (tests) | GoogleTest 1.14.0 (fetched automatically by CMake) |

## Quick start

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Optional CMake flags:

```bash
# Library only
cmake -S . -B build -DCONCURRENTX_BUILD_TESTS=OFF -DCONCURRENTX_BUILD_BENCHMARKS=OFF
```

### Link against ConcurrentX

```cmake
add_executable(app main.cpp)
target_link_libraries(app PRIVATE ConcurrentX::ConcurrentX)
```

```cpp
#include "concurrentx/thread_pool.hpp"

int main() {
    concurrentx::ThreadPool pool{4};

    auto future = pool.submit([](int x) { return x * 2; }, 21);
    return future.get() == 42 ? 0 : 1;
}
```

## Usage

### Submit work

```cpp
concurrentx::ThreadPool pool;  // hardware_concurrency workers, unbounded queue
auto f1 = pool.submit([] { return 42; });
auto f2 = pool.submit([](int a, int b) { return a + b; }, 1, 2);

std::cout << f1.get() << " " << f2.get() << "\n";
```

### Bounded queue

```cpp
// 2 workers, at most 64 queued tasks
concurrentx::ThreadPool pool{2, /*max_queue_size=*/64};
```

### Nested work from inside a task

Prefer `pool.wait()` / `pool.get()` over raw `future.get()` when blocking on nested work from a worker — especially a single-thread pool. They help-run queued tasks and avoid self-deadlock:

```cpp
concurrentx::ThreadPool pool{1};

auto outer = pool.submit([&pool] {
    auto inner = pool.submit([] { return 7; });
    return pool.wait(inner);  // safe nested wait
});

assert(outer.get() == 7);
```

### Shutdown

```cpp
pool.stop();  // cooperative: drain queue, reject further submit()
// Destructor also stops and joins if you have not called stop().
```

### Protect shared mutable state

`ExecutionContext` tracks nesting depth via TLS; it does **not** serialize heap writes across workers. Guard shared state with `ReentrantMutex` (or another lock):

```cpp
#include "concurrentx/reentrant_mutex.hpp"

concurrentx::ReentrantMutex mu;
int shared = 0;

pool.submit([&] {
    std::lock_guard lock(mu);
    ++shared;
});
```

## Architecture

```
include/concurrentx/     Public headers (API)
src/                     Implementations (.cpp)
tests/                   Smoke, reentrancy, error-handling, GoogleTest integration
benchmarks/              CSV profiling harness
```

| Component | Role |
|-----------|------|
| `ThreadPool` | Fixed worker set, mutex-protected queue, `submit` / `wait` / `stop` |
| `Task` | Move-only type-erased callable for the queue boundary |
| `ExecutionContext` / `ReentrancyGuard` | TLS nesting depth and current-pool tracking |
| `ReentrantMutex` | Recursive lock for shared state under nested execution |
| Exceptions | `SchedulerStoppedException`, `TaskQueueOverflowException`, … |
| Logging / asserts | `CX_LOG_*`, `CX_ASSERT*` for diagnostics and invariants |

**Lock discipline:** tasks are dequeued under the pool mutex, then run *outside* the lock so a running task can safely call `submit()` again without deadlock.

**Exception path:** failures inside a task body are captured by `std::packaged_task` and rethrown when the caller calls `future.get()` or `ThreadPool::wait()`.

## Benchmarks

```bash
cmake --build build --target concurrentx_bench

# Windows (MSVC / multi-config)
./build/benchmarks/Debug/concurrentx_bench.exe --threads 1,2,4,8,16 --tasks 20000 --output results.csv

# Single-config generators
./build/benchmarks/concurrentx_bench --threads 1,2,4,8,16 --tasks 20000 --output results.csv
```

Useful options:

| Flag | Description | Default |
|------|-------------|---------|
| `--threads LIST` | Worker counts to profile | `1,2,4,8,16` |
| `--tasks N` | Tasks per measurement | `10000` |
| `--workload LIST` | `light`, `medium`, `heavy`, `reentrant`, or `all` | all |
| `--output FILE` | CSV path (`stdout` if omitted) | — |

## Project layout

```
ConcurrentX/
├── CMakeLists.txt
├── include/concurrentx/
│   ├── thread_pool.hpp
│   ├── task.hpp
│   ├── execution_context.hpp
│   ├── reentrant_mutex.hpp
│   ├── exceptions.hpp
│   ├── log.hpp
│   └── debug.hpp
├── src/
├── tests/
└── benchmarks/
```

## License

No license file is included yet. Add one before publishing if you intend the project to be reused.
