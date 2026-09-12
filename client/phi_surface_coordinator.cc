// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_surface_coordinator.h"

#include <optional>
#include <utility>
#include <vector>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/escape.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_shell_window.h"
#include "phibrowser-win/client/phi_window_model.h"

namespace phi_client {

PhiSurfaceCoordinator::PhiSurfaceCoordinator(const PhiWindowModel& model,
                                             Host& host)
    : window_model_(model), host_(host) {}

PhiSurfaceCoordinator::~PhiSurfaceCoordinator() = default;

void PhiSurfaceCoordinator::Init(phi_framework::PhiFramework* framework) {
  framework_ = framework;
}

void PhiSurfaceCoordinator::CaptureAnalytics(const char* module,
                                             const char* event_name,
                                             base::DictValue properties) {
  host_.CaptureAnalytics(module, event_name, std::move(properties));
}

void PhiSurfaceCoordinator::RespondToExtension(
    const std::string& request_id,
    const std::string& response_json) {
  if (framework_ && !request_id.empty()) {
    framework_->RespondToExtensionMessage(request_id.c_str(),
                                          response_json.c_str());
  }
}

bool PhiSurfaceCoordinator::BroadcastToExtensions(
    const std::string& type,
    const std::string& payload_json) {
  return framework_ &&
         framework_->BroadcastToExtensions(type.c_str(), payload_json.c_str());
}

PhiShellWindow* PhiSurfaceCoordinator::WindowShowingTab(TabId tab_id) {
  return host_.WindowShowingTab(tab_id);
}

bool PhiSurfaceCoordinator::HandleExtensionMessage(
    const std::string& request_id,
    const std::string& sender_id,
    const std::string& type,
    const std::string& payload_json) {
  return reader_.HandleExtensionMessage(request_id, sender_id, type,
                                        payload_json);
}

void PhiSurfaceCoordinator::OnOpenReaderView(int64_t window_id,
                                             int64_t tab_id) {
  // Right-click "Open in Reading Mode" in the page's own context menu.
  // ENTERS rather than toggles: the item is only reachable by right-clicking
  // the LIVE page, which is what is on screen when the reader is closed.
  const int tab = static_cast<int>(tab_id);
  LOG(INFO) << "[PhiClient] context menu reader win=" << window_id
            << " tab=" << tab;
  if (reader_.HasSurface(tab)) {
    return;
  }
  reader_.Open(tab, "context_menu");
}

bool PhiSurfaceCoordinator::HasPeek() const {
  return surfaces_.HasKind(SidebarSurface::kPeek);
}

bool PhiSurfaceCoordinator::DeferRetireOnPlaceholder(BrowserId browser,
                                                     bool entered) {
  if (!entered || !surfaces_.Find(browser)) {
    return false;
  }
  host_.PostReaderRetire(browser);
  return true;
}

void PhiSurfaceCoordinator::RetireReaderPanel(BrowserId panel_window) {
  if (surfaces_.Find(panel_window) && framework_) {
    framework_->CloseBrowserWindow(panel_window.value);
  }
}

namespace {

// A standalone HTML document around a distilled article — the skill's
// saveArticle() writes this to disk. A <base> keeps the article's relative
// image URLs resolving against the source; images are NOT inlined (the
// Mac's inliner is its own service), so the file opens with the network.
std::string BuildReaderDocument(const std::string& title,
                                const std::string& source_url,
                                const std::string& html) {
  const std::string safe_title = base::EscapeForHTML(title);
  return "<!doctype html>\n<html><head><meta charset=\"utf-8\">\n<title>" +
         safe_title + "</title>\n<base href=\"" +
         base::EscapeForHTML(source_url) +
         "\">\n<meta name=\"viewport\" content=\"width=device-width, "
         "initial-scale=1\">\n<style>\n"
         "body{margin:0;background:#fbfaf7;color:#1c1c1e;"
         "font:18px/1.65 -apple-system,\"Segoe UI\",system-ui,sans-serif}\n"
         "main{max-width:680px;margin:0 auto;padding:48px 24px 96px}\n"
         "h1{font-size:2em;line-height:1.2;margin:0 0 .4em}\n"
         ".source{color:#6b6b70;font-size:.85em;margin:0 0 2em;"
         "word-break:break-all}\n"
         "img,video{max-width:100%;height:auto}\n"
         "pre{overflow:auto;background:#f0efe9;padding:12px;"
         "border-radius:8px}\n"
         "a{color:#2f6fd6}\n</style></head>\n<body><main>\n<h1>" +
         safe_title + "</h1>\n<p class=\"source\"><a href=\"" +
         base::EscapeForHTML(source_url) + "\">" +
         base::EscapeForHTML(source_url) + "</a></p>\n<article>\n" + html +
         "\n</article>\n</main></body></html>\n";
}

// "<title>.html" with the characters a filename cannot carry replaced.
std::string ReaderSuggestedFileName(const std::string& title) {
  std::string name;
  for (unsigned char c : title) {
    if (c < 32 || std::string_view("<>:\"/\\|?*").find(static_cast<char>(c)) !=
                      std::string_view::npos) {
      name.push_back('-');
    } else {
      name.push_back(static_cast<char>(c));
    }
  }
  while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
    name.pop_back();
  }
  if (name.size() > 80) {
    name.resize(80);
  }
  return (name.empty() ? std::string("phi-article") : name) + ".html";
}

}  // namespace

