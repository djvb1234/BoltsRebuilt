// Standalone control for src/ultrawide/nb_ultrawide_math.h (run by C:\rex\dl\claude_check_ultrawide_math.ps1):
// the setting parsers, the XUI view squeeze and the FOV arithmetic the hooks and the F10 overlay rely on.
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "nb_ultrawide_math.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("FAIL: %s\n", what);
  }
}

bool Near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

}  // namespace

int main() {
  using namespace nb::uw;

  Aspect a;
  Check(!ParseAspect("off", &a), "off");
  Check(!ParseAspect("", &a), "empty");
  Check(!ParseAspect("16:9", &a), "16:9 is the game's own aspect");
  Check(!ParseAspect("4:3", &a), "narrower than 16:9 is rejected");
  Check(!ParseAspect("32:0", &a), "zero height is rejected");
  Check(!ParseAspect("32:9x", &a), "trailing garbage is rejected");
  Check(!ParseAspect("-32:9", &a), "a negative width is rejected");
  Check(!ParseAspect("100:9", &a), "wider than 8:1 is rejected");
  Check(!ParseAspect("32", &a), "a single number is rejected");
  Check(ParseAspect("32:9", &a) && a.w == 32 && a.h == 9 && Near(a.ratio, 32.0 / 9.0, 1e-12), "32:9");
  Check(ParseAspect(" 5120 x 1440 ", &a) && a.w == 5120 && a.h == 1440 && Near(a.ratio, 32.0 / 9.0, 1e-12),
        "5120x1440 with spaces");
  Check(ParseAspect("21:9", &a) && a.w == 21 && a.h == 9, "21:9");
  Check(ParseAspect("2.37:1", &a) && a.h == 10000 && a.w == 23700 && Near(a.ratio, 2.37, 1e-12), "2.37:1");
  Check(ParseAspect("3440X1440", &a) && a.w == 3440 && a.h == 1440, "uppercase X");

  Check(IsOffToken("off") && IsOffToken(" OFF ") && IsOffToken("") && IsOffToken("None") && IsOffToken("0") &&
            IsOffToken("false"),
        "off tokens");
  Check(!IsOffToken("32:9") && !IsOffToken("abc") && !IsOffToken("16:9"), "non-off values are not off tokens");

  // The presenter state packs into one word and back, including the decimal path's large width.
  {
    uint32_t w = 0, h = 0;
    Check(UnpackPresent(PackPresent(true, 32, 9), &w, &h) && w == 32 && h == 9, "pack 32:9 active");
    Check(!UnpackPresent(PackPresent(false, 16, 9), &w, &h) && w == 16 && h == 9, "pack 16:9 inactive");
    Check(UnpackPresent(PackPresent(true, 80000, 10000), &w, &h) && w == 80000 && h == 10000,
          "pack the widest decimal aspect");
    Check(UnpackPresent(PackPresent(true, 5120, 1440), &w, &h) && w == 5120 && h == 1440, "pack 5120x1440");
  }

  uint32_t sx = 0, sy = 0;
  Check(ParseScale("auto", 2.0, &sx, &sy) && sx == 2 && sy == 2, "auto at 32:9 is 2x2");
  Check(ParseScale("", 1.3125, &sx, &sy) && sx == 2 && sy == 2, "auto at 21:9 is 2x2");
  Check(ParseScale("auto", 1.0, &sx, &sy) && sx == 1 && sy == 1, "auto at 16:9 is 1x1");
  Check(ParseScale("auto", 2.0 + 1e-12, &sx, &sy) && sx == 2 && sy == 2,
        "auto tolerates rounding just above an integer");
  Check(ParseScale("auto", 50.0, &sx, &sy) && sx == 7 && sy == 2, "auto is clamped to the SDK's 7");
  Check(ParseScale("4x2", 2.0, &sx, &sy) && sx == 4 && sy == 2, "4x2");
  Check(ParseScale(" 3 X 2 ", 2.0, &sx, &sy) && sx == 3 && sy == 2, "3x2 with spaces and uppercase");
  Check(!ParseScale("8x2", 2.0, &sx, &sy), "an axis above 7 is rejected");
  Check(!ParseScale("2.5x2", 2.0, &sx, &sy), "a fractional scale is rejected");
  Check(!ParseScale("big", 2.0, &sx, &sy), "garbage is rejected");

  // The HUD squeeze on the game's own view at a 1280-wide target (diag(1, 1, 1, 1)), kh = 0.5. Row
  // vectors: target x = canvas x * row0[0] + row3[0].
  {
    float row0[4] = {1, 0, 0, 0}, row3[4] = {0, 0, 0, 1};
    SqueezeViewRows(row0, row3, 0.5);
    auto x_of = [&](double x) { return x * row0[0] + row3[0]; };
    Check(Near(x_of(0.0), 320.0, 1e-4), "the canvas left edge lands at 320");
    Check(Near(x_of(640.0), 640.0, 1e-4), "the canvas centre stays put");
    Check(Near(x_of(1280.0), 960.0, 1e-4), "the canvas right edge lands at 960");
    Check(row3[3] == 1.0f && row0[1] == 0.0f && row0[3] == 0.0f, "the other elements are untouched");
  }
  // With a scale and an offset in the game's view the squeeze still applies first, in canvas space.
  {
    float row0[4] = {2, 0, 0, 0}, row3[4] = {10, 0, 0, 1};
    SqueezeViewRows(row0, row3, 0.5);
    auto x_of = [&](double x) { return x * row0[0] + row3[0]; };
    // The game's view maps x -> 2x + 10 and the squeeze x -> 0.5x + 320, so together x -> x + 650.
    Check(Near(x_of(0.0), 650.0, 1e-3) && Near(x_of(1280.0), 1930.0, 1e-3), "the squeeze composes before the view");
  }
  {
    float row0[4] = {1.5f, 0.25f, 0, 0}, row3[4] = {3, 4, 5, 1};
    SqueezeViewRows(row0, row3, 1.0);
    Check(row0[0] == 1.5f && row0[1] == 0.25f && row3[0] == 3.0f && row3[1] == 4.0f, "kh = 1 is the identity");
  }

  // FOV arithmetic at the default lens's 55-degree vertical FOV.
  Check(Near(HorizontalFovDeg(55.0, 16.0 / 9.0), 85.57, 0.05), "55 vertical is about 85.6 horizontal at 16:9");
  Check(Near(HorizontalFovDeg(55.0, 32.0 / 9.0), 123.24, 0.05), "55 vertical is about 123.2 horizontal at 32:9");
  Check(Near(ScaledVerticalFovDeg(55.0, 1.0), 55.0, 1e-9), "a tan scale of 1 keeps the FOV");
  Check(Near(ScaledVerticalFovDeg(60.0, 0.5), 32.20, 0.05), "a tan scale of 0.5 on 60 degrees");
  Check(ScaledVerticalFovDeg(170.0, 4.0) == 170.0, "clamped to 170 degrees");
  Check(Near(HorizontalFovDeg(55.0, kMaxAspect), 153.0, 0.1), "the 8:1 cap is about 153 degrees at the default lens");
  Check(ScaledVerticalFovDeg(55.0, std::nan("")) == 55.0, "a NaN scale keeps the FOV");

  // The glyph scissor squeeze: an edge 200 px right of an element origin at 1000 ends 100 px right of it.
  Check(Near(SqueezeFromOrigin(1200.0, 1000.0, 0.5), 1100.0, 1e-9), "scissor edge squeezes toward its origin");
  Check(Near(SqueezeFromOrigin(1000.0, 1000.0, 0.5), 1000.0, 1e-9), "the origin itself stays put");
  Check(Near(SqueezeFromOrigin(1234.0, 1000.0, 1.0), 1234.0, 1e-9), "kh = 1 leaves the scissor alone");

  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
