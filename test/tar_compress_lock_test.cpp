// SPDX-License-Identifier: Apache-2.0

#include "dump-extensions/nvidia-dumps/tar_compress_lock.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace
{

class TarCompressLockTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        char directoryTemplate[] = "/tmp/tar-compress-lock.XXXXXX";
        char* directory = mkdtemp(std::data(directoryTemplate));
        ASSERT_NE(directory, nullptr);
        testDirectory = directory;
        testLockPath = testDirectory / "compress.lock";
        markerPath = testDirectory / "command-ran";
        command = {"touch", markerPath.string()};
    }

    void TearDown() override
    {
        std::filesystem::remove_all(testDirectory);
    }

    std::filesystem::path testDirectory;
    std::filesystem::path testLockPath;
    std::filesystem::path markerPath;
    std::vector<std::string> command;
};

TEST_F(TarCompressLockTest, WaitsForExistingLock)
{
    int lockFd = open(testLockPath.c_str(), O_CREAT | O_RDWR, 0644);
    ASSERT_GE(lockFd, 0);
    ASSERT_EQ(flock(lockFd, LOCK_EX), 0);

    pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        close(lockFd);
        int result = phosphor::dump::compression::runShellWithLock(
            testLockPath.c_str(), command, std::chrono::seconds(2));
        _exit(result == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_FALSE(std::filesystem::exists(markerPath));

    ASSERT_EQ(flock(lockFd, LOCK_UN), 0);
    close(lockFd);

    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), EXIT_SUCCESS);
    EXPECT_TRUE(std::filesystem::exists(markerPath));
}

TEST_F(TarCompressLockTest, TimeoutDoesNotRunCommandUnlocked)
{
    int lockFd = open(testLockPath.c_str(), O_CREAT | O_RDWR, 0644);
    ASSERT_GE(lockFd, 0);
    ASSERT_EQ(flock(lockFd, LOCK_EX), 0);

    int result = phosphor::dump::compression::runShellWithLock(
        testLockPath.c_str(), command, std::chrono::milliseconds(100));

    EXPECT_EQ(result, phosphor::dump::compression::lockFailure);
    EXPECT_FALSE(std::filesystem::exists(markerPath));
    ASSERT_EQ(flock(lockFd, LOCK_UN), 0);
    close(lockFd);
}

TEST_F(TarCompressLockTest, OpenFailureDoesNotRunCommandUnlocked)
{
    const auto invalidLockPath = testDirectory / "missing" / "compress.lock";
    int result = phosphor::dump::compression::runShellWithLock(
        invalidLockPath.c_str(), command, std::chrono::milliseconds(100));

    EXPECT_EQ(result, phosphor::dump::compression::lockFailure);
    EXPECT_FALSE(std::filesystem::exists(markerPath));
}

TEST_F(TarCompressLockTest, PropagatesCommandExitCode)
{
    std::vector<std::string> failingCommand = {"sh", "-c", "exit 3"};
    int result = phosphor::dump::compression::runShellWithLock(
        testLockPath.c_str(), std::move(failingCommand));

    EXPECT_EQ(result, 3);
}

TEST_F(TarCompressLockTest, ReturnsCommandNotFoundForMissingExecutable)
{
    std::vector<std::string> missingCommand = {
        "/nonexistent/tar-compress-lock-test"};
    int result = phosphor::dump::compression::runShellWithLock(
        testLockPath.c_str(), std::move(missingCommand));

    EXPECT_EQ(result, phosphor::dump::compression::commandNotFound);
}

TEST_F(TarCompressLockTest, TerminatesCommandWhenParentDies)
{
    const auto startedPath = testDirectory / "command-started";
    std::vector<std::string> delayedCommand = {
        "sh", "-c",
        "touch '" + startedPath.string() + "'; sleep 1; touch '" +
            markerPath.string() + "'"};

    pid_t parent = fork();
    ASSERT_GE(parent, 0);
    if (parent == 0)
    {
        int result = phosphor::dump::compression::runShellWithLock(
            testLockPath.c_str(), std::move(delayedCommand));
        _exit(result);
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!std::filesystem::exists(startedPath) &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(std::filesystem::exists(startedPath));

    ASSERT_EQ(kill(parent, SIGKILL), 0);
    int status = 0;
    ASSERT_EQ(waitpid(parent, &status, 0), parent);
    ASSERT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGKILL);

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    EXPECT_FALSE(std::filesystem::exists(markerPath));
}

} // namespace
