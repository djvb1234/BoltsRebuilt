// Original synthetic address checks. No captured shader data is used here.
#include "validate_ucode.h"

#include <array>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
using Opcode = rex::graphics::ucode::ControlFlowOpcode;

// Pack two 48-bit CF instructions into one host-order 96-bit group, using the
// SDK's documented encoding. EXEC count occupies low bits 12..14; a branch uses
// low bits 0..12 for its target and bit 13 for an unconditional jump.
std::vector<uint32_t> Pair(Opcode a, uint32_t a_low, Opcode b = Opcode::kNop,
                           uint32_t b_low = 0) {
  return {a_low, (uint32_t(a) << 12) | (b_low << 16),
          (b_low >> 16) | (uint32_t(b) << 28)};
}

void Check(std::string_view name, const std::vector<uint32_t>& words, bool accepted) {
  bool actual = true;
  try {
    shader_disasm::ValidateUcodeAddresses(words);
  } catch (const std::runtime_error&) {
    actual = false;
  }
  if (actual != accepted) {
    throw std::runtime_error(std::string(name) + ": unexpected address-preflight result");
  }
}
}  // namespace

int main() {
  try {
    Check("empty input", {}, false);
    Check("incomplete CF pair", {0, 0}, false);
    Check("empty terminating EXEC at input end", Pair(Opcode::kExecEnd, 1), true);
    Check("all NOPs are address-safe, not proof of validity", {0, 0, 0}, true);
    Check("EXEC overlaps its CF pair", Pair(Opcode::kExecEnd, 0), false);
    Check("zero-count EXEC beyond input", Pair(Opcode::kExecEnd, 2), false);
    auto backwards = Pair(Opcode::kNop, 0);
    const auto later_exec = Pair(Opcode::kExecEnd, 1);
    backwards.insert(backwards.end(), later_exec.begin(), later_exec.end());
    Check("CF discovery shrinks into an already scanned pair", backwards, false);

    for (const auto opcode : std::array{Opcode::kExec, Opcode::kExecEnd,
             Opcode::kCondExec, Opcode::kCondExecEnd, Opcode::kCondExecPred,
             Opcode::kCondExecPredEnd, Opcode::kCondExecPredClean,
             Opcode::kCondExecPredCleanEnd}) {
      auto words = Pair(opcode, 0x1001);  // One ALU instruction at group 1.
      Check("EXEC span lacks its final group", words, false);
      words.insert(words.end(), {0, 0, 0});
      Check("EXEC span ends exactly at input boundary", words, true);
    }
    Check("second-half EXEC also checked",
          Pair(Opcode::kExecEnd, 1, Opcode::kExecEnd, 0x1001), false);
    auto trailing = Pair(Opcode::kExecEnd, 1);
    trailing.insert(trailing.end(), {0x12345678, 0x9ABCDEF0});
    Check("unused incomplete trailing group", trailing, true);

    for (const auto opcode : std::array{Opcode::kLoopStart, Opcode::kLoopEnd,
                                       Opcode::kCondCall, Opcode::kCondJmp}) {
      Check("branch targets final CF instruction",
            Pair(opcode, 1, Opcode::kExecEnd, 1), true);
      Check("branch targets one CF past end",
            Pair(opcode, 2, Opcode::kExecEnd, 1), false);
    }

    // The ALU instruction exports vector X to eM0: destination 33, export bit
    // 15, vector write bit 16. Other operands/opcodes are zero and irrelevant
    // to this address-only test.
    auto memexport = Pair(Opcode::kExecEnd, 0x1001);
    memexport.insert(memexport.end(), {0x00018021, 0, 0});
    Check("memory export ends before NOP padding", memexport, true);
    memexport[1] = uint32_t(Opcode::kExec) << 12;
    Check("memory export falls through NOP beyond CF", memexport, false);
    memexport[1] = uint32_t(Opcode::kCondExecEnd) << 12;
    Check("conditional end retains memory-export fallthrough", memexport, false);

    auto stopped = Pair(Opcode::kExec, 0x1002, Opcode::kAlloc);
    stopped.insert(stopped.end(), {0, 0, 0, 0x00018021, 0, 0});
    Check("alloc stops memory-export walk before padding", stopped, true);
    auto bounded_loop = Pair(Opcode::kExec, 0x1001, Opcode::kCondJmp, 0x2001);
    bounded_loop.insert(bounded_loop.end(), {0x00018021, 0, 0});
    Check("unconditional backward branch has no fallthrough", bounded_loop, true);
    auto call_root = Pair(Opcode::kExecEnd, 0x1001, Opcode::kCondCall, 0);
    call_root.insert(call_root.end(), {0x00018021, 0, 0});
    Check("CALL starts its own memory-export successor walk", call_root, false);

    std::cout << "All synthetic ucode address checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
