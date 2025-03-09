/*
 * Copyright (C) 2023 The Android Open Source Project
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

#define ATRACE_TAG (ATRACE_TAG_GRAPHICS | ATRACE_TAG_HAL)

#include "VariableRefreshRateController.h"

#include <android-base/logging.h>
#include <processgroup/sched_policy.h>
#include <sync/sync.h>
#include <utils/Trace.h>

#include "ExynosHWCHelper.h"
#include "drmmode.h"

#include <chrono>
#include <tuple>

#include "RefreshRateCalculator/RefreshRateCalculatorFactory.h"
#include "display/DisplayContextProviderFactory.h"
#include "interface/Panel_def.h"

namespace android::hardware::graphics::composer {

namespace {

using android::hardware::graphics::composer::VrrControllerEventType;

}

static OperationSpeedMode getOperationSpeedModeWrapper(void* host) {
    VariableRefreshRateController* controller =
            reinterpret_cast<VariableRefreshRateController*>(host);
    return controller->getOperationSpeedMode();
}

static BrightnessMode getBrightnessModeWrapper(void* host) {
    VariableRefreshRateController* controller =
            reinterpret_cast<VariableRefreshRateController*>(host);
    return controller->getBrightnessMode();
}

static int getBrightnessNitsWrapper(void* host) {
    VariableRefreshRateController* controller =
            reinterpret_cast<VariableRefreshRateController*>(host);
    return controller->getBrightnessNits();
}

static const char* getDisplayFileNodePathWrapper(void* host) {
    VariableRefreshRateController* controller =
            reinterpret_cast<VariableRefreshRateController*>(host);
    return controller->getDisplayFileNodePath();
}

static int getEstimateVideoFrameRateWrapper(void* host) {
    VariableRefreshRateController* controller =
            reinterpret_cast<VariableRefreshRateController*>(host);
    return controller->getEstimatedVideoFrameRate();
}

static int getAmbientLightSensorOutputWrapper(void* host) {
    VariableRefreshRateController* controller =
            reinterpret_cast<VariableRefreshRateController*>(host);
    return controller->getAmbientLightSensorOutput();
}

static bool isProximityThrottlingEnabledWrapper(void* host) {
    VariableRefreshRateController* controller =
            reinterpret_cast<VariableRefreshRateController*>(host);
    return controller->isProximityThrottlingEnabled();
}

auto VariableRefreshRateController::CreateInstance(ExynosDisplay* display,
                                                   const std::string& panelName)
        -> std::shared_ptr<VariableRefreshRateController> {
    if (!display) {
        LOG(ERROR)
                << "VrrController: create VariableRefreshRateController without display handler.";
        return nullptr;
    }
    auto controller = std::shared_ptr<VariableRefreshRateController>(
            new VariableRefreshRateController(display, panelName));
    std::thread thread = std::thread(&VariableRefreshRateController::threadBody, controller.get());
    std::string threadName = "VrrCtrl_";
    threadName += display->mIndex == 0 ? "Primary" : "Second";
    int error = pthread_setname_np(thread.native_handle(), threadName.c_str());
    if (error != 0) {
        LOG(WARNING) << "VrrController: Unable to set thread name, error = " << strerror(error);
    }
    thread.detach();

    return controller;
}

VariableRefreshRateController::VariableRefreshRateController(ExynosDisplay* display,
                                                             const std::string& panelName)
      : mDisplay(display), mPanelName(panelName), mPendingVendorRenderingTimeoutTasks(this) {
    mState = VrrControllerState::kDisable;
    std::string displayFileNodePath = mDisplay->getPanelSysfsPath();
    if (displayFileNodePath.empty()) {
        LOG(WARNING) << "VrrController: Cannot find file node of display: "
                     << mDisplay->mDisplayName;
    } else {
        auto& fileNodeManager =
                android::hardware::graphics::composer::FileNodeManager::getInstance();
        mFileNode = fileNodeManager.getFileNode(displayFileNodePath);
        auto content = mFileNode->readString(kRefreshControlNodeName);
        if (!(content.has_value()) ||
            (content.value().compare(0, kRefreshControlNodeEnabled.length(),
                                     kRefreshControlNodeEnabled))) {
            LOG(ERROR) << "VrrController: RefreshControlNode is not enabled";
        }
    }

    // Initialize DisplayContextProviderInterface.
    mDisplayContextProviderInterface.getOperationSpeedMode = (&getOperationSpeedModeWrapper);
    mDisplayContextProviderInterface.getBrightnessMode = (&getBrightnessModeWrapper);
    mDisplayContextProviderInterface.getBrightnessNits = (&getBrightnessNitsWrapper);
    mDisplayContextProviderInterface.getDisplayFileNodePath = (&getDisplayFileNodePathWrapper);
    mDisplayContextProviderInterface.getEstimatedVideoFrameRate =
            (&getEstimateVideoFrameRateWrapper);
    mDisplayContextProviderInterface.getAmbientLightSensorOutput =
            (&getAmbientLightSensorOutputWrapper);
    mDisplayContextProviderInterface.isProximityThrottlingEnabled =
            (&isProximityThrottlingEnabledWrapper);

    // Flow to build refresh rate calculator.
    RefreshRateCalculatorFactory refreshRateCalculatorFactory;
    std::vector<std::shared_ptr<RefreshRateCalculator>> Calculators;

    Calculators.emplace_back(std::move(
            refreshRateCalculatorFactory
                    .BuildRefreshRateCalculator(&mEventQueue, RefreshRateCalculatorType::kAod)));
    Calculators.emplace_back(
            std::move(refreshRateCalculatorFactory
                              .BuildRefreshRateCalculator(&mEventQueue,
                                                          RefreshRateCalculatorType::kExitIdle)));
    // videoFrameRateCalculator will be shared with display context provider.
    auto videoFrameRateCalculator =
            refreshRateCalculatorFactory
                    .BuildRefreshRateCalculator(&mEventQueue,
                                                RefreshRateCalculatorType::kVideoPlayback);
    Calculators.emplace_back(videoFrameRateCalculator);

    PeriodRefreshRateCalculatorParameters peridParams;
    peridParams.mConfidencePercentage = 0;
    Calculators.emplace_back(std::move(
            refreshRateCalculatorFactory.BuildRefreshRateCalculator(&mEventQueue, peridParams)));

    mRefreshRateCalculator =
            refreshRateCalculatorFactory.BuildRefreshRateCalculator(std::move(Calculators));
    mRefreshRateCalculator->registerRefreshRateChangeCallback(
            std::bind(&VariableRefreshRateController::onRefreshRateChanged, this,
                      std::placeholders::_1));

    mPowerModeListeners.push_back(mRefreshRateCalculator.get());

    if (mFileNode->getFileHandler(kFrameRateNodeName) >= 0) {
        mFrameRateReporter =
                refreshRateCalculatorFactory
                        .BuildRefreshRateCalculator(&mEventQueue,
                                                    RefreshRateCalculatorType::kInstant);
        mFrameRateReporter->registerRefreshRateChangeCallback(
                std::bind(&VariableRefreshRateController::onFrameRateChangedForDBI, this,
                          std::placeholders::_1));
    }

    DisplayContextProviderFactory displayContextProviderFactory(mDisplay, this, &mEventQueue);
    mDisplayContextProvider =
            displayContextProviderFactory
                    .buildDisplayContextProvider(DisplayContextProviderType::kExynos,
                                                 std::move(videoFrameRateCalculator));

    mPresentTimeoutEventHandlerLoader.reset(
            new ExternalEventHandlerLoader(std::string(kVendorDisplayPanelLibrary).c_str(),
                                           &mDisplayContextProviderInterface, this,
                                           mPanelName.c_str()));
    mPresentTimeoutEventHandler = mPresentTimeoutEventHandlerLoader->getEventHandler();

    mVariableRefreshRateStatistic =
            std::make_shared<VariableRefreshRateStatistic>(mDisplayContextProvider.get(),
                                                           &mEventQueue, kMaxFrameRate,
                                                           kMaxTefrequency,
                                                           (1 * std::nano::den /*1 second*/));
    mPowerModeListeners.push_back(mVariableRefreshRateStatistic.get());

    mResidencyWatcher =
            ndk::SharedRefBase::make<DisplayStateResidencyWatcher>(mDisplayContextProvider,
                                                                   mVariableRefreshRateStatistic);
}