void PhiSurfaceCoordinator::OnReaderArticleExtracted(int request_id,
                                                     const char* json) {
  // Shape the framework's article into the verb's reply. The Mac's fields
  // that come from ITS extraction ladder (rung, coverage, byline, siteName,
  // lang) are answered honestly: the rung is the distiller, coverage is
  // unknown, and a page the distiller finds nothing in is
  // no_article_detected exactly as on the Mac.
  bool document = false;
  bool include_html = true;
  if (!host_.PendingReplyShape(request_id, &document, &include_html)) {
    return;
  }
  std::optional<base::DictValue> article =
      base::JSONReader::ReadDict(json ? json : "{}", base::JSON_PARSE_RFC);
  base::DictValue out;
  if (!article || !article->FindBool("ok").value_or(false)) {
    out.Set("ok", false);
    const std::string* error = article ? article->FindString("error") : nullptr;
    out.Set("error", error ? *error : "no_article_detected");
  } else {
    const std::string* title = article->FindString("title");
    const std::string* source = article->FindString("sourceURL");
    const std::string* html = article->FindString("contentHTML");
    out.Set("ok", true);
    out.Set("title", title ? *title : "");
    out.Set("sourceURL", source ? *source : "");
    // The reader extension's ladder knows which rung produced the article,
    // how much of the page it covered, who wrote it and which site rule
    // fired; the framework's distiller knows none of that, so each field
    // rides along only when its source supplied it. An answer with no rung
    // came from the distiller and says so.
    const std::string* rung = article->FindString("rung");
    out.Set("rung", rung ? *rung : "distiller");
    for (const char* key : {"byline", "siteName", "lang", "rule"}) {
      if (const std::string* value = article->FindString(key)) {
        out.Set(key, *value);
      }
    }
    if (std::optional<double> coverage = article->FindDouble("coverage")) {
      out.Set("coverage", *coverage);
    }
    if (std::optional<int> blocks = article->FindInt("codeBlockCount")) {
      out.Set("codeBlockCount", *blocks);
    }
    out.Set("isComplete", true);
    if (document) {
      out.Set("suggestedFileName",
              ReaderSuggestedFileName(title ? *title : ""));
      out.Set("document",
              BuildReaderDocument(title ? *title : "", source ? *source : "",
                                  html ? *html : ""));
    } else {
      out.Set("htmlLength", article->FindInt("htmlLength").value_or(0));
      out.Set("pageCount", article->FindInt("pageCount").value_or(1));
      if (include_html) {
        out.Set("contentHTML", html ? *html : "");
      }
    }
  }
  std::string reply;
  base::JSONWriter::Write(out, &reply);
  host_.CompleteReaderReply(request_id, std::move(reply));
}

