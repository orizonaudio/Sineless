#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace orizon::editor {

// One file of the built UI, compiled into the binary (AD-11).
struct EmbeddedAsset
{
    std::string_view path; // Relative to the UI root, e.g. "index.html".
    std::string_view mimeType;
    const unsigned char* data;
    std::size_t size;
};

// Defined in the generated EmbeddedAssetData.cpp (cmake/EmbedAssets.cmake).
std::span<const EmbeddedAsset> embeddedAssets();

const EmbeddedAsset* findEmbeddedAsset(std::string_view path);

} // namespace orizon::editor
