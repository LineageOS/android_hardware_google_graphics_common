#include "pixel-gralloc/utils.h"
#include <log/log.h>
#include <ui/GraphicBuffer.h>
#include <ui/GraphicBufferMapper.h>

using android::GraphicBuffer;
using android::sp;

namespace pixel::graphics::utils {

std::optional<std::vector<FrameworkPlaneLayout>> get_plane_layouts(FrameworkFormat format,
                                                                   uint64_t usage, uint32_t width,
                                                                   uint32_t height) {
    auto& mapper = android::GraphicBufferMapper::getInstance();

    usage = usage | Usage::PLACEHOLDER_BUFFER;
    auto f = static_cast<android::PixelFormat>(format);
    auto buffer = sp<GraphicBuffer>::make(width, height, f, /*layerCount=*/1, usage);
    if (!buffer) {
        ALOGE("Failed to allocate buffer");
        return std::nullopt;
    }

    std::vector<FrameworkPlaneLayout> plane_layouts;
    auto error = mapper.getPlaneLayouts(buffer->handle, &plane_layouts);
    if (error != android::OK) {
        ALOGE("Failed to get plane layouts");
        return std::nullopt;
    }

    return plane_layouts;
}
} // namespace pixel::graphics::utils