namespace {

// The reader's live style (PhiReaderStyle, a base::Value codec) and the
// model's (STL, so the header stays on the host) are the same eight
// fields; the copy lives here, in one place each way.
model::ReaderStyle ReaderStyleToModel(const PhiReaderStyle& style) {
  model::ReaderStyle reader;
  reader.font_size = style.font_size;
  reader.width = style.width;
  reader.theme = style.theme;
  reader.typeface = style.typeface;
  reader.line_height = style.line_height;
  reader.letter_spacing = style.letter_spacing;
  reader.shows_images = style.shows_images;
  reader.highlights_links = style.highlights_links;
  return reader;
}

void ReaderStyleFromModel(const model::ReaderStyle& reader,
                          PhiReaderStyle* style) {
  style->font_size = reader.font_size;
  style->width = reader.width;
  style->theme = reader.theme;
  style->typeface = reader.typeface;
  style->line_height = reader.line_height;
  style->letter_spacing = reader.letter_spacing;
  style->shows_images = reader.shows_images;
  style->highlights_links = reader.highlights_links;
}

}  // namespace

void PhiSurfaceCoordinator::SetReaderStyleFromModel(
    const model::ReaderStyle& style) {
  ReaderStyleFromModel(style, &reader_.mutable_style());
}

void PhiSurfaceCoordinator::OnReaderStyleChanged() {
  // The reader's live style is PhiReaderBridge's; the preferences carry the
  // copy the store writes.
  host_.PersistReaderStyle(ReaderStyleToModel(reader_.style()));
}

bool PhiSurfaceCoordinator::PanelizeReaderSurface(TabId origin_tab_id) {
  if (!framework_ || HasReaderPanelForOrigin(origin_tab_id)) {
    return false;
  }
  PhiShellWindow* window = host_.WindowShowingTab(origin_tab_id);
  if (!window) {
    return false;
  }
  const BrowserId browser = window->active_browser_id();
  TabId surface_tab_id;
  if (const phi_framework::WindowSnapshot* snapshot =
          window_model_.Get(browser)) {
    for (const phi_framework::TabSnapshot& tab : snapshot->tabs) {
      if (tab.surface == phi_framework::TabSurface::kReader &&
          tab.origin == origin_tab_id) {
        surface_tab_id = tab.id;
        break;
      }
    }
  }
  if (!surface_tab_id.valid()) {
    // Not an error: the framework panels a surface at insertion (see
    // DivertReaderSurfaceAfterInsert); by the time the extension reports
    // the open, the tab is usually already out of this strip.
    return false;
  }
  const BrowserId panel{framework_->MoveTabToReaderPanel(
      browser.value, surface_tab_id.value, origin_tab_id.value)};
  if (!panel.valid()) {
    LOG(WARNING) << "[PhiClient] could not panel reader surface "
                 << surface_tab_id.value;
    return false;
  }
  if (PhiSurfacePanel* panelled = surfaces_.Find(panel)) {
    panelled->BindSurfaceTab(surface_tab_id.value);
  }
  // Chromium foregrounded the surface when the extension created it, and
  // moving it out leaves whatever neighbour it landed on active. Hand the
  // strip back to the origin so tab-scoped commands keep targeting the
  // article the reader is showing — the Mac's setAsActiveTab step.
  framework_->ActivateTabById(browser.value, origin_tab_id.value);
  window->ScheduleRefresh();
  return true;
}

bool PhiSurfaceCoordinator::HasReaderPanelForOrigin(TabId origin_tab_id) const {
  return surfaces_.ForOrigin(origin_tab_id, SidebarSurface::kReader) != nullptr;
}