VariableRefreshRateController::~VariableRefreshRateController() {
    stopThread(true);

    const std::lock_guard<std::mutex> lock(mMutex);
    if (mLastPresentFence.has_value()) {
        if (close(mLastPresentFence.value())) {
            LOG(ERROR) << "VrrController: close fence file failed, errno = " << errno;
        }
        mLastPresentFence = std::nullopt;
    }
};

int VariableRefreshRateController::notifyExpectedPresent(int64_t timestamp,
                                                         int32_t frameIntervalNs) {
    ATRACE_CALL();
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        mRecord.mNextExpectedPresentTime = {mVrrActiveConfig, timestamp, frameIntervalNs};
        // Post kNotifyExpectedPresentConfig event.
        postEvent(VrrControllerEventType::kNotifyExpectedPresentConfig, getSteadyClockTimeNs());
    }

    if (mFileNode == nullptr) {
        LOG(WARNING) << "VrrController: Cannot find file node of display: "
                     << mDisplay->mDisplayName;
    } else {
        if (!mFileNode->writeValue("expected_present_time_ns", timestamp)) {
            std::string displayFileNodePath = mDisplay->getPanelSysfsPath();
            ALOGE("%s(): write command to file node %s%s failed.", __func__,
                  displayFileNodePath.c_str(), "expect_present_time");
        }

        if (!mFileNode->writeValue("frame_interval_ns", frameIntervalNs)) {
            std::string displayFileNodePath = mDisplay->getPanelSysfsPath();
            ALOGE("%s(): write command to file node %s%s failed.", __func__,
                  displayFileNodePath.c_str(), "frame_interval");
        }
    }

    mCondition.notify_all();
    return 0;
}

void VariableRefreshRateController::reset() {
    ATRACE_CALL();

    const std::lock_guard<std::mutex> lock(mMutex);
    mEventQueue.mPriorityQueue = std::priority_queue<VrrControllerEvent>();
    mRecord.clear();
    dropEventLocked();
    if (mLastPresentFence.has_value()) {
        if (close(mLastPresentFence.value())) {
            LOG(ERROR) << "VrrController: close fence file failed, errno = " << errno;
        }
        mLastPresentFence = std::nullopt;
    }
}

void VariableRefreshRateController::setActiveVrrConfiguration(hwc2_config_t config) {
    LOG(INFO) << "VrrController: Set active Vrr configuration = " << config
              << ", power mode = " << mPowerMode;
    ATRACE_CALL();
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        if (mVrrConfigs.count(config) == 0) {
            LOG(ERROR) << "VrrController: Set an undefined active configuration";
            return;
        }
        if (mFileNode &&
            mFileNode->writeValue("expected_present_time_ns", mLastExpectedPresentTimeNs)) {
            ATRACE_NAME("WriteExpectedPresentTime");
        } else {
            std::string displayFileNodePath = mDisplay->getPanelSysfsPath();
            ALOGE("%s(): write command to file node %s%s failed.", __func__,
                  displayFileNodePath.c_str(), "expected_present_time_ns");
        }
        if (mFrameRateReporter) {
            mFrameRateReporter->onPresent(getSteadyClockTimeNs(), 0);
        }
        const auto oldMaxFrameRate =
                durationNsToFreq(mVrrConfigs[mVrrActiveConfig].minFrameIntervalNs);
        mVrrActiveConfig = config;
        if ((mPendingMinimumRefreshRateRequest) &&
            (durationNsToFreq(mVrrConfigs[mVrrActiveConfig].vsyncPeriodNs) ==
             durationNsToFreq(mVrrConfigs[mVrrActiveConfig].minFrameIntervalNs))) {
            LOG(INFO) << "The configuration is ready to set minimum refresh rate = "
                      << mMinimumRefreshRate;
            ATRACE_NAME("pending_minimum refresh_rate_with_target_config");
            if (mLastExpectedPresentTimeNs > getSteadyClockTimeNs()) {
                // An upcoming presentation requires aligning the minimum refresh rate configuration
                // with the presentation cadence. Additionally, we can optimize by combining the
                // minimum refresh rate adjustment with the upcoming presentation to directly
                // transition to the maximum refresh rate state.
                auto aheadOfTimeNs =
                        std::min((static_cast<int64_t>(mVrrConfigs[mVrrActiveConfig].vsyncPeriodNs /
                                                       2)),
                                 (2 * kMillisecondToNanoSecond) /*200 ms*/);
                auto scheduledTimeNs = (mLastExpectedPresentTimeNs - aheadOfTimeNs);
                if (getSteadyClockTimeNs() > scheduledTimeNs) {
                    scheduledTimeNs += mVrrConfigs[mVrrActiveConfig].vsyncPeriodNs;
                }
                createMinimumRefreshRateTimeoutEventLocked();
                postEvent(VrrControllerEventType::kMinimumRefreshRateAlignWithPresent,
                          scheduledTimeNs);
            } else {
                mMinimumRefreshRate = mPendingMinimumRefreshRateRequest.value();
                setFixedRefreshRateRangeWorker();
                mPendingMinimumRefreshRateRequest = std::nullopt;
            }
        }
        // If the minimum refresh rate is active and the maximum refresh rate timeout is set,
        // also we are stay at the maximum refresh rate, any change in the active configuration
        // needs to reconfigure the maximum refresh rate according to the newly activated
        // configuration.
        else if (mMinimumRefreshRatePresentState >= kAtMaximumRefreshRate) {
            if (isMinimumRefreshRateActive() && (mMaximumRefreshRateTimeoutNs > 0)) {
                uint32_t command = getCurrentRefreshControlStateLocked();
                auto newMaxFrameRate = durationNsToFreq(mVrrConfigs[config].minFrameIntervalNs);
                setBitField(command, newMaxFrameRate, kPanelRefreshCtrlMinimumRefreshRateOffset,
                            kPanelRefreshCtrlMinimumRefreshRateMask);
                if (!mFileNode->writeValue(composer::kRefreshControlNodeName, command)) {
                    LOG(WARNING) << "VrrController: write file node error, command = " << command;
                }
                ATRACE_INT(kMinimumRefreshRateConfiguredTraceName, newMaxFrameRate);
                onRefreshRateChangedInternal(newMaxFrameRate);
                LOG(INFO) << "VrrController: update maximum refresh rate from " << oldMaxFrameRate
                          << " to " << newMaxFrameRate;
            } else {
                LOG(ERROR) << "VrrController: MinimumRefreshRatePresentState cannot be "
                           << mMinimumRefreshRatePresentState
                           << " when minimum refresh rate = " << mMinimumRefreshRate
                           << " , mMaximumRefreshRateTimeoutNs = " << mMaximumRefreshRateTimeoutNs;
            }
        }
        if (mVariableRefreshRateStatistic) {
            mVariableRefreshRateStatistic
                    ->setActiveVrrConfiguration(config,
                                                durationNsToFreq(mVrrConfigs[mVrrActiveConfig]
                                                                         .vsyncPeriodNs));
        }
        reportRefreshRateIndicator();
        if (mState == VrrControllerState::kDisable) {
            return;
        }
        mState = VrrControllerState::kRendering;
        dropEventLocked(VrrControllerEventType::kSystemRenderingTimeout);

        if (mVrrConfigs[mVrrActiveConfig].isFullySupported) {
            postEvent(VrrControllerEventType::kSystemRenderingTimeout,
                      getSteadyClockTimeNs() +
                              mVrrConfigs[mVrrActiveConfig].notifyExpectedPresentConfig->TimeoutNs);
        }
        if (mRefreshRateCalculator) {
            mRefreshRateCalculator
                    ->setVrrConfigAttributes(mVrrConfigs[mVrrActiveConfig].vsyncPeriodNs,
                                             mVrrConfigs[mVrrActiveConfig].minFrameIntervalNs);
        }
        if (mFrameRateReporter) {
            mFrameRateReporter
                    ->setVrrConfigAttributes(mVrrConfigs[mVrrActiveConfig].vsyncPeriodNs,
                                             mVrrConfigs[mVrrActiveConfig].minFrameIntervalNs);
        }
    }
    mCondition.notify_all();
}

