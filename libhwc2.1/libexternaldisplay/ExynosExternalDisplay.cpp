/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include <hardware/hwcomposer_defs.h>
#include "ExynosExternalDisplay.h"
#include "ExynosDevice.h"
#include "displayport_for_hwc.h"
#include <errno.h>
#include "ExynosLayer.h"
#include "ExynosHWCHelper.h"
#include "ExynosHWCDebug.h"
#include <linux/fb.h>

#define SKIP_FRAME_COUNT 3
extern struct exynos_hwc_control exynosHWCControl;

bool is_same_dv_timings(const struct v4l2_dv_timings *t1,
        const struct v4l2_dv_timings *t2)
{
    if (t1->type == t2->type &&
            t1->bt.width == t2->bt.width &&
            t1->bt.height == t2->bt.height &&
            t1->bt.interlaced == t2->bt.interlaced &&
            t1->bt.polarities == t2->bt.polarities &&
            t1->bt.pixelclock == t2->bt.pixelclock &&
            t1->bt.hfrontporch == t2->bt.hfrontporch &&
            t1->bt.vfrontporch == t2->bt.vfrontporch &&
            t1->bt.vsync == t2->bt.vsync &&
            t1->bt.vbackporch == t2->bt.vbackporch &&
            (!t1->bt.interlaced ||
             (t1->bt.il_vfrontporch == t2->bt.il_vfrontporch &&
              t1->bt.il_vsync == t2->bt.il_vsync &&
              t1->bt.il_vbackporch == t2->bt.il_vbackporch)))
        return true;
    return false;
}

int ExynosExternalDisplay::getDVTimingsIndex(int preset)
{
    for (int i = 0; i < SUPPORTED_DV_TIMINGS_NUM; i++) {
        if (preset == preset_index_mappings[i].preset)
            return preset_index_mappings[i].dv_timings_index;
    }
    return -1;
}

ExynosExternalDisplay::ExynosExternalDisplay(uint32_t __unused type, ExynosDevice *device)
    :   ExynosDisplay(HWC_DISPLAY_EXTERNAL, device)
{
    DISPLAY_LOGD(eDebugExternalDisplay, "");

    mDisplayControl.cursorSupport = true;

    mEnabled = false;
    mBlanked = false;

	mXres = 0;
    mYres = 0;
    mXdpi = 0;
    mYdpi = 0;
    mVsyncPeriod = 0;
    mDisplayId = HWC_DISPLAY_EXTERNAL;
    mSkipStartFrame = 0;
    mSkipFrameCount = -1;
    mIsSkipFrame = false;
    mActiveConfigIndex = 0;
    mVirtualDisplayState = 0;

    //TODO : Hard coded currently
    mNumMaxPriorityAllowed = 1;

    mPowerModeState = (hwc2_power_mode_t)HWC_POWER_MODE_OFF;
    mDisplayName = android::String8("ExternalDisplay");
}

ExynosExternalDisplay::~ExynosExternalDisplay()
{

}

void ExynosExternalDisplay::init()
{

}

void ExynosExternalDisplay::deInit()
{

}

int ExynosExternalDisplay::openExternalDisplay()
{
    DISPLAY_LOGD(eDebugExternalDisplay, "");

    int ret = 0;
    setVsyncEnabled(1);

    mSkipFrameCount = SKIP_FRAME_COUNT;
    mSkipStartFrame = 0;
    mActiveConfigIndex = 0;
    mPlugState = true;

    if (mLayers.size() != 0) {
        mLayers.clear();
    }

    DISPLAY_LOGD(eDebugExternalDisplay, "open fd for External Display(%d)", ret);

    return ret;
}

void ExynosExternalDisplay::closeExternalDisplay()
{
    DISPLAY_LOGD(eDebugExternalDisplay, "");

    setVsyncEnabled(0);

    if (this->mPowerModeState != (hwc2_power_mode_t)HWC_POWER_MODE_OFF) {
        if (mDisplayInterface->setPowerMode(HWC_POWER_MODE_OFF) < 0) {
            DISPLAY_LOGE("%s: set powermode ioctl failed errno : %d", __func__, errno);
            return;
        }
    }

    this->mPowerModeState = (hwc2_power_mode_t)HWC_POWER_MODE_OFF;

    DISPLAY_LOGD(eDebugExternalDisplay, "Close fd for External Display");

    mPlugState = false;
    mEnabled = false;
    mBlanked = false;
    mSkipFrameCount = SKIP_FRAME_COUNT;

    for (size_t i = 0; i < mLayers.size(); i++) {
        ExynosLayer *layer = mLayers[i];
        layer->mAcquireFence = fence_close(layer->mAcquireFence, this, FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_LAYER);
        layer->mReleaseFence = -1;
        layer->mLayerBuffer = NULL;
    }
}

