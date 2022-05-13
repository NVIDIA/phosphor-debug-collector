#pragma once

#include <string>

namespace phosphor
{
namespace dump
{
namespace create
{

using namespace std;

/** socket buffer size */
constexpr auto BUFFER_SIZE = 255;

/** file watcher timeouts after 5 minutes */
constexpr auto TIMEOUT = 1000 * 60 * 5;

/** path of domain socket */
constexpr auto SOCKET_PATH = "/tmp/dump_sock.socket";

/** path where phosphor-dbug-collector keeps dumps */
constexpr auto DUMPS_PATH = "/var/lib/logging/dumps/";

/** command by which the client asks for a dump */
constexpr string_view CREATE_DUMP_CMD = "CREATE_DUMP";

/** command responded by server ends communication */
constexpr string_view END_CMD = "END";

/** @class CreateDumpDbus
 *  @brief calls CreateDump dbus method
 */
class CreateDumpDbus
{
  public:
    ~CreateDumpDbus();

    /** @brief calls CreateDump method on dbus */
    void doCreateDumpCall();

    /** @brief launches create-dump-dbus server that waits for request */
    void launchServer();

  private:
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
    static int copyDumpToTmpDir(string dPath, string& response);

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
    static void sendMsg(int fd, string msg);

    /** @brief calls the CreateDump method on dbus
     *  @param [in] response - response message from dbus (or error message)
     *
     *  @return on success 0, on failure -1
     */
    static int createDump(string& response);

    /** @brief socket descriptors */
    int fd = -1;
    int dataSocket = -1;
};

} // namespace create
} // namespace dump
} // namespace phosphor
