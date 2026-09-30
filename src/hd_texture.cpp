#define DDSKTX_IMPLEMENT
#include "../third_party/dds-ktx.h"

#define BCDEC_IMPLEMENTATION
#include "../third_party/bcdec.h"

#include "mods/svc/log.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>

#include "hd_texture.hpp"

#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// Decode a BC-compressed DDS image into a tightly packed RGBA8 pixel buffer.
// Returns false if the compression format or source data is unsupported.
bool decode_bc_to_rgba(
    ddsktx_format format, const ddsktx_sub_data& sub, std::vector<uint8_t>& output) {
    const int width = sub.width;
    const int height = sub.height;

    // BC textures are stored as 4x4 pixel blocks.
    // BC1 uses 8 bytes per block, while BC2/3/7 use 16.
    int blockSize = 0;
    switch (format) {
    case DDSKTX_FORMAT_BC1:
        blockSize = 8;
        break;

    case DDSKTX_FORMAT_BC2:
    case DDSKTX_FORMAT_BC3:
    case DDSKTX_FORMAT_BC7:
        blockSize = 16;
        break;

    default:
        return false;
    }

    // Round up so partial 4x4 blocks at the image edges are included.
    const int blocksX = (width + 3) / 4;
    const int blocksY = (height + 3) / 4;

    // Make sure each source row contains enough data for all of its blocks.
    if (sub.row_pitch_bytes < blocksX * blockSize) {
        return false;
    }

    const auto* src = static_cast<const uint8_t*>(sub.buff);

    // Allocate four bytes (RGBA) for every output pixel.
    output.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

    // Decode the image one 4x4 BC block at a time.
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            const uint8_t* compressedBlock =
                src + static_cast<size_t>(by) * sub.row_pitch_bytes +
                static_cast<size_t>(bx) * blockSize;

            // Temporary storage for one decoded 4x4 RGBA block.
            uint8_t block[4 * 4 * 4]{};

            switch (format) {
            case DDSKTX_FORMAT_BC1:
                bcdec_bc1(compressedBlock, block, 4 * 4);
                break;

            case DDSKTX_FORMAT_BC2:
                bcdec_bc2(compressedBlock, block, 4 * 4);
                break;

            case DDSKTX_FORMAT_BC3:
                bcdec_bc3(compressedBlock, block, 4 * 4);
                break;

            case DDSKTX_FORMAT_BC7:
                bcdec_bc7(compressedBlock, block, 4 * 4);
                break;

            default:
                return false;
            }

            // Edge blocks may contain fewer than 4 valid pixels.
            const int copyWidth = std::min(4, width - bx * 4);
            const int copyHeight = std::min(4, height - by * 4);

            // Copy each decoded row into its position in the full RGBA image.
            for (int py = 0; py < copyHeight; ++py) {
                const size_t dstOffset =
                    (static_cast<size_t>(by * 4 + py) * static_cast<size_t>(width) +
                        static_cast<size_t>(bx * 4)) *
                    4;

                const size_t srcOffset = static_cast<size_t>(py) * 4 * 4;

                std::memcpy(output.data() + dstOffset, block + srcOffset,
                    static_cast<size_t>(copyWidth) * 4);
            }
        }
    }

    return true;
}

// Build the DDS replacement filename expected for a Dusklight texture key.
// Format: tex1_<width>x<height>_<texture hash>[_<TLUT hash>]_<GX format>.dds
std::string make_texture_filename(const TextureKey& key) {
    std::ostringstream ss;

    ss << "tex1_" << key.width << "x" << key.height << "_" << std::hex << std::setfill('0')
       << std::setw(16) << key.texture_hash;

    if (key.has_tlut) {
        ss << "_" << std::setw(16) << key.tlut_hash;
    }

    ss << "_" << std::dec << static_cast<unsigned>(key.gx_format) << ".dds";

    return ss.str();
}

