// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_feedback_window.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <utility>

#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_feedback_image.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_icons.h"

#pragma comment(lib, "comctl32.lib")

namespace phi_client {

namespace {

constexpr wchar_t kClassName[] = L"PhiFeedbackWindow";
// The Mac window: 520x580 points.
constexpr int kWidth = 520;
constexpr int kHeight = 580;
constexpr int kMargin = 24;
constexpr int kMaxDescriptionLength = 4096;
constexpr int kCounterThreshold = 3000;
// How long Send waits for Chromium's system logs before filing the report
// without them.
constexpr UINT_PTR kLogsTimer = 1;
constexpr UINT kLogsTimeoutMs = 15 * 1000;

enum : UINT_PTR {
  kIdHeadlineDescription = 101,
  kIdDescription,
  kIdCounter,
  kIdHeadlineInfo,
  kIdUrlLabel,
  kIdUrl,
  kIdAttachLabel,
  kIdAttachHint,
  kIdChoose,
  kIdList,
  kIdPreview,
  kIdRemove,
  kIdNotice,
  kIdPrivacy,
  kIdTerms,
  kIdSend,
};

constexpr char kPrivacyUrl[] = "https://phibrowser.com/privacy/";
constexpr char kTermsUrl[] = "https://phibrowser.com/terms-of-service/";

PhiFeedbackWindow* g_window = nullptr;
int g_next_request = 0;

std::wstring Widen(const std::string& utf8) {
  return base::UTF8ToWide(utf8);
}
std::string Narrow(const std::wstring& wide) {
  return base::WideToUTF8(wide);
}

std::string Trim(const std::string& in) {
  const size_t begin = in.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return std::string();
  }
  const size_t end = in.find_last_not_of(" \t\r\n");
  return in.substr(begin, end - begin + 1);
}

std::wstring WindowText(HWND hwnd) {
  const int length = ::GetWindowTextLengthW(hwnd);
  std::wstring text(static_cast<size_t>(length) + 1, L'\0');
  const int copied = ::GetWindowTextW(hwnd, text.data(), length + 1);
  text.resize(static_cast<size_t>(std::max(copied, 0)));
  return text;
}

std::wstring Leaf(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// The Mac's phiBrandEnsuredUrlString: chrome:// shows as phi://.
std::string BrandedUrl(const std::string& url) {
  if (url.rfind("chrome://", 0) == 0) {
    return "phi://" + url.substr(9);
  }
  return url;
}

std::wstring FormatSize(int64_t bytes) {
  if (bytes >= 1024 * 1024) {
    const int64_t tenths = bytes * 10 / (1024 * 1024);
    return std::to_wstring(tenths / 10) + L"." +
           std::to_wstring(tenths % 10) + L" MB";
  }
  if (bytes >= 1024) {
    return std::to_wstring((bytes + 512) / 1024) + L" KB";
  }
  return std::to_wstring(bytes) + L" B";
}

// A pasted image is written out for the previewer — the Mac's temporary
// preview file.
std::wstring WritePreviewFile(const std::wstring& filename,
                              const std::string& bytes) {
  std::wstring temp(MAX_PATH, L'\0');
  const DWORD length =
      ::GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
  if (!length || length >= temp.size()) {
    return std::wstring();
  }
  temp.resize(length);
  const std::wstring dir = temp + L"PhiFeedbackPreviews";
  ::CreateDirectoryW(dir.c_str(), nullptr);
  const std::wstring path =
      dir + L"\\" +
      Widen(base::Uuid::GenerateRandomV4().AsLowercaseString()) + L"-" +
      filename;
  HANDLE f = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return std::wstring();
  }
  DWORD written = 0;
  ::WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
              nullptr);
  ::CloseHandle(f);
  return path;
}

}  // namespace

