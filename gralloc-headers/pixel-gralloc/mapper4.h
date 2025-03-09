#pragma once

#include <android/hardware/graphics/mapper/4.0/IMapper.h>
#include <log/log.h>

#include "metadata.h"
#include "utils-internal.h"

namespace pixel::graphics::mapper {

namespace {

using android::hardware::graphics::mapper::V4_0::Error;
using android::hardware::graphics::mapper::V4_0::IMapper;
using namespace ::pixel::graphics;

static inline android::sp<IMapper> get_mapper() {
    static android::sp<IMapper> mapper = []() {
        auto mapper = IMapper::getService();
        if (!mapper) {
            ALOGE("Failed to get mapper service");
        }
        return mapper;
    }();

    return mapper;
}

} // namespace

template <MetadataType meta>
std::optional<typename metadata::ReturnType<meta>::type> get(buffer_handle_t handle) {
    auto mapper = get_mapper();
    IMapper::MetadataType type = {
            .name = kPixelMetadataTypeName,
            .value = static_cast<int64_t>(meta),
    };

    android::hardware::hidl_vec<uint8_t> vec;
    Error error;
    auto ret = mapper->get(const_cast<native_handle_t*>(handle), type,
                           [&](const auto& tmpError,
                               const android::hardware::hidl_vec<uint8_t>& tmpVec) {
                               error = tmpError;
                               vec = tmpVec;
                           });
    if (!ret.isOk()) {
        return {};
    }

    return utils::decode<typename metadata::ReturnType<meta>::type>(vec);
}

template <MetadataType meta>
int32_t set(buffer_handle_t handle, typename metadata::ReturnType<meta>::type data) {
    android::sp<IMapper> mapper = get_mapper();
    auto encoded_data = utils::encode<typename metadata::ReturnType<meta>::type>(data);
    IMapper::MetadataType type = {
            .name = kPixelMetadataTypeName,
            .value = static_cast<int64_t>(meta),
    };

    Error err = mapper->set(const_cast<native_handle_t*>(handle), type, encoded_data);

    return static_cast<int32_t>(err);
}

} // namespace pixel::graphics::mapper
