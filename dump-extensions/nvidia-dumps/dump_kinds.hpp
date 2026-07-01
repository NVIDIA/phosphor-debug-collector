/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
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

#include <string>
#include <unordered_map>

namespace phosphor::dump
{

// Per dump-kind facts: Redfish OEMDiagnosticDataType base name and whether the
// dump is per-device (fan-out, emits ";DeviceType="). Single source of truth
// for both the AllowableValues builder and createDump dispatch.
struct DumpKind
{
    std::string diagnosticType;
    bool perDevice;
};

inline const std::unordered_map<std::string, DumpKind>& dumpKinds()
{
    static const std::unordered_map<std::string, DumpKind> table = {
        {"RoT", {"ROT", false}},
        {"FwAttrs", {"FirmwareAttributes", false}},
        {"HwCheckout", {"HardwareCheckout", false}},
        {"CPLD", {"CPLD", false}},
        {"SMA", {"SMA", false}},
        {"FPGA", {"FPGA", false}},
        {"GlacierI2cLog", {"GlacierI2cLog", false}},
        {"GPUDiag", {"GPUDeviceDiagnostics", true}},
        {"CPUDiag", {"CPUDiagnosticsData", true}},
        {"NetIR", {"NetIR", true}},
    };
    return table;
}

// Per-role hw_checkout.sh arguments for the checkout-family collectors. Role
// (hmc/bmc) is detected at runtime from the EM record Name (HGX_* = HMC).
struct CheckoutArgs
{
    std::string hmcArgs;
    std::string bmcArgs;
};

inline const std::unordered_map<std::string, CheckoutArgs>& checkoutArgs()
{
    static const std::unordered_map<std::string, CheckoutArgs> table = {
        {"HwCheckout", {"hmc gpu cpu sma", "bmc fpga"}},
        {"FwAttrs", {"firmware", "firmware"}},
    };
    return table;
}

} // namespace phosphor::dump