// static
void PhiFeedbackWindow::Open(PhiFeedbackHost* host,
                             const PhiFeedbackContext& context,
                             HWND anchor) {
  if (g_window && g_window->hwnd_) {
    // App-global, like the Mac's: rebind to the window reopening it so the
    // report's context follows the current window.
    g_window->Rebind(context);
    ::ShowWindow(g_window->hwnd_, SW_SHOWNORMAL);
    ::SetForegroundWindow(g_window->hwnd_);
    ::SetFocus(g_window->description_);
    return;
  }
  auto* window = new PhiFeedbackWindow(host, context);
  if (!window->hwnd_) {
    delete window;
    return;
  }
  g_window = window;
  // Centered on the invoking shell, as the Mac centers its panel.
  RECT frame = {};
  ::GetWindowRect(window->hwnd_, &frame);
  RECT around = {};
  if (!anchor || !::GetWindowRect(anchor, &around)) {
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &around, 0);
  }
  const int x = around.left + ((around.right - around.left) -
                               (frame.right - frame.left)) / 2;
  const int y = around.top + ((around.bottom - around.top) -
                              (frame.bottom - frame.top)) / 3;
  ::SetWindowPos(window->hwnd_, nullptr, std::max(x, 0), std::max(y, 0), 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER);
  ::ShowWindow(window->hwnd_, SW_SHOW);
  ::SetForegroundWindow(window->hwnd_);
  ::SetFocus(window->description_);
}

// static
void PhiFeedbackWindow::OnSystemLogs(int request_id, const char* text) {
  if (!g_window || !g_window->pending_request_ ||
      g_window->pending_request_ != request_id) {
    return;
  }
  ::KillTimer(g_window->hwnd_, kLogsTimer);
  if (!text) {
    LOG(WARNING) << "[Feedback] Chromium system logs skipped because the "
                 << "framework returned no text";
  }
  g_window->CompleteSend(text);
}

// static
HWND PhiFeedbackWindow::Handle() {
  return g_window ? g_window->hwnd_ : nullptr;
}

// static
std::string PhiFeedbackWindow::StateJson() {
  base::DictValue out;
  out.Set("open", g_window != nullptr);
  if (g_window) {
    out.Set("windowId", static_cast<double>(g_window->context_.window_id));
    out.Set("url", g_window->UrlText());
    out.Set("title", g_window->context_.title);
    out.Set("description", g_window->DescriptionText());
    out.Set("submitting", g_window->submitting_);
    out.Set("canSend", ::IsWindowEnabled(g_window->send_) != 0);
    base::ListValue attachments;
    for (const Attachment& a : g_window->attachments_) {
      base::DictValue row;
      row.Set("filename", Narrow(a.draft.filename));
      row.Set("size", static_cast<double>(a.draft.size));
      row.Set("image", a.draft.is_image);
      row.Set("pasted", a.draft.file_path.empty());
      attachments.Append(std::move(row));
    }
    out.Set("attachments", std::move(attachments));
  }
  std::optional<std::string> json = base::WriteJson(out);
  return json ? *json : std::string("{}");
}

// static
std::string PhiFeedbackWindow::Drive(const std::string& description,
                              bool set_description,
                              const std::string& url,
                              bool set_url,
                              const std::vector<std::wstring>& attach_files,
                              bool send) {
  if (!g_window) {
    return "not_open";
  }
  if (set_description) {
    ::SetWindowTextW(g_window->description_, Widen(description).c_str());
    g_window->RefreshCounter();
    g_window->RefreshSendState();
  }
  if (set_url) {
    ::SetWindowTextW(g_window->url_, Widen(url).c_str());
  }
  if (!attach_files.empty()) {
    g_window->AddFiles(attach_files);
  }
  if (send) {
    if (Trim(g_window->DescriptionText()).empty()) {
      return "empty_description";
    }
    std::string email;
    if (!g_window->host_->FeedbackAccount(&email)) {
      return "no_account";
    }
    if (!g_window->Send(/*interactive=*/false)) {
      return "not_sent";
    }
  }
  return std::string();
}

