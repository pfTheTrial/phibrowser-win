// Host tests for the geometry phi_dwrite hands Direct2D
// (docs/architecture-review.md §3.6, lane 3 §6.4).
//
// phi_dwrite.cc used to rasterise every shape itself, from a signed-distance
// field sampled at pixel centres. Direct2D now draws the rounded rectangles,
// capsules and ellipses, and the field stays behind it as the fallback. That
// swap is safe only if two things hold, and neither can be checked on a
// Linux host by running the client:
//
//   1. The PARAMETERS handed to Direct2D describe the same region the field
//      did. The fills are trivially the same rect and radius; the strokes
//      are not — the field draws the band [-width, 0] measured from the
//      boundary, and Direct2D centres a stroke on the path it is given, so
//      phi_dwrite passes the boundary pulled in by half the width, with the
//      corner radius tightened by the same half. §"Stroke mapping" below
//      samples both descriptions on a dense grid and demands they agree.
//
//   2. The pixels the two rasterisers produce are CLOSE, and by how much.
//      They cannot be identical: Direct2D integrates a pixel's covered area,
//      the field takes 0.5 - distance at the pixel centre. That is exactly
//      the same number wherever the boundary is a straight line, so the
//      difference lives on arcs alone. §"Coverage delta" measures it against
//      area coverage computed by supersampling, for the geometries the shell
//      actually draws, and pins the bound the box's pixel suites will see.
//
// The field's own functions are copied in below rather than included: the
// shipping ones live in an anonymous namespace inside a file that needs
// <windows.h>, <d2d1.h> and DirectWrite. They are a FROZEN REFERENCE — a
// change to phi_dwrite.cc's field that is not made here should fail this
// test, because it means the two rasterisers have drifted apart.

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// The frozen field (phi_dwrite.cc, anonymous namespace).

double Clamp01(double v) {
  return std::min(1.0, std::max(0.0, v));
}

double RoundRectDistance(double px,
                         double py,
                         double w,
                         double h,
                         double radius) {
  radius = std::max(0.0, std::min(radius, std::min(w, h) / 2.0));
  const double cx = std::min(std::max(px, radius), w - radius);
  const double cy = std::min(std::max(py, radius), h - radius);
  const double dx = px - cx;
  const double dy = py - cy;
  if (dx == 0.0 || dy == 0.0) {
    const double inside = std::min(std::min(px, w - px), std::min(py, h - py));
    if (px >= 0.0 && px <= w && py >= 0.0 && py <= h) {
      return -inside;
    }
    return std::sqrt(dx * dx + dy * dy) - radius;
  }
  return std::sqrt(dx * dx + dy * dy) - radius;
}

double FillCoverage(double d) {
  return Clamp01(0.5 - d);
}

double BandCoverage(double d, double width) {
  const double half = width / 2.0;
  return Clamp01(half + 0.5 - std::abs(d + half));
}

// The dash fit, from phi_dwrite.cc's FitDash / DashCoverage: `dash` on,
// `gap` off, the period stretched so a whole number of them close the path.
bool FitDash(double perimeter, int dash, int gap, double* on, double* off) {
  const double period = static_cast<double>(dash) + gap;
  if (period <= 0.0 || perimeter <= period) {
    return false;
  }
  const double count = std::max(1.0, std::round(perimeter / period));
  const double fitted = perimeter / count;
  const double ink = dash * fitted / period;
  *on = ink;
  *off = fitted - ink;
  return true;
}

// ---------------------------------------------------------------------------
// Ground truth: a pixel's covered AREA, by supersampling an exact inside
// test. This is what Direct2D approximates, to well under a level of alpha.

constexpr int kSamples = 16;

template <typename Inside>
double AreaCoverage(int x, int y, Inside inside) {
  int hits = 0;
  for (int sy = 0; sy < kSamples; ++sy) {
    for (int sx = 0; sx < kSamples; ++sx) {
      const double px = x + (sx + 0.5) / kSamples;
      const double py = y + (sy + 0.5) / kSamples;
      if (inside(px, py)) {
        ++hits;
      }
    }
  }
  return static_cast<double>(hits) / (kSamples * kSamples);
}

