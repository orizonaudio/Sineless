#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace orizon::editor {

// One file of the built UI, compiled into the binary.
struct EmbeddedAsset
{
    std::string_view path; // Relative to the UI root, e.g. "index.html".
    std::string_view mimeType;
    const unsigned char* data;
    std::size_t size;
};

// What the page may load, sent with every file (only the page's own response applies it): its own
// scripts, styles, fonts and frames from orizon://ui, and its data: icon; nothing else, and no
// inline script or style. Listing orizon://ui beside 'self' keeps the page working should its
// origin ever become opaque, where 'self' matches nothing. Native code still runs script in the
// page (injected, not loaded), and messages to and from it are not requests, so neither is
// affected.
inline constexpr std::string_view kContentSecurityPolicy =
    "default-src 'none'; script-src 'self' orizon://ui; style-src 'self' orizon://ui; "
    "font-src 'self' orizon://ui; img-src data:; connect-src 'self' orizon://ui; "
    "base-uri 'none'; form-action 'none'; object-src 'none'; frame-ancestors 'none'";

// Defined in the generated EmbeddedAssetData.cpp (cmake/EmbedAssets.cmake).
std::span<const EmbeddedAsset> embeddedAssets();

const EmbeddedAsset* findEmbeddedAsset(std::string_view path);

} // namespace orizon::editor
