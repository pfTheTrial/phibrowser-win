// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_profile_card.h"

#include <commdlg.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string_view>
#include <utility>

#include "base/containers/span.h"
#include "base/logging.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/utf_string_conversions.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_i18n.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "comdlg32.lib")

namespace phi_client {

namespace {

using Microsoft::WRL::ComPtr;

// The Mac card's metrics, in dips.
constexpr int kCardWidthDip = 240;
constexpr int kCardHeightDip = 380;
constexpr int kCornerDip = 8;
constexpr int kDragStepDip = 20;      // sensitivity
constexpr int kNameBottomInsetDip = 32;
constexpr int kNameSideInsetDip = 24;
constexpr int kNameFontDip = 16;
constexpr int kDateTopDip = -12;      // The label hangs off the top edge.
constexpr int kDateFontMaxDip = 70;   // adjustDateLabelFontSize's ladder.
constexpr int kDateFontMinDip = 20;
constexpr size_t kMaxNameChars = 20;
constexpr double kReturnSeconds = 0.15;
constexpr double kIntroSeconds = 1.0;
// The colour map: black -> white, and the theme colour from 10% up.
constexpr double kColorMapKnee = 0.1;
// The overlays: the tint view's alpha and the dot image's (0.15 over a
// tint of the accent at 50%).
constexpr double kTintAlpha = 0.10;
constexpr double kDotAlpha = 0.15 * 0.5;
constexpr size_t kComposedCacheCap = 24;

std::wstring ResourceDir(const std::wstring& subdir) {
  wchar_t exe_path[MAX_PATH] = {};
  ::GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  std::wstring path(exe_path);
  const size_t slash = path.find_last_of(L'\\');
  if (slash == std::wstring::npos) {
    return std::wstring();
  }
  path.resize(slash + 1);
  return path + L"resources\\" + subdir + L"\\";
}

ComPtr<IWICImagingFactory> WicFactory() {
  ComPtr<IWICImagingFactory> factory;
  ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&factory));
  return factory;
}

// Decodes `path` to an 8-bit grey plane of exactly `width` x `height`
// (the frames share the card's aspect; Fant scaling for the downsample).
bool DecodeGray(IWICImagingFactory* factory,
                const std::wstring& path,
                int width,
                int height,
                std::vector<uint8_t>* out) {
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(factory->CreateDecoderFromFilename(
          path.c_str(), nullptr, GENERIC_READ,
          WICDecodeMetadataCacheOnDemand, &decoder)) ||
      FAILED(decoder->GetFrame(0, &frame))) {
    return false;
  }
  ComPtr<IWICBitmapScaler> scaler;
  if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
      FAILED(scaler->Initialize(frame.Get(), static_cast<UINT>(width),
                                static_cast<UINT>(height),
                                WICBitmapInterpolationModeFant))) {
    return false;
  }
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(factory->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat8bppGray,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
    return false;
  }
  out->assign(static_cast<size_t>(width) * height, 0);
  return SUCCEEDED(converter->CopyPixels(nullptr, static_cast<UINT>(width),
                                         static_cast<UINT>(out->size()),
                                         out->data()));
}

// HSB -> COLORREF (the Mac's NSColor(hue:saturation:brightness:)).
COLORREF FromHsb(double h, double s, double b) {
  h = std::fmod(std::max(0.0, h), 1.0) * 6.0;
  const int i = static_cast<int>(std::floor(h));
  const double f = h - i;
  const double p = b * (1 - s);
  const double q = b * (1 - s * f);
  const double t = b * (1 - s * (1 - f));
  double r = 0, g = 0, bl = 0;
  switch (i % 6) {
    case 0: r = b; g = t; bl = p; break;
    case 1: r = q; g = b; bl = p; break;
    case 2: r = p; g = b; bl = t; break;
    case 3: r = p; g = q; bl = b; break;
    case 4: r = t; g = p; bl = b; break;
    default: r = b; g = p; bl = q; break;
  }
  return RGB(static_cast<int>(std::lround(r * 255)),
             static_cast<int>(std::lround(g * 255)),
             static_cast<int>(std::lround(bl * 255)));
}

double HueOf(COLORREF c) {
  const double r = GetRValue(c) / 255.0;
  const double g = GetGValue(c) / 255.0;
  const double b = GetBValue(c) / 255.0;
  const double mx = std::max({r, g, b});
  const double mn = std::min({r, g, b});
  const double d = mx - mn;
  if (d <= 0.0) {
    return 0.0;
  }
  double h = 0.0;
  if (mx == r) {
    h = std::fmod((g - b) / d, 6.0);
  } else if (mx == g) {
    h = (b - r) / d + 2.0;
  } else {
    h = (r - g) / d + 4.0;
  }
  h /= 6.0;
  return h < 0 ? h + 1.0 : h;
}

