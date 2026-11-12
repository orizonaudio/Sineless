#include "Branding.h"
#include "EmbeddedAssets.h"

#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <string>

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

// The page compiled in follows the brand switch (cmake/Branding.cmake): only a branded build
// carries the logo, which has terms of its own, and the product's names.
TEST(EmbeddedAssets, CarryTheBrandOnlyInABrandedBuild)
{
    std::ifstream file(ORIZON_TEST_BRAND_MARK);
    const std::string svg{std::istreambuf_iterator<char>(file), {}};
    const std::size_t start = svg.find(" d=\"");
    ASSERT_NE(start, std::string::npos) << ORIZON_TEST_BRAND_MARK;
    const std::string mark = svg.substr(start + 4, 40); // The path data of its first shape.
    std::string page;
    for (const EmbeddedAsset& asset : embeddedAssets()) {
        page.append(reinterpret_cast<const char*>(asset.data), asset.size);
    }
    const bool branded = SINELESS_BRANDED == 1;
    EXPECT_EQ(page.find(mark) != std::string::npos, branded) << "the logo";
    EXPECT_EQ(page.find("Sineless") != std::string::npos, branded) << "the product's name";
    if (!branded) {
        EXPECT_EQ(page.find("Orizon"), std::string::npos) << "the vendor's name";
        EXPECT_EQ(page.find("orizonaudio"), std::string::npos) << "the vendor's domain";
    }
}

// About Us takes the user to the vendor's site at its home page (FR-13.1), not to a page within
// it: a secure address whose path is the site's root.
TEST(Branding, AboutUsOpensTheVendorsSiteAtItsHomePage)
{
    const std::string url = branding::kContactUrl;
    const bool branded = SINELESS_BRANDED == 1;
    if (!branded) {
        EXPECT_TRUE(url.empty()) << "a build without the brand has no vendor's site";
        return;
    }
    const std::string scheme = "https://";
    ASSERT_TRUE(url.starts_with(scheme)) << url;
    const auto path = url.find('/', scheme.size());
    ASSERT_NE(path, std::string::npos) << url;
    EXPECT_GT(path, scheme.size()) << "no host: " << url;
    EXPECT_EQ(url.substr(path), "/") << "a page within the site: " << url;
}

TEST(EmbeddedAssets, ReturnsNullForUnknownPath)
{
    EXPECT_EQ(findEmbeddedAsset("missing.txt"), nullptr);
}

} // namespace
} // namespace orizon::editor
