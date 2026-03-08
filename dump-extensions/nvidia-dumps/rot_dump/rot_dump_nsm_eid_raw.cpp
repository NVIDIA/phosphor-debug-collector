// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
// AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <getopt.h>
#include <linux/mctp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

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
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
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

static int parse_hex_string(const char* in, uint8_t** out, size_t* out_len)
{
    if (!in || !out || !out_len)
    {
        return -EINVAL;
    }

    size_t len = strlen(in);
    char* clean = (char*)calloc(len + 1, 1);
    if (!clean)
    {
        return -ENOMEM;
    }

    size_t ci = 0;
    for (size_t i = 0; i < len; ++i)
    {
        if (in[i] == ' ' || in[i] == '\t' || in[i] == '\n' || in[i] == ':')
        {
            continue;
        }
        if ((in[i] == 'x' || in[i] == 'X') && i > 0 && in[i - 1] == '0')
        {
            continue;
        }
        clean[ci++] = in[i];
    }
    clean[ci] = '\0';

    if ((ci % 2) != 0)
    {
        free(clean);
        return -EINVAL;
    }

    if (ci == 0)
    {
        free(clean);
        *out = NULL;
        *out_len = 0;
        return 0;
    }

    size_t bytes = ci / 2;
    uint8_t* buf = (uint8_t*)calloc(bytes, 1);
    if (!buf)
    {
        free(clean);
        return -ENOMEM;
    }

    for (size_t i = 0; i < bytes; ++i)
    {
        int hi = hexval(clean[i * 2]);
        int lo = hexval(clean[i * 2 + 1]);
        if (hi < 0 || lo < 0)
        {
            free(clean);
            free(buf);
            return -EINVAL;
        }
        buf[i] = (uint8_t)((hi << 4) | lo);
    }

    free(clean);
    *out = buf;
    *out_len = bytes;
    return 0;
}

static int send_recv_in_kernel_mctp(
    uint8_t eid, const uint8_t* req, size_t req_len, uint8_t instance,
    uint8_t** out_buf, size_t* out_len, int timeout_ms)
{
    if (!req || req_len == 0 || !out_buf || !out_len)
    {
        return -EINVAL;
    }

    int fd = socket(AF_MCTP, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return -errno;
    }

    struct sockaddr_mctp addr;
    memset(&addr, 0, sizeof(addr));
    addr.smctp_family = AF_MCTP;
    addr.smctp_network = MCTP_NET_ANY;
    addr.smctp_addr.s_addr = eid;
    addr.smctp_tag = MCTP_TAG_OWNER;
    addr.smctp_type = MCTP_MSG_TYPE_PCI_VDM;

    ssize_t sent =
        sendto(fd, req, req_len, 0, (struct sockaddr*)&addr, sizeof(addr));
    if (sent < 0 || (size_t)sent != req_len)
    {
        int err = (sent < 0) ? errno : EIO;
        close(fd);
        return -err;
    }

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    const uint64_t start_ms = monotonicMs();
    const uint64_t deadline_ms =
        (start_ms == 0) ? 0 : (start_ms + (uint64_t)timeout_ms);
    int nonmatching_packets = 0;
    const int max_nonmatching_packets = 256;

    while (1)
    {
        int poll_timeout = timeout_ms;
        if (deadline_ms != 0)
        {
            uint64_t now_ms = monotonicMs();
            if (now_ms == 0 || now_ms >= deadline_ms)
            {
                close(fd);
                return -ETIMEDOUT;
            }
            uint64_t remaining = deadline_ms - now_ms;
            poll_timeout =
                (remaining > (uint64_t)INT32_MAX) ? INT32_MAX : (int)remaining;
        }

        int prc = poll(&pfd, 1, poll_timeout);
        if (prc == 0)
        {
            close(fd);
            return -ETIMEDOUT;
        }
        if (prc < 0)
        {
            int err = errno;
            close(fd);
            return -err;
        }

        ssize_t pkt_len = recv(fd, NULL, 0, MSG_PEEK | MSG_TRUNC);
        if (pkt_len <= 0)
        {
            close(fd);
            return -EIO;
        }
        if (pkt_len < 5 + 4)
        {
            uint8_t trash[256];
            ssize_t to_read = pkt_len < (ssize_t)sizeof(trash)
                                  ? pkt_len
                                  : (ssize_t)sizeof(trash);
            (void)recv(fd, trash, (size_t)to_read, 0);
            if (++nonmatching_packets > max_nonmatching_packets)
            {
                close(fd);
                return -ETIMEDOUT;
            }
            continue;
        }

        uint8_t* pkt = (uint8_t*)calloc((size_t)pkt_len, 1);
        if (!pkt)
        {
            close(fd);
            return -ENOMEM;
        }

        struct sockaddr_mctp src;
        socklen_t src_len = sizeof(src);
        ssize_t got = recvfrom(fd, pkt, (size_t)pkt_len, 0,
                               (struct sockaddr*)&src, &src_len);
        if (got != pkt_len)
        {
            free(pkt);
            continue;
        }

        // Ensure this is the expected response (same EID, same instance,
        // response bit).
        if (src.smctp_addr.s_addr != eid ||
            src.smctp_type != MCTP_MSG_TYPE_PCI_VDM)
        {
            free(pkt);
            if (++nonmatching_packets > max_nonmatching_packets)
            {
                close(fd);
                return -ETIMEDOUT;
            }
            continue;
        }

        uint8_t hdr_req_flag = (uint8_t)(pkt[2] & 0x80);
        uint8_t hdr_instance = (uint8_t)(pkt[2] & 0x1F);
        if (hdr_req_flag != 0 || hdr_instance != instance)
        {
            free(pkt);
            if (++nonmatching_packets > max_nonmatching_packets)
            {
                close(fd);
                return -ETIMEDOUT;
            }
            continue;
        }

        *out_buf = pkt;
        *out_len = (size_t)pkt_len;
        close(fd);
        return 0;
    }
}

