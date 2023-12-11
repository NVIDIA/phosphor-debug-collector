#pragma once

#include "dump-extensions.hpp"
#include "dump_manager.hpp"

namespace phosphor
{
namespace dump
{
/**
 * @brief load the FDR dump extension
 *
 * @param[in] bus - Bus to attach to
 * @param[out] dumpMgrList - list dump manager objects.
 *
 */
void loadExtensionsFDR(sdbusplus::bus::bus& bus, DumpManagerList& dumpMgrList);
} // namespace dump
} // namespace phosphor
