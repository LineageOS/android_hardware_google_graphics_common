/*
 * Copyright (C) 2022 The Android Open Source Project
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
#include "histogram_mediator.h"

histogram::HistogramMediator::HistogramMediator(ExynosDisplay *display) {
    mDisplay = display;
    ExynosDisplayDrmInterface *moduleDisplayInterface =
            static_cast<ExynosDisplayDrmInterface *>(display->mDisplayInterface.get());

    moduleDisplayInterface->registerHistogramInfo(static_cast<IDLHistogram *>(&mIDLHistogram));
}

bool histogram::HistogramMediator::isDisplayPowerOff() {
    if ((mDisplay->mPowerModeState == HWC2_POWER_MODE_OFF) ||
        (mDisplay->mPowerModeState == HWC2_POWER_MODE_DOZE)) {
        return true;
    }
    return false;
}

bool histogram::HistogramMediator::isSecureContentPresenting() {
    for (uint32_t i = 0; i < mDisplay->mLayers.size(); i++) {
        ExynosLayer *layer = mDisplay->mLayers[i];
        if (layer->isDrm()) { /* there is some DRM layer */
            return true;
        }
    }
    return false;
}
histogram::HistogramErrorCode histogram::HistogramMediator::enableHist() {
    if (isSecureContentPresenting()) { /* there is some DRM layer */
        return histogram::HistogramErrorCode::DRM_PLAYING;
    }
    ExynosDisplayDrmInterface *moduleDisplayInterface =
            static_cast<ExynosDisplayDrmInterface *>(mDisplay->mDisplayInterface.get());

    if (moduleDisplayInterface->setHistogramControl(
                hidl_histogram_control_t::HISTOGRAM_CONTROL_REQUEST) != NO_ERROR) {
        return histogram::HistogramErrorCode::ENABLE_HIST_ERROR;
    }
    return histogram::HistogramErrorCode::NONE;
}

histogram::HistogramErrorCode histogram::HistogramMediator::disableHist() {
    ExynosDisplayDrmInterface *moduleDisplayInterface =
            static_cast<ExynosDisplayDrmInterface *>(mDisplay->mDisplayInterface.get());

    if ((moduleDisplayInterface->setHistogramControl(
                 hidl_histogram_control_t::HISTOGRAM_CONTROL_CANCEL) != NO_ERROR)) {
        return histogram::HistogramErrorCode::DISABLE_HIST_ERROR;
    }
    return histogram::HistogramErrorCode::NONE;
}

void histogram::HistogramMediator::HistogramReceiver::callbackHistogram(char16_t *bin) {
    std::memcpy(mHistData, bin, HISTOGRAM_BINS_SIZE * sizeof(char16_t));
    mHistData_available = true;
    mHistData_cv.notify_all();
}

int histogram::HistogramMediator::calculateThreshold(const RoiRect &roi) {
    int threshold = ((roi.bottom - roi.top) * (roi.right - roi.left)) >> 16;
    return threshold + 1;
}

histogram::HistogramErrorCode histogram::HistogramMediator::setRoiWeightThreshold(
        const RoiRect roi, const Weight weight, const HistogramPos pos) {
    int threshold = calculateThreshold(roi);
    mIDLHistogram.setHistogramROI((uint16_t)roi.left, (uint16_t)roi.top,
                                  (uint16_t)(roi.right - roi.left),
                                  (uint16_t)(roi.bottom - roi.top));
    mIDLHistogram.setHistogramWeights(weight.weightR, weight.weightG, weight.weightB);
    mIDLHistogram.setHistogramThreshold(threshold);
    mIDLHistogram.setHistogramPos(pos);

    return histogram::HistogramErrorCode::NONE;
}

histogram::HistogramErrorCode histogram::HistogramMediator::collectRoiLuma(
        std::vector<char16_t> *buf) {
    std::mutex mDataCollectingMutex; // for data collecting operations
    std::unique_lock<std::mutex> lk(mDataCollectingMutex);

    mIDLHistogram.mHistData_cv.wait_for(lk, std::chrono::milliseconds(50), [this]() {
        return (!isDisplayPowerOff() && mIDLHistogram.mHistData_available);
    });
    buf->assign(mIDLHistogram.mHistData, mIDLHistogram.mHistData + HISTOGRAM_BINS_SIZE);
    mIDLHistogram.mHistData_available = false;

    return histogram::HistogramErrorCode::NONE;
}