void VariableRefreshRateController::setEnable(bool isEnabled) {
    ATRACE_CALL();
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        if (mEnabled == isEnabled) {
            return;
        }
        mEnabled = isEnabled;
        if (mEnabled == false) {
            dropEventLocked();
        }
    }
    mCondition.notify_all();
}

void VariableRefreshRateController::preSetPowerMode(int32_t powerMode) {
    ATRACE_CALL();
    LOG(INFO) << "VrrController: preSet power mode to " << powerMode << ", from " << mPowerMode;
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        if (mPowerMode == powerMode) {
            return;
        }
        switch (powerMode) {
            case HWC_POWER_MODE_DOZE:
            case HWC_POWER_MODE_DOZE_SUSPEND: {
                uint32_t command = getCurrentRefreshControlStateLocked();
                setBit(command, kPanelRefreshCtrlFrameInsertionAutoModeOffset);
                mPresentTimeoutController = PresentTimeoutControllerType::kHardware;
                if (!mFileNode->writeValue(kRefreshControlNodeName, command)) {
                    LOG(ERROR) << "VrrController: write file node error, command = " << command;
                }
                cancelPresentTimeoutHandlingLocked();
                return;
            }
            case HWC_POWER_MODE_OFF: {
                return;
            }
            case HWC_POWER_MODE_NORMAL: {
                mPresentTimeoutController = mDefaultPresentTimeoutController;
                return;
            }
            default: {
                LOG(ERROR) << "VrrController: Unknown power mode = " << powerMode;
                return;
            }
        }
    }
}

void VariableRefreshRateController::postSetPowerMode(int32_t powerMode) {
    ATRACE_CALL();
    LOG(INFO) << "VrrController: postSet power mode to " << powerMode << ", from " << mPowerMode;
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        if (mPowerMode == powerMode) {
            return;
        }
        switch (powerMode) {
            case HWC_POWER_MODE_OFF:
            case HWC_POWER_MODE_DOZE:
            case HWC_POWER_MODE_DOZE_SUSPEND: {
                mState = VrrControllerState::kDisable;
                dropEventLocked(VrrControllerEventType::kGeneralEventMask);
                break;
            }
            case HWC_POWER_MODE_NORMAL: {
                // We should transition from either HWC_POWER_MODE_OFF, HWC_POWER_MODE_DOZE, or
                // HWC_POWER_MODE_DOZE_SUSPEND. At this point, there should be no pending events
                // posted.
                if (!mEventQueue.mPriorityQueue.empty()) {
                    LOG(WARNING) << "VrrController: there should be no pending event when resume "
                                    "from power mode = "
                                 << mPowerMode << " to power mode = " << powerMode;
                    LOG(INFO) << dumpEventQueueLocked();
                }
                mState = VrrControllerState::kRendering;
                const auto& vrrConfig = mVrrConfigs[mVrrActiveConfig];
                if (vrrConfig.isFullySupported) {
                    postEvent(VrrControllerEventType::kSystemRenderingTimeout,
                              getSteadyClockTimeNs() +
                                      vrrConfig.notifyExpectedPresentConfig->TimeoutNs);
                }
                break;
            }
            default: {
                LOG(ERROR) << "VrrController: Unknown power mode = " << powerMode;
                return;
            }
        }
        if (!mPowerModeListeners.empty()) {
            for (const auto& listener : mPowerModeListeners) {
                listener->onPowerStateChange(mPowerMode, powerMode);
            }
        }
        mPowerMode = powerMode;
    }
    mCondition.notify_all();
}

void VariableRefreshRateController::setVrrConfigurations(
        std::unordered_map<hwc2_config_t, VrrConfig_t> configs) {
    ATRACE_CALL();

    std::unordered_map<hwc2_config_t, std::vector<int>> validRefreshRates;
    for (const auto& [id, config] : configs) {
        LOG(INFO) << "VrrController: set Vrr configuration id = " << id;
        if (config.isFullySupported) {
            if (!config.notifyExpectedPresentConfig.has_value()) {
                LOG(ERROR) << "VrrController: full vrr config should have "
                              "notifyExpectedPresentConfig.";
                return;
            }
        }
        validRefreshRates[id] = generateValidRefreshRates(config);
    }

    const std::lock_guard<std::mutex> lock(mMutex);
    mVrrConfigs = std::move(configs);
    mValidRefreshRates = std::move(validRefreshRates);
}

int VariableRefreshRateController::getAmbientLightSensorOutput() const {
    return mDisplayContextProvider->getAmbientLightSensorOutput();
}

BrightnessMode VariableRefreshRateController::getBrightnessMode() const {
    return mDisplayContextProvider->getBrightnessMode();
}

int VariableRefreshRateController::getBrightnessNits() const {
    return mDisplayContextProvider->getBrightnessNits();
}

const char* VariableRefreshRateController::getDisplayFileNodePath() const {
    return mDisplayContextProvider->getDisplayFileNodePath();
}

int VariableRefreshRateController::getEstimatedVideoFrameRate() const {
    return mDisplayContextProvider->getEstimatedVideoFrameRate();
}

OperationSpeedMode VariableRefreshRateController::getOperationSpeedMode() const {
    return mDisplayContextProvider->getOperationSpeedMode();
}

bool VariableRefreshRateController::isProximityThrottlingEnabled() const {
    return mDisplayContextProvider->isProximityThrottlingEnabled();
}

void VariableRefreshRateController::setPresentTimeoutParameters(
        int timeoutNs, const std::vector<std::pair<uint32_t, uint32_t>>& settings) {
    const std::lock_guard<std::mutex> lock(mMutex);

    if (!mPresentTimeoutEventHandler) {
        return;
    }
    if ((timeoutNs >= 0) && (!settings.empty())) {
        auto functor = mPresentTimeoutEventHandler->getHandleFunction();
        mVendorPresentTimeoutOverride = std::make_optional<PresentTimeoutSettings>();
        mVendorPresentTimeoutOverride.value().mTimeoutNs = timeoutNs;
        mVendorPresentTimeoutOverride.value().mFunctor = std::move(functor);
        for (const auto& setting : settings) {
            mVendorPresentTimeoutOverride.value().mSchedule.emplace_back(setting);
        }
    } else {
        mVendorPresentTimeoutOverride = std::nullopt;
    }
}