// "2025-11-05T11:32:18Z" -> "2025.11.5" in local time (the Mac's
// formatToLocalDate). Empty when the string is not that shape.
std::string LocalDate(const std::string& iso) {
  // Fixed positions: "YYYY-MM-DDTHH:MM:SS".
  const auto digits = [&](size_t at, size_t count, int* out) {
    int value = 0;
    for (size_t i = at; i < at + count; ++i) {
      if (i >= iso.size() || iso[i] < '0' || iso[i] > '9') {
        return false;
      }
      value = value * 10 + (iso[i] - '0');
    }
    *out = value;
    return true;
  };
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  if (iso.size() < 19 || iso[4] != '-' || iso[7] != '-' || iso[10] != 'T' ||
      iso[13] != ':' || iso[16] != ':' || !digits(0, 4, &y) ||
      !digits(5, 2, &mo) || !digits(8, 2, &d) || !digits(11, 2, &h) ||
      !digits(14, 2, &mi) || !digits(17, 2, &s)) {
    return std::string();
  }
  SYSTEMTIME utc = {};
  utc.wYear = static_cast<WORD>(y);
  utc.wMonth = static_cast<WORD>(mo);
  utc.wDay = static_cast<WORD>(d);
  utc.wHour = static_cast<WORD>(h);
  utc.wMinute = static_cast<WORD>(mi);
  utc.wSecond = static_cast<WORD>(s);
  SYSTEMTIME local = utc;
  if (iso.back() == 'Z' || iso.find('+', 19) != std::string::npos ||
      iso.find('-', 19) != std::string::npos) {
    // Offsets other than Z are rare from the service; treat them as UTC
    // rather than mis-parsing.
    ::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
  }
  return std::to_string(local.wYear) + "." + std::to_string(local.wMonth) +
         "." + std::to_string(local.wDay);
}

HFONT MakeFont(std::wstring_view face, int pixel_height, int weight) {
  LOGFONTW lf = {};
  lf.lfHeight = -pixel_height;
  lf.lfWeight = weight;
  lf.lfCharSet = DEFAULT_CHARSET;
  lf.lfQuality = CLEARTYPE_QUALITY;
  const base::span<wchar_t> name(lf.lfFaceName);
  const size_t n = std::min(face.size(), name.size() - 1);
  std::ranges::copy(face.substr(0, n), name.begin());
  name[n] = L'\0';
  return ::CreateFontIndirectW(&lf);
}

// A top-down 32bpp DIB with a DC, for rendering text coverage off-screen.
struct Scratch {
  HDC dc = nullptr;
  HBITMAP bitmap = nullptr;
  HGDIOBJ previous = nullptr;
  // The DIB section's pixels, owned by `bitmap` (freed with it).
  RAW_PTR_EXCLUSION uint8_t* bits = nullptr;
  int width = 0;
  int height = 0;

  base::span<uint8_t> pixels() const {
    // SAFETY: the DIB section is width * height * 4 bytes, allocated by
    // CreateDIBSection for exactly that BITMAPINFO.
    return UNSAFE_BUFFERS(base::span(bits, static_cast<size_t>(width) * height * 4));
  }

  bool Create(int w, int h) {
    width = w;
    height = h;
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* raw = nullptr;
    bitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &raw, nullptr,
                                0);
    if (!bitmap) {
      return false;
    }
    bits = static_cast<uint8_t*>(raw);
    dc = ::CreateCompatibleDC(nullptr);
    previous = ::SelectObject(dc, bitmap);
    return true;
  }
  ~Scratch() {
    if (dc) {
      ::SelectObject(dc, previous);
      ::DeleteDC(dc);
    }
    if (bitmap) {
      ::DeleteObject(bitmap);
    }
  }
};

// Renders `text` centred at the top of a `width` x `height` plane in the
// given font and returns its coverage (0..255 per pixel).
std::vector<uint8_t> TextCoverage(const std::wstring& text,
                                  HFONT font,
                                  int width,
                                  int height,
                                  UINT format) {
  std::vector<uint8_t> coverage(static_cast<size_t>(width) * height, 0);
  Scratch scratch;
  if (!scratch.Create(width, height)) {
    return coverage;
  }
  std::ranges::fill(scratch.pixels(), 0);
  HGDIOBJ old_font = ::SelectObject(scratch.dc, font);
  ::SetBkMode(scratch.dc, TRANSPARENT);
  ::SetTextColor(scratch.dc, RGB(255, 255, 255));
  RECT rc = {0, 0, width, height};
  PhiDrawTextAA(scratch.dc, text.c_str(), -1, &rc, format | DT_NOCLIP);
  ::SelectObject(scratch.dc, old_font);
  ::GdiFlush();
  const base::span<uint8_t> pixels = scratch.pixels();
  for (size_t i = 0; i < coverage.size(); ++i) {
    coverage[i] = pixels[i * 4 + 1];  // Green: luminance of white.
  }
  return coverage;
}

