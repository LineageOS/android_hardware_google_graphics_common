/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef DISPLAYCOLOR_H_
#define DISPLAYCOLOR_H_

#include <android/hardware/graphics/common/1.1/types.h>
#include <android/hardware/graphics/common/1.2/types.h>

#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace displaycolor {

namespace hwc {
using android::hardware::graphics::common::V1_1::RenderIntent;
using android::hardware::graphics::common::V1_2::ColorMode;
using android::hardware::graphics::common::V1_2::Dataspace;
using android::hardware::graphics::common::V1_2::PixelFormat;
}  // namespace hwc

/**
 * hwc/displaycolor interface history
 *
 * 7.0.0.2022-03-22 Interface refactor
 * 6.2.0.2022-05-18 Get calibrated serial number.
 * 6.1.0.2022-04-29 dim solid color layer
 * 6.0.0.2022-02-22 Get whether dimming in linear.
 * 5.0.0.2022-02-17 Add layer dim ratio.
 * 4.0.0.2021-12-20 Get pixel format and dataspace of blending stage.
 * 3.0.0.2021-11-18 calibration info intf
 * 2.0.0.2021-08-27 pass brightness table for hdr10+
 * 1.0.0.2021-08-25 Initial release
 */

constexpr struct DisplayColorIntfVer {
    uint16_t major; // increase it for new functionalities
    uint16_t minor; // for bug fix and cause binary incompatible
    uint16_t patch; // for bug fix and binary compatible

    bool operator==(const DisplayColorIntfVer &rhs) const {
        return major == rhs.major &&
            minor == rhs.minor &&
            patch == rhs.patch;
    }

    bool operator!=(const DisplayColorIntfVer &rhs) const {
        return !operator==(rhs);
    }

    bool Compatible(const DisplayColorIntfVer &rhs) const {
        return major == rhs.major &&
            minor == rhs.minor;
    }

} kInterfaceVersion {
    7,
    0,
    0,
};

/// A map associating supported RenderIntents for each supported ColorMode
using ColorModesMap = std::map<hwc::ColorMode, std::vector<hwc::RenderIntent>>;

/// Image data bit depths.
enum class BitDepth { kEight, kTen };

// deprecated by 'int64_t display_id' TODO: remove after all clients upgrade to
// display_id
/// Display type used to get pipeline or update display scene.
enum DisplayType {
    /// builtin primary display
    DISPLAY_PRIMARY = 0,
    /// builtin secondary display
    DISPLAY_SECONDARY = 1,
    /// external display
    DISPLAY_EXTERNAL = 2,
    /// number of display
    DISPLAY_MAX = 3,
};

enum BrightnessMode {
    BM_NOMINAL = 0,
    BM_HBM = 1,
    BM_MAX = 2,
    BM_INVALID = BM_MAX,
};

enum class HdrLayerState {
    /// No HDR layer on screen
    kHdrNone,
    /// One or more small HDR layer(s), < 50% display size, take it as portrait mode.
    kHdrSmall,
    /// At least one large HDR layer, >= 50% display size, take it as full screen mode.
    kHdrLarge,
};

struct DisplayBrightnessRange {
    // inclusive lower bound
    float nits_min{};
    // inclusive upper bound
    float nits_max{};

    // inclusive lower bound
    uint32_t dbv_min{};
    // inclusive upper bound
    uint32_t dbv_max{};

    bool brightness_min_exclusive;
    float brightness_min{};
    // inclusive upper bound
    float brightness_max{};

    bool IsValid() const {
        // Criteria
        // 1. max >= min
        // 2. float min >= 0
        return nits_min >= 0 && brightness_min >= 0 && nits_max >= nits_min && dbv_max >= dbv_min &&
                brightness_max >= brightness_min;
    }
};
typedef std::map<BrightnessMode, DisplayBrightnessRange> BrightnessRangeMap;

class IBrightnessTable {
   public:
    virtual ~IBrightnessTable(){};

