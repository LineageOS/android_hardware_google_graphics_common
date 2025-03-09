#pragma once

#include <aidl/android/hardware/graphics/common/PlaneLayout.h>
#include <pixel-gralloc/format.h>
#include <pixel-gralloc/usage.h>
#include <cstdint>
#include <optional>
#include <vector>

using FrameworkPlaneLayout = aidl::android::hardware::graphics::common::PlaneLayout;

namespace pixel::graphics::utils {

std::optional<std::vector<FrameworkPlaneLayout>> get_plane_layouts(FrameworkFormat format,
                                                                   uint64_t usage, uint32_t width,
                                                                   uint32_t height);

enum class Compression {
    UNCOMPRESSED,
};

inline Usage get_usage_from_compression(Compression compression) {
    switch (compression) {
        case Compression::UNCOMPRESSED:
            return static_cast<Usage>(Usage::CPU_READ_OFTEN | Usage::CPU_WRITE_OFTEN |
                                      Usage::GPU_TEXTURE | Usage::GPU_RENDER_TARGET |
                                      Usage::COMPOSER_OVERLAY);
    }
}

#define FormatCase(f) \
    case Format::f:   \
        return #f

inline std::string get_string_from_format(Format format) {
    switch (format) {
        FormatCase(UNSPECIFIED);
        FormatCase(RGBA_8888);
        FormatCase(RGBX_8888);
        FormatCase(RGB_888);
        FormatCase(RGB_565);
        FormatCase(BGRA_8888);
        FormatCase(YCBCR_422_SP);
        FormatCase(YCRCB_420_SP);
        FormatCase(YCBCR_422_I);
        FormatCase(RGBA_FP16);
        FormatCase(RAW16);
        FormatCase(BLOB);
        FormatCase(IMPLEMENTATION_DEFINED);
        FormatCase(YCBCR_420_888);
        FormatCase(RAW_OPAQUE);
        FormatCase(RAW10);
        FormatCase(RAW12);
        FormatCase(RGBA_1010102);
        FormatCase(Y8);
        FormatCase(Y16);
        FormatCase(YV12);
        FormatCase(DEPTH_16);
        FormatCase(DEPTH_24);
        FormatCase(DEPTH_24_STENCIL_8);
        FormatCase(DEPTH_32F);
        FormatCase(DEPTH_32F_STENCIL_8);
        FormatCase(STENCIL_8);
        FormatCase(YCBCR_P010);
        FormatCase(HSV_888);
        FormatCase(R_8);
        FormatCase(R_16_UINT);
        FormatCase(RG_1616_UINT);
        FormatCase(RGBA_10101010);

        // Pixel specific formats
        FormatCase(GOOGLE_NV12);
        FormatCase(GOOGLE_R8);

        // Unknown formats
        default:
            return "Unknown";
    }
}

#undef FormatCase
} // namespace pixel::graphics::utils