int ExynosExternalDisplay::getDisplayConfigs(uint32_t* outNumConfigs, hwc2_config_t* outConfigs)
{
    DISPLAY_LOGD(eDebugExternalDisplay, "");

    if (!mHpdStatus)
        return -1;
    return mDisplayInterface->getDisplayConfigs(outNumConfigs, outConfigs);
}

int32_t ExynosExternalDisplay::getActiveConfig(
        hwc2_config_t* outConfig) {
    DISPLAY_LOGD(eDebugExternalDisplay, "");

    if (!mHpdStatus)
        return -1;

    *outConfig = mActiveConfigIndex;

    return HWC2_ERROR_NONE;
}

void ExynosExternalDisplay::hotplug(){
    DISPLAY_LOGD(eDebugExternalDisplay, "");

    hwc2_callback_data_t callbackData =
        mDevice->mCallbackInfos[HWC2_CALLBACK_HOTPLUG].callbackData;
    HWC2_PFN_HOTPLUG callbackFunc =
        (HWC2_PFN_HOTPLUG)mDevice->mCallbackInfos[HWC2_CALLBACK_HOTPLUG].funcPointer;
    if (callbackData != NULL && callbackFunc != NULL)
        callbackFunc(callbackData, HWC_DISPLAY_EXTERNAL, mHpdStatus ? HWC2_CONNECTION_CONNECTED : HWC2_CONNECTION_DISCONNECTED);
}

bool ExynosExternalDisplay::handleRotate()
{
    // FIXME: HWC2_COMPOSITION_SCREENSHOT is not dfeind in AOSP
    //        HWC guys should fix this.
    if (mSkipStartFrame < SKIP_EXTERNAL_FRAME) {
#if 0
        for (size_t i = 0; i < mLayers.size(); i++) {
            ExynosLayer *layer = mLayers[i];
            if (layer->mCompositionType == HWC2_COMPOSITION_SCREENSHOT)
                layer->mCompositionType = HWC2_COMPOSITION_DEVICE;
        }
#endif
        mIsSkipFrame = false;
        return false;
    }

#if 0
    for (size_t i = 0; i < mLayers.size(); i++) {
        ExynosLayer *layer = mLayers[i];
        if (layer->mCompositionType == HWC2_COMPOSITION_SCREENSHOT) {
            DISPLAY_LOGD(eDebugExternalDisplay, "include rotation animation layer");
            layer->mOverlayInfo = eSkipRotateAnim;
            for (size_t j = 0; j < mLayers.size(); j++) {
                ExynosLayer *skipLayer = mLayers[j];
                skipLayer->mValidateCompositionType = HWC2_COMPOSITION_DEVICE;
            }
            mIsSkipFrame = true;
            return true;
        }
    }
#endif
    mIsSkipFrame = false;
    return false;
}

bool ExynosExternalDisplay::checkRotate()
{
    // FIXME: HWC2_COMPOSITION_SCREENSHOT is not dfeind in AOSP
    //        HWC guys should fix this.
#if 0
    for (size_t i = 0; i < mLayers.size(); i++) {
        ExynosLayer *layer = mLayers[i];

        if (layer->mCompositionType == HWC2_COMPOSITION_SCREENSHOT) {
            return true;
        }
    }
#endif
    return false;
}

