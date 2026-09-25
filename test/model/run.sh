#!/bin/bash
# Host-compiled tests for the sidebar model and layout
# (docs/sidebar-architecture.md §8) and the window registry
# (docs/architecture-review.md §3.4) and the window model (§3.3). All depend on the STL only, so they
# build with the Linux host g++ — no Windows SDK, framework or Chromium base/.
# The registry test reaches the framework's id/kind header through the fork
# checkout (a symlink to this repo lives beside it as src/phibrowser-win, so
# one include root serves both spellings — a SYSTEM root, so the framework
# header's no-op delegate bodies do not trip -Wextra); PHI_FORK_SRC overrides
# the path. stubs/ sits AHEAD of it: the one base/ header the service
# headers take (raw_ptr_exclusion.h) reaches generated buildflags in the
# real tree, so the host gets a one-macro shim instead.
set -e
cd "$(dirname "$0")"
FORK_SRC="${PHI_FORK_SRC:-../../../chromuim/src}"
CXX="${CXX:-g++} -std=c++20 -Wall -Wextra -Werror -I ../../client -isystem stubs -isystem $FORK_SRC"
$CXX ../../client/phi_sidebar_model.cc sidebar_model_test.cc -o /tmp/phi_sidebar_model_test
$CXX ../../client/phi_sidebar_layout.cc sidebar_layout_test.cc -o /tmp/phi_sidebar_layout_test
$CXX ../../client/phi_sidebar_model.cc ../../client/phi_sidebar_layout.cc \
     ../../client/phi_sidebar_actions.cc ../../client/phi_sidebar_drop.cc \
     sidebar_actions_test.cc -o /tmp/phi_sidebar_actions_test
$CXX ../../client/phi_window_registry.cc window_registry_test.cc \
     -o /tmp/phi_window_registry_test
$CXX ../../client/phi_window_model.cc window_model_test.cc \
     -o /tmp/phi_window_model_test
$CXX ../../client/phi_shell_layout.cc shell_layout_test.cc \
     -o /tmp/phi_shell_layout_test
/tmp/phi_sidebar_model_test
/tmp/phi_sidebar_layout_test
/tmp/phi_sidebar_actions_test
/tmp/phi_window_registry_test
/tmp/phi_window_model_test
/tmp/phi_shell_layout_test
# The app model (docs/architecture-review.md §3.4, phase 3): defaults and
# the pure id helpers; the codecs need base::Value and stay on the target.
$CXX app_model_test.cc -o /tmp/phi_app_model_test
/tmp/phi_app_model_test
# Phase 4's services (§3.4): the Space manager's default-role / delete /
# reorder helpers and the URL router's rule operations over the shared
# model. Both headers are host-clean; the services' bodies stay on the
# target.
$CXX space_manager_test.cc -o /tmp/phi_space_manager_test
/tmp/phi_space_manager_test
$CXX ../../client/phi_url_rule_normalize.cc url_router_test.cc \
     -o /tmp/phi_url_router_test
/tmp/phi_url_router_test
# The lazy-restore service's two pure decisions (the receipt's settle reason
# and the last-active Space); the ghost store and the framework calls stay
# on the target.
$CXX session_restore_test.cc -o /tmp/phi_session_restore_test
/tmp/phi_session_restore_test
# The agent Space switch route (phibrowser-mac 3a9fc111 ported): which user
# window offers an agent Space's pip and where a switch asked from another
# window lands. Header-only; the manager that consults it stays on the target.
$CXX agent_space_route_test.cc -o /tmp/phi_agent_space_route_test
/tmp/phi_agent_space_route_test
# The onboarding's analytics session (phi_oobe_analytics.h): the Mac's
# OOBEAnalyticsSession ported as a pure state machine — the same cases as
# its OOBEAnalyticsSessionTests. Header-only; the window that drives it
# stays on the target.
$CXX ../../client/phi_oobe_analytics.cc oobe_analytics_test.cc \
     -o /tmp/phi_oobe_analytics_test
