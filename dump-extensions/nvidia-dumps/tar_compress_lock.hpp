/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Serialize memory-intensive compression across dump collectors while
 * allowing their data-collection phases to run in parallel.
 */
#pragma once

#include <fcntl.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace phosphor::dump::compression
{

inline constexpr auto lockPath = "/run/phosphor-dump-tar.compress.lock";
inline constexpr auto defaultLockWait = std::chrono::minutes(15);
inline constexpr auto lockRetryInterval = std::chrono::milliseconds(50);
inline constexpr int lockFailure = 125;
inline constexpr int commandNotExecutable = 126;
inline constexpr int commandNotFound = 127;

/** Run a command while holding the global dump compression lock.
 *
 * Lock failures are fail-closed: the command is never run without the lock.
 */
inline int runShellWithLock(
    const char* path, std::vector<std::string> command,
    std::chrono::milliseconds lockWait = defaultLockWait)
{
    if (command.empty())
    {
        return lockFailure;
    }

    int fd = open(path, O_CREAT | O_CLOEXEC | O_RDWR, 0644);
    if (fd < 0)
    {
        return lockFailure;
    }

    const auto deadline = std::chrono::steady_clock::now() + lockWait;
    bool waitingLogged = false;
    while (flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
        if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)
        {
            close(fd);
            return lockFailure;
        }
        if (!waitingLogged)
        {
            const auto waitSeconds =
                std::chrono::ceil<std::chrono::seconds>(lockWait).count();
            std::fprintf(stderr,
                         "Waiting up to %lld seconds for dump compression "
                         "lock %s\n",
                         static_cast<long long>(waitSeconds), path);
            waitingLogged = true;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            std::fprintf(stderr,
                         "Timed out waiting for dump compression lock %s\n",
                         path);
            close(fd);
            return lockFailure;
        }
        std::this_thread::sleep_for(lockRetryInterval);
    }

    std::vector<char*> argv;
    argv.reserve(command.size() + 1);
    for (std::string& argument : command)
    {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    const pid_t parent = getpid();
    pid_t child = fork();
    if (child < 0)
    {
        close(fd);
        return lockFailure;
    }
    if (child == 0)
    {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent)
        {
            _exit(lockFailure);
        }
        execvp(argv.front(), argv.data());
        const int error = errno;
        std::fprintf(stderr, "Failed to execute %s: %s\n", argv.front(),
                     std::strerror(error));
        _exit(error == ENOENT ? commandNotFound : commandNotExecutable);
    }

    int status = 0;
    pid_t waitResult = 0;
    do
    {
        waitResult = waitpid(child, &status, 0);
    } while (waitResult < 0 && errno == EINTR);

    close(fd);
    if (waitResult != child || !WIFEXITED(status))
    {
        return lockFailure;
    }
    return WEXITSTATUS(status);
}

} // namespace phosphor::dump::compression
