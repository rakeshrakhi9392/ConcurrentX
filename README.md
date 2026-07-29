# ConcurrentX

C++17 thread pool library — fixed workers, thread-safe queue, `std::future` results.

## Features

- RAII thread pool (workers joined on destruction)
- Thread-safe task queue (`mutex` + `condition_variable`)
- Generic `submit()` with `std::future` and exception propagation
- Optional bounded queue
- Tests (GoogleTest) and CSV benchmark harness

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires C++17 and CMake ≥ 3.15. Core library has no external dependencies.

## Usage

```cpp
#include "concurrentx/thread_pool.hpp"

concurrentx::ThreadPool pool{4};

auto f = pool.submit([](int x) { return x * 2; }, 21);
std::cout << f.get() << "\n";  // 42

pool.stop();
```

For nested work inside a task, prefer `pool.wait(future)` over `future.get()` to avoid self-deadlock on a single-worker pool.

## Layout

```
include/concurrentx/   Public headers
src/                   Implementations
tests/                 Unit & integration tests
benchmarks/            Throughput / latency harness
```

## Benchmarks

```bash
cmake --build build --target concurrentx_bench
./build/benchmarks/concurrentx_bench --threads 1,2,4,8,16 --tasks 10000 --output results.csv
```
<<<<<<< HEAD

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

=======
>>>>>>> f4b5991 (Updated readme)
