#pragma once

#include <dxgiformat.h>

// Shared between the OpenXR layer and the OBS plugin so both binaries make
// identical format decisions for the shared mirror textures.
namespace obs_mirror_ipc {

    struct DxgiFormatInfo {
        /// The different versions of this format, set to DXGI_FORMAT_UNKNOWN if absent.
        /// Both the SRGB and linear formats should be UNORM.
        DXGI_FORMAT srgb, linear, typeless;

        /// The bits per pixel, bits per channel, and the number of channels.
        int bpp, bpc, channels;
    };

    inline bool GetFormatInfo(const DXGI_FORMAT format, DxgiFormatInfo& out) {
#define DEF_FMT_BASE(typeless, linear, srgb, bpp, bpc, channels)                                                       \
    {                                                                                                                  \
        out = DxgiFormatInfo{srgb, linear, typeless, bpp, bpc, channels};                                              \
        return true;                                                                                                   \
    }

#define DEF_FMT_NOSRGB(name, bpp, bpc, channels)                                                                       \
    case name##_TYPELESS:                                                                                              \
    case name##_UNORM:                                                                                                 \
        DEF_FMT_BASE(name##_TYPELESS, name##_UNORM, DXGI_FORMAT_UNKNOWN, bpp, bpc, channels)

#define DEF_FMT(name, bpp, bpc, channels)                                                                              \
    case name##_TYPELESS:                                                                                              \
    case name##_UNORM:                                                                                                 \
    case name##_UNORM_SRGB:                                                                                            \
        DEF_FMT_BASE(name##_TYPELESS, name##_UNORM, name##_UNORM_SRGB, bpp, bpc, channels)

#define DEF_FMT_UNORM(linear, bpp, bpc, channels)                                                                      \
    case linear:                                                                                                       \
        DEF_FMT_BASE(DXGI_FORMAT_UNKNOWN, linear, DXGI_FORMAT_UNKNOWN, bpp, bpc, channels)

        // Note that this *should* have pretty much all the types we'll ever see in games
        // Filtering out the non-typeless and non-unorm/srgb types, this is all we're left with
        // (note that types that are only typeless and don't have unorm/srgb variants are dropped too)
        switch (format) {
            // The relatively traditional 8bpp 32-bit types
            DEF_FMT(DXGI_FORMAT_R8G8B8A8, 32, 8, 4)
            DEF_FMT(DXGI_FORMAT_B8G8R8A8, 32, 8, 4)
            DEF_FMT(DXGI_FORMAT_B8G8R8X8, 32, 8, 3)

            // Some larger linear-only types
            DEF_FMT_NOSRGB(DXGI_FORMAT_R16G16B16A16, 64, 16, 4)
            DEF_FMT_NOSRGB(DXGI_FORMAT_R10G10B10A2, 32, 10, 4)

            // A jumble of other weird types
            DEF_FMT_UNORM(DXGI_FORMAT_B5G6R5_UNORM, 16, 5, 3)
            DEF_FMT_UNORM(DXGI_FORMAT_B5G5R5A1_UNORM, 16, 5, 4)
            DEF_FMT_UNORM(DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM, 32, 10, 4)
            DEF_FMT_UNORM(DXGI_FORMAT_B4G4R4A4_UNORM, 16, 4, 4)
            DEF_FMT(DXGI_FORMAT_BC1, 64, 16, 4)

        default:
            // Unknown type
            return false;
        }

#undef DEF_FMT
#undef DEF_FMT_NOSRGB
#undef DEF_FMT_BASE
#undef DEF_FMT_UNORM
    }

} // namespace obs_mirror_ipc
