#pragma once

#include <aidl/android/hardware/graphics/common/PixelFormat.h>

#include <cstdint>

namespace pixel::graphics {

using FrameworkFormat = aidl::android::hardware::graphics::common::PixelFormat;

#define MapFormat(f) f = static_cast<uint32_t>(FrameworkFormat::f)

enum class Format : uint32_t {
    MapFormat(UNSPECIFIED),
    MapFormat(RGBA_8888),
    MapFormat(RGBX_8888),
    MapFormat(RGB_888),
    MapFormat(RGB_565),
    MapFormat(BGRA_8888),
    MapFormat(YCBCR_422_SP),
    MapFormat(YCRCB_420_SP),
    MapFormat(YCBCR_422_I),
    MapFormat(RGBA_FP16),
    MapFormat(RAW16),
    MapFormat(BLOB),
    MapFormat(IMPLEMENTATION_DEFINED),
    MapFormat(YCBCR_420_888),
    MapFormat(RAW_OPAQUE),
    MapFormat(RAW10),
    MapFormat(RAW12),
    MapFormat(RGBA_1010102),
    MapFormat(Y8),
    MapFormat(Y16),
    MapFormat(YV12),
    MapFormat(DEPTH_16),
    MapFormat(DEPTH_24),
    MapFormat(DEPTH_24_STENCIL_8),
    MapFormat(DEPTH_32F),
    MapFormat(DEPTH_32F_STENCIL_8),
    MapFormat(STENCIL_8),
    MapFormat(YCBCR_P010),
    MapFormat(HSV_888),
    MapFormat(R_8),
    MapFormat(R_16_UINT),
    MapFormat(RG_1616_UINT),
    MapFormat(RGBA_10101010),

    // Pixel specific formats
    GOOGLE_NV12 = 0x301,
    GOOGLE_RGBX16 = 0x302,
    GOOGLE_R8 = 0x303,
    /**
     * 48-bit format that has 16-bit R, G, B components, in that order,
     * from the lowest memory address to the highest memory address.
     */
    GOOGLE_RGB16 = 0x304,
    GOOGLE_BGRX = 0x305,

    /**
     * 2 plane format following [x:Y2:Y1:Y0], [x:Cr2:Cb2:Cr1 x:Cb1:Cr0:Cb0]
     * With each Y, Cr and Cb being 10 bits, and x representing 2 bits padding
     */
    GOOGLE_YCBCR_P030 = 0x306,
};

#undef MapFormat

} // namespace pixel::graphics
