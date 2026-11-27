// Runs a call that may throw and turns a throw into an error value.
//
// The frame generation provider reports some failures, for example after a GPU reset, by throwing a C++ exception
// instead of returning a code. An exception that escapes into the game aborts it, so a throw is recorded and
// reported as an error the caller already handles. Table-based x64 exception handling costs nothing while nothing
// throws. Kept free of the FidelityFX headers so it can be tested without a GPU or the SDK.
#pragma once

#include <atomic>

namespace edpe {

// Returns call()'s result. If call throws, sets *threw (when not null), runs onThrow() (for logging) and returns
// errorCode. The flag is only ever set here; clearing it is the caller's decision.
template <typename Code, typename Call, typename OnThrow>
Code callGuarded(Code errorCode, std::atomic<bool>* threw, Call&& call, OnThrow&& onThrow) {
    try {
        return call();
    } catch (...) {
        if (threw) threw->store(true);
        onThrow();
        return errorCode;
    }
}

} // namespace edpe
