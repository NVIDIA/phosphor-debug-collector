/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "nvidia_dumps_config.hpp"

#include <string>
#include <unordered_map>

namespace phosphor
{
namespace dump
{
namespace system
{

enum class DiagnosticType
{
    SelfTest,
    FPGA,
    EROT,
    ROT,
    SMA,
    NVSwitch,
    NVLinkManagementNIC,
    GPU_SXM,
    NetIR,
    GPUDeviceDiagnostics,
    RetLTSSM,
    RetRegister,
    FirmwareAttributes,
    HardwareCheckout,
    CPLD,
    CPUDiagnosticDump,
    Unknown
};

inline DiagnosticType getDiagnosticType(const std::string& typeStr)
{
    static const std::unordered_map<std::string, DiagnosticType>
        diagnosticTypeMap = {
            {"SelfTest", DiagnosticType::SelfTest},
            {"FPGA", DiagnosticType::FPGA},
            {"EROT", DiagnosticType::EROT},
            {"ROT", DiagnosticType::ROT},
            {"SMA", DiagnosticType::SMA},
            {"Net_NVSwitch", DiagnosticType::NVSwitch},
            {"Net_NVLinkManagementNIC", DiagnosticType::NVLinkManagementNIC},
            {"Net_GPU_SXM", DiagnosticType::GPU_SXM},
            {"NetIR", DiagnosticType::NetIR},
            {"GPUDeviceDiagnostics", DiagnosticType::GPUDeviceDiagnostics},
            {"RetLTSSM", DiagnosticType::RetLTSSM},
            {"RetRegister", DiagnosticType::RetRegister},
            {"FirmwareAttributes", DiagnosticType::FirmwareAttributes},
            {"HardwareCheckout", DiagnosticType::HardwareCheckout},
            {"CPLD", DiagnosticType::CPLD},
            {"CPUDiagnosticsData", DiagnosticType::CPUDiagnosticDump},
        };

    auto it = diagnosticTypeMap.find(typeStr);
    return it != diagnosticTypeMap.end() ? it->second : DiagnosticType::Unknown;
}

inline const char* getDiagnosticDumpBinaryPath(DiagnosticType type)
{
    switch (type)
    {
        case DiagnosticType::EROT:
            return EROT_DUMP_BIN_PATH;
        case DiagnosticType::ROT:
            return ROT_DUMP_BIN_PATH;
        default:
            return nullptr;
    }
}

} // namespace system
} // namespace dump
} // namespace phosphor