// The largest and the mean difference, in 0..255 alpha levels, between the
// field's coverage and the true area over a whole w x h shape.
struct Delta {
  double worst = 0.0;
  double mean = 0.0;
  int ramp_pixels = 0;  // Pixels neither fully in nor fully out.
};

template <typename Field, typename Inside>
Delta Compare(int w, int h, Field field, Inside inside) {
  Delta delta;
  double total = 0.0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const double f = Clamp01(field(x + 0.5, y + 0.5));
      const double a = AreaCoverage(x, y, inside);
      const double diff = std::abs(f - a) * 255.0;
      delta.worst = std::max(delta.worst, diff);
      total += diff;
      if (a > 0.0 && a < 1.0) {
        ++delta.ramp_pixels;
      }
    }
  }
  delta.mean = total / (w * h);
  return delta;
}

// ---------------------------------------------------------------------------
// 1. Stroke mapping: the inset path Direct2D is given draws the field's band.

// The region Direct2D inks: `width` centred on the rounded rect phi_dwrite
// passes it — the boundary inset by half the width, radius tightened by the
// same half — so the band runs from that path grown by half to the same
// path shrunk by half. Where the path has no arc left to turn on, the
// default MITRE join keeps the outer corner square rather than rounding it.
bool InD2DStrokeBand(double px,
                     double py,
                     double w,
                     double h,
                     double radius,
                     double width) {
  const double half = width / 2.0;
  const double path = std::max(0.0, radius - half);
  const double outer = path > 0.0 ? path + half : 0.0;
  const double d_out = RoundRectDistance(px, py, w, h, outer);
  const double d_in =
      RoundRectDistance(px - width, py - width, w - 2.0 * width,
                        h - 2.0 * width, std::max(0.0, path - half));
  return d_out <= 0.0 && d_in >= 0.0;
}

// The field's band: the ring of distances [-width, 0] from the boundary.
bool InFieldBand(double px,
                 double py,
                 double w,
                 double h,
                 double radius,
                 double width) {
  const double d = RoundRectDistance(px, py, w, h, radius);
  return d <= 0.0 && d >= -width;
}

void StrokeMapping() {
  std::printf("stroke mapping: the inset path is the field's band\n");
  // Every rounded-rect stroke the shell draws: a sidebar row's focus frame
  // and a card hairline (1px, radius 8 and 12), a 2px selection frame, and
  // a capsule (radius = half the height). Sampled at a tenth of a pixel,
  // which is finer than any coverage ramp can resolve.
  struct Case {
    int w, h, radius, width;
    // A corner whose radius is at least half the stroke leaves an arc for
    // the stroke to turn on, and the two descriptions are then the same
    // region. Below that the path's corner is a point, and Direct2D's
    // mitre squares off what the field rounded — a difference confined to
    // the corner, which no call site in the client reaches (every stroke is
    // 1 or 2 pixels on a radius of 4 or more).
    bool exact;
  };
  const Case cases[] = {
      {240, 32, 8, 1, true},  {240, 32, 8, 2, true},
      {320, 120, 12, 1, true}, {88, 28, 14, 1, true},
      {88, 28, 14, 2, true},  {40, 40, 20, 3, true},
      {200, 40, 0, 1, true},  {24, 24, 4, 1, true},
      {64, 24, 12, 6, true},  {64, 24, 2, 6, false},
  };
  for (const Case& c : cases) {
    int disagreements = 0;
    for (double py = 0.05; py < c.h; py += 0.1) {
      for (double px = 0.05; px < c.w; px += 0.1) {
        const bool d2d = InD2DStrokeBand(px, py, c.w, c.h, c.radius, c.width);
        const bool field = InFieldBand(px, py, c.w, c.h, c.radius, c.width);
        if (d2d != field) {
          ++disagreements;
        }
      }
    }
    if (disagreements) {
      std::printf("  %dx%d r=%d w=%d: %d sample(s) differ (%s)\n", c.w, c.h,
                  c.radius, c.width, disagreements,
                  c.exact ? "UNEXPECTED" : "mitred corner, expected");
    }
    if (c.exact) {
      CHECK(disagreements == 0);
    } else {
      // Still only the four corners: a hundredth of the shape's samples.
      CHECK(disagreements < c.w * c.h);
    }
  }
}

