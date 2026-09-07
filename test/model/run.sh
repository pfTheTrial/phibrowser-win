#!/bin/bash
# Host-compiled tests for the sidebar model and layout
# (docs/sidebar-architecture.md §8) and the window registry
# (docs/architecture-review.md §3.4). All depend on the STL only, so they
# build with the Linux host g++ — no Windows SDK, framework or Chromium base/.
# The registry test reaches the framework's id/kind header through the fork
# checkout (a symlink to this repo lives beside it as src/phibrowser-win, so
# one include root serves both spellings — a SYSTEM root, so the framework
# header's no-op delegate bodies do not trip -Wextra); PHI_FORK_SRC overrides
# the path.
set -e
cd "$(dirname "$0")"
FORK_SRC="${PHI_FORK_SRC:-../../../chromuim/src}"
CXX="${CXX:-g++} -std=c++20 -Wall -Wextra -Werror -I ../../client -isystem $FORK_SRC"
$CXX ../../client/phi_sidebar_model.cc sidebar_model_test.cc -o /tmp/phi_sidebar_model_test
$CXX ../../client/phi_sidebar_layout.cc sidebar_layout_test.cc -o /tmp/phi_sidebar_layout_test
$CXX ../../client/phi_sidebar_model.cc ../../client/phi_sidebar_layout.cc \
     ../../client/phi_sidebar_actions.cc ../../client/phi_sidebar_drop.cc \
     sidebar_actions_test.cc -o /tmp/phi_sidebar_actions_test
$CXX ../../client/phi_window_registry.cc window_registry_test.cc \
     -o /tmp/phi_window_registry_test
/tmp/phi_sidebar_model_test
/tmp/phi_sidebar_layout_test
/tmp/phi_sidebar_actions_test
/tmp/phi_window_registry_test
