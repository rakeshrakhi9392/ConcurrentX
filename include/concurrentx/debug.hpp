#pragma once

/**
 * Debug assertion macros for ConcurrentX thread-safety invariants.
 *
 * Enabled by default when NDEBUG is not defined. Force on/off with:
 *   -DCONCURRENTX_ENABLE_ASSERTS=1  or  =0
 *
 * Failure policy (runtime, via set_assert_handler / set_assert_mode):
 *   - Abort (default): log and std::abort()
 *   - Throw: throw AssertionFailureException (useful in unit tests)
 */

#ifndef CONCURRENTX_ENABLE_ASSERTS
#ifdef NDEBUG
#define CONCURRENTX_ENABLE_ASSERTS 0
#else
#define CONCURRENTX_ENABLE_ASSERTS 1
#endif
#endif

namespace concurrentx {

enum class AssertMode {
    Abort,  // log + std::abort (default)
    Throw,  // log + throw AssertionFailureException
};

using AssertHandler = void (*)(const char* file,
                               int line,
                               const char* expression,
                               const char* message);

/** Install a custom handler; nullptr restores the default for the current mode. */
void set_assert_handler(AssertHandler handler) noexcept;

/** Select Abort vs Throw when the default handler is active. */
void set_assert_mode(AssertMode mode) noexcept;

[[nodiscard]] AssertMode assert_mode() noexcept;

namespace detail {

[[noreturn]] void assertion_failed(const char* file,
                                   int line,
                                   const char* expression,
                                   const char* message);

inline void check_assert(bool ok,
                         const char* file,
                         int line,
                         const char* expression,
                         const char* message) {
    if (!ok) {
        assertion_failed(file, line, expression, message);
    }
}

}  // namespace detail

}  // namespace concurrentx

#if CONCURRENTX_ENABLE_ASSERTS

/**
 * Hard check for invariants that must hold (lock state, non-null owners,
 * nesting-depth consistency, etc.). No-op when asserts are disabled.
 */
#define CX_ASSERT(expr)                                                        \
    do {                                                                       \
        if (!(expr)) {                                                         \
            ::concurrentx::detail::assertion_failed(                           \
                __FILE__, __LINE__, #expr, nullptr);                           \
        }                                                                      \
    } while (0)

#define CX_ASSERT_MSG(expr, msg)                                               \
    do {                                                                       \
        if (!(expr)) {                                                         \
            ::concurrentx::detail::assertion_failed(                           \
                __FILE__, __LINE__, #expr, (msg));                             \
        }                                                                      \
    } while (0)

/** Semantic alias for thread-safety / synchronization invariants. */
#define CX_INVARIANT(expr) CX_ASSERT(expr)
#define CX_INVARIANT_MSG(expr, msg) CX_ASSERT_MSG(expr, msg)

#else  // !CONCURRENTX_ENABLE_ASSERTS

#define CX_ASSERT(expr) ((void)0)
#define CX_ASSERT_MSG(expr, msg) ((void)0)
#define CX_INVARIANT(expr) ((void)0)
#define CX_INVARIANT_MSG(expr, msg) ((void)0)

#endif  // CONCURRENTX_ENABLE_ASSERTS
