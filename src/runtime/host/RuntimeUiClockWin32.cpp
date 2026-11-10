#include "runtime/host/RuntimeUiClockWin32.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdint>
#include <ctime>
#endif
#include <new>

namespace {
class RuntimeWin32UiClock final : public RuntimeUiClock {
public:
    RuntimeUiTimeMs nowMs() const noexcept override {
#ifdef _WIN32
        return GetTickCount64();
#else
        // Callers depend on two properties only: milliseconds, and monotonic
        // (never jumping backwards). The epoch is never observed -- every
        // consumer subtracts two samples, see the elapsed helpers in
        // ui/time/RuntimeUiClock.h. CLOCK_MONOTONIC matches GetTickCount64 on
        // both counts: milliseconds since boot, immune to clock adjustments.
        std::timespec now{};
        if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
        return static_cast<RuntimeUiTimeMs>(now.tv_sec) * 1000U
            + static_cast<RuntimeUiTimeMs>(now.tv_nsec) / 1000000U;
#endif
    }
};
} // namespace

const RuntimeUiClock& runtimeWin32UiClock() noexcept {
    // Global prewarmed menu owners are destroyed after function-local statics.
    // This resource-free provider must remain alive throughout that teardown.
    // Fixed static storage avoids a destructor registration and any heap use;
    // the const pointer's initialization retains the language's once-only guard.
    alignas(RuntimeWin32UiClock) static unsigned char storage[sizeof(RuntimeWin32UiClock)]{};
    static const RuntimeWin32UiClock* const clock =
        ::new (static_cast<void*>(storage)) RuntimeWin32UiClock;
    return *clock;
}