int TextWidth(HDC dc, HFONT font, const std::wstring& text) {
  HGDIOBJ old_font = ::SelectObject(dc, font);
  RECT rc = {0, 0, 0, 0};
  PhiDrawTextAA(dc, text.c_str(), -1, &rc, DT_CALCRECT | DT_SINGLELINE);
  ::SelectObject(dc, old_font);
  return rc.right - rc.left;
}

// Writes `bgra` (premultiplied, width x height) as a PNG file.
bool WritePng(const std::wstring& path,
              const std::vector<uint8_t>& bgra,
              int width,
              int height) {
  ComPtr<IWICImagingFactory> factory = WicFactory();
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  if (!factory || FAILED(factory->CreateStream(&stream)) ||
      FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
      FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                    &encoder)) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
      FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
      FAILED(frame->Initialize(nullptr))) {
    return false;
  }
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
  if (FAILED(frame->SetSize(static_cast<UINT>(width),
                            static_cast<UINT>(height))) ||
      FAILED(frame->SetPixelFormat(&format))) {
    return false;
  }
  std::vector<uint8_t> pixels = bgra;
  if (FAILED(frame->WritePixels(static_cast<UINT>(height),
                                static_cast<UINT>(width * 4),
                                static_cast<UINT>(pixels.size()),
                                pixels.data())) ||
      FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
    return false;
  }
  return true;
}

}  // namespace

// The decoded planes for one size. Shared between the worker and the UI
// thread; each slot is filled once, under the lock, and never changes.
struct PhiProfileCard::Frames {
  CRITICAL_SECTION lock;
  int width = 0;
  int height = 0;
  uint32_t generation = 0;
  std::vector<std::vector<uint8_t>> grid;   // kRows * kCols, row-major.
  std::vector<std::vector<uint8_t>> intro;  // kIntroFrames.
  std::vector<uint8_t> dots;                // Coverage; empty until read.
  bool dots_done = false;
  int grid_done = 0;
  int intro_done = 0;
  bool retired = false;  // The size changed; the worker stops early.

  Frames() {
    ::InitializeCriticalSection(&lock);
    grid.resize(static_cast<size_t>(kRows) * kCols);
    intro.resize(kIntroFrames);
  }
  ~Frames() { ::DeleteCriticalSection(&lock); }
};

struct PhiProfileCard::Job {
  std::shared_ptr<Frames> frames;
  std::wstring dir;
  HWND notify_hwnd = nullptr;
  UINT notify_msg = 0;
};

PhiProfileCard::PhiProfileCard(const wchar_t* resource_subdir, bool card_chrome)
    : resource_subdir_(resource_subdir), card_chrome_(card_chrome) {}

PhiProfileCard::~PhiProfileCard() {
  if (frames_) {
    ::EnterCriticalSection(&frames_->lock);
    frames_->retired = true;
    ::LeaveCriticalSection(&frames_->lock);
  }
}

// static
uint64_t PhiProfileCard::Now() {
  return ::GetTickCount64();
}

void PhiProfileCard::SetNotify(HWND hwnd, UINT msg) {
  notify_hwnd_ = hwnd;
  notify_msg_ = msg;
}

void PhiProfileCard::SetPixelSize(int width, int height) {
  if (width <= 0 || height <= 0 || (width == width_ && height == height_)) {
    return;
  }
  width_ = width;
  height_ = height;
  composed_.clear();
  dots_.clear();
  dots_loaded_ = false;
  if (frames_) {
    ::EnterCriticalSection(&frames_->lock);
    frames_->retired = true;
    ::LeaveCriticalSection(&frames_->lock);
  }
  auto frames = std::make_shared<Frames>();
  frames->width = width;
  frames->height = height;
  frames->generation = ++generation_;
  frames_ = frames;
  intro_pending_ = true;
  intro_playing_ = false;
  auto* job = new Job();
  job->frames = frames;
  job->dir = ResourceDir(resource_subdir_);
  job->notify_hwnd = notify_hwnd_;
  job->notify_msg = notify_msg_;
  HANDLE thread =
      ::CreateThread(nullptr, 0, &PhiProfileCard::DecodeThread, job, 0, nullptr);
  if (thread) {
    ::CloseHandle(thread);
  } else {
    delete job;
  }
}