    virtual std::optional<std::reference_wrapper<const DisplayBrightnessRange>> GetBrightnessRange(
        BrightnessMode bm) const = 0;
    virtual std::optional<uint32_t> BrightnessToDbv(float brightness) const = 0;
    virtual std::optional<float> BrightnessToNits(float brightness, BrightnessMode &bm) const = 0;
    virtual std::optional<uint32_t> NitsToDbv(BrightnessMode bm, float nits) const = 0;
    virtual std::optional<float> DbvToNits(BrightnessMode bm, uint32_t dbv) const = 0;
    virtual std::optional<float> NitsToBrightness(float nits) const = 0;
    virtual std::optional<float> DbvToBrightness(uint32_t dbv) const = 0;
};

/**
 * @brief This structure holds data imported from HWC.
 */
struct DisplayInfo {
    // deprecated by display_id
    DisplayType display_type{DISPLAY_MAX};
    int64_t display_id{-1};
    std::string panel_name;
    std::string panel_serial;

    // If brightness table exists in pb file, it will overwrite values in brightness_ranges
    BrightnessRangeMap brightness_ranges;

    // displays that no need to calibrate like virtual or external displays
    // expect the pipeline outputs pixels with a standard color space
    bool standard_calibrated_display{false};
};

struct Color {
    uint8_t r{};
    uint8_t g{};
    uint8_t b{};
    uint8_t a{};

    bool operator==(const Color &rhs) const {
        return r == rhs.r &&
               g == rhs.g &&
               b == rhs.b &&
               a == rhs.a;
    }
};

struct LayerColorData {
    bool operator==(const LayerColorData &rhs) const {
        return dataspace == rhs.dataspace && matrix == rhs.matrix &&
               static_metadata == rhs.static_metadata &&
               dynamic_metadata == rhs.dynamic_metadata &&
               dim_ratio == rhs.dim_ratio &&
               is_solid_color_layer == rhs.is_solid_color_layer &&
               (!is_solid_color_layer || solid_color == rhs.solid_color) &&
               enabled == rhs.enabled;
    }

    bool operator!=(const LayerColorData &rhs) const {
        return !operator==(rhs);
    }

    /**
     * @brief HDR static metadata.
     *
     * See HWC v2.2 (IComposerClient::PerFrameMetadataKey)
     * for more information.
     */
    struct HdrStaticMetadata {
       private:
        std::array<int32_t, 13> data;

       public:
        HdrStaticMetadata() = default;
        HdrStaticMetadata(const HdrStaticMetadata &other)
            : data(other.data), is_valid(other.is_valid) {}
        HdrStaticMetadata(const HdrStaticMetadata &&other) = delete;
        HdrStaticMetadata &operator=(const HdrStaticMetadata &other) {
            data = other.data;
            is_valid = other.is_valid;
            return *this;
        }
        HdrStaticMetadata &operator=(HdrStaticMetadata &&other) = delete;
        ~HdrStaticMetadata() = default;

        bool operator==(const HdrStaticMetadata &rhs) const {
            return data == rhs.data && is_valid == rhs.is_valid;
        }
        bool operator!=(const HdrStaticMetadata &rhs) const {
            return !operator==(rhs);
        }

        /// Indicator for whether the data in this struct should be used.
        bool is_valid = false;
        /// This device's display's peak luminance, in nits.
        int32_t &device_max_luminance = data[0];

        // Mastering display properties
        int32_t &display_red_primary_x = data[1];
        int32_t &display_red_primary_y = data[2];
        int32_t &display_green_primary_x = data[3];
        int32_t &display_green_primary_y = data[4];
        int32_t &display_blue_primary_x = data[5];
        int32_t &display_blue_primary_y = data[6];
        int32_t &white_point_x = data[7];
        int32_t &white_point_y = data[8];
        int32_t &max_luminance = data[9];
        int32_t &min_luminance = data[10];

        // Content properties
        int32_t &max_content_light_level = data[11];
        int32_t &max_frame_average_light_level = data[12];
    };