int32_t ExynosExternalDisplay::validateDisplay(
        uint32_t* outNumTypes, uint32_t* outNumRequests) {
    Mutex::Autolock lock(mExternalMutex);
    DISPLAY_LOGD(eDebugExternalDisplay, "");

    int32_t ret;
    mSkipFrame = false;

    if (mSkipStartFrame < SKIP_EXTERNAL_FRAME) {
        ALOGI("[ExternalDisplay] %s : setGeometryChanged [%d/%d]", __func__, mSkipStartFrame, SKIP_EXTERNAL_FRAME);
        initDisplay();
        /*
         * geometry should be set before ExynosDisplay::validateDisplay is called
         * not to skip resource assignment
         */
        if (mPlugState)
            setGeometryChanged(GEOMETRY_DEVICE_DISPLAY_ADDED);
        else
            setGeometryChanged(GEOMETRY_DEVICE_DISPLAY_REMOVED);
    }

    if (handleRotate()) {
        if ((ret = mResourceManager->initResourcesState(this)) != NO_ERROR)
            DISPLAY_LOGE("[ExternalDisplay] %s : initResourcesState fail, ret(%d)", __func__, ret);
        mDevice->setGeometryChanged(GEOMETRY_LAYER_UNKNOWN_CHANGED);
        mClientCompositionInfo.initializeInfos(this);
        mExynosCompositionInfo.initializeInfos(this);
        mRenderingState = RENDERING_STATE_VALIDATED;
        return HWC2_ERROR_NONE;
    }

    if (mSkipStartFrame < SKIP_EXTERNAL_FRAME) {
        /*
         * Set mIsSkipFrame before calling ExynosDisplay::validateDisplay()
         * startPostProcessing() that is called by ExynosDisplay::validateDisplay()
         * checks mIsSkipFrame.
         */
        mIsSkipFrame = true;
    }

    ret = ExynosDisplay::validateDisplay(outNumTypes, outNumRequests);

    if (mSkipStartFrame < SKIP_EXTERNAL_FRAME) {
        initDisplay();
        mRenderingState = RENDERING_STATE_VALIDATED;
        for (size_t i = 0; i < mLayers.size(); i++) {
            ExynosLayer *layer = mLayers[i];
            if (layer && (layer->mValidateCompositionType == HWC2_COMPOSITION_DEVICE ||
                layer->mValidateCompositionType == HWC2_COMPOSITION_EXYNOS)) {
                layer->mValidateCompositionType = HWC2_COMPOSITION_CLIENT;
                layer->mReleaseFence = layer->mAcquireFence;
            }
        }
        mSkipStartFrame++;

        ALOGI("[ExternalDisplay] %s : Skip start frame [%d/%d]", __func__, mSkipStartFrame, SKIP_EXTERNAL_FRAME);
    }

    return ret;
}

int32_t ExynosExternalDisplay::canSkipValidate() {

    /*
     * SurfaceFlinger may call vadlidate, present for a few frame
     * even though external display is disconnected.
     * Cammands for primary display can be discarded if validate is skipped
     * in this case. HWC should return error not to skip validate.
     */
    if ((mHpdStatus == false) || (mBlanked == true))
        return SKIP_ERR_DISP_NOT_CONNECTED;

    if ((mSkipStartFrame > (SKIP_EXTERNAL_FRAME - 1)) && (mEnabled == false) &&
        (mPowerModeState == (hwc2_power_mode_t)HWC_POWER_MODE_NORMAL))
        return SKIP_ERR_DISP_NOT_POWER_ON;

    if (checkRotate() || (mIsSkipFrame) ||
        (mSkipStartFrame < SKIP_EXTERNAL_FRAME))
        return SKIP_ERR_FORCE_VALIDATE;

    return ExynosDisplay::canSkipValidate();
}