void VariableRefreshRateController::setPresentTimeoutController(uint32_t controllerType) {
    const std::lock_guard<std::mutex> lock(mMutex);

    if (mPowerMode != HWC_POWER_MODE_NORMAL) {
        LOG(WARNING) << "VrrController: Please change the present timeout controller only when the "
                        "power mode is on.";
        return;
    }

    PresentTimeoutControllerType newDefaultControllerType =
            static_cast<PresentTimeoutControllerType>(controllerType);
    if (newDefaultControllerType != mDefaultPresentTimeoutController) {
        mDefaultPresentTimeoutController = newDefaultControllerType;
        PresentTimeoutControllerType oldControllerType = mPresentTimeoutController;
        if (mDefaultPresentTimeoutController == PresentTimeoutControllerType::kHardware) {
            mPresentTimeoutController = PresentTimeoutControllerType::kHardware;
        } else {
            // When change |mDefaultPresentTimeoutController| from |kHardware| to |kSoftware|,
            // only change |mPresentTimeoutController| if the minimum refresh rate has not been set.
            // Otherwise, retain the current |mPresentTimeoutController| until the conditions are
            // met.
            if (!(isMinimumRefreshRateActive())) {
                mPresentTimeoutController = PresentTimeoutControllerType::kSoftware;
            }
        }
        if (oldControllerType == mPresentTimeoutController) return;
        uint32_t command = getCurrentRefreshControlStateLocked();
        if (mPresentTimeoutController == PresentTimeoutControllerType::kHardware) {
            cancelPresentTimeoutHandlingLocked();
            setBit(command, kPanelRefreshCtrlFrameInsertionAutoModeOffset);
        } else {
            clearBit(command, kPanelRefreshCtrlFrameInsertionAutoModeOffset);
        }
        if (!mFileNode->writeValue(composer::kRefreshControlNodeName, command)) {
            LOG(ERROR) << "VrrController: write file node error, command = " << command;
        }
    }
}

int VariableRefreshRateController::setFixedRefreshRateRange(
        uint32_t minimumRefreshRate, uint64_t minLockTimeForPeakRefreshRate) {
    ATRACE_CALL();
    ATRACE_INT(kMinimumRefreshRateRequestTraceName, minimumRefreshRate);
    const std::lock_guard<std::mutex> lock(mMutex);
    // Discontinue handling fixed refresh rate range settings after power-off, as we will
    // immediately configure it again.
    if (mPowerMode == HWC_POWER_MODE_OFF) {
        return NO_ERROR;
    }
    if (minimumRefreshRate == 0) {
        minimumRefreshRate = 1;
    }
    mMaximumRefreshRateTimeoutNs = minLockTimeForPeakRefreshRate;

    if ((mPendingMinimumRefreshRateRequest) &&
        (mPendingMinimumRefreshRateRequest.value() == minimumRefreshRate)) {
        return NO_ERROR;
    }

    mPendingMinimumRefreshRateRequest = std::nullopt;
    dropEventLocked(VrrControllerEventType::kMinimumRefreshRateControlEventMask);
    if (minimumRefreshRate == mMinimumRefreshRate) {
        return NO_ERROR;
    }

    if ((minimumRefreshRate == 1) ||
        (durationNsToFreq(mVrrConfigs[mVrrActiveConfig].vsyncPeriodNs) ==
         durationNsToFreq(mVrrConfigs[mVrrActiveConfig].minFrameIntervalNs))) {
        mMinimumRefreshRate = minimumRefreshRate;
        return setFixedRefreshRateRangeWorker();
    } else {
        LOG(INFO) << "Set the minimum refresh rate to " << mMinimumRefreshRate
                  << " but wait until the configuration is ready before applying.";
        mPendingMinimumRefreshRateRequest = minimumRefreshRate;
        postEvent(VrrControllerEventType::kMinimumRefreshRateWaitForConfigTimeout,
                  getSteadyClockTimeNs() + kWaitForConfigTimeoutNs);
        return NO_ERROR;
    }
}

int VariableRefreshRateController::setFixedRefreshRateRangeWorker() {
    uint32_t command = getCurrentRefreshControlStateLocked();
    if (isMinimumRefreshRateActive()) {
        cancelPresentTimeoutHandlingLocked();
        // Delegate timeout management to hardware.
        setBit(command, kPanelRefreshCtrlFrameInsertionAutoModeOffset);
        // Configure panel to maintain the minimum refresh rate.
        setBitField(command, mMinimumRefreshRate, kPanelRefreshCtrlMinimumRefreshRateOffset,
                    kPanelRefreshCtrlMinimumRefreshRateMask);
        // TODO(b/333204544): ensure the correct refresh rate is set when calling
        // setFixedRefreshRate().
        // Inform Statistics to stay at the minimum refresh rate change.
        if (mVariableRefreshRateStatistic) {
            mVariableRefreshRateStatistic->setFixedRefreshRate(mMinimumRefreshRate);
        }
        mMinimumRefreshRatePresentState = kAtMinimumRefreshRate;
        createMinimumRefreshRateTimeoutEventLocked();
        if (!mFileNode->writeValue(composer::kRefreshControlNodeName, command)) {
            return -1;
        }
        mPresentTimeoutController = PresentTimeoutControllerType::kHardware;
        // Report refresh rate change.
        onRefreshRateChangedInternal(mMinimumRefreshRate);
    } else {
        // If the minimum refresh rate is 1, check |mDefaultPresentTimeoutController|.
        // Only disable auto mode if |mDefaultPresentTimeoutController| is |kSoftware|.
        mPresentTimeoutController = mDefaultPresentTimeoutController;
        if (mPresentTimeoutController == PresentTimeoutControllerType::kSoftware) {
            clearBit(command, kPanelRefreshCtrlFrameInsertionAutoModeOffset);
            // Configure panel with the minimum refresh rate = 1.
            setBitField(command, 1, kPanelRefreshCtrlMinimumRefreshRateOffset,
                        kPanelRefreshCtrlMinimumRefreshRateMask);
            // Inform Statistics about the minimum refresh rate change.
            if (!mFileNode->writeValue(composer::kRefreshControlNodeName, command)) {
                return -1;
            }
        }
        // TODO(b/333204544): ensure the correct refresh rate is set when calling
        // setFixedRefreshRate().
        if (mVariableRefreshRateStatistic) {
            mVariableRefreshRateStatistic->setFixedRefreshRate(0);
        }
        mMaximumRefreshRateTimeoutNs = 0;
        onRefreshRateChangedInternal(1);
        mMinimumRefreshRateTimeoutEvent = std::nullopt;
        mMinimumRefreshRatePresentState = kMinRefreshRateUnset;
    }
    command = getCurrentRefreshControlStateLocked();
    ATRACE_INT(kMinimumRefreshRateConfiguredTraceName,
               ((command & kPanelRefreshCtrlMinimumRefreshRateMask) >>
                kPanelRefreshCtrlFrameInsertionFrameCountBits));
    return 1;
}

void VariableRefreshRateController::stopThread(bool exit) {
    ATRACE_CALL();
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        mThreadExit = exit;
        mEnabled = false;
        mState = VrrControllerState::kDisable;
    }
    mCondition.notify_all();
}