    /**
     * @brief HDR dynamic metadata.
     *
     * The members defined here are a subset of metadata define in
     * SMPTE ST 2094-40:2016.
     * Also see module videoapi information.
     */
    struct HdrDynamicMetadata {
        bool operator==(const HdrDynamicMetadata &rhs) const {
            return is_valid == rhs.is_valid &&
                   display_maximum_luminance == rhs.display_maximum_luminance &&
                   maxscl == rhs.maxscl &&
                   maxrgb_percentages == rhs.maxrgb_percentages &&
                   maxrgb_percentiles == rhs.maxrgb_percentiles &&
                   tm_flag == rhs.tm_flag && tm_knee_x == rhs.tm_knee_x &&
                   tm_knee_y == rhs.tm_knee_y &&
                   bezier_curve_anchors == rhs.bezier_curve_anchors;
        }
        bool operator!=(const HdrDynamicMetadata &rhs) const {
            return !operator==(rhs);
        }

        /// Indicator for whether the data in this struct should be used.
        bool is_valid = false;

        uint32_t display_maximum_luminance{};
        std::array<uint32_t, 3> maxscl;
        std::vector<uint8_t> maxrgb_percentages;
        std::vector<uint32_t> maxrgb_percentiles;
        uint16_t tm_flag{};
        uint16_t tm_knee_x{};
        uint16_t tm_knee_y{};
        std::vector<uint16_t> bezier_curve_anchors;
    };

    /// This layer's dataspace (color gamut, transfer function, and range).
    hwc::Dataspace dataspace = hwc::Dataspace::UNKNOWN;
    /// Color transform for this layer. See SET_LAYER_COLOR_TRANSFORM HWC v2.3.
    // clang-format off
    std::array<float, 16> matrix {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
    // clang-format on
    /**
     * @brief This layer's HDR static metadata. Only applicable when dataspace
     * indicates this is an HDR layer.
     */
    HdrStaticMetadata static_metadata;
    /**
     * @brief This layer's HDR dynamic metadata. Only applicable when dataspace
     * indicates this is an HDR layer.
     */
    HdrDynamicMetadata dynamic_metadata;

    /**
     * @brief the layer's luminance dim ratio
     */
    float dim_ratio = 1.0f;

    /**
     * @brief is layer solid color
     */
    bool is_solid_color_layer{};

    /**
     * @brief color for solid color layer
     */
    Color solid_color;

    /**
     * @brief indicates if the layer is client target
     *
     */
    bool is_client_target = false;

    /**
     * @brief indicates if this layer data has enabled. Do not compute the
     * colordata if its false. true by default for backward compatibility.
     */
    bool enabled = true;
};

struct LtmParams {
    struct Display {
        int32_t width{};
        int32_t height{};
        bool operator==(const Display &rhs) const {
          return width == rhs.width && height == rhs.height;
        }
    };

    struct Roi {
        int32_t left{};
        int32_t top{};
        int32_t right{};
        int32_t bottom{};

        bool Valid(int32_t display_width, int32_t display_height) const {
            return left >= 0 && right > left && right <= display_width &&
                top >= 0 && bottom > top && bottom <= display_height;
        }

        bool operator==(const Roi &rhs) const {
          return left == rhs.left &&
              top == rhs.top &&
              right == rhs.right &&
              bottom == rhs.bottom;
        }
    };

    Display display;
    Roi roi;
    // for debug purpose
    bool force_enable{};
    bool sr_in_gtm{true};
    bool ConfigUpdateNeeded(const LtmParams &rhs) const {
        return display == rhs.display && roi == rhs.roi && force_enable == rhs.force_enable;
    }
};

/**
 * @brief DisplayScene holds all the information required for libdisplaycolor to
 * return correct data.
 */
struct DisplayScene {
    bool operator==(const DisplayScene &rhs) const {
        // TODO: if lux is used by HDR tone mapping, need to check here
        // but should trigger scene change as less as possible, for example,
        // only when HDR is on screen and lux change exceeds some threshold.
        return layer_data == rhs.layer_data &&
               dpu_bit_depth == rhs.dpu_bit_depth &&
               color_mode == rhs.color_mode &&
               render_intent == rhs.render_intent &&
               matrix == rhs.matrix &&
               force_hdr == rhs.force_hdr &&
               bm == rhs.bm &&
               lhbm_on == rhs.lhbm_on &&
               dbv == rhs.dbv &&
               refresh_rate == rhs.refresh_rate &&
               operation_rate == rhs.operation_rate &&
               hdr_layer_state == rhs.hdr_layer_state &&
               temperature == rhs.temperature;
    }
    bool operator!=(const DisplayScene &rhs) const {
        return !(*this == rhs);
    }