void PhiSurfaceCoordinator::CloseReaderPanelForOrigin(TabId origin_tab_id) {
  CloseSurfacesForOrigin(origin_tab_id, SidebarSurface::kReader);
}

void PhiSurfaceCoordinator::CloseSurfacesForOrigin(TabId origin_tab_id,
                                                   SidebarSurface kind,
                                                   const char* peek_end_reason) {
  if (!framework_) {
    return;
  }
  // Snapshot first: closing a browser runs OnBrowserWindowClosing, which
  // detaches the record from the registry.
  std::vector<BrowserId> doomed;
  surfaces_.ForEach([&](PhiSurfacePanel& panel) {
    if (panel.origin_tab_id() == origin_tab_id.value &&
        (kind == SidebarSurface::kContent || panel.kind() == kind)) {
      panel.NoteEndReason(peek_end_reason);
      doomed.push_back(BrowserId{panel.window_id()});
    }
  });
  for (const BrowserId window : doomed) {
    // Closing the window closes the surface tab with it; the reader
    // extension drops its cache on tabs.onRemoved, so both sides agree.
    framework_->CloseBrowserWindow(window.value);
  }
}

void PhiSurfaceCoordinator::ExpandReaderPanelIntoTab(int64_t panel_window_id,
                                                     const std::string& url) {
  const BrowserId panel{panel_window_id};
  if (!surfaces_.Find(panel)) {
    return;
  }
  pending_reader_expands_[panel] = url;
  host_.PostReaderExpand(panel);
}

void PhiSurfaceCoordinator::FinishReaderPanelExpand(BrowserId panel_window) {
  auto pending = pending_reader_expands_.find(panel_window);
  if (pending == pending_reader_expands_.end()) {
    return;
  }
  const std::string url = pending->second;
  pending_reader_expands_.erase(pending);
  PhiSurfacePanel* reader = surfaces_.Find(panel_window);
  if (!reader || !framework_) {
    return;
  }
  const BrowserId origin_window{reader->origin_window_id()};
  // Like Peek's expand, this RE-OPENS the url rather than moving the live tab
  // across: the destination has only just started loading, and moving the tab
  // out would empty the panel browser mid-move.
  framework_->OpenTab(origin_window, url.c_str());
  framework_->CloseBrowserWindow(panel_window.value);
}

void PhiSurfaceCoordinator::UpdateReaderPanelsForWindow(BrowserId window_id) {
  const phi_framework::WindowSnapshot* snapshot = window_model_.Get(window_id);
  if (!framework_ || !snapshot) {
    return;
  }
  const phi_framework::TabSnapshot* active_snapshot =
      window_model_.ActiveTab(window_id);
  const TabId active_tab = active_snapshot ? active_snapshot->id : TabId();
  // The framework panels a reader surface at insertion and closes husks
  // (DivertReaderSurfaceAfterInsert); this is the FALLBACK for one it could
  // not place — a live surface (the extension reported it up, or an open is
  // still in flight) goes into a panel over its origin, anything else is a
  // husk and closes, as the Mac does on sight.
  for (const phi_framework::TabSnapshot& tab : snapshot->tabs) {
    if (tab.surface != phi_framework::TabSurface::kReader) {
      continue;
    }
    const TabId origin = tab.origin;
    if (!origin.valid() || HasReaderPanelForOrigin(origin)) {
      continue;
    }
    if (reader_.HasSurface(origin.value) ||
        reader_.HasPendingOpen(origin.value)) {
      PanelizeReaderSurface(origin);
      return;  // The strip changed either way; its push re-runs this.
    }
    LOG(INFO) << "[PhiClient] closing orphaned reader surface tab in window "
              << window_id.value;
    framework_->CloseTabById(window_id.value, tab.id.value);
    return;  // The close's own push re-runs this on the new strip.
  }
  // Every surface over this window, both kinds: an origin that left the
  // strip — closed, torn off, moved to another Space — takes its surface
  // with it (the Mac's origin watch); the rest re-fit and show only over
  // the active tab.
  std::vector<BrowserId> orphaned;
  for (PhiSurfacePanel* panel : surfaces_.OverWindow(window_id)) {
    if (!window_model_.FindTab(
            window_id, TabId{static_cast<int32_t>(panel->origin_tab_id())})) {
      panel->NoteEndReason("opener_closed");
      orphaned.push_back(BrowserId{panel->window_id()});
      continue;
    }
    panel->Reposition();
    panel->SetVisibleForActiveTab(active_tab.value);
  }
  for (const BrowserId panel_id : orphaned) {
    LOG(INFO) << "[PhiClient] surface " << panel_id.value
              << " origin left the strip — closing";
    if (framework_) {
      framework_->CloseBrowserWindow(panel_id.value);
    }
  }
}

