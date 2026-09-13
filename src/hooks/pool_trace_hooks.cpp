// nb - streaming buddy-pool lifecycle trace and optional serialization (see pool_trace_hooks.h).
//
// Facts the hooks rest on (all read from generated/default, 2026-09-06):
//   sub_82364A88(bytes, count)  pool reset: if [0x82FAC600] != 0 -> sub_82364BB8(pool0, 1), store 0;
//                               same for [0x82FAC604]; then, when both args are non-zero, meInternalAlloc(48)
//                               + sub_823646E8 for each pool and store the new pointers. No lock anywhere.
//                               Callers: sub_823C6260 (the audio re-init on the main thread), sub_823C5F50,
//                               sub_822BC520, sub_823CF5C0 (shutdown) and the lazy create below.
//   sub_82364A20(selector)      pool lookup: if [0x82FAC600] == 0 -> sub_82364A88(0x100000, 1); returns
//                               pool 0 or pool 1. No lock. Reached from the XACT allocator hooks
//                               (sub_822C9FF0 / sub_822CA098, vtable targets) on the bundle-load worker.
//   sub_82364BB8(pool, flag)    teardown: frees the per-level bitmaps, the level table, XPhysicalFree(+28),
//                               closes the lock event, meInternalFree(pool).
//   sub_823646E8(obj, orders, count, flags)  constructor.
//   sub_8228D108(pool, size)    sub-allocation; sub_82364858(pool, chunk) range test used by the release
//                               path sub_822CA098. Both take the pool's own lock, so a stale pool pointer
//                               reaching them is the "use after teardown" we want to see.
//   Message pump sub_82327560: loc_823275EC is the type-21 (pump XACT) job loop head, loc_82327710 the
//                               common exit; both are mid-asm hooks in config/hooks.toml.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#include "generated/default/nb_init.h"

#include "pool_trace_hooks.h"

REXCVAR_DEFINE_BOOL(nb_pool_trace, false, "nb",
                    "Log the streaming pool reset/lookup/teardown/construct sequence with thread ids");
REXCVAR_DEFINE_BOOL(nb_pool_reset_lock, false, "nb",
                    "Serialize the streaming pool reset (sub_82364A88) and lookup (sub_82364A20) under one "
                    "host mutex (experiment: does the boot heap corruption disappear?)");

REX_EXTERN(__imp__sub_82364A88);
REX_EXTERN(__imp__sub_82364A20);
REX_EXTERN(__imp__sub_82364BB8);
REX_EXTERN(__imp__sub_823646E8);
REX_EXTERN(__imp__sub_8228D108);
REX_EXTERN(__imp__sub_82364858);

namespace {

constexpr uint32_t kPool0 = 0x82FAC600u;
constexpr uint32_t kPool1 = 0x82FAC604u;

inline uint32_t LoadU32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, sizeof v);
  return __builtin_bswap32(v);
}

// Milliseconds since the first hook fired; matches the "~11 s after boot" language of the crash notes.
uint64_t NowMs() {
  static const auto t0 = std::chrono::steady_clock::now();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
}

uint32_t GuestTid() { return rex::system::XThread::GetCurrentThreadId(); }

// Host thread currently inside sub_82364A88 (0 = none). A second thread entering, or a lookup from a
// thread other than the owner while it is set, is the race the crash notes predict.
std::atomic<uint32_t> g_reset_owner{0};

std::mutex g_state_mutex;
std::unordered_set<uint32_t> g_torn_down;  // pool objects torn down since their last construction

// Serialization for the nb_pool_reset_lock experiment. Recursive because the lazy create in
// sub_82364A20 calls sub_82364A88 on the same thread.
std::recursive_mutex g_pool_mutex;

std::atomic<uint32_t> g_stale_logs{0};

bool Tracing() { return REXCVAR_GET(nb_pool_trace); }
bool Locking() { return REXCVAR_GET(nb_pool_reset_lock); }

}  // namespace

namespace nb::hooks {

bool PoolWasTornDown(uint32_t pool) {
  std::lock_guard<std::mutex> guard(g_state_mutex);
  return g_torn_down.contains(pool);
}

}  // namespace nb::hooks

