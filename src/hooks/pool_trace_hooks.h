// nb - streaming buddy-pool lifecycle trace (docs/known_issues.md, "boot heap corruption").
//
// The game keeps two streaming buffer pools (48-byte objects at 0x82FAC600 / 0x82FAC604, each a buddy
// allocator over an XPhysicalAlloc region). The main thread destroys and rebuilds them without a lock
// during the audio re-init (sub_82364A88), while a worker thread that is still finishing boot-time
// bundle loads looks them up through sub_82364A20 (which lazily rebuilds on a null). Hooks in
// pool_trace_hooks.cpp record every reset, lookup, teardown and construction with thread ids so the
// interleaving can be read off one boot's log, and can serialize the two under one host mutex.

#pragma once

#include <cstdint>

namespace nb::hooks {

// True when `pool` (guest address of a 48-byte pool object) has been torn down by sub_82364BB8 and
// not constructed again since. Used by the meInternalFree skip hook to classify the pointer it skips.
bool PoolWasTornDown(uint32_t pool);

}  // namespace nb::hooks
