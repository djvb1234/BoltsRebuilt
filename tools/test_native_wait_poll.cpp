// Standalone controls for src/gpu/native/native_wait_poll.h. No game data, no GPU.
// Build: clang++ -std=c++20 -O2 -Wall -Wextra -Werror -DNOMINMAX -Isrc/gpu/native
//        tools/test_native_wait_poll.cpp -o control.exe
#include "native_wait_poll.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
int g_checks = 0;
int g_failures = 0;
void Check(bool ok, const char* what, int line) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("FAIL line %d: %s\n", line, what);
  }
}
#define CHECK(x) Check((x), #x, __LINE__)

using nb::gpu::NativeWaitSample;
using nb::gpu::NativeWaitSpinBudget;
using nb::gpu::NativeWaitTargetTable;

void BudgetDisabled() {
  NativeWaitSpinBudget budget(0);
  for (uint64_t t : {0ull, 1ull, 1000000ull, ~0ull}) CHECK(!budget.ShouldSpin(t));
  CHECK(!budget.started());
  CHECK(!budget.exhausted());
}

void BudgetSpinsUntilSpent() {
  NativeWaitSpinBudget budget(5000000);  // 5 ms
  CHECK(budget.ShouldSpin(1000));        // first failure starts the budget
  CHECK(budget.started());
  CHECK(budget.ShouldSpin(1000));        // same instant
  CHECK(budget.ShouldSpin(1000 + 4999999));
  CHECK(!budget.ShouldSpin(1000 + 5000000));  // exactly spent
  CHECK(budget.exhausted());
  // Once spent it never spins again, even if a later read looks earlier.
  CHECK(!budget.ShouldSpin(1000));
  CHECK(!budget.ShouldSpin(1000 + 10));
}

void BudgetClockBackwards() {
  NativeWaitSpinBudget budget(5000000);
  CHECK(budget.ShouldSpin(900000));
  CHECK(!budget.ShouldSpin(899999));  // backwards: stop spinning
  CHECK(budget.exhausted());
}

void BudgetNearWrap() {
  NativeWaitSpinBudget budget(5000000);
  const uint64_t start = ~0ull - 100;
  CHECK(budget.ShouldSpin(start));
  CHECK(budget.ShouldSpin(start + 50));
  CHECK(!budget.ShouldSpin(start - 1));  // arithmetic would wrap; treated as backwards
}

// Models the vendored WAIT_REG_MEM loop's decision per failed read: with a
// budget, reads continue at yield cadence until either the predicate matches or
// the budget is spent, after which reads happen only at the original sleep
// cadence. Both loops complete at most one sleep period after the condition
// holds. The spinning loop is not pointwise earlier: after the budget its
// sleep-cadence reads have a different phase from the original loop's.
uint64_t SimulateCompletion(uint64_t condition_true_at_ns, uint64_t budget_ns,
                            uint64_t yield_ns, uint64_t sleep_ns) {
  NativeWaitSpinBudget budget(budget_ns);
  uint64_t now = 0;
  for (int reads = 0; reads < 1000000; ++reads) {
    if (now >= condition_true_at_ns) return now;
    now += budget.ShouldSpin(now) ? yield_ns : sleep_ns;
  }
  return ~0ull;
}

void LoopModel() {
  const uint64_t sleep = 1300000;  // measured: a requested 1 ms precise sleep takes about 1.3 ms
  const uint64_t yield = 2000;
  std::vector<uint64_t> conditions;
  for (uint64_t t = 0; t <= 20000000; t += 37000) conditions.push_back(t);
  for (uint64_t c : conditions) {
    const uint64_t original = SimulateCompletion(c, 0, yield, sleep);
    const uint64_t spinning = SimulateCompletion(c, 5000000, yield, sleep);
    CHECK(original >= c && spinning >= c);                 // never completes before the condition
    CHECK(original - c <= sleep && spinning - c <= sleep);  // the shared one-sleep-period bound
    if (c < 5000000) CHECK(spinning - c < yield + 1);       // inside the budget: within one yield
  }
  // Disabled budget reproduces the original cadence exactly.
  CHECK(SimulateCompletion(2100000, 0, yield, sleep) == 2600000);
  // The intended effect: over condition times spread across one 288 Hz vblank
  // period, mean lateness falls from about half a sleep period to about one yield.
  uint64_t original_late = 0, spinning_late = 0, samples = 0;
  for (uint64_t c = 0; c < 3472222; c += 1000, ++samples) {
    original_late += SimulateCompletion(c, 0, yield, sleep) - c;
    spinning_late += SimulateCompletion(c, 5000000, yield, sleep) - c;
  }
  CHECK(original_late / samples > 500000);
  CHECK(spinning_late / samples <= yield);
}