/tmp/phi_oobe_analytics_test
# Phase 5's UI substrate (docs/architecture-review.md §3.6): the PURE half
# of phi_window.cc — popover placement with monitor clamping, hit-rect
# resolution, timer id allocation, easing. The Win32 half of the same file
# is behind #if defined(_WIN32) and compiles away on the host.
$CXX ../../client/phi_window.cc ui_substrate_test.cc \
     -o /tmp/phi_ui_substrate_test
/tmp/phi_ui_substrate_test
# Phase 5's secondary-window family (§3.6, lane 3 §6.2): the ONE caption
# frame's button geometry and hit-test (four copies before), and the
# active-dialog list's two decisions — which keys a window hands to the
# dialog manager, and which registered window owns a message. Both are
# pure; the hook and IsDialogMessage stay on the target.
$CXX ../../client/phi_window.cc ../../client/phi_secondary_window.cc \
     secondary_window_test.cc -o /tmp/phi_secondary_window_test
/tmp/phi_secondary_window_test
# Phase 5's rasteriser swap (§3.6): the geometry phi_dwrite hands Direct2D —
# the stroke inset that reproduces the software field's band, the dash fit
# that closes a ring — and how far a pixel can move when the rasteriser
# changes. Pure math (the drawing itself needs Direct2D and stays on the
# target); the field's own functions are copied into the test as a frozen
# reference, so a change to one that is not made in the other fails here.
$CXX dwrite_geometry_test.cc -o /tmp/phi_dwrite_geometry_test
/tmp/phi_dwrite_geometry_test
# Phase 5's accessibility provider (§3.6, §4 item 31): the PURE half of
# phi_uia.cc — the role -> UIA control-type table, the patterns each role
# genuinely has, the client->screen rect transform, identity across a hit
# list rebuild, the point a UIA Invoke clicks at, and the version/event
# decision table. The COM plumbing that carries those answers needs a
# message loop and is checked on the box (test/uia-probe.ps1, Inspect.exe).
$CXX ../../client/phi_window.cc ../../client/phi_uia.cc uia_test.cc \
     -o /tmp/phi_uia_test
/tmp/phi_uia_test
# Phase 6's first component (§3.5): the pure half of the omnibox session —
# where the EDIT sits inside the address capsule, and the dropdown's row and
# selection arithmetic. Header-only, so the test needs no client .cc; the
# session's Win32 half (the EDIT, its subclass, the popup window) stays on
# the target and the test defines PHI_OMNIBOX_SESSION_MODEL_ONLY to skip it.
$CXX omnibox_session_test.cc -o /tmp/phi_omnibox_session_test
/tmp/phi_omnibox_session_test
# Phase 5's PANEL family (§3.6, lane 3 §6.2): the pure half of
# phi_panel.cc — the card stack's z-order rules (which replace nine
# scattered SetWindowPos(HWND_TOP) calls) and the shaped-layer geometry
# that replaces the two SetWindowRgn surfaces. The Win32 half is behind
# #if defined(_WIN32) and compiles away on the host.
$CXX ../../client/phi_window.cc ../../client/phi_panel.cc panel_test.cc \
     -o /tmp/phi_panel_test
/tmp/phi_panel_test
# Phase 6's second component (§3.5): the pure half of the content host — the
# table of which browser backs each Space in one slot, which of them the slot
# shows, and the no-fallback rule for the active BrowserId. Header-only for
# the same reason as the omnibox model; the adoption, caps and bubble stay on
# the target.
$CXX browser_host_test.cc -o /tmp/phi_browser_host_test
/tmp/phi_browser_host_test
# Phase 6's menu data (§3.5, §3.1 principle 4, lane 1 §3.2): every context
# menu the shell drops, built by a PURE function of a snapshot — contents,
# order, the enable/check states and the targets each row carries. The HMENU
# tree and the TrackPopupMenu loop are behind #if defined(_WIN32) and compile
# away here; the string catalog is stubbed inside the test.
$CXX ../../client/phi_shell_menus.cc shell_menus_test.cc \
     -o /tmp/phi_shell_menus_test