PhiFeedbackWindow::PhiFeedbackWindow(PhiFeedbackHost* host,
                                     const PhiFeedbackContext& context)
    : host_(host), context_(context) {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &PhiFeedbackWindow::WndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    return ::RegisterClassExW(&wc);
  }();
  (void)atom;
  const DWORD style =
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  hwnd_ = ::CreateWindowExW(0, kClassName, PhiStr("app.feedback.windowTitle"),
                            style, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                            nullptr, nullptr, ::GetModuleHandleW(nullptr),
                            this);
  if (!hwnd_) {
    LOG(ERROR) << "[Feedback] CreateWindowEx failed err=" << ::GetLastError();
    return;
  }
  if (const UINT dpi = ::GetDpiForWindow(hwnd_)) {
    dpi_ = dpi;
  }
  RECT client = {0, 0, Scaled(kWidth), Scaled(kHeight)};
  ::AdjustWindowRectExForDpi(&client, style, FALSE, 0, dpi_);
  ::SetWindowPos(hwnd_, nullptr, 0, 0, client.right - client.left,
                 client.bottom - client.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  if (HICON big = PhiAppIcon(32)) {
    ::SendMessageW(hwnd_, WM_SETICON, ICON_BIG,
                   reinterpret_cast<LPARAM>(big));
  }
  if (HICON small_icon = PhiAppIcon(16)) {
    ::SendMessageW(hwnd_, WM_SETICON, ICON_SMALL,
                   reinterpret_cast<LPARAM>(small_icon));
  }
  Build();
  Rebind(context);
}

PhiFeedbackWindow::~PhiFeedbackWindow() {
  for (HFONT font : {font_, headline_font_, small_font_}) {
    if (font) {
      ::DeleteObject(font);
    }
  }
  for (const Attachment& a : attachments_) {
    if (!a.preview_path.empty()) {
      ::DeleteFileW(a.preview_path.c_str());
    }
  }
}

int PhiFeedbackWindow::Scaled(int design) const {
  return ::MulDiv(design, static_cast<int>(dpi_), 96);
}

HWND PhiFeedbackWindow::Child(const wchar_t* cls,
                              const wchar_t* text,
                              DWORD style,
                              UINT_PTR id,
                              DWORD ex_style) {
  HWND control = ::CreateWindowExW(
      ex_style, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
      hwnd_, reinterpret_cast<HMENU>(id), ::GetModuleHandleW(nullptr),
      nullptr);
  if (control) {
    // Keyboard: Tab cycles, Escape closes, Ctrl+Enter sends, Ctrl+V in the
    // description pastes an image — the dialog manners a modeless window
    // does not get for free.
    ::SetWindowSubclass(control, &PhiFeedbackWindow::ControlProc, id,
                        reinterpret_cast<DWORD_PTR>(this));
  }
  return control;
}