NativeWaitSample Sample(uint32_t address, uint64_t ns, bool vblank, bool timed, uint64_t advance) {
  return NativeWaitSample{true, address, 3, 0xFFFFFFFFu, 7, 5, 7, ns, vblank, timed, advance};
}

void TableAggregates() {
  NativeWaitTargetTable table;
  table.Record(Sample(0x100, 2000000, true, true, 50000));
  table.Record(Sample(0x100, 1000000, false, false, 0));
  table.Record(Sample(0x100, 3000000, true, true, 400000));
  const auto* entry = table.begin();
  CHECK(entry->used && entry->address == 0x100 && entry->packets == 3);
  CHECK(entry->blocked_ns == 6000000 && entry->max_ns == 3000000);
  CHECK(entry->vblank_advanced == 2 && entry->timed_advances == 2);
  CHECK(entry->advance_to_match_ns == 450000 && entry->matched_within_100us == 1);
  CHECK(entry->last_ref == 7 && entry->last_first_value == 5 && entry->last_match_value == 7);
  CHECK(!(entry + 1)->used);
  CHECK(table.untracked() == 0);
}

void TableKeysAndCapacity() {
  NativeWaitTargetTable table;
  for (uint32_t i = 0; i < NativeWaitTargetTable::kCapacity; ++i) table.Record(Sample(0x1000 + i, 10, false, false, 0));
  table.Record(Sample(0x9999, 10, false, false, 0));  // ninth distinct key
  CHECK(table.untracked() == 1);
  table.Record(Sample(0x1003, 20, false, false, 0));  // existing key still aggregates when full
  size_t used = 0;
  for (const auto* e = table.begin(); e != table.end(); ++e) {
    if (!e->used) continue;
    ++used;
    if (e->address == 0x1003) CHECK(e->packets == 2 && e->blocked_ns == 30);
  }
  CHECK(used == NativeWaitTargetTable::kCapacity);
  // Register and memory targets with the same number are different keys, as are functions and masks.
  NativeWaitTargetTable split;
  NativeWaitSample reg = Sample(0x20, 1, false, false, 0);
  reg.is_memory = false;
  NativeWaitSample fn = Sample(0x20, 1, false, false, 0);
  fn.function = 5;
  NativeWaitSample mask = Sample(0x20, 1, false, false, 0);
  mask.mask = 0xFF;
  split.Record(Sample(0x20, 1, false, false, 0));
  split.Record(reg);
  split.Record(fn);
  split.Record(mask);
  size_t split_used = 0;
  for (const auto* e = split.begin(); e != split.end(); ++e) split_used += e->used;
  CHECK(split_used == 4);
}

void TableFreeSlotAfterRemovedOrderIsStable() {
  // A key recorded later must find its existing slot, never a new one.
  NativeWaitTargetTable table;
  table.Record(Sample(0xA, 1, false, false, 0));
  table.Record(Sample(0xB, 1, false, false, 0));
  table.Record(Sample(0xB, 1, false, false, 0));
  table.Record(Sample(0xA, 1, false, false, 0));
  size_t used = 0;
  for (const auto* e = table.begin(); e != table.end(); ++e) {
    if (!e->used) continue;
    ++used;
    CHECK(e->packets == 2);
  }
  CHECK(used == 2);
}
}  // namespace

int main() {
  BudgetDisabled();
  BudgetSpinsUntilSpent();
  BudgetClockBackwards();
  BudgetNearWrap();
  LoopModel();
  TableAggregates();
  TableKeysAndCapacity();
  TableFreeSlotAfterRemovedOrderIsStable();
  std::printf("native_wait_poll controls: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