int32_t ExynosExternalDisplay::presentDisplay(
    int32_t* outRetireFence)
{
    Mutex::Autolock lock(mExternalMutex);
    DISPLAY_LOGD(eDebugExternalDisplay, "");
    int32_t ret;

    if (mSkipFrame) {
        ALOGI("[%d] presentDisplay is skipped by mSkipFrame", mDisplayId);
        closeFencesForSkipFrame(RENDERING_STATE_PRESENTED);
        setGeometryChanged(GEOMETRY_DISPLAY_FORCE_VALIDATE);
        *outRetireFence = -1;
        for (size_t i=0; i < mLayers.size(); i++) {
            mLayers[i]->mReleaseFence = -1;
        }
        if (mRenderingState == RENDERING_STATE_NONE) {
            ALOGD("\tThis is the first frame after power on");
            ret = HWC2_ERROR_NONE;
        } else {
            ALOGD("\tThis is the second frame after power on");
            ret = HWC2_ERROR_NOT_VALIDATED;
        }
        mRenderingState = RENDERING_STATE_PRESENTED;
        mDevice->invalidate();
        return ret;
    }

    if ((mIsSkipFrame) || (mHpdStatus == false) || (mBlanked == true)) {
        if ((exynosHWCControl.skipValidate == true) &&
            ((mRenderingState == RENDERING_STATE_PRESENTED) ||
             (mRenderingState == RENDERING_STATE_NONE))) {

            if (mDevice->canSkipValidate() == false) {
                mRenderingState = RENDERING_STATE_NONE;
                return HWC2_ERROR_NOT_VALIDATED;
            } else {
                DISPLAY_LOGD(eDebugSkipValidate, "validate is skipped");
            }
        }

        *outRetireFence = -1;
        for (size_t i = 0; i < mLayers.size(); i++) {
            ExynosLayer *layer = mLayers[i];
            layer->mAcquireFence = fence_close(layer->mAcquireFence, this,
                    FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_LAYER);
            layer->mReleaseFence = -1;
        }
        mClientCompositionInfo.mAcquireFence =
            fence_close(mClientCompositionInfo.mAcquireFence, this,
                    FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_FB);
        mClientCompositionInfo.mReleaseFence = -1;

        /* this frame is not presented, but mRenderingState is updated to RENDERING_STATE_PRESENTED */
        initDisplay();

        /*
         * Resource assignment information was initialized during skipping frames
         * So resource assignment for the first displayed frame after skpping frames
         * should not be skipped
         */
        setGeometryChanged(GEOMETRY_DISPLAY_FORCE_VALIDATE);

        mDevice->invalidate();

        return HWC2_ERROR_NONE;
    }

    ret = ExynosDisplay::presentDisplay(outRetireFence);

    return ret;
}
int32_t ExynosExternalDisplay::setClientTarget(
        buffer_handle_t target,
        int32_t acquireFence, int32_t /*android_dataspace_t*/ dataspace) {
    private_handle_t *handle = NULL;
    if (target != NULL)
        handle = private_handle_t::dynamicCast(target);
    if ((mClientCompositionInfo.mHasCompositionLayer == true) &&
        (handle == NULL) &&
        (mClientCompositionInfo.mSkipFlag == false)) {
        /*
         * openExternalDisplay() can be called between validateDisplay and getChangedCompositionTypes.
         * Then getChangedCompositionTypes() returns no layer because openExternalDisplay() clears mLayers.
         * SurfaceFlinger might not change compositionType to HWC2_COMPOSITION_CLIENT.
         * Handle can be NULL in this case. It is not error case.
         */
        if (mSkipStartFrame == 0) {
            if (acquireFence >= 0)
                fence_close(acquireFence, this, FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_FB);
            acquireFence = -1;
            mClientCompositionInfo.setTargetBuffer(this, handle, acquireFence, (android_dataspace)dataspace);
            return NO_ERROR;
        }
    }
    return ExynosDisplay::setClientTarget(target, acquireFence, dataspace);
}

int ExynosExternalDisplay::enable()
{
    ALOGI("[ExternalDisplay] %s +", __func__);

    if (mEnabled)
        return HWC2_ERROR_NONE;

    if (mHpdStatus == false) {
        ALOGI("HPD is not connected");
        return HWC2_ERROR_NONE;
    }

    if (openExternalDisplay() < 0)
        return HWC2_ERROR_UNSUPPORTED;

    if (mDisplayInterface->setPowerMode(HWC_POWER_MODE_NORMAL) < 0){
        DISPLAY_LOGE("set powermode ioctl failed errno : %d", errno);
        return HWC2_ERROR_UNSUPPORTED;
    }

    mEnabled = true;

    ALOGI("[ExternalDisplay] %s -", __func__);

    return HWC2_ERROR_NONE;
}

