/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#include "config.h"

// Dispatch forks the per-type C++ collector binaries under libexecdir.

// SelfTest keeps the legacy selftest_dump.sh dispatch (no C++ collector).
// FPGA is an EM-driven register-table collector (I2C Pages[], like CPLD).
constexpr auto SELFTEST_BIN_PATH = "/usr/bin/selftest_dump.sh";
constexpr auto FPGA_DUMP_BIN_PATH =
    "/usr/libexec/phosphor-debug-collector/nv-collector-fpga";
constexpr auto EROT_DUMP_BIN_PATH =
    "/usr/libexec/phosphor-debug-collector/nv-collector-erot";
constexpr auto FWATTRS_DUMP_BIN_PATH =
    "/usr/libexec/phosphor-debug-collector/nv-collector-fw-attrs";
constexpr auto HWCHECKOUT_DUMP_BIN_PATH =
    "/usr/libexec/phosphor-debug-collector/nv-collector-hw-checkout";
constexpr auto SMA_DUMP_BIN_PATH =
    "/usr/libexec/phosphor-debug-collector/nv-collector-sma";
constexpr auto CPLD_DUMP_BIN_PATH =
    "/usr/libexec/phosphor-debug-collector/nv-collector-cpld";
constexpr auto CPU_DIAGNOSTIC_DUMP_BIN_PATH =
    "/usr/libexec/phosphor-debug-collector/nv-collector-cpu-diag";
// nsmDump() invokes nsm-dump-tool directly with -o <mode>; it produces the
// obmcdump archive itself, so no per-type nv-collector wrapper is needed.
constexpr auto NSM_DUMP_BIN_PATH = "/usr/bin/nsm-dump-tool";

// Existing binaries / config files not in scope for per-type C++ migration.
constexpr auto SELFTEST_DAT_CFG_PATH = "/usr/share/oobaml/dat.json";
constexpr auto RETIMER_LTSSM_DUMP_BIN_PATH = "/usr/bin/retimerLtssmDump.sh";
constexpr auto RETIMER_REGISTER_DUMP_BIN_PATH =
    "/usr/bin/retimerRegisterDump.sh";
constexpr auto NSM_DUMP_TEMP_PATH = "/var/emmc/user-logs";
constexpr auto CPU_DIAGNOSTIC_DUMP_TEMP_PATH = "/tmp/cpu_diagnostic_dump";