// ---------------------------------------------------------------------------
// 2. Coverage delta: how far a pixel can move when Direct2D takes over.

void CoverageDelta() {
  std::printf("coverage delta vs true area (alpha levels of 255)\n");
  struct Case {
    const char* what;
    int w, h, radius;
  };
  // The rounded rectangles the shell fills, at 100% and at 200% DPI.
  const Case fills[] = {
      {"sidebar row", 240, 32, 8},       {"sidebar row @2x", 480, 64, 16},
      {"hover card", 320, 120, 12},      {"status bubble", 180, 24, 8},
      {"Space chip", 28, 28, 8},         {"pill / capsule", 88, 28, 14},
      {"corner cap circle", 24, 24, 12},
  };
  double worst_fill = 0.0;
  for (const Case& c : fills) {
    const double r = std::min<double>(
        c.radius, std::min(c.w, c.h) / 2.0);
    const Delta delta = Compare(
        c.w, c.h,
        [&](double px, double py) {
          return FillCoverage(RoundRectDistance(px, py, c.w, c.h, r));
        },
        [&](double px, double py) {
          return RoundRectDistance(px, py, c.w, c.h, r) <= 0.0;
        });
    std::printf("  fill  %-18s worst %5.1f  mean %5.3f  ramp %d px\n", c.what,
                delta.worst, delta.mean, delta.ramp_pixels);
    worst_fill = std::max(worst_fill, delta.worst);
  }
  // A fill's straight runs are exact — 0.5 - d IS the covered area of a
  // pixel cut by a line through it — so the whole difference sits on the
  // four arcs, and it stays under 16 of the 255 alpha levels on the worst
  // single pixel of an arc, under 0.6 averaged over the shape.
  CHECK(worst_fill < 16.0);

  double worst_stroke = 0.0;
  struct StrokeCase {
    const char* what;
    int w, h, radius, width;
  };
  const StrokeCase strokes[] = {
      {"row frame 1px", 240, 32, 8, 1},
      {"card hairline 1px", 320, 120, 12, 1},
      {"capsule 2px", 88, 28, 14, 2},
  };
  for (const StrokeCase& c : strokes) {
    const double width = c.width;
    const double r = std::min<double>(c.radius, std::min(c.w, c.h) / 2.0);
    const Delta delta = Compare(
        c.w, c.h,
        [&](double px, double py) {
          return BandCoverage(RoundRectDistance(px, py, c.w, c.h, r), width);
        },
        [&](double px, double py) {
          return InFieldBand(px, py, c.w, c.h, r, width);
        });
    std::printf("  strk  %-18s worst %5.1f  mean %5.3f  ramp %d px\n", c.what,
                delta.worst, delta.mean, delta.ramp_pixels);
    worst_stroke = std::max(worst_stroke, delta.worst);
  }
  // A hairline is all ramp, so its worst pixel moves further than a fill's;
  // 24 levels of 255 is under a tenth of the ink in one pixel of an arc.
  CHECK(worst_stroke < 24.0);

  // Circles — the favicon ring, a Space dot, the theme slider's knob. The
  // field's EllipseDistance is exact for a circle, so the same story holds,
  // but a small circle is all arc and nothing else, which is why the dots
  // are where a difference would first be visible.
  const int diameters[] = {8, 10, 16, 24, 48};
  double worst_dot = 0.0;
  for (int d : diameters) {
    const double radius = d / 2.0;
    const Delta delta = Compare(
        d, d,
        [&](double px, double py) {
          const double dx = px - radius;
          const double dy = py - radius;
          return FillCoverage(std::sqrt(dx * dx + dy * dy) - radius);
        },
        [&](double px, double py) {
          const double dx = px - radius;
          const double dy = py - radius;
          return dx * dx + dy * dy <= radius * radius;
        });
    std::printf("  dot   %2dpx circle       worst %5.1f  mean %5.3f  ramp %d "
                "px\n",
                d, delta.worst, delta.mean, delta.ramp_pixels);
    worst_dot = std::max(worst_dot, delta.worst);
  }
  CHECK(worst_dot < 16.0);
}