/tmp/phi_shell_menus_test
# Phase 6's settings rows (§3.5, lane 1 §1.3 and §5 D7): the typed rows the
# settings panes read, and the three records that still travel as TEXT — the
# search-engine rows agentSpace.settings.get answers verbatim, the rule rows
# the urlRules.* verbs build their JSON from, and the audit lines the consent
# FILE stores. Each is round-tripped through its encoding, plus the Shortcuts
# pane's row order. Header-only, so the test needs no client .cc.
$CXX settings_rows_test.cc -o /tmp/phi_settings_rows_test
/tmp/phi_settings_rows_test
# Phase 6's sidebar (§3.5, lane 1 clusters H and J): the pure half of the
# view — the MRU order the Ctrl+Tab switcher walks, the bookmark outline's
# disclosure rule, and the key -> row resolution that lets drag state
# survive a rebuild. The row pipeline's placement, the band's paint and the
# hit tests are Win32 and stay on the target; the test defines
# PHI_SIDEBAR_VIEW_MODEL_ONLY to skip them, and needs no client .cc because
# the pure half is inline in the header (the key type comes from the model).
$CXX ../../client/phi_sidebar_model.cc sidebar_view_test.cc \
     -o /tmp/phi_sidebar_view_test
/tmp/phi_sidebar_view_test
# The same for the drag session (cluster I): what arming a drag on a
# SidebarKey drags, when a press becomes a drag, and the reorder slide's
# easing. The capture, the drag image and the drop resolution stay on the
# target and are driven end to end on the box by test/sv-drag.mjs.
$CXX ../../client/phi_sidebar_model.cc sidebar_drag_test.cc \
     -o /tmp/phi_sidebar_drag_test
/tmp/phi_sidebar_drag_test
# The Continuous track's cross-cutting foundations
# (docs/architecture-review.md §3.9, lane 7 §5.5): phi_util's shared helpers
# — the one blend, the one clamp, the one DPI scaler, the trims, the plural
# rule, the digit grouping and the relative-time ladder — plus phi_log's
# redactors and PhiFw::Must, and the theme tokens phi_theme resolves. Each
# file is in two halves: everything the host compiles is pure, and the Win32
# half (file I/O, UTF-16 conversion, GetNumberFormatEx, LOG) is behind
# #if defined(_WIN32) and compiles away here. phi_theme.cc reaches
# base::NoDestructor, which stubs/ shims.
$CXX ../../client/phi_util.cc ../../client/phi_log.cc \
     ../../client/phi_theme.cc util_test.cc -o /tmp/phi_util_test
/tmp/phi_util_test
# Bucket D's auth (docs/login-port.md): the pure half of the Auth0 sign-in —
# base64url, SHA-256 and the PKCE challenge (RFC 7636's vectors), the
# loopback callback's request line, the JWT payload split and the tenant ->
# account-service rule. The sockets, WinHTTP, DPAPI and the message window
# stay in phi_auth.cc and are proved by a real sign-in on the box.
$CXX ../../client/phi_auth_codec.cc auth_codec_test.cc \
     -o /tmp/phi_auth_codec_test
/tmp/phi_auth_codec_test
# The sync conformance gate (docs/user-data-parity.md, phase S0): the C++
# port of the Mac's sync merge core replayed against vectors the Mac's own
# code wrote (test/sync-vectors). Families the port has not reached yet are
# listed as PENDING, never silently skipped.
# The vendored SQLite, compiled once as C for the tests that need it (the
# local store, and the URL rule normalisation the vectors pin through it).
SQLITE_SRC=../../third_party/sqlite/sqlite3.c
SQLITE_OBJ=/tmp/phi_sqlite3.o
if [[ ! -f $SQLITE_OBJ || $SQLITE_OBJ -ot $SQLITE_SRC ]]; then
  ${CC:-gcc} -O1 -w -DSQLITE_THREADSAFE=1 -DSQLITE_DQS=0 \
    -DSQLITE_OMIT_LOAD_EXTENSION -c $SQLITE_SRC -o $SQLITE_OBJ
