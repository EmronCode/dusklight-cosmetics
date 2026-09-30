#pragma once

#include "mod.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

// Decoded HD texture data in tightly packed RGBA8 format.
struct HdTexture {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};

// Find the matching HD replacement file for a Dusklight texture key.
// Returns no value if a matching replacement cannot be found.
std::optional<std::filesystem::path>
find_hd_texture_path(const TextureKey& key);

// Load and decode the matching HD replacement texture into RGBA8.
// Returns no value if the texture cannot be found or decoded.
std::optional<HdTexture>
load_hd_texture(const TextureKey& key);

// Recolor the decoded HD texture in place using the selected Cosmetics color.
void recolor_hd_texture(
    HdTexture& texture,
    GXColor color);