// swap_hook.cpp - the ONE strong definition of rex_D3DDevice_Swap (0x82287590) in nb.exe.
//
// Adopt as src/hooks/swap_hook.cpp. Four deliverables want this symbol (N hooks_m1, S
// swap_busy_hook, U/I nb_frame_capture_plugin, X draw_hooks); only one strong definition
// links, so each feature exposes enter/exit functions and this wrapper calls them in a
// fixed order. Static-registration would leave the order to initialization luck, and the
// order matters: S's "inside" time must be stamped right around the original body, and
// X's frame boundary must come after everything that belongs to the finished frame.
//
// Rule for the repo: no other translation unit may write REX_HOOK_RAW(rex_D3DDevice_Swap)
// or REX_HOOK(rex_D3DDevice_Swap, ...). Add a slot here instead.

#include <bit>
#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "hooks_m1.h"
#include "ultrawide/nb_ultrawide.h"

REX_EXTERN(__imp__rex_D3DDevice_Swap);

namespace {

// Swaps that present the previous image again, with no rendering in between:
// - the D3D worker thread, every 30 ms while the title has released the device, presents the device's copy
//   of the last frontbuffer header at device+14844 (nb_recomp.102.cpp:14067-14075, lr 0x82662F50);
// - two terminal loops re-present fb[idx] (lr 0x8223C6B8, nb_recomp.136.cpp:858-879; lr 0x822B6DA0,
//   nb_recomp.105.cpp:2645-2660).
constexpr uint32_t kDeviceHeldFrontbuffer = 14844;
constexpr uint32_t kRepresentLoopReturn = 0x8223C6B8;
constexpr uint32_t kRepresentThreadReturn = 0x822B6DA0;

uint32_t LoadU32(uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, rex::memory::GuestPtr<uint8_t*>(base, address), sizeof(value));
  return std::byteswap(value);
}

// Whether this Swap emits a swap packet, and the frontbuffer address the packet will carry.
// - rex_D3DDevice_Swap allocates ring space and calls VdSwap only while [device+21532] is zero; otherwise it
//   skips both (nb_recomp.131.cpp:1844-1857). r3 is the device, r4 the frontbuffer's texture header.
// - VdSwap takes the virtual address from dword 1 of the fetch constant the Swap body copies from
//   header+28 (base_address << 12, above the 6 format bits; nb_recomp.131.cpp:1530-1556). It emits no packet
//   when that address has no physical mapping, and otherwise writes the physical address into the packet,
//   which rexgpu-nb's IssueSwap receives as frontbuffer_ptr (SDK xboxkrnl_video.cpp, VdSwap).
bool SwapPacketFrontbuffer(uint8_t* base, uint32_t device, uint32_t header, uint32_t* frontbuffer) {
  if (!device || !header || LoadU32(base, device + 21532) != 0) return false;
  const uint32_t virtual_address = LoadU32(base, header + 32) & 0xFFFFF000u;
  const uint32_t physical = rex::system::kernel_state()->memory()->GetPhysicalAddress(virtual_address);
  if (physical == UINT32_MAX) return false;
  *frontbuffer = physical;
  return true;
}

}  // namespace

REX_HOOK_RAW(rex_D3DDevice_Swap) {
  // --- before the original: observers that need the pre-call registers ---------------
  nb_m1_swap_enter(ctx);             // N: swap-rate log (nb_m1_log_swap)
  // UW: queue the presenter aspect of the image this Swap presents, once per emitted swap packet. A
  // re-present repeats the held image's aspect and latches nothing.
  uint32_t frontbuffer = 0;
  const bool emits_packet = SwapPacketFrontbuffer(base, ctx.r3.u32, ctx.r4.u32, &frontbuffer);
  const bool represent = ctx.r4.u32 == ctx.r3.u32 + kDeviceHeldFrontbuffer || ctx.lr == kRepresentLoopReturn ||
                         ctx.lr == kRepresentThreadReturn;
  if (represent) nb::uw::OnGuestRepresent(emits_packet, frontbuffer);
  else nb::uw::OnGuestSwapEnter(emits_packet, frontbuffer);
  // nb_attr_swap_enter(ctx, base);  // X: RecordHook(kSwap) callstack capture (when adopted)
  // nb_swaptrace_enter();           // S: entry timestamp, last before the body (when adopted)

  __imp__rex_D3DDevice_Swap(ctx, base);

  // --- after the original ------------------------------------------------------------
  // nb_swaptrace_exit();            // S: inside-time stamp, first after the body
  if (!represent) nb::uw::OnGuestSwap();  // UW: latch nb_ultrawide for the next guest frame
  // nb_attr_swap_exit();            // X: OnGuestSwap frame boundary
  // U/I: nothing here; its capture point is CP-side (NbCommandProcessor::IssueSwap) and
  // its guest-side hook was a no-op call-through.
}