void VariableRefreshRateController::onPresent(int fence) {
    if (fence < 0) {
        return;
    }
    ATRACE_CALL();
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        if (!mRecord.mPendingCurrentPresentTime.has_value()) {
            LOG(WARNING) << "VrrController: VrrController: Present without expected present time "
                            "information";
            return;
        } else {
            if (mRefreshRateCalculator) {
                mRefreshRateCalculator->onPresent(mRecord.mPendingCurrentPresentTime.value().mTime,
                                                  getPresentFrameFlag());
            }
            if (mFrameRateReporter) {
                mFrameRateReporter->onPresent(mRecord.mPendingCurrentPresentTime.value().mTime, 0);
            }
            if (mVariableRefreshRateStatistic) {
                mVariableRefreshRateStatistic
                        ->onPresent(mRecord.mPendingCurrentPresentTime.value().mTime,
                                    getPresentFrameFlag());
            }
            mRecord.mPresentHistory.next() = mRecord.mPendingCurrentPresentTime.value();
        }
        if (mState == VrrControllerState::kDisable) {
            return;
        } else if (mState == VrrControllerState::kHibernate) {
            LOG(WARNING) << "VrrController: Present during hibernation without prior notification "
                            "via notifyExpectedPresent.";
            mState = VrrControllerState::kRendering;
            dropEventLocked(VrrControllerEventType::kHibernateTimeout);
        }

        if ((mMaximumRefreshRateTimeoutNs > 0) && (mMinimumRefreshRate > 1) &&
            (!mPendingMinimumRefreshRateRequest)) {
            auto maxFrameRate = durationNsToFreq(mVrrConfigs[mVrrActiveConfig].minFrameIntervalNs);
            // If the target minimum refresh rate equals the maxFrameRate, there's no need to
            // promote the refresh rate to maxFrameRate during presentation.
            // E.g. in low-light conditions, with |maxFrameRate| and |mMinimumRefreshRate| both at
            // 120, no refresh rate promotion is needed.
            if (maxFrameRate != mMinimumRefreshRate) {
                if (mMinimumRefreshRatePresentState == kAtMinimumRefreshRate) {
                    if (mPresentTimeoutController != PresentTimeoutControllerType::kHardware) {
                        LOG(WARNING)
                                << "VrrController: incorrect type of present timeout controller.";
                    }
                    uint32_t command = getCurrentRefreshControlStateLocked();
                    // Delegate timeout management to hardware.
                    setBit(command, kPanelRefreshCtrlFrameInsertionAutoModeOffset);
                    // Configure panel to maintain the minimum refresh rate.
                    setBitField(command, maxFrameRate, kPanelRefreshCtrlMinimumRefreshRateOffset,
                                kPanelRefreshCtrlMinimumRefreshRateMask);
                    if (!mFileNode->writeValue(composer::kRefreshControlNodeName, command)) {
                        LOG(WARNING)
                                << "VrrController: write file node error, command = " << command;
                        return;
                    }
                    ATRACE_INT(kMinimumRefreshRateConfiguredTraceName, maxFrameRate);
                    mMinimumRefreshRatePresentState = kAtMaximumRefreshRate;
                    onRefreshRateChangedInternal(maxFrameRate);
                    mMinimumRefreshRateTimeoutEvent->mIsRelativeTime = false;
                    mMinimumRefreshRateTimeoutEvent->mWhenNs =
                            mRecord.mPendingCurrentPresentTime.value().mTime +
                            mMaximumRefreshRateTimeoutNs;
                    postEvent(VrrControllerEventType::kMinLockTimeForPeakRefreshRate,
                              mMinimumRefreshRateTimeoutEvent.value());
                } else if (mMinimumRefreshRatePresentState == kTransitionToMinimumRefreshRate) {
                    dropEventLocked(VrrControllerEventType::kMinLockTimeForPeakRefreshRate);
                    mMinimumRefreshRateTimeoutEvent->mIsRelativeTime = false;
                    auto delayNs =
                            (std::nano::den / mMinimumRefreshRate) + kMillisecondToNanoSecond;
                    mMinimumRefreshRateTimeoutEvent->mWhenNs =
                            mRecord.mPendingCurrentPresentTime.value().mTime + delayNs;
                    postEvent(VrrControllerEventType::kMinLockTimeForPeakRefreshRate,
                              mMinimumRefreshRateTimeoutEvent.value());
                } else {
                    if (mMinimumRefreshRatePresentState != kAtMaximumRefreshRate) {
                        LOG(ERROR) << "VrrController: wrong state when setting min refresh rate: "
                                   << mMinimumRefreshRatePresentState;
                    }
                }
            }
            return;
        }
    }

    // Prior to pushing the most recent fence update, verify the release timestamps of all preceding
    // fences.
    // TODO(b/309873055): delegate the task of executing updateVsyncHistory to the Vrr controller's
    // loop thread in order to reduce the workload of calling thread.
    updateVsyncHistory();
    int dupFence = dup(fence);
    if (dupFence < 0) {
        LOG(ERROR) << "VrrController: duplicate fence file failed." << errno;
    }

    {
        const std::lock_guard<std::mutex> lock(mMutex);
        if (mLastPresentFence.has_value()) {
            LOG(WARNING) << "VrrController: last present fence remains open.";
        }
        mLastPresentFence = dupFence;
        // Post next rendering timeout.
        int64_t timeoutNs;
        if (mVrrConfigs[mVrrActiveConfig].isFullySupported) {
            timeoutNs = getSteadyClockTimeNs() +
                    mVrrConfigs[mVrrActiveConfig].notifyExpectedPresentConfig->TimeoutNs;
        } else {
            timeoutNs = kDefaultSystemPresentTimeoutNs;
        }
        postEvent(VrrControllerEventType::kSystemRenderingTimeout,
                  getSteadyClockTimeNs() + timeoutNs);
        if (shouldHandleVendorRenderingTimeout()) {
            // Post next frame insertion event.
            int64_t firstTimeOutNs;
            if (mVendorPresentTimeoutOverride) {
                firstTimeOutNs = mVendorPresentTimeoutOverride.value().mTimeoutNs;
            } else {
                firstTimeOutNs = mPresentTimeoutEventHandler->getPresentTimeoutNs();
            }
            mPendingVendorRenderingTimeoutTasks.baseTimeNs += firstTimeOutNs;
            firstTimeOutNs -= kDefaultAheadOfTimeNs;
            if (firstTimeOutNs >= 0) {
                auto vendorPresentTimeoutNs =
                        mRecord.mPendingCurrentPresentTime.value().mTime + firstTimeOutNs;
                postEvent(VrrControllerEventType::kVendorRenderingTimeoutInit,
                          vendorPresentTimeoutNs);
            } else {
                LOG(ERROR) << "VrrController: the first vendor present timeout is negative";
            }
        }
        mRecord.mPendingCurrentPresentTime = std::nullopt;
    }
    mCondition.notify_all();
}

void VariableRefreshRateController::setExpectedPresentTime(int64_t timestampNanos,
                                                           int frameIntervalNs) {
    ATRACE_CALL();

    const std::lock_guard<std::mutex> lock(mMutex);
    mLastExpectedPresentTimeNs = timestampNanos;
    // Drop the out of date timeout.
    dropEventLocked(VrrControllerEventType::kSystemRenderingTimeout);
    cancelPresentTimeoutHandlingLocked();
    mPendingVendorRenderingTimeoutTasks.baseTimeNs = timestampNanos;
    mRecord.mPendingCurrentPresentTime = {mVrrActiveConfig, timestampNanos, frameIntervalNs};
}

void VariableRefreshRateController::onVsync(int64_t timestampNanos,
                                            int32_t __unused vsyncPeriodNanos) {
    const std::lock_guard<std::mutex> lock(mMutex);
    mRecord.mVsyncHistory
            .next() = {.mType = VariableRefreshRateController::VsyncEvent::Type::kVblank,
                       .mTime = timestampNanos};
}

void VariableRefreshRateController::cancelPresentTimeoutHandlingLocked() {
    dropEventLocked(VrrControllerEventType::kVendorRenderingTimeoutInit);
    dropEventLocked(VrrControllerEventType::kVendorRenderingTimeoutPost);
    mPendingVendorRenderingTimeoutTasks.reset();
}