int main(int argc, char** argv)
{
    int eid = -1;
    int instance = 0;
    int message_type = -1;
    int command_code = -1;
    int timeout_ms = DEFAULT_TIMEOUT_MS;
    const char* payload_hex = NULL;

    static struct option long_options[] = {
        {"eid", required_argument, 0, 'e'},
        {"instance", required_argument, 0, 'i'},
        {"message-type", required_argument, 0, 'm'},
        {"command-code", required_argument, 0, 'c'},
        {"payload-hex", required_argument, 0, 'p'},
        {"timeout-ms", required_argument, 0, 't'},
        {0, 0, 0, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "", long_options, NULL)) != -1)
    {
        switch (opt)
        {
            case 'e':
                eid = atoi(optarg);
                break;
            case 'i':
                instance = atoi(optarg);
                break;
            case 'm':
                message_type = atoi(optarg);
                break;
            case 'c':
                command_code = atoi(optarg);
                break;
            case 'p':
                payload_hex = optarg;
                break;
            case 't':
                timeout_ms = atoi(optarg);
                break;
            default:
                usage(argv[0]);
                return 2;
        }
    }

    if (eid < 0 || eid > 255 || instance < 0 || instance > 31 ||
        message_type < 0 || message_type > 255 || command_code < 0 ||
        command_code > 255 || payload_hex == NULL)
    {
        usage(argv[0]);
        return 2;
    }

    uint8_t* payload = NULL;
    size_t payload_len = 0;
    int rc = parse_hex_string(payload_hex, &payload, &payload_len);
    if (rc < 0 || payload_len > 255)
    {
        fprintf(stderr, "Invalid --payload-hex\n");
        free(payload);
        return 2;
    }

    size_t req_len = 5 + 2 + payload_len;
    uint8_t* req = (uint8_t*)calloc(req_len, 1);
    if (!req)
    {
        free(payload);
        return 2;
    }

    req[0] = NSM_PCI_VENDOR_LO;
    req[1] = NSM_PCI_VENDOR_HI;
    req[2] = (uint8_t)(NSM_REQ_FLAG | (instance & 0x1F));
    req[3] = NSM_REQ_OCP_BYTE;
    req[4] = (uint8_t)message_type;
    req[5] = (uint8_t)command_code;
    req[6] = (uint8_t)payload_len;
    if (payload_len > 0)
    {
        memcpy(req + 7, payload, payload_len);
    }

    uint8_t* resp = NULL;
    size_t resp_len = 0;
    rc = send_recv_in_kernel_mctp((uint8_t)eid, req, req_len, (uint8_t)instance,
                                  &resp, &resp_len, timeout_ms);
    if (rc < 0)
    {
        fprintf(stderr, "AF_MCTP transport failed for EID %d: %s\n", eid,
                strerror(-rc));
        free(payload);
        free(req);
        return 1;
    }

    free(payload);
    free(req);

    if (resp_len < 9)
    {
        fprintf(stderr, "Invalid response length\n");
        free(resp);
        return 1;
    }

    // NSM payload starts after 5-byte header.
    uint8_t* p = resp + 5;
    size_t p_len = resp_len - 5;
    uint8_t cc = p[1];
    if (cc == 0)
    {
        // Match nsmd raw helper output format:
        // [cc][reserved(2)][data_size(2)][response_payload...]
        uint8_t out_cc = 0;
        if (write(STDOUT_FILENO, &out_cc, 1) != 1)
        {
            free(resp);
            return 1;
        }
        if (write(STDOUT_FILENO, p + 2, p_len - 2) != (ssize_t)(p_len - 2))
        {
            free(resp);
            return 1;
        }
        free(resp);
        return 0;
    }

    // Non-success: output [cc][reason_lo][reason_hi]
    uint8_t out[3] = {cc, 0, 0};
    if (p_len >= 4)
    {
        out[1] = p[2];
        out[2] = p[3];
    }
    if (write(STDOUT_FILENO, out, sizeof(out)) != (ssize_t)sizeof(out))
    {
        free(resp);
        return 1;
    }
    free(resp);
    return 0;
}