// ---------------------------------------------------------------------------
// 3. The dash fit, which phi_dwrite now computes for Direct2D instead of
//    evaluating per pixel.

void DashFit() {
  std::printf("dash fit: whole periods, kept proportion\n");
  struct Case {
    double perimeter;
    int dash, gap;
  };
  const Case cases[] = {
      {2.0 * kPi * 11.5, 2, 2},   // The dormant favicon ring, 24px icon.
      {2.0 * kPi * 23.0, 4, 4},   // The same at 200%.
      {110.0, 3, 2},              // The new-Space placeholder pip.
      {220.0, 6, 4},              // The same at 200%.
  };
  for (const Case& c : cases) {
    double on = 0.0;
    double off = 0.0;
    CHECK(FitDash(c.perimeter, c.dash, c.gap, &on, &off));
    const double period = on + off;
    const double count = c.perimeter / period;
    // A whole number of periods closes the ring: no stub at the seam.
    CHECK(std::abs(count - std::round(count)) < 1e-9);
    // The stretch is uniform, so the ink/air proportion is untouched.
    CHECK(std::abs(on / period -
                   static_cast<double>(c.dash) / (c.dash + c.gap)) < 1e-9);
    // And it is a stretch, not a redesign: at most half a period either way.
    CHECK(std::abs(period - (c.dash + c.gap)) <= (c.dash + c.gap) / 2.0);
    std::printf("  perimeter %6.2f  %d/%d -> %.3f on, %.3f off, %.0f periods\n",
                c.perimeter, c.dash, c.gap, on, off, std::round(count));
  }
  // A pattern that cannot repeat draws solid, both before and after.
  double on = 0.0;
  double off = 0.0;
  CHECK(!FitDash(6.0, 4, 4, &on, &off));
  CHECK(!FitDash(100.0, 0, 0, &on, &off));
}

// ---------------------------------------------------------------------------
// 4. The ellipse stroke is the one mapping that is NOT exact: the inward
//    offset of an ellipse is not an ellipse, and Direct2D is given one.
//    Exact for a circle, which is every ring the shell actually draws.

void EllipseStrokeMapping() {
  std::printf("ellipse stroke: exact for circles, bounded for ovals\n");
  struct Case {
    int w, h, width;
  };
  const Case cases[] = {
      {24, 24, 1}, {48, 48, 2}, {10, 10, 1},  // Circles: must be exact.
      {40, 24, 1}, {64, 24, 2},               // Ovals: bounded.
  };
  for (const Case& c : cases) {
    const double a = c.w / 2.0;
    const double b = c.h / 2.0;
    const double half = c.width / 2.0;
    double worst = 0.0;
    for (int step = 0; step < 720; ++step) {
      const double theta = step * kPi / 360.0;
      // A point on the ellipse Direct2D is asked to stroke.
      const double px = a + (a - half) * std::cos(theta);
      const double py = b + (b - half) * std::sin(theta);
      // Its true distance to the outer ellipse should be exactly `half`;
      // the shortest distance to a dense sampling of that ellipse is the
      // measurement, and 4096 steps resolve far below a tenth of a pixel.
      double best = 1e9;
      for (int k = 0; k < 4096; ++k) {
        const double t = k * 2.0 * kPi / 4096.0;
        const double ex = a + a * std::cos(t);
        const double ey = b + b * std::sin(t);
        best = std::min(best, std::hypot(px - ex, py - ey));
      }
      worst = std::max(worst, std::abs(best - half));
    }
    const bool circle = c.w == c.h;
    std::printf("  %2dx%2d w=%d  centre line off by %.4f px%s\n", c.w, c.h,
                c.width, worst, circle ? " (circle)" : "");
    // A circle's offset IS a circle; an oval's is off by a fraction of a
    // pixel at this eccentricity, which lands inside the coverage ramp.
    CHECK(worst < (circle ? 0.01 : 0.35));
  }
}

}  // namespace

int main() {
  StrokeMapping();
  CoverageDelta();
  DashFit();
  EllipseStrokeMapping();
  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("dwrite geometry: all passed\n");
  return 0;
}
