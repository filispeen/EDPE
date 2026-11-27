// CPU-only unit tests for src/guarded_call.h: a call that throws becomes an error value and sets the fault flag,
// a call that does not throw passes through untouched. No GPU, no FidelityFX SDK and no game needed.
#include "guarded_call.h"

#include <atomic>
#include <cstdio>
#include <stdexcept>

using namespace edpe;

namespace {

int g_failures = 0;
// A macro so a failure reports the text of the failing condition and its line.
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                        \
            ++g_failures;                                                              \
        }                                                                              \
    } while (0)

constexpr unsigned kError = 0xDEADu;

void testPassThrough() {
    std::atomic<bool> threw{false};
    int calls = 0;
    int logged = 0;
    const unsigned rc = callGuarded<unsigned>(kError, &threw, [&] { ++calls; return 7u; }, [&] { ++logged; });
    CHECK(rc == 7u);
    CHECK(calls == 1);       // the call runs exactly once
    CHECK(!threw);           // nothing threw, so the flag is untouched
    CHECK(logged == 0);      // and nothing is reported
}

void testStdExceptionBecomesError() {
    std::atomic<bool> threw{false};
    int logged = 0;
    const unsigned rc = callGuarded<unsigned>(
        kError, &threw, []() -> unsigned { throw std::runtime_error("device removed"); }, [&] { ++logged; });
    CHECK(rc == kError);
    CHECK(threw);
    CHECK(logged == 1);      // reported once
}

void testNonStdExceptionBecomesError() {
    std::atomic<bool> threw{false};
    const unsigned rc = callGuarded<unsigned>(kError, &threw, []() -> unsigned { throw 42; }, [] {});
    CHECK(rc == kError);     // catch (...) also covers exceptions that do not derive from std::exception
    CHECK(threw);
}

void testNullFlagIsAccepted() {
    int logged = 0;
    const unsigned rc = callGuarded<unsigned>(
        kError, nullptr, []() -> unsigned { throw std::runtime_error("x"); }, [&] { ++logged; });
    CHECK(rc == kError);
    CHECK(logged == 1);
}

void testFlagIsOnlySetNeverCleared() {
    std::atomic<bool> threw{false};
    callGuarded<unsigned>(kError, &threw, []() -> unsigned { throw 1; }, [] {});
    CHECK(threw);
    // A later call that works must not clear the fault: only the owner decides when it is cleared.
    const unsigned rc = callGuarded<unsigned>(kError, &threw, [] { return 3u; }, [] {});
    CHECK(rc == 3u);
    CHECK(threw);
}

void testErrorValueIsReturnedForEnumCodes() {
    enum class Rc { Ok, Error };
    std::atomic<bool> threw{false};
    const Rc ok = callGuarded<Rc>(Rc::Error, &threw, [] { return Rc::Ok; }, [] {});
    const Rc bad = callGuarded<Rc>(Rc::Error, &threw, []() -> Rc { throw 1; }, [] {});
    CHECK(ok == Rc::Ok);
    CHECK(bad == Rc::Error);
}

} // namespace

int main() {
    testPassThrough();
    testStdExceptionBecomesError();
    testNonStdExceptionBecomesError();
    testNullFlagIsAccepted();
    testFlagIsOnlySetNeverCleared();
    testErrorValueIsReturnedForEnumCodes();
    if (g_failures == 0) {
        std::printf("guarded_call_test: all checks passed\n");
        return 0;
    }
    std::printf("guarded_call_test: %d check(s) failed\n", g_failures);
    return 1;
}
