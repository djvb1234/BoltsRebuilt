#include "native_upload_shadow_window.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

int main() {
  using nb::gpu::NativeUploadShadowWindowEnabled;
  uint64_t checks = 0;
  auto check = [&](bool value) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL check %llu\n", static_cast<unsigned long long>(checks)); std::exit(1); }
  };
  for (uint32_t first : {0u, 3u, 6000u}) {
    for (uint32_t period = 1; period <= 17; ++period) {
      for (uint64_t frame = 0; frame < uint64_t(first) + 16 * period; ++frame) {
        bool expected = false;
        for (uint64_t block = 1; block < 16; block += 2) {
          const uint64_t begin = uint64_t(first) + block * period;
          expected |= frame >= begin && frame < begin + period;
        }
        check(NativeUploadShadowWindowEnabled(true, frame, first, period) == expected);
        check(!NativeUploadShadowWindowEnabled(false, frame, first, period));
        const uint64_t block = frame >= first ? (frame - first) / period : 0;
        check(NativeUploadShadowWindowEnabled(true, frame, first, period, true) ==
              (frame >= first && (block % 4 == 1 || block % 4 == 2)));
        check(NativeUploadShadowWindowEnabled(true, frame, first, 0));
      }
    }
  }
  check(!NativeUploadShadowWindowEnabled(true, 5999, 6000, 240));
  check(!NativeUploadShadowWindowEnabled(true, 6000, 6000, 240));
  check(NativeUploadShadowWindowEnabled(true, 6240, 6000, 240));
  check(!NativeUploadShadowWindowEnabled(true, 6480, 6000, 240));
  check(NativeUploadShadowWindowEnabled(true, UINT64_MAX, 0, 1));
  check(!NativeUploadShadowWindowEnabled(true, UINT64_MAX, 1, 1));
  std::printf("PASS %llu schedule checks\n", static_cast<unsigned long long>(checks));
}