// sub_82364A88(bytes = r3, count = r4): destroy both pools, rebuild when both args are non-zero.
REX_HOOK_RAW(sub_82364A88) {
  if (!Tracing() && !Locking()) {
    __imp__sub_82364A88(ctx, base);
    return;
  }
  std::unique_lock<std::recursive_mutex> serialize(g_pool_mutex, std::defer_lock);
  if (Locking()) serialize.lock();

  const uint32_t host_tid = rex::thread::current_thread_id();
  const uint32_t previous_owner = g_reset_owner.exchange(host_tid);
  if (previous_owner != 0 && previous_owner != host_tid) {
    REXLOG_ERROR("nb_pool: CONCURRENT RESET at {} ms: host tid {:#x} entered sub_82364A88 while host tid {:#x} "
                 "is still inside it (guest tid {:#x}, lr 0x{:08X})",
                 NowMs(), host_tid, previous_owner, GuestTid(), static_cast<uint32_t>(ctx.lr));
  }
  if (Tracing()) {
    REXLOG_INFO("nb_pool: reset begin at {} ms: bytes={} count={} pool0=0x{:08X} pool1=0x{:08X} host tid {:#x} "
                "guest tid {:#x} lr 0x{:08X}",
                NowMs(), ctx.r3.u32, ctx.r4.u32, LoadU32(base, kPool0), LoadU32(base, kPool1), host_tid,
                GuestTid(), static_cast<uint32_t>(ctx.lr));
  }

  __imp__sub_82364A88(ctx, base);

  if (Tracing()) {
    REXLOG_INFO("nb_pool: reset end at {} ms: pool0=0x{:08X} pool1=0x{:08X} host tid {:#x}", NowMs(),
                LoadU32(base, kPool0), LoadU32(base, kPool1), host_tid);
  }
  // Same-thread nesting (lazy create -> reset) keeps the owner; otherwise release it.
  g_reset_owner.store(previous_owner == host_tid ? host_tid : 0);
}

// sub_82364A20(selector = r3): returns pool 0 or pool 1, lazily rebuilding both when pool 0 is null.
REX_HOOK_RAW(sub_82364A20) {
  if (!Tracing() && !Locking()) {
    __imp__sub_82364A20(ctx, base);
    return;
  }
  std::unique_lock<std::recursive_mutex> serialize(g_pool_mutex, std::defer_lock);
  if (Locking()) serialize.lock();

  const uint32_t host_tid = rex::thread::current_thread_id();
  const uint32_t owner = g_reset_owner.load();
  const uint32_t pool0 = LoadU32(base, kPool0);
  if (owner != 0 && owner != host_tid) {
    REXLOG_ERROR("nb_pool: LOOKUP DURING RESET at {} ms: host tid {:#x} (guest {:#x}) called sub_82364A20 while "
                 "host tid {:#x} is resetting; pool0=0x{:08X} lr 0x{:08X}",
                 NowMs(), host_tid, GuestTid(), owner, pool0, static_cast<uint32_t>(ctx.lr));
  }
  if (pool0 == 0 && Tracing()) {
    REXLOG_WARN("nb_pool: lazy create taken at {} ms by host tid {:#x} (guest {:#x}), lr 0x{:08X}, reset owner {:#x}",
                NowMs(), host_tid, GuestTid(), static_cast<uint32_t>(ctx.lr), owner);
  }

  __imp__sub_82364A20(ctx, base);
}