int ExynosExternalDisplay::disable()
{
    ALOGI("[ExternalDisplay] %s +", __func__);

    if (!mEnabled)
        return HWC2_ERROR_NONE;

    if (mSkipStartFrame > (SKIP_EXTERNAL_FRAME - 1)) {
        clearDisplay();
    } else {
        ALOGI("Skip clearDisplay to avoid resource conflict");
    }

    if (mDisplayInterface->setPowerMode(HWC_POWER_MODE_OFF) < 0){
        DISPLAY_LOGE("set powermode ioctl failed errno : %d", errno);
        return HWC2_ERROR_UNSUPPORTED;
    }

    this->mPowerModeState = (hwc2_power_mode_t)HWC_POWER_MODE_OFF;

    ALOGI("[ExternalDisplay] %s -", __func__);

    return HWC2_ERROR_NONE;
}

int32_t ExynosExternalDisplay::setPowerMode(
        int32_t /*hwc2_power_mode_t*/ mode) {
    Mutex::Autolock lock(mExternalMutex);
    {
        Mutex::Autolock lock(mDisplayMutex);

        /* TODO state check routine should be added */

        int fb_blank = 0;
        int err = 0;
        if (mode == HWC_POWER_MODE_OFF) {
            fb_blank = FB_BLANK_POWERDOWN;
            err = disable();
        } else {
            fb_blank = FB_BLANK_UNBLANK;
            err = enable();
        }

        if (err != 0) {
            DISPLAY_LOGE("set powermode ioctl failed errno : %d", errno);
            return HWC2_ERROR_UNSUPPORTED;
        }

        if (fb_blank == FB_BLANK_POWERDOWN)
            mDREnable = false;
        else if (fb_blank == FB_BLANK_UNBLANK)
            mDREnable = mDRDefault;

        // check the dynamic recomposition thread by following display power status
        mDevice->checkDynamicRecompositionThread();

        mPowerModeState = (hwc2_power_mode_t)mode;

        DISPLAY_LOGD(eDebugExternalDisplay, "%s:: mode(%d), blank(%d)", __func__, mode, fb_blank);

        if (mode == HWC_POWER_MODE_OFF) {
            /* It should be called from validate() when the screen is on */
            mSkipFrame = true;
            setGeometryChanged(GEOMETRY_DISPLAY_POWER_OFF);
            if ((mRenderingState >= RENDERING_STATE_VALIDATED) &&
                (mRenderingState < RENDERING_STATE_PRESENTED))
                closeFencesForSkipFrame(RENDERING_STATE_VALIDATED);
            mRenderingState = RENDERING_STATE_NONE;
        } else {
            setGeometryChanged(GEOMETRY_DISPLAY_POWER_ON);
        }
    }
    return HWC2_ERROR_NONE;
}

int32_t ExynosExternalDisplay::setVsyncEnabled(
        int32_t /*hwc2_vsync_t*/ enabled) {
    __u32 val = !!enabled;

    DISPLAY_LOGD(eDebugExternalDisplay, "enabled : %d", enabled);

    if (mDisplayInterface->setVsyncEnabled(val) < 0) {
        DISPLAY_LOGI("Vsync ioctl failed errno : %d", errno);
        return HWC2_ERROR_BAD_DISPLAY;
    }

    DISPLAY_LOGI("Vsync of external display is %s.", val ? "enabled" : "disabled");

    return HWC2_ERROR_NONE;
}

int32_t ExynosExternalDisplay::startPostProcessing() {
    if ((mHpdStatus == false) || (mBlanked == true) || mIsSkipFrame) {
        ALOGI("%s:: skip startPostProcessing display(%d) mHpdStatus(%d)",
                __func__, mDisplayId, mHpdStatus);
        return NO_ERROR;
    }
    return ExynosDisplay::startPostProcessing();
}

bool ExynosExternalDisplay::getHDRException(ExynosLayer* __unused layer)
{
    bool ret = false;

    if (mExternalHdrSupported) {
        ret = true;
    }
    return ret;
}

