// Pure helpers for nb_ultrawide (docs/ultrawide.md). This file holds three things the hooks, the overlay
// and the state code share:
// - the parsers for the aspect and render-scale settings;
// - the packed presenter state;
// - the small amount of matrix and FOV arithmetic.
// It has no SDK dependencies, so tools/test_ultrawide_math.cpp checks it standalone.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace nb::uw {

// The game's own display aspect. Every camera mode it ships is authored against 16:9 (sub_82239C40).
inline constexpr double kBaseAspect = 16.0 / 9.0;
// The XUI canvas is a virtual 1280x720; sub_82208338 scales it by rtW/1280 and rtH/720.
inline constexpr double kCanvasCenterX = 640.0;
// A policy cap. With the default 55-degree lens, 8:1 already gives about 153 degrees horizontally
// (HorizontalFovDeg(55, 8)), and wider aspects add mostly edge distortion.
inline constexpr double kMaxAspect = 8.0;
// The SDK clamps each draw-resolution-scale axis to 7.
inline constexpr uint32_t kMaxScale = 7;

struct Aspect {
  double ratio = kBaseAspect;
  uint32_t w = 16;  // integers for the presenter, which fits the guest output to w:h
  uint32_t h = 9;
};

namespace detail {
inline std::string Normalize(std::string_view text) {
  std::string s;
  for (char c : text) {
    const auto u = static_cast<unsigned char>(c);
    if (!std::isspace(u)) s.push_back(static_cast<char>(std::tolower(u)));
  }
  return s;
}
// A positive finite number that spans the whole string.
inline bool ParseNumber(const std::string& s, double* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (end != s.c_str() + s.size() || !std::isfinite(v) || v <= 0.0) return false;
  *out = v;
  return true;
}
}  // namespace detail

// The values nb_ultrawide treats as off: empty, "off", "0", "none" and "false", in any case and with any
// spaces. Anything else that does not parse is a mistake, and the log says so.
inline bool IsOffToken(std::string_view text) {
  const std::string s = detail::Normalize(text);
  return s.empty() || s == "off" || s == "0" || s == "none" || s == "false";
}

// nb_ultrawide accepts "W:H" or "WxH" with positive numbers, e.g. 32:9, 21:9, 5120x1440 or 2.37:1.
// Only aspects wider than 16:9, and at most 8:1, are accepted. A narrower aspect would need Vert+, and the
// HUD squeeze would become a stretch.
inline bool ParseAspect(std::string_view text, Aspect* out) {
  if (IsOffToken(text)) return false;
  const std::string s = detail::Normalize(text);
  const size_t sep = s.find_first_of(":x");
  if (sep == std::string::npos) return false;
  double a = 0.0, b = 0.0;
  if (!detail::ParseNumber(s.substr(0, sep), &a) || !detail::ParseNumber(s.substr(sep + 1), &b)) return false;
  const double ratio = a / b;
  if (!(ratio > kBaseAspect + 1e-4) || ratio > kMaxAspect) return false;
  out->ratio = ratio;
  if (a == std::floor(a) && b == std::floor(b) && a <= 65535.0 && b <= 65535.0) {
    out->w = static_cast<uint32_t>(a);
    out->h = static_cast<uint32_t>(b);
  } else {
    out->w = static_cast<uint32_t>(std::llround(ratio * 10000.0));
    out->h = 10000;
  }
  return true;
}

// nb_ultrawide_scale accepts two forms:
// - "auto" (or empty): ceil(k) x 2 whenever the view is wider than 16:9, else 1x1. That renders 1440 lines,
//   the height of today's 32:9 and 21:9 panels: 2560x1440 at 32:9, which the presenter widens to 5120x1440.
//   With the draw-extent estimate on (nb.toml), 2x2 measured as fast per draw as 2x1 in Town, still and
//   sweeping, because the command thread limits both (docs/ultrawide.md, "The render scale").
// - "SxT": picks it explicitly, with each axis 1..7.
inline bool ParseScale(std::string_view text, double k, uint32_t* sx, uint32_t* sy) {
  const std::string s = detail::Normalize(text);
  if (s.empty() || s == "auto") {
    const double wanted = std::ceil(std::max(1.0, k) - 1e-9);
    *sx = static_cast<uint32_t>(std::min<double>(wanted, kMaxScale));
    *sy = *sx > 1 ? 2 : 1;
    return true;
  }
  const size_t sep = s.find('x');
  if (sep == std::string::npos) return false;
  double a = 0.0, b = 0.0;
  if (!detail::ParseNumber(s.substr(0, sep), &a) || !detail::ParseNumber(s.substr(sep + 1), &b)) return false;
  if (a != std::floor(a) || b != std::floor(b) || a > kMaxScale || b > kMaxScale) return false;
  *sx = static_cast<uint32_t>(a);
  *sy = static_cast<uint32_t>(b);
  return true;
}

// The presenter's aspect packed into one word, so a reader on another thread can never see a torn w:h.
// Layout: bit 63 = active, bits 32..62 = h, bits 0..31 = w. The decimal aspect path gives w up to 80000.
inline uint64_t PackPresent(bool active, uint32_t w, uint32_t h) {
  return (uint64_t(active) << 63) | (uint64_t(h & 0x7FFFFFFFu) << 32) | uint64_t(w);
}
inline bool UnpackPresent(uint64_t word, uint32_t* w, uint32_t* h) {
  *w = static_cast<uint32_t>(word);
  *h = static_cast<uint32_t>(word >> 32) & 0x7FFFFFFFu;
  return (word >> 63) != 0;
}

// The XUI view transform is a D3DX row-vector matrix, with the translation in row 3. Squeezing
// horizontally around the canvas centre, x' = center + (x - center) * kh, before that transform is H * M,
// where H = [kh 0 0 0; 0 1 0 0; 0 0 1 0; center*(1-kh) 0 0 1]. So row 3 gains center*(1-kh)*row 0, and
// then row 0 scales by kh.
inline void SqueezeViewRows(float row0[4], float row3[4], double kh, double center = kCanvasCenterX) {
  const double shift = center * (1.0 - kh);
  for (int j = 0; j < 4; ++j) {
    row3[j] = static_cast<float>(double(row3[j]) + shift * double(row0[j]));
    row0[j] = static_cast<float>(double(row0[j]) * kh);
  }
}

// A scissor edge the glyph renderer built as origin + extent, with no scale. Under the squeeze it becomes
// origin + extent * kh.
inline double SqueezeFromOrigin(double edge, double origin, double kh) { return origin + kh * (edge - origin); }

inline double RadiansToDegrees(double r) { return r * 57.29577951308232; }
inline double DegreesToRadians(double d) { return d * 0.017453292519943295; }
// The horizontal FOV for a vertical FOV at an aspect. Hor+ keeps the vertical fixed.
inline double HorizontalFovDeg(double vfov_deg, double aspect) {
  return RadiansToDegrees(2.0 * std::atan(std::tan(DegreesToRadians(vfov_deg) * 0.5) * aspect));
}
// The vertical FOV after scaling tan(vfov/2), clamped to 1..170 degrees. A non-finite result keeps the
// input.
inline double ScaledVerticalFovDeg(double vfov_deg, double tan_scale) {
  const double v =
      RadiansToDegrees(2.0 * std::atan(std::tan(DegreesToRadians(vfov_deg) * 0.5) * tan_scale));
  return std::isfinite(v) ? std::clamp(v, 1.0, 170.0) : vfov_deg;
}

}  // namespace nb::uw
