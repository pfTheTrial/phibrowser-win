// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_PROFILE_CARD_H_
#define PHIBROWSER_WIN_CLIENT_PHI_PROFILE_CARD_H_

#include <windows.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace phi_client {

// The Mac's profile card (Sources/UserInterface/Preferences/Account/
// ProfileCardViewController.swift + ConchFrameAnimationBaseViewController):
// a 240x380 card showing the account's conch — a 10x10 grid of renders the
// user DRAGS through (20px per step, springing back to the centre frame on
// release), after a 24-frame intro played once — colourised to the theme:
// every frame is a grey conch on black, mapped black -> white and anything
// brighter -> the theme colour (HSB hue, .85, .75; Pure = grey). Over it a
// 10% tint of that colour and the dot pattern at 15%; the sign-up date in
// Impact at the top, fading out toward its baseline; the name at the
// bottom in the accent (hue at full saturation, brightness .37; Pure =
// black). "Download image" exports the card at 2x on an accent canvas.
//
// The frames ship as resources/profile-card/frame-<row>-<col>.jpg and
// pre-<n>.jpg (the Mac's setting_<r>-<c> / setting-pre-<n> assets, 600x949)
// with dot-bg.png; they are decoded on a worker at the card's pixel size,
// centre frame first, then the intro, then by distance from the centre —
// the Mac's preload order. UI thread except where noted.
class PhiProfileCard {
 public:
  // `resource_subdir` names the frame set under resources/ ("profile-card"
  // — the Settings card — or "onboarding", the Welcome page's wider conch);
  // `card_chrome` draws the card as a card: rounded corners, the border,
  // the date and the name. Off, it is the bare colourised conch with its
  // tint and dots, filling `bounds` (the Mac's OnboardingWelcome ground).
  explicit PhiProfileCard(const wchar_t* resource_subdir = L"profile-card",
                          bool card_chrome = true);
  ~PhiProfileCard();

  PhiProfileCard(const PhiProfileCard&) = delete;
  PhiProfileCard& operator=(const PhiProfileCard&) = delete;

  // `msg` is posted to `hwnd` whenever a frame worth repainting for has
  // landed (the centre, the intro's last frame, the whole grid).
  void SetNotify(HWND hwnd, UINT msg);
  // The card's size in PIXELS (240x380 dips scaled). A new size restarts
  // the decode; the same size is a no-op.
  void SetPixelSize(int width, int height);
  // The account: the profile's name (truncated to 20 characters like the
  // Mac) and its created_at (ISO 8601, "2025-11-05T11:32:18Z").
  void SetIdentity(const std::string& name, const std::string& created_at);
  // The theme: the overlay colour whose hue drives everything, whether the
  // theme is Pure (greys), and the appearance (the card border).
  void SetTheme(COLORREF overlay, bool pure, bool dark);

  // Paints the whole card into `bounds` (its pixel size), rounded and
  // bordered. Before the centre frame has decoded, the card is its ground.
  void Paint(HDC dc, const RECT& bounds);

  // The drag through the grid, in client pixels. Drag() and Tick() return
  // true when the card wants a repaint; animating() says whether the owner
  // should keep a ~16ms timer running for Tick().
  void BeginDrag(POINT pt);
  bool Drag(POINT pt);
  void EndDrag();
  bool Tick();
  bool animating() const;
  bool dragging() const { return dragging_; }

  // The Mac's snapshotAndExport: the card rendered at 2x, centred on a
  // canvas twice its size in the accent colour, saved as PNG through the
  // system save dialog (default name "<name>.png"). Blocks on the dialog.
  void ExportPng(HWND owner);

 private:
  struct Job;
  struct Frames;

  static constexpr int kRows = 10;
  static constexpr int kCols = 10;
  static constexpr int kIntroFrames = 24;

  static DWORD WINAPI DecodeThread(void* param);
  // The grey plane of grid frame (row, col) or intro frame `n` at the
  // card's size, or null when not decoded yet. UI thread.
  const std::vector<uint8_t>* GridFrame(int row, int col);
  const std::vector<uint8_t>* IntroFrame(int index);
  // The frame the card shows right now: the intro while it plays, else the
  // grid position, else the centre.
  const std::vector<uint8_t>* CurrentFrame(int* key);
  // Composes frame + tint + dots into premultiplied BGRA of `width` x
  // `height` (the grey plane must be that size).
  std::vector<uint8_t> Compose(const std::vector<uint8_t>& gray,
                               int width,
                               int height,
                               const std::vector<uint8_t>* dots) const;
  // Draws the date and the name over a composed card on `dc` at `bounds`,
  // for a card `scale` pixels per dip.
  void DrawLabels(HDC dc, const RECT& bounds, double scale) const;
  // Pixels per dip for the current width (240 dips of card, or the
  // Welcome page's 640).
  double Scale() const;
  void ResetColorCache();
  void StartIntroIfReady();
  static uint64_t Now();

  const std::wstring resource_subdir_;
  const bool card_chrome_;
  HWND notify_hwnd_ = nullptr;
  UINT notify_msg_ = 0;
  int width_ = 0;
  int height_ = 0;
  std::string name_;
  std::string date_;  // "yyyy.M.d", local time.
  // Theme-derived colours.
  double hue_ = 0.0;
  bool pure_ = false;
  bool dark_ = false;
  COLORREF conch_ = RGB(0, 0, 0);
  COLORREF accent_ = RGB(0, 0, 0);

  // The decoded planes, owned by the worker while it runs and read here
  // under the lock. `generation_` retires a decode when the size changes.
  std::shared_ptr<Frames> frames_;
  uint32_t generation_ = 0;
  // Composed cards per frame key, for the current size and theme.
  std::map<int, std::vector<uint8_t>> composed_;
  std::vector<uint8_t> dots_;  // The dot pattern's coverage at card size.
  bool dots_loaded_ = false;

  // The grid position (1-based, the Mac's), the drag and the spring-back.
  int x_ = (kCols + 1) / 2;
  int y_ = (kRows + 1) / 2;
  bool dragging_ = false;
  POINT drag_origin_ = {};
  bool returning_ = false;
  uint64_t return_start_ = 0;
  int return_from_x_ = 0;
  int return_from_y_ = 0;
  // The intro: plays once, 24 frames over one second, when its frames are
  // all in; a drag cuts it short.
  bool intro_pending_ = true;
  bool intro_playing_ = false;
  uint64_t intro_start_ = 0;
  int intro_frame_ = 0;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_PROFILE_CARD_H_