fi
# BoringSSL for the sync tests (client tag hashes, the key layer): the
# fork's own copy, built once for the host with CMake (the Windows client
# links the framework kit's boringssl.lib instead).
BSSL_SRC=$FORK_SRC/third_party/boringssl/src
BSSL_OUT=/tmp/phi-bssl
if [[ ! -f $BSSL_OUT/libcrypto.a ]]; then
  NINJA=$FORK_SRC/third_party/ninja/ninja
  cmake -S $BSSL_SRC -B $BSSL_OUT -G Ninja -DCMAKE_MAKE_PROGRAM=$NINJA \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DOPENSSL_NO_ASM=1 >/dev/null
  $NINJA -C $BSSL_OUT crypto >/dev/null
fi
BSSL="-isystem $BSSL_SRC/include $BSSL_OUT/libcrypto.a -lpthread"
$CXX ../../client/phi_sync_rank.cc ../../client/phi_url_rule_normalize.cc \
     ../../client/phi_sync_proto.cc ../../client/phi_sync_merge.cc \
     ../../client/phi_key_crypto.cc sync_vectors_test.cc $BSSL \
     -o /tmp/phi_sync_vectors_test
/tmp/phi_sync_vectors_test ../sync-vectors
# The account's local store (docs/user-data-parity.md, phase S1): the
# Mac's LocalStore write rules — stamps, renumbering, pin-scope migration,
# Space cascades, the URL rule edit set — and persistence.
$CXX ../../client/phi_local_store.cc ../../client/phi_url_rule_normalize.cc \
     local_store_test.cc $SQLITE_OBJ \
     -lpthread -ldl -o /tmp/phi_local_store_test
/tmp/phi_local_store_test
# Guest → account migration (docs/user-data-parity.md S6): the Mac's
# GuestDataMigration planner, import and verification over two stores.
$CXX ../../client/phi_local_store.cc ../../client/phi_url_rule_normalize.cc \
     ../../client/phi_guest_migration.cc guest_migration_test.cc $SQLITE_OBJ \
     -lpthread -ldl -o /tmp/phi_guest_migration_test
/tmp/phi_guest_migration_test
# The Sync pane's status card: the Mac's summary reduction and the
# framework payload decode. Header-only.
$CXX sync_status_test.cc -o /tmp/phi_sync_status_test
/tmp/phi_sync_status_test
# The sync key layer (docs/user-data-parity.md, phase S3): the Mac's
# PhiKeyCrypto + RecoveryCode.
$CXX ../../client/phi_key_crypto.cc key_crypto_test.cc $BSSL \
     -o /tmp/phi_key_crypto_test
/tmp/phi_key_crypto_test
# The key layer's managers (AccountKeyManager, approvals, profile keys, the
# controller's mapping pass and pairing gate) device to device, against an
# in-memory /keys/v1 that keeps the server's refusals.
$CXX ../../client/phi_key_crypto.cc ../../client/phi_sync_keys.cc \
     sync_keys_test.cc $BSSL -o /tmp/phi_sync_keys_test
/tmp/phi_sync_keys_test
# Phi native sync's engine (docs/user-data-parity.md S5): the settings and
# Space paths device to device through a fake sync server that keeps the
# server's rules.
$CXX ../../client/phi_key_crypto.cc ../../client/phi_sync_proto.cc \
     ../../client/phi_sync_merge.cc ../../client/phi_sync_protocol.cc \
     ../../client/phi_sync_rank.cc ../../client/phi_sync_spaces.cc \
     ../../client/phi_sync_owned.cc ../../client/phi_sync_owned_kinds.cc \
     ../../client/phi_sync_bookmarks.cc ../../client/phi_sync_pins.cc \
     ../../client/phi_sync_rules.cc ../../client/phi_url_rule_normalize.cc \
     ../../client/phi_sync_engine.cc sync_engine_test.cc $BSSL \
     -o /tmp/phi_sync_engine_test
# The invalidation stream's SSE parser.
$CXX ../../client/phi_sync_invalidation.cc sync_invalidation_test.cc \
     -o /tmp/phi_sync_invalidation_test
/tmp/phi_sync_invalidation_test
/tmp/phi_sync_engine_test
