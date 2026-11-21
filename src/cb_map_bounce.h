#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace edpe {

enum class MapBounceMode : int {
    Auto = 0,
    On = 1,
    Off = 2,
};

enum class MapBounceTrip : uint8_t {
    None = 0,
    Remap = 1,
    Present = 2,
    Context = 3,
};

inline const char* mapBounceTripName(MapBounceTrip trip) {
    switch (trip) {
    case MapBounceTrip::Remap:   return "Remap";
    case MapBounceTrip::Present: return "Present";
    case MapBounceTrip::Context: return "Context";
    default:                     return "None";
    }
}

inline const wchar_t* mapBounceTripNameW(MapBounceTrip trip) {
    switch (trip) {
    case MapBounceTrip::Remap:   return L"Remap";
    case MapBounceTrip::Present: return L"Present";
    case MapBounceTrip::Context: return L"Context";
    default:                     return L"None";
    }
}

// Adaptive constant-buffer Map(WRITE_DISCARD) bounce pool.
//
// Under DXVK (Wine/Proton) and some write-combined GPU drivers, D3D11_USAGE_DYNAMIC
// constant buffers live in uncached WC BAR memory where CPU reads are ~100x slower
// than cached RAM. When active, eligible WRITE_DISCARD maps are redirected to a
// 64-byte-aligned cached RAM slot; EDPE reads and mutates the cached slot during
// observedUnmap, then flushes with a single sequential memcpy to the real WC pointer
// right before forwarding to the real Unmap.
class CbMapBounce {
public:
    static constexpr size_t kSlotCount = 8;
    static constexpr size_t kMaxByteWidth = 65536;
    static constexpr size_t kGuardBytes = 64;
    static constexpr uint32_t kAutoSampleCount = 16;
    // Threshold in ns per 1024 bytes read: cached RAM reads at ~20-300 ns/KB,
    // while WC BAR memory reads at ~3,000-20,000+ ns/KB.
    static constexpr uint64_t kSlowReadThresholdNsPerKb = 2000;

    enum class AutoState : uint8_t {
        Probing = 0,
        Enabled = 1,
        Disabled = 2,
    };

    static constexpr bool isEligibleByteWidth(uint32_t byteWidth) {
        return byteWidth == 192u ||
               byteWidth == 208u ||
               (byteWidth >= 272u && byteWidth <= 4096u) ||
               byteWidth == 5376u;
    }

    void configure(MapBounceMode mode, bool isWine) {
        mode_.store(mode, std::memory_order_release);
        isWine_.store(isWine, std::memory_order_release);
        if (mode == MapBounceMode::Auto) {
            if (isWine) {
                autoState_.store(AutoState::Enabled, std::memory_order_release);
            } else {
                autoState_.store(AutoState::Probing, std::memory_order_release);
                sampleCount_.store(0, std::memory_order_relaxed);
                sampleBytes_.store(0, std::memory_order_relaxed);
                sampleNs_.store(0, std::memory_order_relaxed);
            }
        }
    }

    void reset() {
        for (auto& slot : slots_) {
            slot.key.store(0, std::memory_order_relaxed);
            slot.context = 0;
            slot.realPtr = nullptr;
            slot.byteWidth = 0;
            slot.frame = 0;
        }
        mode_.store(MapBounceMode::Auto, std::memory_order_relaxed);
        isWine_.store(false, std::memory_order_relaxed);
        autoState_.store(AutoState::Probing, std::memory_order_relaxed);
        tripped_.store(false, std::memory_order_relaxed);
        tripReason_.store(MapBounceTrip::None, std::memory_order_relaxed);
        tripLogPending_.store(false, std::memory_order_relaxed);
        autoActivatedLogPending_.store(false, std::memory_order_relaxed);
        sampleCount_.store(0, std::memory_order_relaxed);
        sampleBytes_.store(0, std::memory_order_relaxed);
        sampleNs_.store(0, std::memory_order_relaxed);
        bouncedMaps_.store(0, std::memory_order_relaxed);
        flushedUnmaps_.store(0, std::memory_order_relaxed);
    }