    /// A vector of layer color data.
    std::vector<LayerColorData> layer_data;
    /// The bit depth the DPU is currently outputting
    BitDepth dpu_bit_depth = BitDepth::kTen;
    /// The current ColorMode (typically set by SurfaceFlinger)
    hwc::ColorMode color_mode = hwc::ColorMode::NATIVE;
    /// The current RenderIntent (typically set by SurfaceFlinger)
    hwc::RenderIntent render_intent = hwc::RenderIntent::COLORIMETRIC;
    /// Color transform for this layer. See SET_COLOR_TRANSFORM HWC v2.1.
    // clang-format off
    std::array<float, 16> matrix {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
    // clang-format on
    /// When this bit is set, process hdr layers and the layer matrix even if
    //it's in native color mode.
    bool force_hdr = false;

    /// display brightness mode
    BrightnessMode bm = BrightnessMode::BM_NOMINAL;

    /// dbv level
    uint32_t dbv = 0;

    /// the nits value corresponding to the dbv above
    float nits = 0;

    /// lhbm status
    bool lhbm_on = false;

    /// refresh rate
    float refresh_rate = 60.0f;

    /// operation rate to switch between hs/ns mode
    uint32_t operation_rate = 120;

    /// display temperature in degrees Celsius
    uint32_t temperature = UINT_MAX;

    /// hdr layer state on screen
    HdrLayerState hdr_layer_state = HdrLayerState::kHdrNone;

    /// ambient lux
    float lux{};

    /// Ltm params gathered in HWC
    LtmParams ltm_params;
};

struct CalibrationInfo {
    bool factory_cal_loaded = false;
    bool golden_cal_loaded = false;
    bool common_cal_loaded = false;
    bool dev_cal_loaded = false;
};

/// An interface specifying functions that are HW-agnostic.
class IDisplayColorGeneric {
   public:
    /// A generic stage in the display pipeline.
    template <typename T>
    struct DisplayStage {
        using ConfigType = T;

        std::function<void(void)> data_applied_notifier = nullptr;
        void NotifyDataApplied() const {
            if (data_applied_notifier) {
                data_applied_notifier();
            }
        }

        bool enable = false;
        /// A flag indicating if the data has been changed in last Update call.
        // It should be set when enable is changed from false to true.
        bool dirty = false;

        const ConfigType *config = nullptr;
    };

    /// A collection of stages. For example, It could be pre-blending stages
    //(per-channel) or post-blending stages.
    template <typename ... IStageData>
    struct IStageDataCollection : public IStageData ... {
        virtual ~IStageDataCollection() {}
    };

    /// Interface for accessing data for panel
    class IPanel {
      public:
        /// Get the adjusted dbv for panel.
        virtual uint32_t GetAdjustedBrightnessLevel() const = 0;

        virtual ~IPanel() {}
    };

    virtual ~IDisplayColorGeneric() {}

    /**
     * @brief Update display color data. This function is expected to be called
     * in the context of HWC::validateDisplay, if the display scene has changed.
     *
     * @param display The display relating to the scene.
     * @param scene Display scene data to use during the update.
     * @return OK if successful, error otherwise.
     */
    //deprecated by the 'int64_t display' version
    virtual int Update(const DisplayType display, const DisplayScene &scene) = 0;
    virtual int Update(const int64_t display, const DisplayScene &scene) = 0;

    /**
     * @brief Update display color data. This function is expected to be called
     * in the context of HWC::presentDisplay, if the display scene has changed
     * since the Update call for HWC::validateDisplay.
     *
     * @param display The display relating to the scene.
     * @param scene Display scene data to use during the update.
     * @return OK if successful, error otherwise.
     */
    //deprecated by the 'int64_t display' version
    virtual int UpdatePresent(const DisplayType display, const DisplayScene &scene) = 0;
    virtual int UpdatePresent(const int64_t display, const DisplayScene &scene) = 0;