// static
DWORD WINAPI PhiProfileCard::DecodeThread(void* param) {
  std::unique_ptr<Job> job(static_cast<Job*>(param));
  const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ComPtr<IWICImagingFactory> factory = WicFactory();
  Frames& frames = *job->frames;
  const std::wstring dir = job->dir;
  const auto retired = [&] {
    ::EnterCriticalSection(&frames.lock);
    const bool r = frames.retired;
    ::LeaveCriticalSection(&frames.lock);
    return r;
  };
  const auto notify = [&] {
    if (job->notify_hwnd && ::IsWindow(job->notify_hwnd)) {
      ::PostMessageW(job->notify_hwnd, job->notify_msg, 0, 0);
    }
  };
  if (factory) {
    const int cx = (kCols + 1) / 2;
    const int cy = (kRows + 1) / 2;
    // The Mac's order: the centre, then the intro, then everything by
    // distance from the centre.
    std::vector<std::pair<int, int>> order;
    for (int r = 1; r <= kRows; ++r) {
      for (int c = 1; c <= kCols; ++c) {
        order.emplace_back(r, c);
      }
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](const auto& a, const auto& b) {
                       return std::abs(a.first - cy) + std::abs(a.second - cx) <
                              std::abs(b.first - cy) + std::abs(b.second - cx);
                     });
    const auto decode_grid = [&](int r, int c) {
      std::vector<uint8_t> plane;
      const std::wstring path = dir + L"frame-" + std::to_wstring(r) + L"-" +
                                std::to_wstring(c) + L".jpg";
      const bool ok = DecodeGray(factory.Get(), path, frames.width,
                                 frames.height, &plane);
      ::EnterCriticalSection(&frames.lock);
      if (ok) {
        frames.grid[static_cast<size_t>(r - 1) * kCols + (c - 1)] =
            std::move(plane);
      }
      ++frames.grid_done;
      ::LeaveCriticalSection(&frames.lock);
      if (!ok) {
        LOG(WARNING) << "[ProfileCard] frame " << r << "-" << c
                     << " did not decode";
      }
    };
    decode_grid(cy, cx);
    notify();
    {
      std::vector<uint8_t> dots;
      const bool ok = DecodeGray(factory.Get(), dir + L"dot-bg.png",
                                 frames.width, frames.height, &dots);
      ::EnterCriticalSection(&frames.lock);
      if (ok) {
        // The pattern is dark dots on white: coverage is the darkness.
        for (uint8_t& v : dots) {
          v = static_cast<uint8_t>(255 - v);
        }
        frames.dots = std::move(dots);
      }
      frames.dots_done = true;
      ::LeaveCriticalSection(&frames.lock);
    }
    for (int i = 1; i <= kIntroFrames && !retired(); ++i) {
      std::vector<uint8_t> plane;
      const bool ok = DecodeGray(factory.Get(),
                                 dir + L"pre-" + std::to_wstring(i) + L".jpg",
                                 frames.width, frames.height, &plane);
      ::EnterCriticalSection(&frames.lock);
      if (ok) {
        frames.intro[static_cast<size_t>(i - 1)] = std::move(plane);
      }
      ++frames.intro_done;
      ::LeaveCriticalSection(&frames.lock);
    }
    notify();
    for (const auto& [r, c] : order) {
      if (retired()) {
        break;
      }
      if (r == cy && c == cx) {
        continue;
      }
      decode_grid(r, c);
    }
    notify();
  } else {
    LOG(ERROR) << "[ProfileCard] no WIC factory; the card stays blank";
  }
  if (SUCCEEDED(com)) {
    ::CoUninitialize();
  }
  return 0;
}

void PhiProfileCard::SetIdentity(const std::string& name,
                                 const std::string& created_at) {
  std::string shown = name;
  // The Mac truncates by character count; count UTF-8 code points.
  size_t chars = 0;
  size_t cut = std::string::npos;
  for (size_t i = 0; i < shown.size(); ++i) {
    if ((static_cast<uint8_t>(shown[i]) & 0xC0) != 0x80) {
      if (chars == kMaxNameChars) {
        cut = i;
        break;
      }
      ++chars;
    }
  }
  if (cut != std::string::npos) {
    shown = shown.substr(0, cut) + "...";
  }
  const std::string date = LocalDate(created_at);
  if (shown == name_ && date == date_) {
    return;
  }
  name_ = shown;
  date_ = date;
}

