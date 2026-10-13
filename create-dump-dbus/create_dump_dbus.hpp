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

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace phosphor
{
namespace dump
{
namespace create
{

/** socket buffer size */
constexpr auto BUFFER_SIZE = 255;

/** file watcher timeouts after 5 minutes */
constexpr auto TIMEOUT = 1000 * 60 * 5;

/** path of domain socket */
constexpr auto SOCKET_PATH = "/tmp/dump_sock.socket";

/** tmp directory path */
constexpr std::string_view TMP_DIR_PATH = "/tmp/";

/** dump file copy prefix */
constexpr std::string_view DUMP_COPY_PREFIX = "copy_";

/** command by which the client asks for a dump */
constexpr std::string_view CREATE_DUMP_CMD = "CREATE_DUMP";

/** command responded by server ends communication */
constexpr std::string_view END_CMD = "END";

/** end of communication when the request failed */
constexpr std::string_view END_ERROR_CMD = "END_ERROR";

/** command by which the client asks for the supported dump types */
constexpr std::string_view LIST_TYPES_CMD = "LIST_DUMP_TYPES";

/** dump types handled by the tool itself, not read from the dump manager */
constexpr std::string_view ALL_DUMP_TYPE = "all";
constexpr std::string_view BMC_DUMP_TYPE = "BMC";

/** default dump type used when no type is specified by the client / user */
constexpr std::string_view DEFAULT_DUMP_TYPE = BMC_DUMP_TYPE;

/** system dump types used when the dump manager doesn't publish them */
const std::vector<std::string> LEGACY_SYSTEM_DUMP_TYPES{
    "DiagnosticType=EROT", "DiagnosticType=FPGA", "DiagnosticType=SelfTest"};

/**
 * @brief handles error message in CreateDumpDbus
 *
 */
class CreateDumpDbusException : std::exception
{
  public:
    CreateDumpDbusException(std::string error) : errorMsg(error) {}

    /**
     * @brief
     *
     * @return error message
     */
    const char* what() const noexcept override
    {
        return this->errorMsg.c_str();
    }

  protected:
    std::string errorMsg;
};

/** @class CreateDumpDbus
 *  @brief calls CreateDump dbus method
 */
// NOLINTBEGIN
class CreateDumpDbus
{
  public:
    ~CreateDumpDbus();

    /** @brief calls CreateDump method on dbus
     *
     *  @return EXIT_SUCCESS, or EXIT_FAILURE if the request failed
     */
    int doCreateDumpCall(const std::string& type);

    /** @brief asks the server for the dump types supported on this platform
     *
     *  @return EXIT_SUCCESS, or EXIT_FAILURE if the request failed
     */
    int doListTypesCall();

    /** @brief launches create-dump-dbus server that waits for request */
    void launchServer();

    /** @brief path to which debug collector saves BMC dump files */
    static std::string bmcDumpPath;

    /** @brief path to which debug collector saves system dump files */
    static std::string systemDumpPath;

  private:
    /** @brief sends a command to the server and prints its responses
     *
     *  @return EXIT_SUCCESS, or EXIT_FAILURE if the server reported an error
     */
    int sendCommand(const std::string& command);

    /** @brief reads the system dump types the dump manager advertises (the
     *         Redfish OEMDiagnosticDataType AllowableValues)
     *  @param [in] fd - file descriptor of the socket, for error messages
     *
     *  @return "DiagnosticType=<type>[;<key>=<value>...]" strings,
     *          LEGACY_SYSTEM_DUMP_TYPES if the list isn't published, or
     *          nullopt if it can't be read
     */
    static std::optional<std::vector<std::string>> getSystemDumpTypes(int fd);

    /** @brief maps a requested system dump type to a supported one
     *  @param [in] requested - "<type>[;<key>=<value>...]" or the same with
     *                          a "DiagnosticType=" prefix
     *  @param [in] systemTypes - supported system dump types
     *
     *  @return the matching supported type, or empty if not supported
     */
    static std::string resolveDumpType(
        const std::string& requested,
        const std::vector<std::string>& systemTypes);

    /** @brief sends the supported dump types to the client
     *  @param [in] fd - file descriptor of the socket
     *
     *  @return true on success
     */
    static bool processListRequest(int fd);

    /** @brief closes connection and free resources */
    void dispose();

    /** @brief after request to dbus is sent, function creates simple
     *         file watcher that waits for dump file, then copies it to
     *         tmp directory
     *  @param [in] dPath - path of created dump in dbus tree
     *  @param [in] response - response message
     *
     *  @return on success 0, on failure -1
     */
    static int copyDumpToTmpDir(const std::string& dPath,
                                std::string& response);

    /** @brief creates domain socket to allow communication between server and
     *         client
     */
    void createSocket();

    /** @brief wrapper that sends message to interlocutor
     *
     *  @param [in] fd - socket descriptor
     *  @param [in] msg - message
     *
     *  @throws std::exception on failure
     **/
    static void sendMsg(int fd, const std::string& msg);

    /**
     * @brief check dump creation status by checking dump entry progress
     * interface. Method is blocking.
     *
     * @param [in] entryPath - path of entry's dbus object
     */
    static void waitForDumpCreation(const std::string& entryPath);

    /** @brief calls the CreateDump method on dbus
     *  @param [in] response - response message from dbus (or error message)
     *  @param [in] type - "BMC" or a system dump type from getSystemDumpTypes
     *
     *  @return on success 0, on failure -1
     */
    static int createDump(const std::string& type, std::string& response);

    /** @brief creates dump and copies it to target location
     *  @param [in] fd - file descriptor of the socket
     *  @param [in] type - dump type
     *
     *  @return true if the dump was created and copied
     */
    static bool processSingleDump(int fd, const std::string& type);

    /** @brief clears previously created dumps and processes the requested ones
     *  @param [in] fd - file descriptor of the socket
     *  @param [in] type - dump type
     *
     *  @return true if the type is supported and every dump succeeded
     */
    static bool processDumpRequest(int fd, const std::string& type);

    /** @brief socket descriptors */
    int fd = -1;
    int dataSocket = -1;
};
// NOLINTEND

} // namespace create
} // namespace dump
} // namespace phosphor