// Get Dusklight's texture replacement directory for the current platform.
// Returns no path if the replacement directory cannot be determined.
// Linux and macOS paths are based on Dusklight's documented data locations but have not been tested yet.
std::optional<fs::path> get_texture_replacement_root() {
#ifdef _WIN32
    const char* appData = std::getenv("APPDATA");

    if (!appData) {
        return std::nullopt;
    }

    return fs::path(appData) / "TwilitRealm" / "Dusklight" / "texture_replacements";

#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");

    if (!home) {
        return std::nullopt;
    }

    return fs::path(home) / "Library" / "Application Support" / "TwilitRealm" / "Dusklight" /
           "texture_replacements";

#elif defined(__linux__)
    const char* xdgDataHome = std::getenv("XDG_DATA_HOME");

    if (xdgDataHome && *xdgDataHome) {
        return fs::path(xdgDataHome) / "TwilitRealm" / "Dusklight" / "texture_replacements";
    }

    const char* home = std::getenv("HOME");

    if (!home) {
        return std::nullopt;
    }

    return fs::path(home) / ".local" / "share" / "TwilitRealm" / "Dusklight" /
           "texture_replacements";
#else
    return std::nullopt;
#endif
}

} // namespace

// Search Dusklight's texture replacement directories for the DDS file matching the given texture key.
// Returns no path if no match is found.
std::optional<fs::path> find_hd_texture_path(const TextureKey& key) {
    const auto root = get_texture_replacement_root();

    // Stop if the replacement directory cannot be found.
    if (!root || !fs::exists(*root)) {
        return std::nullopt;
    }

    // Build the filename expected for this texture key.
    const std::string wanted = make_texture_filename(key);

    // Use error_code so filesystem errors do not throw exceptions.
    std::error_code ec;

    // Search the replacement directory and all of its subdirectories.
    fs::recursive_directory_iterator it(
        *root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;

    while (it != end) {
        // Only compare regular files when no filesystem error occurred.
        if (!ec && it->is_regular_file()) {
            if (it->path().filename().string() == wanted) {
                return it->path();
            }
        }

        // Move to the next file and keep searching past recoverable errors.
        it.increment(ec);
        if (ec) {
            ec.clear();
        }
    }

    return std::nullopt;
}

// Load and decode the HD replacement for a texture key into RGBA8.
// Returns no texture if the replacement cannot be found, read, or decoded.
std::optional<HdTexture> load_hd_texture(const TextureKey& key) {
    const auto path = find_hd_texture_path(key);

    if (!path) {
        return std::nullopt;
    }

    // Open at the end of the file first so we can determine its total size.
    std::ifstream file(*path, std::ios::binary | std::ios::ate);
    if (!file) {
        mods::log::error("Failed to open HD texture: {}", path->string());
        return std::nullopt;
    }

    const std::streamsize fileSize = file.tellg();
    if (fileSize <= 0 || fileSize > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }

    // Read the entire DDS file into memory.
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> fileData(static_cast<size_t>(fileSize));

    if (!file.read(reinterpret_cast<char*>(fileData.data()), fileSize)) {
        return std::nullopt;
    }

    // Parse the DDS header and texture metadata.
    ddsktx_texture_info info{};
    ddsktx_error error{};

    if (!ddsktx_parse(
            &info, fileData.data(), static_cast<int>(fileData.size()), &error))
    {
        mods::log::error("Failed to parse DDS '{}': {}", path->string(), error.msg);
        return std::nullopt;
    }

    // Only ordinary 2D textures are supported for now.
    if (info.depth != 1 || info.num_layers != 1 ||
        (info.flags & DDSKTX_TEXTURE_FLAG_CUBEMAP))
    {
        mods::log::error("Unsupported DDS layout for '{}'", path->string());
        return std::nullopt;
    }

    // Get mip level 0, which contains the full-resolution texture.
    ddsktx_sub_data sub{};
    ddsktx_get_sub(
        &info, &sub, fileData.data(), static_cast<int>(fileData.size()), 0, 0, 0);

    if (!sub.buff || sub.width <= 0 || sub.height <= 0) {
        return std::nullopt;
    }

    HdTexture result;
    result.width = static_cast<uint32_t>(sub.width);
    result.height = static_cast<uint32_t>(sub.height);

    // Convert the DDS pixel data into RGBA8.
    switch (info.format) {
    case DDSKTX_FORMAT_BC1:
    case DDSKTX_FORMAT_BC2:
    case DDSKTX_FORMAT_BC3:
    case DDSKTX_FORMAT_BC7:
        if (!decode_bc_to_rgba(info.format, sub, result.rgba)) {
            return std::nullopt;
        }
        break;

    case DDSKTX_FORMAT_RGBA8: {
        const size_t rowSize = static_cast<size_t>(sub.width) * 4;
        result.rgba.resize(rowSize * static_cast<size_t>(sub.height));

        const auto* src = static_cast<const uint8_t*>(sub.buff);

        // Copy each row while respecting the DDS row pitch.
        for (int y = 0; y < sub.height; ++y) {
            std::memcpy(
                result.rgba.data() + static_cast<size_t>(y) * rowSize,
                src + static_cast<size_t>(y) * sub.row_pitch_bytes,
                rowSize);
        }

        break;
    }

    case DDSKTX_FORMAT_BGRA8: {
        result.rgba.resize(
            static_cast<size_t>(sub.width) * static_cast<size_t>(sub.height) * 4);

        const auto* src = static_cast<const uint8_t*>(sub.buff);

        // Convert BGRA pixels to RGBA one row at a time.
        for (int y = 0; y < sub.height; ++y) {
            const uint8_t* srcRow =
                src + static_cast<size_t>(y) * sub.row_pitch_bytes;

            uint8_t* dstRow =
                result.rgba.data() +
                static_cast<size_t>(y) * static_cast<size_t>(sub.width) * 4;

            for (int x = 0; x < sub.width; ++x) {
                dstRow[x * 4 + 0] = srcRow[x * 4 + 2];
                dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
                dstRow[x * 4 + 2] = srcRow[x * 4 + 0];

                // Some DDS files mark alpha as unused rather than storing it.
                dstRow[x * 4 + 3] =
                    (info.flags & DDSKTX_TEXTURE_FLAG_ALPHA_X) ? 255 : srcRow[x * 4 + 3];
            }
        }

        break;
    }

    default:
        mods::log::error(
            "Unsupported DDS format '{}' for {}",
            ddsktx_format_str(info.format),
            path->string());
        return std::nullopt;
    }

    mods::log::info(
        "Decoded HD texture: {}x{} {} -> RGBA8 ({})",
        result.width,
        result.height,
        ddsktx_format_str(info.format),
        path->filename().string());

    return result;
}

// Recolor an RGBA8 HD texture using the same grayscale + overlay method as the original Cosmetics recoloring path.
// Alpha is left unchanged.
void recolor_hd_texture(HdTexture& texture, GXColor color) {
    // Apply an overlay blend to a single color channel.
    auto blend_overlay_channel = [](uint8_t base, uint8_t blend) -> uint8_t {
        if (base < 128) {
            return static_cast<uint8_t>((2 * base * blend) / 255);
        }

        return static_cast<uint8_t>(
            255 - (2 * (255 - base) * (255 - blend)) / 255);
    };

    // Process one RGBA pixel at a time.
    for (size_t i = 0; i + 3 < texture.rgba.size(); i += 4) {
        const uint8_t r = texture.rgba[i + 0];
        const uint8_t g = texture.rgba[i + 1];
        const uint8_t b = texture.rgba[i + 2];

        // Convert the original color to grayscale while preserving brightness.
        const uint8_t gray =
            static_cast<uint8_t>((77 * r + 150 * g + 29 * b) >> 8);

        texture.rgba[i + 0] = blend_overlay_channel(gray, color.r);
        texture.rgba[i + 1] = blend_overlay_channel(gray, color.g);
        texture.rgba[i + 2] = blend_overlay_channel(gray, color.b);

        // Leave the original alpha channel unchanged.
    }
}