void ExynosExternalDisplay::handleHotplugEvent()
{
    bool hpd_temp = 0;

    if (mDevice->mCallbackInfos[HWC2_CALLBACK_HOTPLUG].callbackData == NULL)
        return;

    char cablestate_name[MAX_DEV_NAME + 1];
    cablestate_name[MAX_DEV_NAME] = '\0';
    sprintf(cablestate_name, DP_CABLE_STATE_NAME, DP_LINK_NAME);

    int sw_fd = open(cablestate_name, O_RDONLY);
    char val;

    if (sw_fd >= 0) {
        if (read(sw_fd, &val, 1) == 1) {
            if (val == '1')
                hpd_temp = true;
            else if (val == '0')
                hpd_temp = false;
        }
        hwcFdClose(sw_fd);
    }

    {
        Mutex::Autolock lock(mExternalMutex);
        {
            Mutex::Autolock lock(mDisplayMutex);
            mHpdStatus = hpd_temp;
            if (mHpdStatus) {
                if (openExternalDisplay() < 0) {
                    ALOGE("Failed to openExternalDisplay");
                    mHpdStatus = false;
                    return;
                }
            }
            else {
                disable();
                closeExternalDisplay();
            }
            hotplug();
            mDevice->invalidate();
        }
    }

    ALOGI("HPD status changed to %s", mHpdStatus ? "enabled" : "disabled");
}

void ExynosExternalDisplay::initDisplayInterface(uint32_t __unused interfaceType)
{
    mDisplayInterface = new ExynosExternalDisplayFbInterface((ExynosDisplay *)this);
    mDisplayInterface->init(this);
}

ExynosExternalDisplay::ExynosExternalDisplayFbInterface::ExynosExternalDisplayFbInterface(ExynosDisplay *exynosDisplay)
    : ExynosDisplayFbInterface(exynosDisplay)
{
}

void ExynosExternalDisplay::ExynosExternalDisplayFbInterface::init(ExynosDisplay *exynosDisplay)
{
    mDisplayFd = open(DECON_EXTERNAL_DEV_NAME, O_RDWR);
    if (mDisplayFd < 0)
        ALOGE("%s:: failed to open framebuffer", __func__);

    mExynosDisplay = exynosDisplay;
    mExternalDisplay = (ExynosExternalDisplay *)exynosDisplay;

    memset(&dv_timings, 0, sizeof(dv_timings));
}

int32_t ExynosExternalDisplay::ExynosExternalDisplayFbInterface::getDisplayAttribute(
        hwc2_config_t config,
        int32_t attribute, int32_t* outValue)
{
    if (config >= SUPPORTED_DV_TIMINGS_NUM) {
        HWC_LOGE(mExternalDisplay, "%s:: Invalid config(%d), mConfigurations(%zu)", __func__, config, mConfigurations.size());
        return -EINVAL;
    }

    v4l2_dv_timings dv_timing = dv_timings[config];
    switch(attribute) {
    case HWC2_ATTRIBUTE_VSYNC_PERIOD:
        {
            *outValue = calVsyncPeriod(dv_timing);
            break;
        }
    case HWC2_ATTRIBUTE_WIDTH:
        *outValue = dv_timing.bt.width;
        break;

    case HWC2_ATTRIBUTE_HEIGHT:
        *outValue = dv_timing.bt.height;
        break;

    case HWC2_ATTRIBUTE_DPI_X:
        *outValue = mExternalDisplay->mXdpi;
        break;

    case HWC2_ATTRIBUTE_DPI_Y:
        *outValue = mExternalDisplay->mYdpi;
        break;

    default:
        HWC_LOGE(mExternalDisplay, "%s unknown display attribute %u",
                mExternalDisplay->mDisplayName.string(), attribute);
        return HWC2_ERROR_BAD_CONFIG;
    }

    return HWC2_ERROR_NONE;
}


