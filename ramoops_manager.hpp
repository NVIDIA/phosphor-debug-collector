#pragma once

#include "config.h"

#include "dump_manager.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace phosphor
{
namespace dump
{
namespace ramoops
{

/** @class Manager
 *  @brief OpenBMC Core manager implementation.
 */
class Manager
{
  public:
    Manager() = delete;
    Manager(const Manager&) = default;
    Manager& operator=(const Manager&) = delete;
    Manager(Manager&&) = delete;
    Manager& operator=(Manager&&) = delete;
    virtual ~Manager() = default;

    /** @brief Constructor to create ramoops
     *  @param[in] filePath - Path where the ramoops are stored.
     */
    Manager(const std::string& filePath);

    /** @brief Build the CreateDump parameters for a ramoops dump request.
     *  @param [in] files - ramoops files list
     *  @return the parameters to pass to the dump manager's CreateDump
     *          D-Bus method
     */
    static phosphor::dump::DumpCreateParams createDumpParams(
        const std::vector<std::string>& files);

  private:
    /** @brief Helper function for initiating dump request using
     *         createDump D-Bus interface.
     *  @param [in] files - ramoops files list
     */
    void createHelper(const std::vector<std::string>& files);

    /** @brief Create an error indicating ramoops was found
     *
     */
    void createError();
};

} // namespace ramoops
} // namespace dump
} // namespace phosphor
