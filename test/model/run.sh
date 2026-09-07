#!/bin/bash
# Host-compiled tests for the sidebar model and layout
# (docs/sidebar-architecture.md §8). Both depend on the STL only, so they
# build with the Linux host g++ — no Windows SDK, framework or Chromium base/.
set -e
cd "$(dirname "$0")"
CXX="${CXX:-g++} -std=c++20 -Wall -Wextra -Werror -I ../../client"
$CXX ../../client/phi_sidebar_model.cc sidebar_model_test.cc -o /tmp/phi_sidebar_model_test
$CXX ../../client/phi_sidebar_layout.cc sidebar_layout_test.cc -o /tmp/phi_sidebar_layout_test
$CXX ../../client/phi_sidebar_model.cc ../../client/phi_sidebar_layout.cc \
     ../../client/phi_sidebar_actions.cc ../../client/phi_sidebar_drop.cc \
     sidebar_actions_test.cc -o /tmp/phi_sidebar_actions_test
/tmp/phi_sidebar_model_test
/tmp/phi_sidebar_layout_test
/tmp/phi_sidebar_actions_test
