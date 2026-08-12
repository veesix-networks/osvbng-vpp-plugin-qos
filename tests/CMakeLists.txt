# Copyright 2026 Veesix Networks Ltd
# Licensed under the GNU General Public License v3.0 or later.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Standalone build for the DRR/shaper harness. Deliberately not part of the
# plugin's add_vpp_plugin build: the harness exists so the scheduling
# arithmetic can be exercised without a VPP tree, and wiring it into the
# plugin build would give it the dependency it is meant to avoid.
#
#   cmake -S tests -B build/tests && cmake --build build/tests
#   ctest --test-dir build/tests --output-on-failure

cmake_minimum_required(VERSION 3.13)
project(osvbng_qos_sched_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(drr_harness drr_harness.c)

# The headers under test carry no include paths of their own, by contract.
target_include_directories(drr_harness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(drr_harness PRIVATE Threads::Threads)
target_compile_options(drr_harness PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME drr_harness COMMAND drr_harness)
