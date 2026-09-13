"""Apply sdk-patches 0005 (audio null-driver guard) and 0006 (XamAlloc assert -> warning).

0005: rex::audio::AudioSystem::SubmitFrame dereferences clients_[index].driver without a null
check in release builds (only an assert). The game re-initializes its audio device about nine
seconds after boot (XAudio CXenonRenderer), and a frame submitted for a client that was just
unregistered crashed the Release soak (audio_system.cpp:276, read of address 0). The guard
drops the frame with a rate-limited warning.

0006: rex::kernel::xam::XamAlloc_entry asserts unk == 0; Nuts & Bolts passes a non-zero flag,
which stops every Debug build in a breakpoint (xam_info.cpp:301). The assert becomes a
warning logged once per distinct flag value.

Usage: python apply_0005_0006_audio_xam.py <sdk_root>   (idempotent)
"""
import pathlib
import sys

sdk = pathlib.Path(sys.argv[1])


def patch(rel, old, new, marker):
    path = sdk / rel
    s = path.read_text(encoding="utf-8")
    if marker in s:
        print(f"{rel}: already applied")
        return
    assert s.count(old) == 1, (rel, s.count(old), old[:60])
    path.write_text(s.replace(old, new), encoding="utf-8", newline="\n")
    print(f"{rel}: patched")


patch("src/audio/audio_system.cpp",
      "  auto global_lock = global_critical_region_.Acquire();\n"
      "  assert_true(index < kMaximumClientCount);\n"
      "  assert_true(clients_[index].driver != NULL);\n"
      "  (clients_[index].driver)->SubmitFrame(samples_ptr);\n",
      "  auto global_lock = global_critical_region_.Acquire();\n"
      "  // nb patch 0005: a frame can arrive for a client that was just unregistered (the game\n"
      "  // re-creates its audio device shortly after boot); in release builds the asserts below\n"
      "  // are compiled out and the null driver crashed the process. Drop the frame instead.\n"
      "  if (index >= kMaximumClientCount || clients_[index].driver == nullptr) {\n"
      "    static uint32_t dropped = 0;\n"
      "    if (++dropped <= 8 || (dropped % 1000) == 0) {\n"
      "      REXAPU_WARN(\"AudioSystem::SubmitFrame: no driver for client {} (dropped {} frames)\", index,\n"
      "                  dropped);\n"
      "    }\n"
      "    return;\n"
      "  }\n"
      "  (clients_[index].driver)->SubmitFrame(samples_ptr);\n",
      "nb patch 0005")

patch("src/kernel/xam/xam_info.cpp",
      "u32 XamAlloc_entry(u32 unk, u32 size, mapped_u32 out_ptr) {\n"
      "  assert_true(unk == 0);\n",
      "u32 XamAlloc_entry(u32 unk, u32 size, mapped_u32 out_ptr) {\n"
      "  // nb patch 0006: Nuts & Bolts passes a non-zero first argument; Xenia asserts it is zero,\n"
      "  // which halted every Debug build. Report each distinct value once and carry on.\n"
      "  if (unk != 0) {\n"
      "    static uint32_t seen = 0xFFFFFFFFu;\n"
      "    if (seen != unk) {\n"
      "      seen = unk;\n"
      "      REXKRNL_WARN(\"XamAlloc: unhandled flags 0x{:08X} (size {})\", unk, size);\n"
      "    }\n"
      "  }\n",
      "nb patch 0006")