void VariableRefreshRateController::createMinimumRefreshRateTimeoutEventLocked() {
    // Set up peak refresh rate timeout event accordingly.
    mMinimumRefreshRateTimeoutEvent = std::make_optional<TimedEvent>("MinimumRefreshRateTimeout");
    mMinimumRefreshRateTimeoutEvent->mFunctor = [this]() -> int {
        if (mMinimumRefreshRatePresentState == kAtMaximumRefreshRate) {
            mMinimumRefreshRatePresentState = kTransitionToMinimumRefreshRate;
            mMinimumRefreshRateTimeoutEvent->mIsRelativeTime = false;
            auto delayNs = (std::nano::den / mMinimumRefreshRate) + kMillisecondToNanoSecond;
            mMinimumRefreshRateTimeoutEvent->mWhenNs = getSteadyClockTimeNs() + delayNs;
            postEvent(VrrControllerEventType::kMinLockTimeForPeakRefreshRate,
                      mMinimumRefreshRateTimeoutEvent.value());
            return 1;
        } else {
            if (mMinimumRefreshRatePresentState != kTransitionToMinimumRefreshRate) {
                LOG(ERROR) << "VrrController: expect mMinimumRefreshRatePresentState is "
                              "kTransitionToMinimumRefreshRate, but it is "
                           << mMinimumRefreshRatePresentState;
                return -1;
            }
            mMinimumRefreshRatePresentState = kAtMinimumRefreshRate;
            // TODO(b/333204544): ensure the correct refresh rate is set when calling
            // setFixedRefreshRate().
            if (mVariableRefreshRateStatistic) {
                mVariableRefreshRateStatistic->setFixedRefreshRate(mMinimumRefreshRate);
            }
            if (mPresentTimeoutController != PresentTimeoutControllerType::kHardware) {
                LOG(WARNING) << "VrrController: incorrect type of present timeout controller.";
            }
            uint32_t command = getCurrentRefreshControlStateLocked();
            setBit(command, kPanelRefreshCtrlFrameInsertionAutoModeOffset);
            setBitField(command, mMinimumRefreshRate, kPanelRefreshCtrlMinimumRefreshRateOffset,
                        kPanelRefreshCtrlMinimumRefreshRateMask);
            onRefreshRateChangedInternal(mMinimumRefreshRate);
            auto res = mFileNode->writeValue(composer::kRefreshControlNodeName, command);
            ATRACE_INT(kMinimumRefreshRateConfiguredTraceName, mMinimumRefreshRate);
            return res;
        }
    };
}

void VariableRefreshRateController::dropEventLocked() {
    mEventQueue.mPriorityQueue = std::priority_queue<VrrControllerEvent>();
}

void VariableRefreshRateController::dropEventLocked(VrrControllerEventType eventType) {
    std::priority_queue<VrrControllerEvent> q;
    auto target = static_cast<int>(eventType);
    while (!mEventQueue.mPriorityQueue.empty()) {
        const auto& it = mEventQueue.mPriorityQueue.top();
        if ((static_cast<int>(it.mEventType) & target) != target) {
            q.push(it);
        }
        mEventQueue.mPriorityQueue.pop();
    }
    mEventQueue.mPriorityQueue = std::move(q);
}

std::string VariableRefreshRateController::dumpEventQueueLocked() {
    std::string content;
    if (mEventQueue.mPriorityQueue.empty()) {
        return content;
    }

    std::priority_queue<VrrControllerEvent> q;
    while (!mEventQueue.mPriorityQueue.empty()) {
        const auto& it = mEventQueue.mPriorityQueue.top();
        content += "VrrController: event = ";
        content += it.toString();
        content += "\n";
        q.push(it);
        mEventQueue.mPriorityQueue.pop();
    }
    mEventQueue.mPriorityQueue = std::move(q);
    return content;
}

void VariableRefreshRateController::dump(String8& result, const std::vector<std::string>& args) {
    result.appendFormat("\nVariableRefreshRateStatistic: \n");
    if (mDisplay) {
        result.appendFormat("[%s] ", mDisplay->mDisplayName.c_str());
    }
    result.appendFormat("Physical Refresh Rate = %i \n", mLastRefreshRate);
    mVariableRefreshRateStatistic->dump(result, args);
}

uint32_t VariableRefreshRateController::getCurrentRefreshControlStateLocked() const {
    uint32_t state = 0;
    return (mFileNode->getLastWrittenValue(kRefreshControlNodeName, state) == NO_ERROR)
            ? (state & kPanelRefreshCtrlStateBitsMask)
            : 0;
}

int64_t VariableRefreshRateController::getLastFenceSignalTimeUnlocked(int fd) {
    if (fd == -1) {
        return SIGNAL_TIME_INVALID;
    }
    struct sync_file_info* finfo = sync_file_info(fd);
    if (finfo == nullptr) {
        LOG(ERROR) << "VrrController: sync_file_info returned NULL for fd " << fd;
        return SIGNAL_TIME_INVALID;
    }
    if (finfo->status != 1) {
        const auto status = finfo->status;
        if (status < 0) {
            LOG(ERROR) << "VrrController: sync_file_info contains an error: " << status;
        }
        sync_file_info_free(finfo);
        return status < 0 ? SIGNAL_TIME_INVALID : SIGNAL_TIME_PENDING;
    }
    uint64_t timestamp = 0;
    struct sync_fence_info* pinfo = sync_get_fence_info(finfo);
    if (finfo->num_fences != 1) {
        LOG(WARNING) << "VrrController:: there is more than one fence in the file descriptor = "
                     << fd;
    }
    for (size_t i = 0; i < finfo->num_fences; i++) {
        if (pinfo[i].timestamp_ns > timestamp) {
            timestamp = pinfo[i].timestamp_ns;
        }
    }
    sync_file_info_free(finfo);
    return timestamp;
}

int64_t VariableRefreshRateController::getNextEventTimeLocked() const {
    if (mEventQueue.mPriorityQueue.empty()) {
        LOG(WARNING) << "VrrController: event queue should NOT be empty.";
        return -1;
    }
    const auto& event = mEventQueue.mPriorityQueue.top();
    return event.mWhenNs;
}

std::string VariableRefreshRateController::getStateName(VrrControllerState state) const {
    switch (state) {
        case VrrControllerState::kDisable:
            return "Disable";
        case VrrControllerState::kRendering:
            return "Rendering";
        case VrrControllerState::kHibernate:
            return "Hibernate";
        default:
            return "Unknown";
    }
}

void VariableRefreshRateController::handleCadenceChange() {
    ATRACE_CALL();
    if (!mRecord.mNextExpectedPresentTime.has_value()) {
        LOG(WARNING) << "VrrController: cadence change occurs without the expected present timing "
                        "information.";
        return;
    }
    // TODO(b/305311056): handle frame rate change.
    mRecord.mNextExpectedPresentTime = std::nullopt;
}

void VariableRefreshRateController::handleResume() {
    ATRACE_CALL();
    if (!mRecord.mNextExpectedPresentTime.has_value()) {
        LOG(WARNING)
                << "VrrController: resume occurs without the expected present timing information.";
        return;
    }
    // TODO(b/305311281): handle panel resume.
    mRecord.mNextExpectedPresentTime = std::nullopt;
}

void VariableRefreshRateController::handleHibernate() {
    ATRACE_CALL();
    if (mFrameRateReporter) {
        mFrameRateReporter->reset();
    }
    // TODO(b/305311206): handle entering panel hibernate.
    postEvent(VrrControllerEventType::kHibernateTimeout,
              getSteadyClockTimeNs() + kDefaultWakeUpTimeInPowerSaving);
}

void VariableRefreshRateController::handleStayHibernate() {
    ATRACE_CALL();
    // TODO(b/305311698): handle keeping panel hibernate.
    postEvent(VrrControllerEventType::kHibernateTimeout,
              getSteadyClockTimeNs() + kDefaultWakeUpTimeInPowerSaving);
}