void PhiFeedbackWindow::Build() {
  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = sizeof(metrics);
  if (::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                   &metrics, 0, dpi_)) {
    font_ = ::CreateFontIndirectW(&metrics.lfMessageFont);
    LOGFONTW headline = metrics.lfMessageFont;
    headline.lfWeight = FW_SEMIBOLD;
    headline.lfHeight = headline.lfHeight * 8 / 7;
    headline_font_ = ::CreateFontIndirectW(&headline);
    LOGFONTW small_font = metrics.lfMessageFont;  // (`small` is a macro.)
    small_font.lfHeight = small_font.lfHeight * 11 / 12;
    small_font_ = ::CreateFontIndirectW(&small_font);
  }

  Child(L"STATIC", PhiStr("feedback.form.descriptionSection.title"), 0,
        kIdHeadlineDescription);
  description_ = Child(L"EDIT", L"",
                       WS_BORDER | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE |
                           ES_AUTOVSCROLL | ES_WANTRETURN,
                       kIdDescription);
  ::SendMessageW(description_, EM_SETLIMITTEXT, kMaxDescriptionLength, 0);
  counter_ = Child(L"STATIC", L"", SS_RIGHT, kIdCounter);
  Child(L"STATIC", PhiStr("feedback.form.additionalInfoSection.title"), 0,
        kIdHeadlineInfo);
  Child(L"STATIC", PhiStr("feedback.form.urlField.label"), 0, kIdUrlLabel);
  url_ = Child(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, kIdUrl);
  Child(L"STATIC", PhiStr("feedback.form.attachmentsSection.title"), 0,
        kIdAttachLabel);
  Child(L"STATIC", PhiStr("feedback.form.attachmentsSection.pasteImageHint"),
        0, kIdAttachHint);
  choose_ = Child(L"BUTTON",
                  PhiStr("feedback.form.attachmentsSection.chooseFilesButton"),
                  WS_TABSTOP | BS_PUSHBUTTON, kIdChoose);
  list_ = Child(L"LISTBOX", L"",
                WS_BORDER | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
                    LBS_NOINTEGRALHEIGHT,
                kIdList);
  preview_ = Child(L"BUTTON", PhiStr("feedback.form.attachment.previewTooltip"),
                   WS_TABSTOP | BS_PUSHBUTTON, kIdPreview);
  remove_ = Child(L"BUTTON", PhiStr("feedback.form.attachment.removeTooltip"),
                  WS_TABSTOP | BS_PUSHBUTTON, kIdRemove);
  Child(L"STATIC", PhiStr("feedback.form.privacyNotice"), 0, kIdNotice);
  Child(L"BUTTON", PhiStr("feedback.form.privacyPolicyLink"),
        WS_TABSTOP | BS_PUSHBUTTON, kIdPrivacy);
  Child(L"BUTTON", PhiStr("feedback.form.termsOfServiceLink"),
        WS_TABSTOP | BS_PUSHBUTTON, kIdTerms);
  send_ = Child(L"BUTTON", PhiStr("feedback.form.sendButton"),
                WS_TABSTOP | BS_DEFPUSHBUTTON, kIdSend);

  for (HWND child = ::GetWindow(hwnd_, GW_CHILD); child;
       child = ::GetWindow(child, GW_HWNDNEXT)) {
    const UINT_PTR id = static_cast<UINT_PTR>(::GetDlgCtrlID(child));
    HFONT font = font_;
    if (id == kIdHeadlineDescription || id == kIdHeadlineInfo) {
      font = headline_font_;
    } else if (id == kIdAttachHint || id == kIdNotice || id == kIdCounter ||
               id == kIdPreview || id == kIdRemove || id == kIdPrivacy ||
               id == kIdTerms) {
      font = small_font_;
    }
    if (font) {
      ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
  }
  Layout();
  RefreshAttachmentList();
  RefreshCounter();
  RefreshSendState();
}

void PhiFeedbackWindow::Layout() {
  const auto place = [&](UINT_PTR id, int x, int y, int w, int h) {
    if (HWND control = ::GetDlgItem(hwnd_, static_cast<int>(id))) {
      ::SetWindowPos(control, nullptr, Scaled(x), Scaled(y), Scaled(w),
                     Scaled(h), SWP_NOZORDER | SWP_NOACTIVATE);
    }
  };
  const int inner = kWidth - 2 * kMargin;  // 472
  place(kIdHeadlineDescription, kMargin, 20, inner, 20);
  place(kIdDescription, kMargin, 46, inner, 144);
  place(kIdCounter, kMargin, 194, inner, 16);
  place(kIdHeadlineInfo, kMargin, 218, inner, 20);
  place(kIdUrlLabel, kMargin, 250, 60, 18);
  place(kIdUrl, kMargin + 64, 246, inner - 64, 24);
  place(kIdAttachLabel, kMargin, 282, 300, 18);
  place(kIdAttachHint, kMargin, 300, 300, 16);
  place(kIdChoose, kMargin + inner - 120, 282, 120, 26);
  place(kIdList, kMargin, 328, inner, 84);
  place(kIdPreview, kMargin + inner - 208, 418, 100, 24);
  place(kIdRemove, kMargin + inner - 100, 418, 100, 24);
  place(kIdNotice, kMargin, 452, inner, 56);
  place(kIdPrivacy, kMargin, 510, 110, 22);
  place(kIdTerms, kMargin + 116, 510, 120, 22);
  place(kIdSend, kMargin + inner - 96, 536, 96, 30);
}

