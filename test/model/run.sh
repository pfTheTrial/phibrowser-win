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
$CXX url_router_test.cc -o /tmp/phi_url_router_test
/tmp/phi_url_router_test
# The lazy-restore service's two pure decisions (the receipt's settle reason
# and the last-active Space); the ghost store and the framework calls stay
# on the target.
$CXX session_restore_test.cc -o /tmp/phi_session_restore_test
/tmp/phi_session_restore_test
