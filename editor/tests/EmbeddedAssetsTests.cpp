#include "EmbeddedAssets.h"

#include <gtest/gtest.h>

namespace orizon::editor {
namespace {

TEST(EmbeddedAssets, ContainsTheUiEntryPoint)
{
    const EmbeddedAsset* index = findEmbeddedAsset("index.html");
    ASSERT_NE(index, nullptr);
    EXPECT_EQ(index->mimeType, "text/html; charset=utf-8");
    EXPECT_GT(index->size, 0u);
}

TEST(EmbeddedAssets, ServesScriptsWithJavaScriptMimeType)
{
    bool foundScript = false;
    for (const EmbeddedAsset& asset : embeddedAssets()) {
        if (asset.path.ends_with(".js")) {
            foundScript = true;
            EXPECT_EQ(asset.mimeType, "text/javascript; charset=utf-8") << asset.path;
        }
    }
    EXPECT_TRUE(foundScript);
}

TEST(EmbeddedAssets, ReturnsNullForUnknownPath)
{
    EXPECT_EQ(findEmbeddedAsset("missing.txt"), nullptr);
}

} // namespace
} // namespace orizon::editor
