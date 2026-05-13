/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <string>

// Helpers used by nsm-dump-tool to consume nsmd's NetIR debug-dump D-Bus
// surface (Async.Status, Async.Value, conditional Dump.* interfaces).
// Kept out of main.cpp so the dump-orchestration flow stays readable.

// Probe whether `targetDevice`'s inventory path exposes a given D-Bus
// interface. Lets the caller skip optional NetIR features (LogInfo,
// Erase) when nsmd has not exposed them for the device class.
// Matches the device name as a complete '/'-delimited path segment, so
// "GPU1" does not spuriously match "GPU10".
bool deviceHasInterface(const std::string& targetDevice,
                        const std::string& iface);

// Probe whether one *exact* object path implements an interface (via
// ObjectMapper GetObject). Use this when the capability check must match
// the specific object being invoked rather than any object under the
// device (e.g. erasing on the log object's own path, which may differ
// from the dump object's path).
bool objectPathHasInterface(const std::string& objectPath,
                            const std::string& iface);

// Categorized form of com.nvidia.Async.Status.AsyncOperationStatus
// ready to render in Execution_Report.txt. Severity / retry / resolution
// follow the contract in nsmd/docs/netir-debug-dump-improvements.md §6.5.
struct AsyncStatusInfo
{
    std::string text;
    std::string severity; // "OK" / "Warning" / "Critical" / "" (other)
    std::string resolution;
    bool retryable = false;
};

AsyncStatusInfo asyncStatusToHuman(const std::string& enumStr);

// Decoded form of the raw NSM error nsmd packs into
// com.nvidia.Async.Value.Value. Lets the report carry the original NSM
// verdict so triage doesn't depend on journald retention.
//
// Layout MUST match nsm::packNsmError() in nsmd:
//   bits 32-63: swRc       (int32_t)
//   bits 16-31: reasonCode (uint16_t)
//   bits  8-15: cc         (uint8_t)
//   bits  0- 7: reserved
struct UnpackedNsmError
{
    int32_t swRc;
    uint8_t cc;
    uint16_t reasonCode;
};

UnpackedNsmError unpackNsmError(uint64_t packed);

// Read com.nvidia.Async.Value.Value at the AsyncHandle path. Soft-fails
// (returns false) when the property is absent, so the tool stays
// compatible with older nsmd builds that do not publish the packed
// value.
bool getAsyncValue(const std::string& path, uint64_t& outValue);

// Build the multi-line failure block emitted to Execution_Report.txt
// for a failed asynchronous dump/log/erase operation. Centralises the
// formatting so the caller does a single logMsg(), and one open/close
// of the report file per failure.
std::string renderAsyncFailureBlock(
    const std::string& asyncHandlePath, const std::string& targetDevice,
    const std::string& dataLabel, const std::string& asyncStatusEnum);