void PhiFeedbackWindow::Rebind(const PhiFeedbackContext& context) {
  // An in-flight Send already captured its window and is awaiting the logs
  // (the Mac's rebindHost guard).
  if (submitting_) {
    return;
  }
  context_ = context;
  ::SetWindowTextW(url_, Widen(BrandedUrl(context.url)).c_str());
}

void PhiFeedbackWindow::RefreshAttachmentList() {
  const LRESULT selected = ::SendMessageW(list_, LB_GETCURSEL, 0, 0);
  ::SendMessageW(list_, LB_RESETCONTENT, 0, 0);
  for (const Attachment& a : attachments_) {
    std::wstring row = a.draft.filename + L"   (" +
                       FormatSize(a.draft.size) + L")";
    ::SendMessageW(list_, LB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(row.c_str()));
  }
  if (!attachments_.empty()) {
    const LRESULT keep =
        selected == LB_ERR
            ? static_cast<LRESULT>(attachments_.size()) - 1
            : std::min(selected,
                       static_cast<LRESULT>(attachments_.size()) - 1);
    ::SendMessageW(list_, LB_SETCURSEL, static_cast<WPARAM>(keep), 0);
  }
  const bool any = ::SendMessageW(list_, LB_GETCURSEL, 0, 0) != LB_ERR;
  ::EnableWindow(preview_, any);
  ::EnableWindow(remove_, any);
}

void PhiFeedbackWindow::RefreshSendState() {
  const bool can_send = !submitting_ && !Trim(DescriptionText()).empty();
  ::EnableWindow(send_, can_send);
  ::SetWindowTextW(send_, submitting_ ? PhiStr("feedback.form.sending")
                                      : PhiStr("feedback.form.sendButton"));
  ::EnableWindow(choose_, !submitting_);
}

void PhiFeedbackWindow::RefreshCounter() {
  const int length = ::GetWindowTextLengthW(description_);
  if (length >= kCounterThreshold) {
    const std::wstring text = std::to_wstring(length) + L"/" +
                              std::to_wstring(kMaxDescriptionLength);
    ::SetWindowTextW(counter_, text.c_str());
  } else {
    ::SetWindowTextW(counter_, L"");
  }
  ::InvalidateRect(counter_, nullptr, TRUE);
}

std::string PhiFeedbackWindow::DescriptionText() const {
  return Narrow(WindowText(description_));
}

std::string PhiFeedbackWindow::UrlText() const {
  return Trim(Narrow(WindowText(url_)));
}

bool PhiFeedbackWindow::AddFiles(const std::vector<std::wstring>& paths) {
  std::vector<std::string> errors;
  for (const std::wstring& path : paths) {
    int64_t size = 0;
    std::string mime_type;
    bool is_image = false;
    std::string error;
    if (!PhiFeedbackOutbox::SelectedAttachmentInfo(path, &size, &mime_type,
                                                   &is_image, &error)) {
      errors.push_back(Narrow(Leaf(path)) + ": " + error);
      continue;
    }
    Attachment attachment;
    attachment.draft.filename = Leaf(path);
    attachment.draft.size = size;
    attachment.draft.is_image = is_image;
    attachment.draft.file_path = path;
    attachments_.push_back(std::move(attachment));
  }
  RefreshAttachmentList();
  if (!errors.empty()) {
    std::string shown;
    for (size_t i = 0; i < errors.size() && i < 3; ++i) {
      shown += (i ? "\n" : "") + errors[i];
    }
    if (errors.size() > 3) {
      shown += "\n+" + std::to_string(errors.size() - 3) + " more";
    }
    ::MessageBoxW(hwnd_, Widen(shown).c_str(),
                  PhiStr("feedback.form.attachmentFailure.title"),
                  MB_OK | MB_ICONWARNING);
    return false;
  }
  return true;
}