void PhiProfileCard::SetTheme(COLORREF overlay, bool pure, bool dark) {
  const double hue = HueOf(overlay);
  if (hue == hue_ && pure == pure_ && dark == dark_ && !composed_.empty()) {
    return;
  }
  hue_ = hue;
  pure_ = pure;
  dark_ = dark;
  // The conch: the tint at (.85, .75), or grey for Pure (saturation 0,
  // brightness .5); the accent: full saturation at .37, or black for Pure.
  conch_ = pure ? FromHsb(hue, 0.0, 0.5) : FromHsb(hue, 0.85, 0.75);
  accent_ = pure ? RGB(0, 0, 0) : FromHsb(hue, 1.0, 0.37);
  ResetColorCache();
}

void PhiProfileCard::ResetColorCache() {
  composed_.clear();
}

const std::vector<uint8_t>* PhiProfileCard::GridFrame(int row, int col) {
  if (!frames_ || row < 1 || row > kRows || col < 1 || col > kCols) {
    return nullptr;
  }
  const std::vector<uint8_t>& plane =
      frames_->grid[static_cast<size_t>(row - 1) * kCols + (col - 1)];
  // A slot is written once, under the lock, and read here after the lock
  // says it exists — the vector's buffer never moves afterwards.
  ::EnterCriticalSection(&frames_->lock);
  const bool ready = !plane.empty();
  ::LeaveCriticalSection(&frames_->lock);
  return ready ? &plane : nullptr;
}

const std::vector<uint8_t>* PhiProfileCard::IntroFrame(int index) {
  if (!frames_ || index < 0 || index >= kIntroFrames) {
    return nullptr;
  }
  const std::vector<uint8_t>& plane = frames_->intro[static_cast<size_t>(index)];
  ::EnterCriticalSection(&frames_->lock);
  const bool ready = !plane.empty();
  ::LeaveCriticalSection(&frames_->lock);
  return ready ? &plane : nullptr;
}

void PhiProfileCard::StartIntroIfReady() {
  if (!intro_pending_ || intro_playing_ || dragging_ || !frames_) {
    return;
  }
  ::EnterCriticalSection(&frames_->lock);
  const bool ready = frames_->intro_done >= kIntroFrames;
  ::LeaveCriticalSection(&frames_->lock);
  if (!ready) {
    return;
  }
  intro_pending_ = false;
  intro_playing_ = true;
  intro_start_ = Now();
  intro_frame_ = 0;
}

const std::vector<uint8_t>* PhiProfileCard::CurrentFrame(int* key) {
  if (intro_playing_) {
    if (const std::vector<uint8_t>* plane = IntroFrame(intro_frame_)) {
      *key = 1000 + intro_frame_;
      return plane;
    }
  }
  const int cx = (kCols + 1) / 2;
  const int cy = (kRows + 1) / 2;
  if (const std::vector<uint8_t>* plane = GridFrame(y_, x_)) {
    *key = y_ * 100 + x_;
    return plane;
  }
  *key = cy * 100 + cx;
  return GridFrame(cy, cx);
}

std::vector<uint8_t> PhiProfileCard::Compose(
    const std::vector<uint8_t>& gray,
    int width,
    int height,
    const std::vector<uint8_t>* dots) const {
  const size_t count = static_cast<size_t>(width) * height;
  std::vector<uint8_t> out(count * 4, 255);
  if (gray.size() != count) {
    return out;
  }
  // The colour map, precomputed per grey level: white below the knee
  // blending into the conch colour.
  std::array<uint8_t, 256 * 3> map;
  for (int v = 0; v < 256; ++v) {
    const double t = std::min(1.0, (v / 255.0) / kColorMapKnee);
    map[static_cast<size_t>(v) * 3] =
        static_cast<uint8_t>(std::lround(255 + (GetBValue(conch_) - 255) * t));
    map[static_cast<size_t>(v) * 3 + 1] =
        static_cast<uint8_t>(std::lround(255 + (GetGValue(conch_) - 255) * t));
    map[static_cast<size_t>(v) * 3 + 2] =
        static_cast<uint8_t>(std::lround(255 + (GetRValue(conch_) - 255) * t));
  }
  // The tint over everything, and the dots over that.
  const double tint_b = GetBValue(conch_), tint_g = GetGValue(conch_),
               tint_r = GetRValue(conch_);
  const double dot_b = GetBValue(accent_), dot_g = GetGValue(accent_),
               dot_r = GetRValue(accent_);
  const bool has_dots = dots && dots->size() == count;
  for (size_t i = 0; i < count; ++i) {
    const size_t m = static_cast<size_t>(gray[i]) * 3;
    double b = map[m], g = map[m + 1], r = map[m + 2];
    b += (tint_b - b) * kTintAlpha;
    g += (tint_g - g) * kTintAlpha;
    r += (tint_r - r) * kTintAlpha;
    if (has_dots && (*dots)[i]) {
      const double a = kDotAlpha * ((*dots)[i] / 255.0);
      b += (dot_b - b) * a;
      g += (dot_g - g) * a;
      r += (dot_r - r) * a;
    }
    out[i * 4] = static_cast<uint8_t>(std::lround(b));
    out[i * 4 + 1] = static_cast<uint8_t>(std::lround(g));
    out[i * 4 + 2] = static_cast<uint8_t>(std::lround(r));
  }
  if (!card_chrome_) {
    return out;
  }
  // The rounded corners, into the alpha (premultiplied: every channel).
  const double radius = kCornerDip * (width / static_cast<double>(kCardWidthDip));
  const int span = static_cast<int>(std::ceil(radius)) + 1;
  for (int y = 0; y < height; ++y) {
    if (y >= span && y < height - span) {
      continue;
    }
    for (int x = 0; x < width; ++x) {
      if (x >= span && x < width - span) {
        continue;
      }
      const double coverage =
          PhiRoundRectCoverage(x + 0.5, y + 0.5, width, height, radius);
      if (coverage >= 1.0) {
        continue;
      }
      const size_t p = (static_cast<size_t>(y) * width + x) * 4;
      for (size_t c = 0; c < 4; ++c) {
        out[p + c] = static_cast<uint8_t>(std::lround(out[p + c] * coverage));
      }
    }
  }
  return out;
}