    MapBounceMode mode() const { return mode_.load(std::memory_order_acquire); }
    AutoState autoState() const { return autoState_.load(std::memory_order_acquire); }
    bool isTripped() const { return tripped_.load(std::memory_order_acquire); }
    MapBounceTrip lastTripReason() const { return tripReason_.load(std::memory_order_acquire); }
    uint64_t bouncedMapCount() const { return bouncedMaps_.load(std::memory_order_relaxed); }
    uint64_t flushedUnmapCount() const { return flushedUnmaps_.load(std::memory_order_relaxed); }

    bool isActive() const {
        if (tripped_.load(std::memory_order_acquire)) return false;
        const MapBounceMode m = mode_.load(std::memory_order_acquire);
        if (m == MapBounceMode::Off) return false;
        if (m == MapBounceMode::On) return true;
        return autoState_.load(std::memory_order_acquire) == AutoState::Enabled;
    }

    bool needsAutoProbe() const {
        if (tripped_.load(std::memory_order_acquire)) return false;
        return mode_.load(std::memory_order_acquire) == MapBounceMode::Auto &&
               autoState_.load(std::memory_order_acquire) == AutoState::Probing;
    }

    // Feed a direct-read timing measurement from an unbounced eligible CB during Auto probing.
    // Returns true on the call that transitions AutoState::Probing -> Enabled.
    bool recordDirectReadSample(uint32_t byteWidth, uint64_t durationNs) {
        if (!needsAutoProbe() || byteWidth == 0) return false;
        const uint32_t count = sampleCount_.fetch_add(1, std::memory_order_relaxed) + 1;
        const uint64_t totalBytes = sampleBytes_.fetch_add(byteWidth, std::memory_order_relaxed) + byteWidth;
        const uint64_t totalNs = sampleNs_.fetch_add(durationNs, std::memory_order_relaxed) + durationNs;
        if (count < kAutoSampleCount) return false;

        // Avoid division by zero; compare (totalNs * 1024 / totalBytes) >= threshold.
        const uint64_t nsPerKb = (totalBytes > 0)
            ? ((totalNs > (std::numeric_limits<uint64_t>::max() / 1024ull))
                ? (totalNs / (totalBytes / 1024ull + 1ull))
                : ((totalNs * 1024ull) / totalBytes))
            : 0ull;

        AutoState expected = AutoState::Probing;
        if (nsPerKb >= kSlowReadThresholdNsPerKb) {
            if (autoState_.compare_exchange_strong(expected, AutoState::Enabled, std::memory_order_acq_rel)) {
                autoActivatedLogPending_.store(true, std::memory_order_release);
                return true;
            }
        } else {
            autoState_.compare_exchange_strong(expected, AutoState::Disabled, std::memory_order_acq_rel);
        }
        return false;
    }

    // Called after the real D3D11 Map succeeds.
    // Returns the cached RAM slot pointer if bounced, or realPtr untouched otherwise.
    void* onMap(uintptr_t resource, uintptr_t context, void* realPtr,
                uint32_t byteWidth, bool isWriteDiscard, uint64_t frame) {
        if (resource == 0 || resource == kBusyKey || !realPtr) return realPtr;

        // Invariant check: if this resource is already open in the bounce pool, trip Remap.
        for (auto& slot : slots_) {
            if (slot.key.load(std::memory_order_acquire) == resource) {
                tripAndFlushAll(MapBounceTrip::Remap);
                return realPtr;
            }
        }

        if (!isActive() || !isWriteDiscard || byteWidth == 0 ||
            byteWidth > kMaxByteWidth || !isEligibleByteWidth(byteWidth)) {
            return realPtr;
        }

        for (auto& slot : slots_) {
            uintptr_t expected = 0;
            if (slot.key.compare_exchange_strong(expected, kBusyKey, std::memory_order_acq_rel)) {
                slot.context = context;
                slot.realPtr = realPtr;
                slot.byteWidth = byteWidth;
                slot.frame = frame;
                slot.key.store(resource, std::memory_order_release);
                bouncedMaps_.fetch_add(1, std::memory_order_relaxed);
                return slot.data;
            }
        }

        // Pool full: pass through untouched.
        return realPtr;
    }

