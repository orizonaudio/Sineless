#include "engine/Version.h"

#include <gtest/gtest.h>
#include <string>

namespace orizon::engine {
namespace {

TEST(Version, LinkedVersionMatchesHeader)
{
    EXPECT_EQ(linkedVersion(), std::string_view(kVersionString));
}

TEST(Version, StringIsMajorMinorPatch)
{
    const std::string expected = std::to_string(kVersionMajor) + "." +
                                 std::to_string(kVersionMinor) + "." +
                                 std::to_string(kVersionPatch);
    EXPECT_EQ(kVersionString, expected);
}

} // namespace
} // namespace orizon::engine
