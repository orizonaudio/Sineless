#include "EmbeddedAssets.h"

#include <algorithm>

namespace orizon::editor {

const EmbeddedAsset* findEmbeddedAsset(std::string_view path)
{
    const auto assets = embeddedAssets();
    const auto found = std::ranges::find(assets, path, &EmbeddedAsset::path);
    return found == assets.end() ? nullptr : &*found;
}

} // namespace orizon::editor
