#pragma once

#include "dump-extensions.hpp"
#include "dump_manager.hpp"

namespace phosphor
{
namespace dump
{
/**
 * @brief load the faultLog dump extension
 *
 * @param[in] bus - Bus to attach to
 * @param[out] dumpMgrList - list dump manager objects.
 *
 */
void loadExtensionsFaultLog(sdbusplus::bus::bus& bus,
                            DumpManagerList& dumpMgrList);
} // namespace dump
} // namespace phosphor
