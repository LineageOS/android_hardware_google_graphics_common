#pragma once

#include <log/log.h>

#include <android/hardware/graphics/mapper/IMapper.h>
#include <android/hardware/graphics/mapper/utils/IMapperMetadataTypes.h>
#include <android/hardware/graphics/mapper/utils/IMapperProvider.h>
#include <dlfcn.h>
#include <vndksupport/linker.h>

#include "metadata.h"
#include "utils-internal.h"

namespace pixel::graphics::mapper {

namespace {

using namespace ::pixel::graphics;

AIMapper* get_mapper() {
    static AIMapper* mMapper = []() {
        AIMapper* mapper = nullptr;
        std::string_view so_name = "mapper.pixel.so";
        void* so_lib = android_load_sphal_library(so_name.data(), RTLD_LOCAL | RTLD_NOW);
        if (so_lib == nullptr) return mapper;
        auto load_fn = reinterpret_cast<decltype(AIMapper_loadIMapper)*>(
                dlsym(so_lib, "AIMapper_loadIMapper"));
        if (load_fn == nullptr) return mapper;
        load_fn(&mapper);
        return mapper;
    }();
    if (!mMapper) {
        ALOGI("Mapper5 unavailable");
        return {};
    } else {
        return mMapper;
    }
}

} // namespace

template <MetadataType meta>
std::optional<typename metadata::ReturnType<meta>::type> get(buffer_handle_t handle) {
    AIMapper_MetadataType type = {
            .name = kPixelMetadataTypeName,
            .value = static_cast<int64_t>(meta),
    };

    auto mapper = get_mapper();
    android::hardware::hidl_vec<uint8_t> vec;
    std::vector<uint8_t> metabuf;
    auto ret = mapper->v5.getMetadata(handle, type, metabuf.data(), 0);
    if (ret < 0) {
        return {};
    }

    metabuf.resize(ret);
    ret = mapper->v5.getMetadata(handle, type, metabuf.data(), metabuf.size());

    if (ret < 0) {
        return {};
    }
    return utils::decode<typename metadata::ReturnType<meta>::type>(metabuf);
}

template <MetadataType meta>
int64_t set(buffer_handle_t handle, typename metadata::ReturnType<meta>::type data) {
    auto encoded_data = utils::encode<typename metadata::ReturnType<meta>::type>(data);
    auto mapper = get_mapper();
    AIMapper_MetadataType type = {
            .name = kPixelMetadataTypeName,
            .value = static_cast<int64_t>(meta),
    };

    auto ret = mapper->v5.setMetadata(handle, type, encoded_data.data(), encoded_data.size());

    return ret;
}

} // namespace pixel::graphics::mapper