    // Called right before forwarding to the real D3D11 Unmap.
    // Performs a single sequential memcpy(realPtr, cachedSlot, byteWidth) and releases the slot.
    // Returns true if the resource was bounced and flushed, false otherwise.
    bool onUnmap(uintptr_t resource, uintptr_t context) {
        if (resource == 0 || resource == kBusyKey) return false;
        for (auto& slot : slots_) {
            uintptr_t expected = resource;
            if (slot.key.compare_exchange_strong(expected, kBusyKey, std::memory_order_acq_rel)) {
                const bool contextMismatch = (slot.context != context);
                void* const dst = slot.realPtr;
                const uint32_t width = slot.byteWidth;
                if (dst && width > 0 && width <= kMaxByteWidth) {
                    std::memcpy(dst, slot.data, width);
                }
                slot.context = 0;
                slot.realPtr = nullptr;
                slot.byteWidth = 0;
                slot.frame = 0;
                slot.key.store(0, std::memory_order_release);
                flushedUnmaps_.fetch_add(1, std::memory_order_relaxed);
                if (contextMismatch) {
                    tripAndFlushAll(MapBounceTrip::Context);
                }
                return true;
            }
        }
        return false;
    }

    // Called at Present: any slot still open indicates a missed Unmap -> trip Present.
    // Returns true if clean (no open slots), false if tripped.
    bool onPresent(uint64_t /*frame*/) {
        bool anyOpen = false;
        for (auto& slot : slots_) {
            const uintptr_t k = slot.key.load(std::memory_order_acquire);
            if (k != 0 && k != kBusyKey) {
                anyOpen = true;
                break;
            }
        }
        if (anyOpen) {
            tripAndFlushAll(MapBounceTrip::Present);
            return false;
        }
        return true;
    }

    // Returns true once after a fail-safe trip so the caller can log it once to edpe.log.
    bool consumeTripLogPending(MapBounceTrip* outReason) {
        if (tripLogPending_.exchange(false, std::memory_order_acq_rel)) {
            if (outReason) *outReason = tripReason_.load(std::memory_order_acquire);
            return true;
        }
        return false;
    }

    // Returns true once when Auto probing detects slow reads and activates map-bounce.
    bool consumeAutoActivatedLogPending() {
        return autoActivatedLogPending_.exchange(false, std::memory_order_acq_rel);
    }

    size_t openSlotCount() const {
        size_t count = 0;
        for (const auto& slot : slots_) {
            const uintptr_t k = slot.key.load(std::memory_order_acquire);
            if (k != 0 && k != kBusyKey) ++count;
        }
        return count;
    }

private:
    static constexpr uintptr_t kBusyKey = std::numeric_limits<uintptr_t>::max();

    struct alignas(64) Slot {
        alignas(64) unsigned char data[kMaxByteWidth + kGuardBytes]{};
        std::atomic<uintptr_t> key{0};
        uintptr_t context = 0;
        void* realPtr = nullptr;
        uint32_t byteWidth = 0;
        uint64_t frame = 0;
    };

    void tripAndFlushAll(MapBounceTrip reason) {
        if (!tripped_.exchange(true, std::memory_order_acq_rel)) {
            tripReason_.store(reason, std::memory_order_release);
            tripLogPending_.store(true, std::memory_order_release);
        }
        for (auto& slot : slots_) {
            uintptr_t k = slot.key.load(std::memory_order_acquire);
            if (k == 0 || k == kBusyKey) continue;
            if (slot.key.compare_exchange_strong(k, kBusyKey, std::memory_order_acq_rel)) {
                void* const dst = slot.realPtr;
                const uint32_t width = slot.byteWidth;
                if (dst && width > 0 && width <= kMaxByteWidth) {
                    std::memcpy(dst, slot.data, width);
                }
                slot.context = 0;
                slot.realPtr = nullptr;
                slot.byteWidth = 0;
                slot.frame = 0;
                slot.key.store(0, std::memory_order_release);
            }
        }
    }

    std::array<Slot, kSlotCount> slots_{};
    std::atomic<MapBounceMode> mode_{MapBounceMode::Auto};
    std::atomic<bool> isWine_{false};
    std::atomic<AutoState> autoState_{AutoState::Probing};
    std::atomic<bool> tripped_{false};
    std::atomic<MapBounceTrip> tripReason_{MapBounceTrip::None};
    std::atomic<bool> tripLogPending_{false};
    std::atomic<bool> autoActivatedLogPending_{false};
    std::atomic<uint32_t> sampleCount_{0};
    std::atomic<uint64_t> sampleBytes_{0};
    std::atomic<uint64_t> sampleNs_{0};
    std::atomic<uint64_t> bouncedMaps_{0};
    std::atomic<uint64_t> flushedUnmaps_{0};
};

} // namespace edpe