void VariableRefreshRateController::handlePresentTimeout() {
    ATRACE_CALL();

    if (mState == VrrControllerState::kDisable) {
        cancelPresentTimeoutHandlingLocked();
        return;
    }

    // During doze, the present timeout controller switches to |kHardware|.
    // This remains until |handlePresentTimeout| is first called here where the controller type is
    // reset back to |mDefaultPresentTimeoutController|(|kSoftware|).
    if (mDefaultPresentTimeoutController != PresentTimeoutControllerType::kSoftware) {
        LOG(WARNING) << "VrrController: incorrect type of default present timeout controller.";
    }
    uint32_t command = 0;
    if (mFileNode->getLastWrittenValue(composer::kRefreshControlNodeName, command) == NO_ERROR) {
        clearBit(command, kPanelRefreshCtrlFrameInsertionAutoModeOffset);
        setBitField(command, 1, kPanelRefreshCtrlFrameInsertionFrameCountOffset,
                    kPanelRefreshCtrlFrameInsertionFrameCountMask);
        mFileNode->writeValue(composer::kRefreshControlNodeName, command);
        if (mPresentTimeoutController != PresentTimeoutControllerType::kSoftware) {
            mPresentTimeoutController = PresentTimeoutControllerType::kSoftware;
        }
    } else {
        LOG(ERROR) << "VrrController: no last wrttien value for kRefreshControlNodeName";
    }
    if (mFrameRateReporter) {
        mFrameRateReporter->onPresent(getSteadyClockTimeNs(), 0);
    }
    if (mVariableRefreshRateStatistic) {
        mVariableRefreshRateStatistic
                ->onNonPresentRefresh(getSteadyClockTimeNs(),
                                      RefreshSource::kRefreshSourceFrameInsertion);
    }
    mPendingVendorRenderingTimeoutTasks.scheduleNextTask();
}

void VariableRefreshRateController::onFrameRateChangedForDBI(int refreshRate) {
    // By default, if the refresh rate calculator cannot lock onto a specific frame rate, it may
    // return -1 to reflect this. To avoid reporting a negative frame frequency, return 1 instead in
    // this case.
    auto maxFrameRate = durationNsToFreq(mVrrConfigs[mVrrActiveConfig].minFrameIntervalNs);
    refreshRate = std::max(1, refreshRate);
    mFrameRate = std::min(maxFrameRate, refreshRate);
    postEvent(VrrControllerEventType::kUpdateDbiFrameRate, getSteadyClockTimeNs());
}

void VariableRefreshRateController::onRefreshRateChanged(int refreshRate) {
    if (mMinimumRefreshRate > 1) {
        // If the minimum refresh rate has been set, the refresh rate remains fixed at a specific
        // value.
        return;
    }
    onRefreshRateChangedInternal(refreshRate);
}

void VariableRefreshRateController::onRefreshRateChangedInternal(int refreshRate) {
    if (!(mDisplay) || !(mDisplay->mDevice)) {
        LOG(ERROR) << "VrrController: absence of a device or display.";
        return;
    }
    refreshRate =
            refreshRate == kDefaultInvalidRefreshRate ? kDefaultMinimumRefreshRate : refreshRate;
    refreshRate = convertToValidRefreshRate(refreshRate);
    if (mLastRefreshRate == refreshRate) {
        return;
    }
    mLastRefreshRate = refreshRate;
    for (const auto& listener : mRefreshRateChangeListeners) {
        if (listener) listener->onRefreshRateChange(refreshRate);
    }
    reportRefreshRateIndicator();
}

void VariableRefreshRateController::reportRefreshRateIndicator() {
    if (mRefreshRateCalculatorEnabled) {
        if (!mDisplay->mDevice->isVrrApiSupported()) {
            // For legacy API, vsyncPeriodNanos is utilized to denote the refresh rate,
            // refreshPeriodNanos is disregarded.
            mDisplay->mDevice->onRefreshRateChangedDebug(mDisplay->mDisplayId,
                                                         freqToDurationNs(mLastRefreshRate));
        } else {
            mDisplay->mDevice
                    ->onRefreshRateChangedDebug(mDisplay->mDisplayId,
                                                mVrrConfigs[mVrrActiveConfig].vsyncPeriodNs,
                                                freqToDurationNs(mLastRefreshRate));
        }
    }
}

std::vector<int> VariableRefreshRateController::generateValidRefreshRates(
        const VrrConfig_t& config) const {
    std::vector<int> refreshRates;
    int teFrequency = durationNsToFreq(config.vsyncPeriodNs);
    int minVsyncNum = roundDivide(config.minFrameIntervalNs, config.vsyncPeriodNs);
    for (int vsyncNum = minVsyncNum; vsyncNum <= teFrequency; vsyncNum++) {
        refreshRates.push_back(roundDivide(teFrequency, vsyncNum));
    }
    std::set<int> uniqueRefreshRates(refreshRates.begin(), refreshRates.end());
    refreshRates.assign(uniqueRefreshRates.begin(), uniqueRefreshRates.end());
    return refreshRates;
}

int VariableRefreshRateController::convertToValidRefreshRate(int refreshRate) {
    const auto& validRefreshRates = mValidRefreshRates[mVrrActiveConfig];
    auto it = std::lower_bound(validRefreshRates.begin(), validRefreshRates.end(), refreshRate);
    if (it != validRefreshRates.end()) {
        return *it;
    }
    LOG(ERROR) << "Could not match to any valid refresh rate: " << refreshRate;
    return durationNsToFreq(mVrrConfigs[mVrrActiveConfig].minFrameIntervalNs);
}

bool VariableRefreshRateController::shouldHandleVendorRenderingTimeout() const {
    // We skip the check |mPresentTimeoutController| == |kSoftware| here because, even if it's set
    // to |kHardware| when resuming from doze, we still allow vendor rendering timeouts. Once this
    // timeout occurs, |mPresentTimeoutController| will be reset to
    // |mDefaultPresentTimeoutController| (which should be |kSoftware|).
    return (mPresentTimeoutController == PresentTimeoutControllerType::kSoftware) &&
            ((!mVendorPresentTimeoutOverride) ||
             (mVendorPresentTimeoutOverride.value().mSchedule.size() > 0)) &&
            (mPowerMode == HWC_POWER_MODE_NORMAL);
}

