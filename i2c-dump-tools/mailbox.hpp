// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.

// The I2C mailbox hmc-dump-client (BMC) and hmc-dump-server (HMC) talk over:
// two I2C target EEPROMs the HMC exposes to the BMC.
//
// Control EEPROM: byte 0 takes start/stop of the dump server, byte 1 the
// opcode, bytes 2-17 arguments, byte 18 the request flag, byte 19 is set
// while the command runs, byte 20 holds the return code, bytes 21-36 the
// result. Data EEPROM: staged dump block size at 0x700 (big-endian), data at
// 0x704.

#pragma once

#include <cstddef>
#include <cstdint>

namespace phosphor::dump::hmc_mailbox
{

constexpr uint8_t control_address = 0x45;
constexpr uint8_t data_address = 0x4f;

constexpr uint8_t start_dump_server = 0x05;
constexpr uint8_t stop_dump_server = 0x06;

constexpr uint8_t opcode_offset = 1;
constexpr std::size_t max_args = 16;
constexpr uint8_t request_offset = 18;
constexpr uint8_t running_offset = 19;
constexpr uint8_t return_code_offset = 20;
constexpr uint8_t result_offset = 21;

constexpr uint8_t create_dump = 1;
constexpr uint8_t dump_progress = 2;
constexpr uint8_t dump_block = 3;
constexpr uint8_t dump_busy = 2;

constexpr uint16_t block_size_offset = 0x700;
constexpr uint16_t block_data_offset = 0x704;
constexpr uint32_t max_block_size = 8192;

} // namespace phosphor::dump::hmc_mailbox