int32_t ExynosExternalDisplay::ExynosExternalDisplayFbInterface::getDisplayConfigs(
        uint32_t* outNumConfigs,
        hwc2_config_t* outConfigs)
{
    int ret = 0;
    exynos_displayport_data dp_data;
    size_t index = 0;

    if (outConfigs != NULL) {
        while (index < *outNumConfigs) {
            outConfigs[index] = mConfigurations[index];
            index++;
        }

        dp_data.timings = dv_timings[outConfigs[0]];
        dp_data.state = dp_data.EXYNOS_DISPLAYPORT_STATE_PRESET;
        if(ioctl(this->mDisplayFd, EXYNOS_SET_DISPLAYPORT_CONFIG, &dp_data) <0) {
            HWC_LOGE(mExternalDisplay, "%s fail to send selected config data, %d",
                    mExternalDisplay->mDisplayName.string(), errno);
            return -1;
        }

        mExternalDisplay->mXres = dv_timings[outConfigs[0]].bt.width;
        mExternalDisplay->mYres = dv_timings[outConfigs[0]].bt.height;
        mExternalDisplay->mVsyncPeriod = calVsyncPeriod(dv_timings[outConfigs[0]]);
        HDEBUGLOGD(eDebugExternalDisplay, "ExternalDisplay is connected to (%d x %d, %d fps) sink",
                mExternalDisplay->mXres, mExternalDisplay->mYres, mExternalDisplay->mVsyncPeriod);

        dumpDisplayConfigs();

        return HWC2_ERROR_NONE;
    }

    memset(&dv_timings, 0, sizeof(dv_timings));
    cleanConfigurations();

    /* configs store the index of mConfigurations */
    dp_data.state = dp_data.EXYNOS_DISPLAYPORT_STATE_ENUM_PRESET;
    while (index < SUPPORTED_DV_TIMINGS_NUM) {
        dp_data.etimings.index = index;
        ret = ioctl(this->mDisplayFd, EXYNOS_GET_DISPLAYPORT_CONFIG, &dp_data);
        if (ret < 0) {
            if (errno == EINVAL) {
                HDEBUGLOGD(eDebugExternalDisplay, "%s:: Unmatched config index %zu", __func__, index);
                index++;
                continue;
            }
            else if (errno == E2BIG) {
                HDEBUGLOGD(eDebugExternalDisplay, "%s:: Total configurations %zu", __func__, index);
                break;
            }
            HWC_LOGE(mExternalDisplay, "%s: enum_dv_timings error, %d", __func__, errno);
            return -1;
        }

        dv_timings[index] = dp_data.etimings.timings;
        mConfigurations.push_back(index);
        index++;
    }

    if (mConfigurations.size() == 0){
        HWC_LOGE(mExternalDisplay, "%s do not receivce any configuration info",
                mExternalDisplay->mDisplayName.string());
        mExternalDisplay->closeExternalDisplay();
        return -1;
    }

    int config = 0;
    v4l2_dv_timings temp_dv_timings = dv_timings[mConfigurations[mConfigurations.size()-1]];
    for (config = 0; config < (int)mConfigurations[mConfigurations.size()-1]; config++) {
        if (dv_timings[config].bt.width != 0) {
            dv_timings[mConfigurations[mConfigurations.size()-1]] = dv_timings[config];
            break;
        }
    }

    dv_timings[config] = temp_dv_timings;
    mExternalDisplay->mActiveConfigIndex = config;

    *outNumConfigs = mConfigurations.size();

    return 0;
}

void ExynosExternalDisplay::ExynosExternalDisplayFbInterface::cleanConfigurations()
{
    mConfigurations.clear();
}

void ExynosExternalDisplay::ExynosExternalDisplayFbInterface::dumpDisplayConfigs()
{
    HDEBUGLOGD(eDebugExternalDisplay, "External display configurations:: total(%zu), active configuration(%d)",
            mConfigurations.size(), mExternalDisplay->mActiveConfigIndex);

    for (size_t i = 0; i <  mConfigurations.size(); i++ ) {
        unsigned int dv_timings_index = mConfigurations[i];
        v4l2_dv_timings configuration = dv_timings[dv_timings_index];
        float refresh_rate = (float)((float)configuration.bt.pixelclock /
                ((configuration.bt.width + configuration.bt.hfrontporch + configuration.bt.hsync + configuration.bt.hbackporch) *
                 (configuration.bt.height + configuration.bt.vfrontporch + configuration.bt.vsync + configuration.bt.vbackporch)));
        uint32_t vsyncPeriod = 1000000000 / refresh_rate;
        HDEBUGLOGD(eDebugExternalDisplay, "%zu : index(%d) type(%d), %d x %d, fps(%f), vsyncPeriod(%d)", i, dv_timings_index, configuration.type, configuration.bt.width,
                configuration.bt.height,
                refresh_rate, vsyncPeriod);
    }
}

int32_t ExynosExternalDisplay::ExynosExternalDisplayFbInterface::calVsyncPeriod(v4l2_dv_timings dv_timing)
{
    int32_t result;
    float refreshRate = (float)((float)dv_timing.bt.pixelclock /
            ((dv_timing.bt.width + dv_timing.bt.hfrontporch + dv_timing.bt.hsync + dv_timing.bt.hbackporch) *
             (dv_timing.bt.height + dv_timing.bt.vfrontporch + dv_timing.bt.vsync + dv_timing.bt.vbackporch)));

    result = (1000000000/refreshRate);
    return result;
}