    /**
     * @brief Check if refresh rate regamma compensation is enabled.
     *
     * @return true for yes.
     */
    //deprecated by the 'int64_t display' version
    virtual bool IsRrCompensationEnabled(const DisplayType display) = 0;
    virtual bool IsRrCompensationEnabled(const int64_t display) = 0;

    /**
     * @brief Get calibration information for each profiles.
     * @param display The display to get the calibration information.
     */
    //deprecated by the 'int64_t display' version
    virtual const CalibrationInfo &GetCalibrationInfo(const DisplayType display) const = 0;
    virtual const CalibrationInfo &GetCalibrationInfo(const int64_t display) const = 0;

    /**
     * @brief Get a map of supported ColorModes, and supported RenderIntents for
     * each ColorMode.
     * @param display The display to get the color modes and render intents.
     */
    //deprecated by the 'int64_t display' version
    virtual const ColorModesMap &ColorModesAndRenderIntents(const DisplayType display) const = 0;
    virtual const ColorModesMap &ColorModesAndRenderIntents(const int64_t display) const = 0;

    /**
     * @brief Get pixel format and dataspace of blending stage.
     * @param display to read the properties.
     * @param pixel_format Pixel format of blending stage
     * @param dataspace Dataspace of blending stage
     * @return OK if successful, error otherwise.
     */
    //deprecated by the 'int64_t display' version
    virtual int GetBlendingProperty(const DisplayType display,
                                    hwc::PixelFormat &pixel_format,
                                    hwc::Dataspace &dataspace,
                                    bool &dimming_linear) const = 0;
    virtual int GetBlendingProperty(const int64_t display,
                                    hwc::PixelFormat &pixel_format,
                                    hwc::Dataspace &dataspace,
                                    bool &dimming_linear) const = 0;

    /**
     * @brief Get the serial number for the panel used during calibration.
     * @param display to get the calibrated serial number.
     * @return The calibrated serial number.
     */
    //deprecated by the 'int64_t display' version
    virtual const std::string& GetCalibratedSerialNumber(DisplayType display) const = 0;
    virtual const std::string& GetCalibratedSerialNumber(const int64_t display) const = 0;

    /**
     * @brief Get brightness table to do brightness conversion between {normalized brightness, nits,
     * dbv}.
     * @param display Reserved field to choose display type.
     * @param table Return brightness table if successful, nullptr if the table is not valid.
     * @return OK if successful, error otherwise.
     */
    //deprecated by the 'int64_t display' version
    virtual int GetBrightnessTable(DisplayType display,
                                   std::unique_ptr<const IBrightnessTable> &table) const = 0;
    virtual int GetBrightnessTable(const int64_t display,
                                   std::unique_ptr<const IBrightnessTable> &table) const = 0;

    /**
     * @brief Add a display for color pipeline configuration.
     * @param display_info info of this display
     * @return OK if successful, error otherwise.
     */
    virtual int AddDisplay(const DisplayInfo &display_info) = 0;

    /**
     * @brief Remove a display and release its resources.
     */
    virtual void RemoveDisplay(const int64_t display) = 0;

    /**
     * @brief request a Update call. For example, a debug command has changed
     * the displaycolor internal states and need to apply to next frame update.
     */
    virtual bool CheckUpdateNeeded(const int64_t display) = 0;

    /**
     * @brief Check if early power on is needed.
     *
     * @return true for yes.
     */
    //deprecated by the 'int64_t display' version
    virtual bool IsEarlyPowerOnNeeded(const DisplayType display) = 0;
    virtual bool IsEarlyPowerOnNeeded(const int64_t display) = 0;

    /**
     * @brief a debug call from command line with arguments, output will show on screen.
     * @param display id
     * @param cur_obj for the current object
     * @param obj_sel a path (object names concatenated by dots) to locate the target object
     * @param action to apply to the target object
     * @param args the arguments for the action
     * @return string to show on screen
     */
    virtual std::string Debug(const int64_t display,
                              const std::string& cur_obj,
                              const std::string& obj_sel,
                              const std::string& action,
                              const std::vector<std::string>& args) = 0;
};

extern "C" {
    const DisplayColorIntfVer *GetInterfaceVersion();
}

}  // namespace displaycolor

#endif  // DISPLAYCOLOR_H_