void PhiFeedbackWindow::ChooseFiles() {
  using Microsoft::WRL::ComPtr;
  ComPtr<IFileOpenDialog> dialog;
  if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
    return;
  }
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM |
                     FOS_FILEMUSTEXIST);
  dialog->SetTitle(
      PhiStr("feedback.form.attachmentsSection.chooseFilesButton"));
  if (FAILED(dialog->Show(hwnd_))) {
    return;  // Cancelled.
  }
  ComPtr<IShellItemArray> items;
  DWORD count = 0;
  if (FAILED(dialog->GetResults(&items)) || FAILED(items->GetCount(&count))) {
    return;
  }
  std::vector<std::wstring> paths;
  for (DWORD i = 0; i < count; ++i) {
    ComPtr<IShellItem> item;
    wchar_t* path = nullptr;
    if (SUCCEEDED(items->GetItemAt(i, &item)) &&
        SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
      paths.emplace_back(path);
      ::CoTaskMemFree(path);
    }
  }
  AddFiles(paths);
}

void PhiFeedbackWindow::PasteImage() {
  std::string png;
  if (!PhiClipboardImageToPng(&png)) {
    LOG(ERROR) << "[Feedback] paste image failed: could not encode image";
    return;
  }
  int images = 0;
  for (const Attachment& a : attachments_) {
    images += a.draft.is_image ? 1 : 0;
  }
  Attachment attachment;
  attachment.draft.filename =
      L"image-" + std::to_wstring(images + 1) + L".png";
  attachment.draft.size = static_cast<int64_t>(png.size());
  attachment.draft.is_image = true;
  attachment.draft.pasted_png = std::move(png);
  attachments_.push_back(std::move(attachment));
  RefreshAttachmentList();
}

void PhiFeedbackWindow::RemoveSelected() {
  const LRESULT selected = ::SendMessageW(list_, LB_GETCURSEL, 0, 0);
  if (selected == LB_ERR ||
      static_cast<size_t>(selected) >= attachments_.size()) {
    return;
  }
  Attachment& a = attachments_[static_cast<size_t>(selected)];
  if (!a.preview_path.empty()) {
    ::DeleteFileW(a.preview_path.c_str());
  }
  attachments_.erase(attachments_.begin() + selected);
  RefreshAttachmentList();
}