int32_t ExynosExternalDisplay::ExynosExternalDisplayFbInterface::getHdrCapabilities(
        uint32_t* outNumTypes, int32_t* outTypes, float* outMaxLuminance,
        float* outMaxAverageLuminance, float* outMinLuminance)
{
    HDEBUGLOGD(eDebugExternalDisplay, "HWC2: %s, %d", __func__, __LINE__);
    if (outTypes == NULL) {
        struct decon_hdr_capabilities_info outInfo;
        memset(&outInfo, 0, sizeof(outInfo));

        exynos_displayport_data dp_data;
        memset(&dp_data, 0, sizeof(dp_data));
        dp_data.state = dp_data.EXYNOS_DISPLAYPORT_STATE_HDR_INFO;
        int ret = ioctl(mDisplayFd, EXYNOS_GET_DISPLAYPORT_CONFIG, &dp_data);
        if (ret < 0) {
            ALOGE("%s: EXYNOS_DISPLAYPORT_STATE_HDR_INFO ioctl error, %d", __func__, errno);
        }

        mExternalDisplay->mExternalHdrSupported = dp_data.hdr_support;
        if (ioctl(mDisplayFd, S3CFB_GET_HDR_CAPABILITIES_NUM, &outInfo) < 0) {
            ALOGE("getHdrCapabilities: S3CFB_GET_HDR_CAPABILITIES_NUM ioctl failed");
            return -1;
        }

        if (mExternalDisplay->mExternalHdrSupported) {
            *outMaxLuminance = 50 * pow(2.0 ,(double)outInfo.max_luminance / 32);
            *outMaxAverageLuminance = 50 * pow(2.0 ,(double)outInfo.max_average_luminance / 32);
            *outMinLuminance = *outMaxLuminance * (float)pow(outInfo.min_luminance, 2.0) / pow(255.0, 2.0) / (float)100;
        }
        else {
            *outMaxLuminance = (float)outInfo.max_luminance / (float)10000;
            *outMaxAverageLuminance = (float)outInfo.max_average_luminance / (float)10000;
            *outMinLuminance = (float)outInfo.min_luminance / (float)10000;
        }

#ifndef USES_HDR_GLES_CONVERSION
        mExternalDisplay->mExternalHdrSupported = 0;
#endif

        *outNumTypes = outInfo.out_num;
        // Save to member variables
        mExternalDisplay->mHdrTypeNum = *outNumTypes;
        mExternalDisplay->mMaxLuminance = *outMaxLuminance;
        mExternalDisplay->mMaxAverageLuminance = *outMaxAverageLuminance;
        mExternalDisplay->mMinLuminance = *outMinLuminance;
        ALOGI("%s: hdrTypeNum(%d), maxLuminance(%f), maxAverageLuminance(%f), minLuminance(%f), externalHdrSupported(%d)",
                mExternalDisplay->mDisplayName.string(), mExternalDisplay->mHdrTypeNum,
                mExternalDisplay->mMaxLuminance, mExternalDisplay->mMaxAverageLuminance,
                mExternalDisplay->mMinLuminance, mExternalDisplay->mExternalHdrSupported);
        return 0;
    }

    struct decon_hdr_capabilities outData;
    memset(&outData, 0, sizeof(outData));

    for (uint32_t i = 0; i < *outNumTypes; i += SET_HDR_CAPABILITIES_NUM) {
        if (ioctl(mDisplayFd, S3CFB_GET_HDR_CAPABILITIES, &outData) < 0) {
            ALOGE("getHdrCapabilities: S3CFB_GET_HDR_CAPABILITIES ioctl Failed");
            return -1;
        }
        for (uint32_t j = 0; j < *outNumTypes - i; j++)
            outTypes[i+j] = outData.out_types[j];
        // Save to member variables
        mExternalDisplay->mHdrTypes[i] = (android_hdr_t)outData.out_types[i];
        HDEBUGLOGD(eDebugExternalDisplay, "HWC2: Types : %d", mExternalDisplay->mHdrTypes[i]);
    }
    return 0;
}
