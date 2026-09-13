// Original Windows timer implementation. API reference and fallback semantics:
// https://learn.microsoft.com/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw
// https://learn.microsoft.com/windows/win32/api/synchapi/nf-synchapi-setwaitabletimerex
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "native_precise_wait.h"
#include "native_precise_wait_policy.h"

#include <chrono>

namespace nb::gpu {
namespace {
class ThreadTimer {
 public:
  ThreadTimer() = default;
  ~ThreadTimer() { Close(); }
  ThreadTimer(const ThreadTimer&) = delete;
  ThreadTimer& operator=(const ThreadTimer&) = delete;

  uint64_t NowNs() const noexcept {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
  }
  bool EnsureTimer() noexcept {
    if (timer_) return true;
    if (failed_) return false;
    // 0x2 is CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+).
    // Keep it explicit so older Windows SDK headers can compile the fallback.
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0x2,
                                  TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (!timer_) failed_ = true;
    return timer_ != nullptr;
  }
  bool WaitRelative(int64_t ticks) noexcept {
    LARGE_INTEGER due;
    due.QuadPart = ticks;
    // Automatic reset, one shot, no APC, no wake request, no coalescing delay.
    if (!SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0))
      return Fail();
    if (WaitForSingleObject(timer_, INFINITE) != WAIT_OBJECT_0) return Fail();
    return true;
  }
  bool Fail() noexcept {
    // No wait is pending on this thread when this is called. Latch unsupported
    // or broken API state to avoid repeated failed kernel work on later packets.
    failed_ = true;
    Close();
    return false;
  }

 private:
  void Close() noexcept {
    if (timer_) { CloseHandle(timer_); timer_ = nullptr; }
  }
  HANDLE timer_ = nullptr;
  bool failed_ = false;
};
}  // namespace

bool NativePreciseSleep(uint32_t milliseconds) noexcept {
  return NativePreciseSleepNs(uint64_t(milliseconds) * 1000000);
}

bool NativePreciseSleepNs(uint64_t nanoseconds) noexcept {
  // Bad caller input is not a broken timer and must not poison later calls.
  if (nanoseconds > kNativeWaitMaxNanoseconds) return false;
  // The CP worker is joined before plugin teardown. Never share this handle or
  // close it from another thread while a synchronous wait is outstanding.
  thread_local ThreadTimer timer;
  if (NativeWaitAtLeastNs(timer, nanoseconds)) return true;
  return timer.Fail();
}
}  // namespace nb::gpu