void PhiFeedbackWindow::PreviewSelected() {
  const LRESULT selected = ::SendMessageW(list_, LB_GETCURSEL, 0, 0);
  if (selected == LB_ERR ||
      static_cast<size_t>(selected) >= attachments_.size()) {
    return;
  }
  Attachment& a = attachments_[static_cast<size_t>(selected)];
  if (a.draft.is_image) {
    // The Mac opens images in Preview; here the image viewer registered
    // for the type.
    std::wstring path = a.draft.file_path;
    if (path.empty()) {
      if (a.preview_path.empty()) {
        a.preview_path = WritePreviewFile(a.draft.filename, a.draft.pasted_png);
      }
      path = a.preview_path;
    }
    if (!path.empty()) {
      ::ShellExecuteW(hwnd_, L"open", path.c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
    }
    return;
  }
  // Other files: reveal in Explorer (the Mac's "Show in Finder").
  const std::wstring args = L"/select,\"" + a.draft.file_path + L"\"";
  ::ShellExecuteW(hwnd_, L"open", L"explorer.exe", args.c_str(), nullptr,
                  SW_SHOWNORMAL);
}

bool PhiFeedbackWindow::Send(bool interactive) {
  if (submitting_ || Trim(DescriptionText()).empty()) {
    return false;
  }
  std::string email;
  if (!host_->FeedbackAccount(&email)) {
    if (!interactive) {
      return false;
    }
    // The Mac fails the enqueue with "No account is available"; here the
    // sign-in is one click away, so offer it.
    if (::MessageBoxW(hwnd_, PhiStr("feedback.form.signInPrompt"),
                      PhiStr("app.feedback.windowTitle"),
                      MB_YESNO | MB_ICONQUESTION) == IDYES) {
      host_->RequestFeedbackSignIn();
    }
    return false;
  }
  submitting_ = true;
  RefreshSendState();
  pending_request_ = ++g_next_request;
  if (!host_->FetchFeedbackSystemLogs(context_.window_id, pending_request_)) {
    LOG(WARNING) << "[Feedback] Chromium system logs skipped because the "
                 << "framework will not answer";
    CompleteSend(nullptr);
    return true;
  }
  ::SetTimer(hwnd_, kLogsTimer, kLogsTimeoutMs, nullptr);
  return true;
}

void PhiFeedbackWindow::CompleteSend(const char* logs) {
  pending_request_ = 0;
  PhiFeedbackDraft draft;
  draft.description = Trim(DescriptionText());
  draft.page_url = UrlText();
  draft.page_title = context_.title;
  draft.extension_versions = host_->PhiExtensionVersions(context_.window_id);
  if (logs) {
    draft.chromium_system_logs = std::string(logs);
  }
  for (const Attachment& a : attachments_) {
    draft.attachments.push_back(a.draft);
  }
  std::string error;
  if (PhiFeedbackOutbox::Get().Enqueue(draft, &error)) {
    ::DestroyWindow(hwnd_);
    return;
  }
  LOG(ERROR) << "[Feedback] enqueue failed: " << error;
  submitting_ = false;
  RefreshSendState();
  ::MessageBoxW(hwnd_, Widen(error).c_str(),
                PhiStr("feedback.form.localSaveFailure.title"),
                MB_OK | MB_ICONERROR);
}

// static
LRESULT CALLBACK PhiFeedbackWindow::ControlProc(HWND hwnd,
                                                UINT msg,
                                                WPARAM wparam,
                                                LPARAM lparam,
                                                UINT_PTR id,
                                                DWORD_PTR data) {
  auto* self = reinterpret_cast<PhiFeedbackWindow*>(data);
  if (msg == WM_NCDESTROY) {
    ::RemoveWindowSubclass(hwnd, &PhiFeedbackWindow::ControlProc, id);
    return ::DefSubclassProc(hwnd, msg, wparam, lparam);
  }
  if (!self) {
    return ::DefSubclassProc(hwnd, msg, wparam, lparam);
  }
  const bool ctrl = ::GetKeyState(VK_CONTROL) < 0;
  const bool shift = ::GetKeyState(VK_SHIFT) < 0;
  if (msg == WM_KEYDOWN) {
    switch (wparam) {
      case VK_TAB:
        if (HWND next = ::GetNextDlgTabItem(self->hwnd_, hwnd, shift)) {
          ::SetFocus(next);
        }
        return 0;
      case VK_ESCAPE:
        ::PostMessageW(self->hwnd_, WM_CLOSE, 0, 0);
        return 0;
      case VK_RETURN: {
        wchar_t cls[16] = {};
        ::GetClassNameW(hwnd, cls, 16);
        const bool is_button = ::_wcsicmp(cls, L"Button") == 0;
        if (is_button) {
          ::SendMessageW(hwnd, BM_CLICK, 0, 0);
          return 0;
        }
        if (hwnd != self->description_ || ctrl) {
          // Enter anywhere but the description sends; there, Ctrl+Enter.
          if (::IsWindowEnabled(self->send_)) {
            self->Send(/*interactive=*/true);
          }
          return 0;
        }
        break;
      }
      case VK_DELETE:
        if (hwnd == self->list_) {
          self->RemoveSelected();
          return 0;
        }
        break;
      case 'V':
        if (ctrl && hwnd == self->description_ &&
            (::IsClipboardFormatAvailable(CF_BITMAP) ||
             ::IsClipboardFormatAvailable(CF_DIB))) {
          self->PasteImage();
          return 0;
        }
        break;
      default:
        break;
    }
  }
  if (msg == WM_CHAR && (wparam == VK_TAB || wparam == VK_ESCAPE)) {
    return 0;  // Swallow the beep the edit would emit.
  }
  if (msg == WM_CHAR && wparam == VK_RETURN && hwnd != self->description_) {
    return 0;
  }
  if (msg == WM_CHAR && wparam == 0x16 && hwnd == self->description_ &&
      (::IsClipboardFormatAvailable(CF_BITMAP) ||
       ::IsClipboardFormatAvailable(CF_DIB))) {
    return 0;  // The Ctrl+V character after an image paste.
  }
  return ::DefSubclassProc(hwnd, msg, wparam, lparam);
}

// static
LRESULT CALLBACK PhiFeedbackWindow::WndProc(HWND hwnd,
                                            UINT msg,
                                            WPARAM wparam,
                                            LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  auto* self = reinterpret_cast<PhiFeedbackWindow*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!self) {
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  switch (msg) {
    case WM_COMMAND: {
      const UINT_PTR id = LOWORD(wparam);
      const UINT code = HIWORD(wparam);
      if (id == kIdDescription && code == EN_CHANGE) {
        self->RefreshCounter();
        self->RefreshSendState();
        return 0;
      }
      if (id == kIdList && code == LBN_SELCHANGE) {
        const bool any = ::SendMessageW(self->list_, LB_GETCURSEL, 0, 0) !=
                         LB_ERR;
        ::EnableWindow(self->preview_, any);
        ::EnableWindow(self->remove_, any);
        return 0;
      }
      if (id == kIdList && code == LBN_DBLCLK) {
        self->PreviewSelected();
        return 0;
      }
      if (code != BN_CLICKED) {
        break;
      }
      switch (id) {
        case kIdChoose:
          self->ChooseFiles();
          return 0;
        case kIdPreview:
          self->PreviewSelected();
          return 0;
        case kIdRemove:
          self->RemoveSelected();
          return 0;
        case kIdPrivacy:
          self->host_->OpenFeedbackLink(self->context_.window_id, kPrivacyUrl);
          return 0;
        case kIdTerms:
          self->host_->OpenFeedbackLink(self->context_.window_id, kTermsUrl);
          return 0;
        case kIdSend:
          self->Send(/*interactive=*/true);
          return 0;
        default:
          break;
      }
      break;
    }
    case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(wparam);
      HWND control = reinterpret_cast<HWND>(lparam);
      const int id = ::GetDlgCtrlID(control);
      ::SetBkMode(dc, TRANSPARENT);
      if (id == kIdCounter) {
        const int length = ::GetWindowTextLengthW(self->description_);
        ::SetTextColor(dc, length >= kMaxDescriptionLength
                               ? RGB(0xC0, 0x1E, 0x1E)
                               : RGB(0xB0, 0x80, 0x00));
      } else if (id == kIdAttachHint || id == kIdNotice) {
        ::SetTextColor(dc, ::GetSysColor(COLOR_GRAYTEXT));
      }
      return reinterpret_cast<LRESULT>(::GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_TIMER:
      if (wparam == kLogsTimer) {
        ::KillTimer(hwnd, kLogsTimer);
        if (self->pending_request_) {
          LOG(WARNING) << "[Feedback] Chromium system logs skipped: the "
                       << "framework did not answer in time";
          self->CompleteSend(nullptr);
        }
        return 0;
      }
      break;
    case WM_DPICHANGED: {
      self->dpi_ = HIWORD(wparam);
      const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
      ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      self->Layout();
      return 0;
    }
    case WM_CLOSE:
      ::DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      ::KillTimer(hwnd, kLogsTimer);
      if (g_window == self) {
        g_window = nullptr;
      }
      self->hwnd_ = nullptr;
      delete self;
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      return 0;
    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace phi_client