// sub_82364BB8(pool = r3, flag = r4): teardown.
REX_HOOK_RAW(sub_82364BB8) {
  const uint32_t pool = ctx.r3.u32;
  bool duplicate = false;
  {
    std::lock_guard<std::mutex> guard(g_state_mutex);
    duplicate = !g_torn_down.insert(pool).second;
  }
  if (duplicate) {
    REXLOG_ERROR("nb_pool: DOUBLE TEARDOWN at {} ms: pool 0x{:08X} torn down again without a construction in "
                 "between; host tid {:#x} guest tid {:#x} lr 0x{:08X}",
                 NowMs(), pool, rex::thread::current_thread_id(), GuestTid(), static_cast<uint32_t>(ctx.lr));
  } else if (Tracing()) {
    REXLOG_INFO("nb_pool: teardown at {} ms: pool 0x{:08X} levels={} region=0x{:08X} table=0x{:08X} host tid {:#x} "
                "guest tid {:#x} lr 0x{:08X}",
                NowMs(), pool, LoadU32(base, pool + 16), LoadU32(base, pool + 28), LoadU32(base, pool + 44),
                rex::thread::current_thread_id(), GuestTid(), static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_82364BB8(ctx, base);
}

// sub_823646E8(obj = r3, orders = r4, count = r5, flags = r6): constructor.
REX_HOOK_RAW(sub_823646E8) {
  const uint32_t pool = ctx.r3.u32;
  {
    std::lock_guard<std::mutex> guard(g_state_mutex);
    g_torn_down.erase(pool);
  }
  if (Tracing()) {
    REXLOG_INFO("nb_pool: construct at {} ms: pool 0x{:08X} orders={} count={} flags=0x{:08X} host tid {:#x} "
                "guest tid {:#x} lr 0x{:08X}",
                NowMs(), pool, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, rex::thread::current_thread_id(), GuestTid(),
                static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_823646E8(ctx, base);
}

namespace {

// Shared by the two pool access points: a pool pointer that was torn down, or an access while another
// thread is inside the reset, is the stale-pointer scenario. Rate-limited: the first 32, then every 256th.
void CheckPoolAccess(const char* site, PPCContext& ctx) {
  if (!Tracing()) return;
  const uint32_t pool = ctx.r3.u32;
  const uint32_t host_tid = rex::thread::current_thread_id();
  const uint32_t owner = g_reset_owner.load();
  const bool stale = nb::hooks::PoolWasTornDown(pool);
  const bool during_reset = owner != 0 && owner != host_tid;
  if (!stale && !during_reset) return;
  const uint32_t n = ++g_stale_logs;
  if (n > 32 && (n % 256) != 0) return;
  REXLOG_ERROR("nb_pool: {} at {} ms: {}{}pool 0x{:08X} host tid {:#x} guest tid {:#x} lr 0x{:08X} [{}]", site,
               NowMs(), stale ? "USE AFTER TEARDOWN " : "", during_reset ? "DURING RESET BY ANOTHER THREAD " : "",
               pool, host_tid, GuestTid(), static_cast<uint32_t>(ctx.lr), n);
}

}  // namespace

// sub_8228D108(pool = r3, size = r4): buddy sub-allocation.
REX_HOOK_RAW(sub_8228D108) {
  CheckPoolAccess("alloc", ctx);
  __imp__sub_8228D108(ctx, base);
}

// sub_82364858(pool = r3, chunk = r4): "does this pool own the chunk", used by the release path.
REX_HOOK_RAW(sub_82364858) {
  CheckPoolAccess("owns", ctx);
  __imp__sub_82364858(ctx, base);
}

// Worker message pump (sub_82327560): the type-21 job pumps the XACT engine until idle. Mid-asm hooks
// at loc_823275EC (job loop head) and loc_82327710 (common exit) time the job per thread, so the log
// shows whether a boot-time job is still running when the re-init resets the pools.
namespace {
thread_local uint64_t t_job_begin_ms = 0;
thread_local uint32_t t_job_rounds = 0;
}  // namespace

void nb_pool_trace_worker_job_round(PPCRegister& r28) {
  if (!Tracing()) return;
  if (t_job_rounds++ == 0) {
    t_job_begin_ms = NowMs();
    REXLOG_INFO("nb_pool: worker type-21 job begin at {} ms: message 0x{:08X} host tid {:#x} guest tid {:#x}",
                t_job_begin_ms, r28.u32, rex::thread::current_thread_id(), GuestTid());
  }
}

void nb_pool_trace_worker_job_exit(PPCRegister& r28) {
  if (!Tracing() || t_job_rounds == 0) return;
  REXLOG_INFO("nb_pool: worker type-21 job end at {} ms after {} rounds ({} ms): message 0x{:08X} host tid {:#x}",
              NowMs(), t_job_rounds, NowMs() - t_job_begin_ms, r28.u32, rex::thread::current_thread_id());
  t_job_rounds = 0;
}