void PhiSurfaceCoordinator::OnReaderOfferabilityChanged(TabId tab_id) {
  // The reader button is laid out from this verdict, so the window showing
  // the tab has to repaint. At the next idle: the report can arrive during a
  // navigation the strip has not caught up with yet.
  host_.ScheduleRefreshForTab(tab_id);
}

bool PhiSurfaceCoordinator::IsReaderOfferable(int tab_id,
                                              const std::string& url) const {
  return reader_.IsOfferable(tab_id, url);
}

bool PhiSurfaceCoordinator::IsReaderActive(int tab_id,
                                           const std::string& url) const {
  return PhiReaderBridge::IsReaderPageUrl(url) || reader_.HasSurface(tab_id);
}

void PhiSurfaceCoordinator::RequestToggleReaderView(int tab_id,
                                                    const std::string& url,
                                                    const char* entry_point) {
  reader_.Toggle(tab_id, url, entry_point);
}

void PhiSurfaceCoordinator::RaiseReaderPanels(SlotId slot) {
  surfaces_.ForEach([&](PhiSurfacePanel& panel) {
    if (host_.SlotForBrowser(BrowserId{panel.origin_window_id()}) == slot) {
      panel.Raise();
    }
  });
}

void PhiSurfaceCoordinator::NoteContentLayoutChanged(SlotId slot) {
  surfaces_.ForEach([&](PhiSurfacePanel& panel) {
    if (host_.SlotForBrowser(BrowserId{panel.origin_window_id()}) == slot) {
      panel.Reposition();
    }
  });
}

bool PhiSurfaceCoordinator::DismissPeeksForShell(HWND shell,
                                                 HWND inside,
                                                 const POINT* screen_point) {
  if (!shell) {
    return false;
  }
  std::vector<BrowserId> to_dismiss;
  surfaces_.ForEach([&](PhiSurfacePanel& panel) {
    if (panel.kind() != SidebarSurface::kPeek) {
      return;
    }
    const HWND card = panel.hwnd();
    if (!card || ::GetAncestor(card, GA_ROOT) != shell) {
      return;
    }
    // A peek whose opener tab is not the active one is hidden, not "around"
    // anything: a click on another tab's page (or an Esc there) leaves it
    // for its own tab to come back to, as the Mac's per-tab peeks do.
    if (!::IsWindowVisible(card)) {
      return;
    }
    if (inside && panel.ContainsWindow(inside)) {
      return;  // A click INSIDE the card is the card's own business.
    }
    if (screen_point && !panel.ClickDismisses(*screen_point)) {
      return;  // Chrome, not the page pane: the peek keeps its place.
    }
    panel.NoteEndReason("closed");  // Esc / a click outside: the user's.
    to_dismiss.push_back(BrowserId{panel.window_id()});
  });
  // Closing the peek's browser is the dismissal (the card's own close chip
  // does the same); OnBrowserWindowClosing then detaches the record — so
  // iterate a snapshot, never the registry.
  for (const BrowserId window : to_dismiss) {
    if (framework_) {
      framework_->CloseBrowserWindow(window.value);
    }
  }
  return !to_dismiss.empty();
}

}  // namespace phi_client