void PhiProfileCard::DrawLabels(HDC dc, const RECT& bounds, double scale) const {
  const int width = bounds.right - bounds.left;
  const int height = bounds.bottom - bounds.top;
  // The date: Impact, shrunk from 70 until it fits the width, hanging 12
  // dips off the top, fading to nothing at its own baseline (the label's
  // white -> clear gradient at 40% of its height).
  if (!date_.empty()) {
    const std::wstring date = base::UTF8ToWide(date_);
    int size = kDateFontMaxDip;
    HFONT font = nullptr;
    for (;;) {
      font = MakeFont(L"Impact", static_cast<int>(std::lround(size * scale)),
                      FW_NORMAL);
      if (!font) {
        break;
      }
      // Impact's glyphs overhang their advance a little; keep 4 dips of
      // air on each side so the digits never touch the card's edge.
      const int fit = width - static_cast<int>(std::lround(8 * scale));
      if (TextWidth(dc, font, date) <= fit || size <= kDateFontMinDip) {
        break;
      }
      ::DeleteObject(font);
      --size;
    }
    if (font) {
      const int label_h = static_cast<int>(std::lround(size * scale * 1.22));
      std::vector<uint8_t> coverage =
          TextCoverage(date, font, width, label_h, DT_CENTER | DT_TOP);
      ::DeleteObject(font);
      std::vector<uint8_t> bgra(static_cast<size_t>(width) * label_h * 4, 0);
      for (int y = 0; y < label_h; ++y) {
        // Opaque from the top down to 40% above the bottom, then fading.
        const double from_bottom = (label_h - y) / static_cast<double>(label_h);
        const double alpha = std::min(1.0, from_bottom / 0.4);
        for (int x = 0; x < width; ++x) {
          const size_t i = static_cast<size_t>(y) * width + x;
          const uint8_t a =
              static_cast<uint8_t>(std::lround(coverage[i] * alpha));
          bgra[i * 4] = a;
          bgra[i * 4 + 1] = a;
          bgra[i * 4 + 2] = a;
          bgra[i * 4 + 3] = a;
        }
      }
      const int top = bounds.top + static_cast<int>(std::lround(kDateTopDip * scale));
      // Clip to the card: rows above its top edge are dropped.
      const int skip = std::max(0, static_cast<int>(bounds.top) - top);
      if (skip < label_h) {
        RECT dest = {bounds.left, top + skip, bounds.right, top + label_h};
        PhiBlendBgra(dc, dest,
                     base::span(bgra).subspan(static_cast<size_t>(skip) * width * 4).data(),
                     width, label_h - skip);
      }
    }
  }
  // The name: the accent, centred, 32 dips up from the bottom.
  if (!name_.empty()) {
    HFONT font = MakeFont(L"Segoe UI",
                          static_cast<int>(std::lround(kNameFontDip * scale)),
                          FW_SEMIBOLD);
    if (font) {
      HGDIOBJ old_font = ::SelectObject(dc, font);
      ::SetBkMode(dc, TRANSPARENT);
      ::SetTextColor(dc, accent_);
      const int inset = static_cast<int>(std::lround(kNameSideInsetDip * scale));
      const int line = static_cast<int>(std::lround(kNameFontDip * scale * 1.4));
      RECT rc = {bounds.left + inset,
                 bounds.bottom -
                     static_cast<int>(std::lround(kNameBottomInsetDip * scale)) -
                     line,
                 bounds.right - inset,
                 bounds.bottom -
                     static_cast<int>(std::lround(kNameBottomInsetDip * scale))};
      PhiDrawTextAA(dc, base::UTF8ToWide(name_).c_str(), -1, &rc,
                    DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS |
                        DT_NOPREFIX);
      ::SelectObject(dc, old_font);
      ::DeleteObject(font);
    }
  }
  (void)height;
}