void VariableRefreshRateController::threadBody() {
    struct sched_param param = {.sched_priority = sched_get_priority_min(SCHED_FIFO)};
    if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
        LOG(ERROR) << "VrrController: fail to set scheduler to SCHED_FIFO.";
        return;
    }
    for (;;) {
        bool stateChanged = false;
        uint32_t frameRate = 0;
        {
            std::unique_lock<std::mutex> lock(mMutex);
            if (mThreadExit) break;
            if (!mEnabled) mCondition.wait(lock);
            if (!mEnabled) continue;

            if (mEventQueue.mPriorityQueue.empty()) {
                mCondition.wait(lock);
            }
            int64_t whenNs = getNextEventTimeLocked();
            int64_t nowNs = getSteadyClockTimeNs();
            if (whenNs > nowNs) {
                int64_t delayNs = whenNs - nowNs;
                auto res = mCondition.wait_for(lock, std::chrono::nanoseconds(delayNs));
                if (res != std::cv_status::timeout) {
                    continue;
                }
            }

            if (mEventQueue.mPriorityQueue.empty()) {
                continue;
            }

            auto event = mEventQueue.mPriorityQueue.top();
            if (event.mWhenNs > getSteadyClockTimeNs()) {
                continue;
            }
            mEventQueue.mPriorityQueue.pop();
            if (static_cast<int>(event.mEventType) &
                static_cast<int>(VrrControllerEventType::kCallbackEventMask)) {
                handleCallbackEventLocked(event);
                continue;
            }
            if (event.mEventType == VrrControllerEventType::kUpdateDbiFrameRate) {
                frameRate = mFrameRate;
            }
            if (event.mEventType == VrrControllerEventType::kMinimumRefreshRateAlignWithPresent) {
                if (mPendingMinimumRefreshRateRequest) {
                    mMinimumRefreshRate = mPendingMinimumRefreshRateRequest.value();
                    mPendingMinimumRefreshRateRequest = std::nullopt;
                    auto maxFrameRate =
                            durationNsToFreq(mVrrConfigs[mVrrActiveConfig].minFrameIntervalNs);
                    uint32_t command = getCurrentRefreshControlStateLocked();
                    // Delegate timeout management to hardware.
                    setBit(command, kPanelRefreshCtrlFrameInsertionAutoModeOffset);
                    // Configure panel to maintain the minimum refresh rate.
                    setBitField(command, maxFrameRate, kPanelRefreshCtrlMinimumRefreshRateOffset,
                                kPanelRefreshCtrlMinimumRefreshRateMask);
                    if (!mFileNode->writeValue(composer::kRefreshControlNodeName, command)) {
                        LOG(WARNING)
                                << "VrrController: write file node error, command = " << command;
                        return;
                    }
                    ATRACE_INT(kMinimumRefreshRateConfiguredTraceName, maxFrameRate);
                    mMinimumRefreshRatePresentState = kAtMaximumRefreshRate;
                    // Even though we transition directly to the maximum refresh rate, we still
                    // report the refresh rate change for |mMinimumRefreshRate| to maintain
                    // consistency. It will soon ovewrite by |maxFrameRate| below.
                    onRefreshRateChangedInternal(mMinimumRefreshRate);
                    onRefreshRateChangedInternal(maxFrameRate);
                    mMinimumRefreshRateTimeoutEvent->mIsRelativeTime = false;
                    mMinimumRefreshRateTimeoutEvent->mWhenNs =
                            getSteadyClockTimeNs() + mMaximumRefreshRateTimeoutNs;
                    postEvent(VrrControllerEventType::kMinLockTimeForPeakRefreshRate,
                              mMinimumRefreshRateTimeoutEvent.value());
                }
                continue;
            }
            if (event.mEventType ==
                VrrControllerEventType::kMinimumRefreshRateWaitForConfigTimeout) {
                LOG(ERROR) << "Set minimum refresh rate to " << mMinimumRefreshRate
                           << " but wait for config timeout.";
                mPendingMinimumRefreshRateRequest = std::nullopt;
                continue;
            }
            if (mState == VrrControllerState::kRendering) {
                if (event.mEventType == VrrControllerEventType::kHibernateTimeout) {
                    LOG(ERROR) << "VrrController: receiving a hibernate timeout event while in the "
                                  "rendering state.";
                }
                switch (event.mEventType) {
                    case VrrControllerEventType::kSystemRenderingTimeout: {
                        handleHibernate();
                        mState = VrrControllerState::kHibernate;
                        stateChanged = true;
                        break;
                    }
                    case VrrControllerEventType::kNotifyExpectedPresentConfig: {
                        handleCadenceChange();
                        break;
                    }
                    case VrrControllerEventType::kVendorRenderingTimeoutInit: {
                        if (mPresentTimeoutEventHandler) {
                            size_t numberOfIntervals = 0;
                            // Verify whether a present timeout override exists, and if so, execute
                            // it first.
                            if (mVendorPresentTimeoutOverride) {
                                const auto& params = mVendorPresentTimeoutOverride.value();
                                int64_t whenFromNowNs = 0;
                                for (int i = 0; i < params.mSchedule.size(); ++i) {
                                    numberOfIntervals += params.mSchedule[i].first;
                                }
                                if (numberOfIntervals > 0) {
                                    mPendingVendorRenderingTimeoutTasks.reserveSpace(
                                            numberOfIntervals);
                                    for (int i = 0; i < params.mSchedule.size(); ++i) {
                                        uint32_t intervalNs = params.mSchedule[i].second;
                                        for (int j = 0; j < params.mSchedule[i].first; ++j) {
                                            mPendingVendorRenderingTimeoutTasks.addTask(
                                                    whenFromNowNs);
                                            whenFromNowNs += intervalNs;
                                        }
                                    }
                                }
                            } else {
                                auto handleEvents = mPresentTimeoutEventHandler->getHandleEvents();
                                if (!handleEvents.empty()) {
                                    numberOfIntervals = handleEvents.size();
                                    mPendingVendorRenderingTimeoutTasks.reserveSpace(
                                            numberOfIntervals);
                                    for (int i = 0; i < handleEvents.size(); ++i) {
                                        mPendingVendorRenderingTimeoutTasks.addTask(
                                                handleEvents[i].mWhenNs);
                                    }
                                }
                            }
                            if (numberOfIntervals > 0) {
                                // Start from 1 since we will execute the first task immediately
                                // below.
                                mPendingVendorRenderingTimeoutTasks.nextTaskIndex = 1;
                                handlePresentTimeout();
                            }
                        }
                        break;
                    }
                    case VrrControllerEventType::kVendorRenderingTimeoutPost: {
                        handlePresentTimeout();
                        if (event.mFunctor) {
                            event.mFunctor();
                        }
                        break;
                    }
                    default: {
                        break;
                    }
                }
            } else {
                if (event.mEventType == VrrControllerEventType::kSystemRenderingTimeout) {
                    LOG(ERROR) << "VrrController: receiving a rendering timeout event while in the "
                                  "hibernate state.";
                }
                if (mState != VrrControllerState::kHibernate) {
                    LOG(ERROR) << "VrrController: expecting to be in hibernate, but instead in "
                                  "state = "
                               << getStateName(mState);
                }
                switch (event.mEventType) {
                    case VrrControllerEventType::kHibernateTimeout: {
                        handleStayHibernate();
                        break;
                    }
                    case VrrControllerEventType::kNotifyExpectedPresentConfig: {
                        handleResume();
                        mState = VrrControllerState::kRendering;
                        stateChanged = true;
                        break;
                    }
                    default: {
                        break;
                    }
                }
            }
        }
        // TODO(b/309873055): implement a handler to serialize all outer function calls to the same
        // thread owned by the VRR controller.
        if (stateChanged) {
            updateVsyncHistory();
        }
        // Write pending values without holding mutex shared with HWC main thread.
        if (frameRate) {
            if (!mFileNode->writeValue(kFrameRateNodeName, frameRate)) {
                LOG(ERROR) << "VrrController: write to node = " << kFrameRateNodeName
                           << " failed, value = " << frameRate;
            }
            ATRACE_INT("frameRate", frameRate);
        }
    }
}

void VariableRefreshRateController::postEvent(VrrControllerEventType type, int64_t when) {
    VrrControllerEvent event;
    event.mEventType = type;
    event.mWhenNs = when;
    mEventQueue.mPriorityQueue.emplace(event);
}

void VariableRefreshRateController::postEvent(VrrControllerEventType type, TimedEvent& timedEvent) {
    VrrControllerEvent event;
    event.mEventType = type;
    event.mWhenNs = timedEvent.mIsRelativeTime ? (getSteadyClockTimeNs() + timedEvent.mWhenNs)
                                               : timedEvent.mWhenNs;
    event.mFunctor = std::move(timedEvent.mFunctor);
    mEventQueue.mPriorityQueue.emplace(event);
}

void VariableRefreshRateController::updateVsyncHistory() {
    int fence = -1;

    {
        const std::lock_guard<std::mutex> lock(mMutex);
        if (!mLastPresentFence.has_value()) {
            return;
        }
        fence = mLastPresentFence.value();
        mLastPresentFence = std::nullopt;
    }

    // Execute the following logic unlocked to enhance performance.
    int64_t lastSignalTime = getLastFenceSignalTimeUnlocked(fence);
    if (close(fence)) {
        LOG(ERROR) << "VrrController: close fence file failed, errno = " << errno;
        return;
    } else if (lastSignalTime == SIGNAL_TIME_PENDING || lastSignalTime == SIGNAL_TIME_INVALID) {
        return;
    }

    {
        // Acquire the mutex again to store the vsync record.
        const std::lock_guard<std::mutex> lock(mMutex);
        mRecord.mVsyncHistory
                .next() = {.mType = VariableRefreshRateController::VsyncEvent::Type::kReleaseFence,
                           .mTime = lastSignalTime};
    }
}

} // namespace android::hardware::graphics::composer
