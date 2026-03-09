// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
// AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0

#include <getopt.h>
#include <linux/mctp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

enum
{
    NSM_PCI_VENDOR_LO = 0x10,
    NSM_PCI_VENDOR_HI = 0xDE,
    NSM_REQ_OCP_BYTE = 0x89,
    NSM_REQ_FLAG = 0x80,
    MCTP_MSG_TYPE_PCI_VDM = 0x7E,
    DEFAULT_TIMEOUT_MS = 5000,
};

static uint64_t monotonicMs()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now);
    if (ms.count() < 0)
    {
        return 0;
    }
    return static_cast<uint64_t>(ms.count());
}

static void usage(const char* argv0)
{
    fprintf(stderr,
            "Usage: %s --eid <n> --message-type <n> --command-code <n> "
            "--payload-hex <hex> [--instance <n>] [--timeout-ms <n>]\n",
            argv0);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

static int parseHexString(std::string_view in, std::vector<uint8_t>& out)
{
    std::string clean;
    clean.reserve(in.size());

    for (size_t i = 0; i < in.size(); ++i)
    {
        const char c = in[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == ':')
        {
            continue;
        }
        if ((c == 'x' || c == 'X') && i > 0 && in[i - 1] == '0')
        {
            continue;
        }
        clean.push_back(c);
    }

    if ((clean.size() % 2) != 0)
    {
        return -EINVAL;
    }
    if (clean.empty())
    {
        out.clear();
        return 0;
    }

    out.assign(clean.size() / 2, 0);
    for (size_t i = 0; i < out.size(); ++i)
    {
        const int hi = hexval(clean[i * 2]);
        const int lo = hexval(clean[i * 2 + 1]);
        if (hi < 0 || lo < 0)
        {
            out.clear();
            return -EINVAL;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return 0;
}

static int parseIntArg(const char* arg, int& out)
{
    if (arg == nullptr)
    {
        return -EINVAL;
    }

    const char* begin = arg;
    const char* end = arg + std::strlen(arg);
    int parsed = 0;
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc() || ptr != end)
    {
        return -EINVAL;
    }

    out = parsed;
    return 0;
}

static int sendRecvInKernelMctp(uint8_t eid, const std::vector<uint8_t>& req,
                                uint8_t instance, std::vector<uint8_t>& outBuf,
                                int timeoutMs)
{
    if (req.empty())
    {
        return -EINVAL;
    }

    const int fd = socket(AF_MCTP, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return -errno;
    }

    struct sockaddr_mctp addr
    {};
    addr.smctp_family = AF_MCTP;
    addr.smctp_network = MCTP_NET_ANY;
    addr.smctp_addr.s_addr = eid;
    addr.smctp_tag = MCTP_TAG_OWNER;
    addr.smctp_type = MCTP_MSG_TYPE_PCI_VDM;

    const ssize_t sent = sendto(
        fd, req.data(), req.size(), 0, reinterpret_cast<struct sockaddr*>(&addr),
        sizeof(addr));
    if (sent < 0 || static_cast<size_t>(sent) != req.size())
    {
        const int err = (sent < 0) ? errno : EIO;
        close(fd);
        return -err;
    }

    struct pollfd pfd
    {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    const uint64_t startMs = monotonicMs();
    const uint64_t deadlineMs =
        (startMs == 0) ? 0 : (startMs + static_cast<uint64_t>(timeoutMs));
    int nonmatchingPackets = 0;
    static constexpr int maxNonmatchingPackets = 256;

    for (;;)
    {
        int pollTimeout = timeoutMs;
        if (deadlineMs != 0)
        {
            const uint64_t nowMs = monotonicMs();
            if (nowMs == 0 || nowMs >= deadlineMs)
            {
                close(fd);
                return -ETIMEDOUT;
            }
            const uint64_t remaining = deadlineMs - nowMs;
            pollTimeout = (remaining > static_cast<uint64_t>(INT32_MAX))
                              ? INT32_MAX
                              : static_cast<int>(remaining);
        }

        const int prc = poll(&pfd, 1, pollTimeout);
        if (prc == 0)
        {
            close(fd);
            return -ETIMEDOUT;
        }
        if (prc < 0)
        {
            const int err = errno;
            close(fd);
            return -err;
        }

        const ssize_t pktLen = recv(fd, nullptr, 0, MSG_PEEK | MSG_TRUNC);
        if (pktLen <= 0)
        {
            close(fd);
            return -EIO;
        }
        if (pktLen < 9)
        {
            std::array<uint8_t, 256> trash{};
            const ssize_t toRead = pktLen < static_cast<ssize_t>(trash.size())
                                       ? pktLen
                                       : static_cast<ssize_t>(trash.size());
            (void)recv(fd, trash.data(), static_cast<size_t>(toRead), 0);
            if (++nonmatchingPackets > maxNonmatchingPackets)
            {
                close(fd);
                return -ETIMEDOUT;
            }
            continue;
        }

        std::vector<uint8_t> packet(static_cast<size_t>(pktLen));
        struct sockaddr_mctp src
        {};
        socklen_t srcLen = sizeof(src);
        const ssize_t got = recvfrom(
            fd, packet.data(), packet.size(), 0,
            reinterpret_cast<struct sockaddr*>(&src), &srcLen);
        if (got != pktLen)
        {
            continue;
        }

        // Ensure this is the expected response (same EID, same instance, response bit).
        if (src.smctp_addr.s_addr != eid || src.smctp_type != MCTP_MSG_TYPE_PCI_VDM)
        {
            if (++nonmatchingPackets > maxNonmatchingPackets)
            {
                close(fd);
                return -ETIMEDOUT;
            }
            continue;
        }

        const uint8_t hdrReqFlag = static_cast<uint8_t>(packet[2] & 0x80);
        const uint8_t hdrInstance = static_cast<uint8_t>(packet[2] & 0x1F);
        if (hdrReqFlag != 0 || hdrInstance != instance)
        {
            if (++nonmatchingPackets > maxNonmatchingPackets)
            {
                close(fd);
                return -ETIMEDOUT;
            }
            continue;
        }

        outBuf = std::move(packet);
        close(fd);
        return 0;
    }
}

int main(int argc, char** argv)
{
    int eid = -1;
    int instance = 0;
    int messageType = -1;
    int commandCode = -1;
    int timeoutMs = DEFAULT_TIMEOUT_MS;
    const char* payloadHexArg = nullptr;

    static struct option longOptions[] = {
        {"eid", required_argument, nullptr, 'e'},
        {"instance", required_argument, nullptr, 'i'},
        {"message-type", required_argument, nullptr, 'm'},
        {"command-code", required_argument, nullptr, 'c'},
        {"payload-hex", required_argument, nullptr, 'p'},
        {"timeout-ms", required_argument, nullptr, 't'},
        {nullptr, 0, nullptr, 0},
    };

    int opt = 0;
    while ((opt = getopt_long(argc, argv, "", longOptions, nullptr)) != -1)
    {
        switch (opt)
        {
            case 'e':
                if (parseIntArg(optarg, eid) < 0)
                {
                    usage(argv[0]);
                    return 2;
                }
                break;
            case 'i':
                if (parseIntArg(optarg, instance) < 0)
                {
                    usage(argv[0]);
                    return 2;
                }
                break;
            case 'm':
                if (parseIntArg(optarg, messageType) < 0)
                {
                    usage(argv[0]);
                    return 2;
                }
                break;
            case 'c':
                if (parseIntArg(optarg, commandCode) < 0)
                {
                    usage(argv[0]);
                    return 2;
                }
                break;
            case 'p':
                payloadHexArg = optarg;
                break;
            case 't':
                if (parseIntArg(optarg, timeoutMs) < 0)
                {
                    usage(argv[0]);
                    return 2;
                }
                break;
            default:
                usage(argv[0]);
                return 2;
        }
    }

    if (eid < 0 || eid > 255 || instance < 0 || instance > 31 || messageType < 0 ||
        messageType > 255 || commandCode < 0 || commandCode > 255 ||
        payloadHexArg == nullptr)
    {
        usage(argv[0]);
        return 2;
    }

    std::vector<uint8_t> payload;
    const int parseRc = parseHexString(payloadHexArg, payload);
    if (parseRc < 0 || payload.size() > 255)
    {
        fprintf(stderr, "Invalid --payload-hex\n");
        return 2;
    }

    std::vector<uint8_t> req(7 + payload.size(), 0);
    req[0] = NSM_PCI_VENDOR_LO;
    req[1] = NSM_PCI_VENDOR_HI;
    req[2] = static_cast<uint8_t>(NSM_REQ_FLAG | (instance & 0x1F));
    req[3] = NSM_REQ_OCP_BYTE;
    req[4] = static_cast<uint8_t>(messageType);
    req[5] = static_cast<uint8_t>(commandCode);
    req[6] = static_cast<uint8_t>(payload.size());
    if (!payload.empty())
    {
        std::copy(payload.begin(), payload.end(), req.begin() + 7);
    }

    std::vector<uint8_t> resp;
    const int rc = sendRecvInKernelMctp(static_cast<uint8_t>(eid), req,
                                        static_cast<uint8_t>(instance), resp,
                                        timeoutMs);
    if (rc < 0)
    {
        fprintf(stderr, "AF_MCTP transport failed for EID %d: %s\n", eid,
                strerror(-rc));
        return 1;
    }

    if (resp.size() < 9)
    {
        fprintf(stderr, "Invalid response length\n");
        return 1;
    }

    // NSM payload starts after 5-byte header.
    const uint8_t* payloadStart = resp.data() + 5;
    const size_t payloadLen = resp.size() - 5;
    const uint8_t cc = payloadStart[1];
    if (cc == 0)
    {
        // Match nsmd raw helper output format:
        // [cc][reserved(2)][data_size(2)][response_payload...]
        uint8_t outCc = 0;
        if (write(STDOUT_FILENO, &outCc, 1) != 1)
        {
            return 1;
        }
        if (write(STDOUT_FILENO, payloadStart + 2, payloadLen - 2) !=
            static_cast<ssize_t>(payloadLen - 2))
        {
            return 1;
        }
        return 0;
    }

    // Non-success: output [cc][reason_lo][reason_hi]
    std::array<uint8_t, 3> out{cc, 0, 0};
    if (payloadLen >= 4)
    {
        out[1] = payloadStart[2];
        out[2] = payloadStart[3];
    }
    if (write(STDOUT_FILENO, out.data(), out.size()) !=
        static_cast<ssize_t>(out.size()))
    {
        return 1;
    }
    return 0;
}