double PhiProfileCard::Scale() const {
  const int dips = card_chrome_ ? kCardWidthDip : 640;
  return width_ > 0 ? width_ / static_cast<double>(dips) : 1.0;
}

void PhiProfileCard::Paint(HDC dc, const RECT& bounds) {
  const int width = bounds.right - bounds.left;
  const int height = bounds.bottom - bounds.top;
  if (width <= 0 || height <= 0) {
    return;
  }
  SetPixelSize(width, height);
  StartIntroIfReady();
  const double scale = Scale();
  const int radius = static_cast<int>(std::lround(kCornerDip * scale));
  if (!dots_loaded_ && frames_) {
    ::EnterCriticalSection(&frames_->lock);
    if (frames_->dots_done) {
      dots_ = frames_->dots;
      dots_loaded_ = true;
    }
    ::LeaveCriticalSection(&frames_->lock);
  }
  int key = 0;
  const std::vector<uint8_t>* plane = CurrentFrame(&key);
  if (plane) {
    auto found = composed_.find(key);
    if (found == composed_.end()) {
      if (composed_.size() >= kComposedCacheCap) {
        composed_.erase(composed_.begin());
      }
      found = composed_.emplace(key, Compose(*plane, width, height,
                                             dots_loaded_ ? &dots_ : nullptr))
                  .first;
    }
    PhiBlendBgra(dc, bounds, found->second.data(), width, height);
  } else {
    // Nothing decoded yet: the card's white ground.
    PhiFillRoundRectAA(dc, bounds, card_chrome_ ? radius : 0,
                       RGB(255, 255, 255));
  }
  if (!card_chrome_) {
    return;
  }
  DrawLabels(dc, bounds, scale);
  // The 1px border: black 13% light / white 22% dark, over the ground.
  const COLORREF border = dark_ ? RGB(0x5A, 0x5A, 0x5A) : RGB(0xDE, 0xDE, 0xDE);
  PhiStrokeRoundRectAA(dc, bounds, radius, border,
                       std::max(1, static_cast<int>(std::lround(scale))));
}

void PhiProfileCard::BeginDrag(POINT pt) {
  // A drag cuts the intro short and snaps to the centre (the Mac's
  // stopDragHintAnimation).
  if (intro_playing_) {
    intro_playing_ = false;
    x_ = (kCols + 1) / 2;
    y_ = (kRows + 1) / 2;
  }
  returning_ = false;
  dragging_ = true;
  drag_origin_ = pt;
}

bool PhiProfileCard::Drag(POINT pt) {
  if (!dragging_) {
    return false;
  }
  const int step =
      std::max(1, static_cast<int>(std::lround(kDragStepDip * Scale())));
  const int dx = pt.x - drag_origin_.x;
  const int dy = pt.y - drag_origin_.y;
  bool changed = false;
  if (std::abs(dx) > step) {
    // The Mac: a drag to the right walks the column DOWN.
    x_ = std::clamp(x_ + (dx > 0 ? -1 : 1), 1, kCols);
    drag_origin_.x = pt.x;
    changed = true;
  }
  if (std::abs(dy) > step) {
    // AppKit's y points up: its "dy < 0 -> row - 1" is a drag DOWN here.
    y_ = std::clamp(y_ + (dy > 0 ? -1 : 1), 1, kRows);
    drag_origin_.y = pt.y;
    changed = true;
  }
  return changed;
}

void PhiProfileCard::EndDrag() {
  if (!dragging_) {
    return;
  }
  dragging_ = false;
  const int cx = (kCols + 1) / 2;
  const int cy = (kRows + 1) / 2;
  if (x_ == cx && y_ == cy) {
    return;
  }
  returning_ = true;
  return_start_ = Now();
  return_from_x_ = x_;
  return_from_y_ = y_;
}

bool PhiProfileCard::animating() const {
  return returning_ || intro_playing_ || (intro_pending_ && frames_ != nullptr);
}

