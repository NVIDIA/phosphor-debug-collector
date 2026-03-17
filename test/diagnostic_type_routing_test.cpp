#include "../dump-extensions/nvidia-dumps/diagnostic_type.hpp"

#include <gtest/gtest.h>

namespace phosphor
{
namespace dump
{
namespace system
{

TEST(DiagnosticTypeRouting, RotAndErotMapToDistinctCollectors)
{
    EXPECT_EQ(getDiagnosticType("ROT"), DiagnosticType::ROT);
    EXPECT_EQ(getDiagnosticType("EROT"), DiagnosticType::EROT);

    ASSERT_NE(getDiagnosticDumpBinaryPath(DiagnosticType::ROT), nullptr);
    ASSERT_NE(getDiagnosticDumpBinaryPath(DiagnosticType::EROT), nullptr);
    EXPECT_STREQ(getDiagnosticDumpBinaryPath(DiagnosticType::ROT),
                 ROT_DUMP_BIN_PATH);
    EXPECT_STREQ(getDiagnosticDumpBinaryPath(DiagnosticType::EROT),
                 EROT_DUMP_BIN_PATH);
    EXPECT_STRNE(getDiagnosticDumpBinaryPath(DiagnosticType::ROT),
                 getDiagnosticDumpBinaryPath(DiagnosticType::EROT));
}

} // namespace system
} // namespace dump
} // namespace phosphor