bool PhiProfileCard::Tick() {
  bool repaint = false;
  StartIntroIfReady();
  if (intro_playing_) {
    const double elapsed = (Now() - intro_start_) / 1000.0;
    const int frame = std::min(
        kIntroFrames - 1,
        static_cast<int>(elapsed / (kIntroSeconds / kIntroFrames)));
    if (frame != intro_frame_) {
      intro_frame_ = frame;
      repaint = true;
    }
    if (elapsed >= kIntroSeconds) {
      intro_playing_ = false;
      repaint = true;
    }
  }
  if (returning_) {
    const int cx = (kCols + 1) / 2;
    const int cy = (kRows + 1) / 2;
    const double elapsed = (Now() - return_start_) / 1000.0;
    double t = std::min(1.0, elapsed / kReturnSeconds);
    const double ease = 1 - std::pow(1 - t, 3);  // Ease out, the Mac's.
    const int nx = static_cast<int>(
        std::lround(return_from_x_ + (cx - return_from_x_) * ease));
    const int ny = static_cast<int>(
        std::lround(return_from_y_ + (cy - return_from_y_) * ease));
    if (nx != x_ || ny != y_) {
      x_ = nx;
      y_ = ny;
      repaint = true;
    }
    if (t >= 1.0) {
      returning_ = false;
      x_ = cx;
      y_ = cy;
    }
  }
  return repaint;
}

void PhiProfileCard::ExportPng(HWND owner) {
  // The snapshot at 2x, on a canvas twice its size in the accent.
  const int card_w = kCardWidthDip * 2;
  const int card_h = kCardHeightDip * 2;
  const int canvas_w = card_w * 2;
  const int canvas_h = card_h * 2;
  const int cx = (kCols + 1) / 2;
  const int cy = (kRows + 1) / 2;
  const int row = intro_playing_ ? cy : y_;
  const int col = intro_playing_ ? cx : x_;
  ComPtr<IWICImagingFactory> factory = WicFactory();
  std::vector<uint8_t> gray;
  std::vector<uint8_t> dots;
  const std::wstring dir = ResourceDir(resource_subdir_);
  if (!factory ||
      !DecodeGray(factory.Get(),
                  dir + L"frame-" + std::to_wstring(row) + L"-" +
                      std::to_wstring(col) + L".jpg",
                  card_w, card_h, &gray)) {
    LOG(WARNING) << "[ProfileCard] export: the frame did not decode";
    return;
  }
  if (DecodeGray(factory.Get(), dir + L"dot-bg.png", card_w, card_h, &dots)) {
    for (uint8_t& v : dots) {
      v = static_cast<uint8_t>(255 - v);
    }
  }
  const std::vector<uint8_t> card = Compose(gray, card_w, card_h,
                                            dots.empty() ? nullptr : &dots);
  Scratch canvas;
  if (!canvas.Create(canvas_w, canvas_h)) {
    return;
  }
  // The accent ground, then the card centred, then its text.
  const base::span<uint8_t> ground = canvas.pixels();
  for (size_t i = 0; i < static_cast<size_t>(canvas_w) * canvas_h; ++i) {
    ground[i * 4] = GetBValue(accent_);
    ground[i * 4 + 1] = GetGValue(accent_);
    ground[i * 4 + 2] = GetRValue(accent_);
    ground[i * 4 + 3] = 255;
  }
  const RECT card_rect = {(canvas_w - card_w) / 2, (canvas_h - card_h) / 2,
                          (canvas_w - card_w) / 2 + card_w,
                          (canvas_h - card_h) / 2 + card_h};
  PhiBlendBgra(canvas.dc, card_rect, card.data(), card_w, card_h);
  DrawLabels(canvas.dc, card_rect, 2.0);
  ::GdiFlush();
  const base::span<uint8_t> done = canvas.pixels();
  std::vector<uint8_t> pixels(done.begin(), done.end());
  for (size_t i = 3; i < pixels.size(); i += 4) {
    pixels[i] = 255;  // GDI text left the alpha behind; the canvas is opaque.
  }

  std::wstring file = base::UTF8ToWide(name_.empty() ? "Phi" : name_);
  constexpr std::wstring_view kReserved = L"\\/:*?\"<>|";
  for (wchar_t& c : file) {
    if (kReserved.find(c) != std::wstring_view::npos) {
      c = L'_';
    }
  }
  file += L".png";
  std::wstring path(MAX_PATH, L'\0');
  std::ranges::copy(file.substr(0, MAX_PATH - 1), path.begin());
  OPENFILENAMEW ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = L"PNG image (*.png)\0*.png\0\0";
  ofn.lpstrFile = path.data();
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrDefExt = L"png";
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!::GetSaveFileNameW(&ofn)) {
    return;
  }
  if (!WritePng(path.c_str(), pixels, canvas_w, canvas_h)) {
    LOG(ERROR) << "[ProfileCard] export: PNG write failed";
  }
}

}  // namespace phi_client
