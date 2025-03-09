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

#pragma once

#include <utils/Mutex.h>
#include <condition_variable>
#include <list>
#include <map>
#include <optional>
#include <queue>
#include <thread>

#include "../libdevice/ExynosDisplay.h"
#include "../libdevice/ExynosLayer.h"
#include "EventQueue.h"
#include "ExternalEventHandlerLoader.h"
#include "FileNode.h"
#include "Power/DisplayStateResidencyWatcher.h"
#include "RefreshRateCalculator/RefreshRateCalculator.h"
#include "RingBuffer.h"
#include "Statistics/VariableRefreshRateStatistic.h"
#include "Utils.h"
#include "display/common/DisplayConfigurationOwner.h"
#include "interface/DisplayContextProvider.h"
#include "interface/VariableRefreshRateInterface.h"

namespace android::hardware::graphics::composer {

class VariableRefreshRateController : public VsyncListener,
                                      public RefreshListener,
                                      public DisplayContextProvider,
                                      public DisplayConfigurationsOwner {
public:
    ~VariableRefreshRateController();

    auto static CreateInstance(ExynosDisplay* display, const std::string& panelName)
            -> std::shared_ptr<VariableRefreshRateController>;

    const displayConfigs_t* getCurrentDisplayConfiguration() const override {
        auto configs = mDisplayContextProvider->getDisplayConfigs();
        if (configs) {
            const auto& it = configs->find(mVrrActiveConfig);
            if (it != configs->end()) {
                return &(it->second);
            }
        }
        return nullptr;
    }

    void enableRefreshRateCalculator(bool enabled) {
        const std::lock_guard<std::mutex> lock(mMutex);

        if (mRefreshRateCalculator) {
            mRefreshRateCalculatorEnabled = enabled;
            if (mRefreshRateCalculatorEnabled) {
                if (mDisplay->isVrrSupported()) {
                    reportRefreshRateIndicator();
                } else {
                    // This is a VTS hack for MRRv2
                    mDisplay->mDevice->onRefreshRateChangedDebug(mDisplay->mDisplayId,
                                                                 freqToDurationNs(
                                                                         mLastRefreshRate));
                }
            }
        }
    };

    int notifyExpectedPresent(int64_t timestamp, int32_t frameIntervalNs);

    // Clear historical record data.
    void reset();

    // After setting the active Vrr configuration, we will automatically transition into the
    // rendering state and post the timeout event.
    void setActiveVrrConfiguration(hwc2_config_t config);

    void setEnable(bool isEnabled);

    // |preSetPowerMode| is called before the power mode is configured.
    void preSetPowerMode(int32_t mode);
    //|postSetPowerMode| is called after the setting to new power mode has been done.
    void postSetPowerMode(int32_t mode);

    void setVrrConfigurations(std::unordered_map<hwc2_config_t, VrrConfig_t> configs);

    // Inherit from DisplayContextProvider.
    int getAmbientLightSensorOutput() const override;
    BrightnessMode getBrightnessMode() const override;
    int getBrightnessNits() const override;
    const char* getDisplayFileNodePath() const override;
    int getEstimatedVideoFrameRate() const override;
    OperationSpeedMode getOperationSpeedMode() const override;
    bool isProximityThrottlingEnabled() const override;

    const DisplayContextProviderInterface* getDisplayContextProviderInterface() const {
        return &mDisplayContextProviderInterface;
    }

    void registerRefreshRateChangeListener(std::shared_ptr<RefreshRateChangeListener> listener) {
        mRefreshRateChangeListeners.emplace_back(listener);
    }

    void setPresentTimeoutParameters(int timeoutNs,
                                     const std::vector<std::pair<uint32_t, uint32_t>>& settings);

    void setPresentTimeoutController(uint32_t controllerType);

    // Set refresh rate within the range [minimumRefreshRate, maximumRefreshRateOfCurrentConfig].
    // The maximum refresh rate, |maximumRefreshRateOfCurrentConfig|, is intrinsic to the current
    // configuration, hence only |minimumRefreshRate| needs to be specified. If
    // |minLockTimeForPeakRefreshRate| does not equal zero, upon arrival of new frames, the
    // current refresh rate will be set to |maximumRefreshRateOfCurrentConfig| and will remain so
    // for |minLockTimeForPeakRefreshRate| duration. Afterward, the current refresh rate will
    // revert to |minimumRefreshRate|. Alternatively, when |minLockTimeForPeakRefreshRate| equals
    // zero, if no new frame update, refresh rate will drop to |minimumRefreshRate| immediately.
    int setFixedRefreshRateRange(uint32_t minimumRefreshRate,
                                 uint64_t minLockTimeForPeakRefreshRate);

    void dump(String8& result, const std::vector<std::string>& args = {});

private:
    static constexpr char kMinimumRefreshRateRequestTraceName[] = "MinimumRefreshRateRequest";
    static constexpr char kMinimumRefreshRateConfiguredTraceName[] = "MinimumRefreshRateConfigured";

    static constexpr int kMaxFrameRate = 120;
    static constexpr int kMaxTefrequency = 240;

    static constexpr int64_t kWaitForConfigTimeoutNs = std::nano::den; // 1 second.

    static constexpr int kDefaultRingBufferCapacity = 128;
    static constexpr int64_t kDefaultWakeUpTimeInPowerSaving =
            500 * (std::nano::den / std::milli::den); // 500 ms
    static constexpr int64_t SIGNAL_TIME_PENDING = INT64_MAX;
    static constexpr int64_t SIGNAL_TIME_INVALID = -1;

    static constexpr int64_t kDefaultSystemPresentTimeoutNs =
            500 * (std::nano::den / std::milli::den); // 500 ms

    static constexpr int64_t kDefaultVendorPresentTimeoutNs =
            33 * (std::nano::den / std::milli::den); // 33 ms

    static constexpr std::string_view kVendorDisplayPanelLibrary = "libdisplaypanel.so";

    static constexpr int64_t kDefaultAheadOfTimeNs = 1000000; // 1 ms;

    enum class VrrControllerState {
        kDisable = 0,
        kRendering,
        kHibernate,
    };

    typedef struct PendingVendorRenderingTimeoutTasks {
        PendingVendorRenderingTimeoutTasks(VariableRefreshRateController* controller)
              : host(controller), taskExecutionTimeNs(kDefaultMaximumNumberOfTasks, 0) {}

        void addTask(int64_t executionIntervalNs) {
            taskExecutionTimeNs[numberOfTasks++] = baseTimeNs + executionIntervalNs;
        }

        void scheduleNextTask() {
            if (!isDone()) {
                host->postEvent(VrrControllerEventType::kVendorRenderingTimeoutPost,
                                std::max(getSteadyClockTimeNs(),
                                         taskExecutionTimeNs[nextTaskIndex++] -
                                                 kDefaultAheadOfTimeNs));
            }
        }

        bool isDone() const { return (numberOfTasks == nextTaskIndex); }

        void reserveSpace(size_t size) {
            if (size > taskExecutionTimeNs.size()) {
                taskExecutionTimeNs.resize(size);
            }
        }

        void reset() { numberOfTasks = nextTaskIndex = 0; }

        static constexpr size_t kDefaultMaximumNumberOfTasks = 10;

        VariableRefreshRateController* host;
        int64_t baseTimeNs = 0;
        int numberOfTasks = 0;
        int nextTaskIndex = 0;
        std::vector<int64_t> taskExecutionTimeNs;
    } PendingVendorRenderingTimeoutTasks;

    typedef struct PresentEvent {
        hwc2_config_t config;
        int64_t mTime;
        int mDuration;
    } PresentEvent;

    typedef struct PresentTimeoutSettings {
        PresentTimeoutSettings() = default;
        int mTimeoutNs = 0;
        std::vector<std::pair<uint32_t, uint32_t>> mSchedule;
        std::function<int()> mFunctor;
    } PresentTimeoutSettings;

    enum PresentTimeoutControllerType {
        kNone = 0,
        kSoftware,
        kHardware,
    };

    // 0: If the minimum refresh rate is unset, the state is set to |kMinRefreshRateUnset|.
    //
    // Otherwise, if the minimum refresh rate has been set:
    // 1: The state is set to |kAtMinRefreshRate|.
    // 2: If a presentation occurs when the state is |kAtMinRefreshRate|.
    // 2.1: If |mMaximumRefreshRateTimeoutNs| = 0, no action is taken.
    // 2.2: If |mMaximumRefreshRateTimeoutNs| > 0, the frame rate is promoted to the maximum refresh
    //      rate and maintained for |mMaximumRefreshRateTimeoutNs| by setting a timer. During this
    //      period, the state is set to |kAtMaximumRefreshRate|.
    // 3: When a timeout occurs at step 2.2, state is set to |kTransitionToMinimumRefreshRate|.
    //    It remains in this state until there are no further presentations after a period =
    //    |kGotoMinimumRefreshRateIfNoPresentTimeout|.
    // 4. Then, frame rate reverts to the minimum refresh rate, state is set to |kAtMinRefreshRate|.
    //    Returns to step 1.
    // Steps 1, 2, 3 and 4 continue until the minimum refresh rate is unset (by inputting 0 or 1);
    // at this point, the state is set to |kMinRefreshRateUnset| and goto 0.
    enum MinimumRefreshRatePresentStates {
        kMinRefreshRateUnset = 0,
        kAtMinimumRefreshRate,
        kAtMaximumRefreshRate,
        kTransitionToMinimumRefreshRate,
    };

    typedef struct VsyncEvent {
        enum class Type {
            kVblank,
            kReleaseFence,
        };
        Type mType;
        int64_t mTime;
    } VsyncEvent;

    typedef struct VrrRecord {
        static constexpr int kDefaultRingBufferCapacity = 128;

        void clear() {
            mNextExpectedPresentTime = std::nullopt;
            mPendingCurrentPresentTime = std::nullopt;
            mPresentHistory.clear();
            mVsyncHistory.clear();
        }

        std::optional<PresentEvent> mNextExpectedPresentTime = std::nullopt;
        std::optional<PresentEvent> mPendingCurrentPresentTime = std::nullopt;

        typedef RingBuffer<PresentEvent, kDefaultRingBufferCapacity> PresentTimeRecord;
        typedef RingBuffer<VsyncEvent, kDefaultRingBufferCapacity> VsyncRecord;
        PresentTimeRecord mPresentHistory;
        VsyncRecord mVsyncHistory;
    } VrrRecord;

    VariableRefreshRateController(ExynosDisplay* display, const std::string& panelName);

    // Implement interface RefreshListener.
    virtual void onPresent(int32_t fence) override;
    virtual void setExpectedPresentTime(int64_t timestampNanos, int frameIntervalNs) override;

    // Implement interface VsyncListener.
    virtual void onVsync(int64_t timestamp, int32_t vsyncPeriodNanos) override;

    void cancelPresentTimeoutHandlingLocked();

    void createMinimumRefreshRateTimeoutEventLocked();

    void dropEventLocked();
    void dropEventLocked(VrrControllerEventType eventType);

    std::string dumpEventQueueLocked();

    uint32_t getCurrentRefreshControlStateLocked() const;

    int64_t getLastFenceSignalTimeUnlocked(int fd);

    int64_t getNextEventTimeLocked() const;

    int getPresentFrameFlag() const {
        int flag = 0;
        // Is Yuv.
        for (size_t i = 0; i < mDisplay->mLayers.size(); i++) {
            auto layer = mDisplay->mLayers[i];
            if (layer->isLayerFormatYuv()) {
                flag |= static_cast<int>(PresentFrameFlag::kIsYuv);
            }
        }
        if (mDisplay->isUpdateRRIndicatorOnly()) {
            flag |= static_cast<int>(PresentFrameFlag::kUpdateRefreshRateIndicatorLayerOnly);
        }
        // Present when doze.
        if ((mPowerMode == HWC_POWER_MODE_DOZE) || (mPowerMode == HWC_POWER_MODE_DOZE_SUSPEND)) {
            flag |= static_cast<int>(PresentFrameFlag::kPresentingWhenDoze);
        }
        return flag;
    }

    std::string getStateName(VrrControllerState state) const;

    // Functions responsible for state machine transitions.
    void handleCadenceChange();
    void handleResume();
    void handleHibernate();
    void handleStayHibernate();

    void handleCallbackEventLocked(VrrControllerEvent& event) {
        if (event.mFunctor) {
            event.mFunctor();
        }
    }

    void handlePresentTimeout();

    inline bool isMinimumRefreshRateActive() const { return (mMinimumRefreshRate > 1); }

    // Report frame frequency changes to the kernel via the sysfs node.
    void onFrameRateChangedForDBI(int refreshRate);
    // Report refresh rate changes to the framework(SurfaceFlinger) or other display HWC components.
    void onRefreshRateChanged(int refreshRate);
    void onRefreshRateChangedInternal(int refreshRate);
    void reportRefreshRateIndicator();
    std::vector<int> generateValidRefreshRates(const VrrConfig_t& config) const;
    int convertToValidRefreshRate(int refreshRate);

    void postEvent(VrrControllerEventType type, TimedEvent& timedEvent);
    void postEvent(VrrControllerEventType type, int64_t when);

    int setFixedRefreshRateRangeWorker();

    bool shouldHandleVendorRenderingTimeout() const;

    void stopThread(bool exit);

    // The core function of the VRR controller thread.
    void threadBody();

    void updateVsyncHistory();

    ExynosDisplay* mDisplay;

    // The subsequent variables must be guarded by mMutex when accessed.
    EventQueue mEventQueue;
    VrrRecord mRecord;

    int32_t mPowerMode = -1;
    std::vector<PowerModeListener*> mPowerModeListeners;

    VrrControllerState mState;
    hwc2_config_t mVrrActiveConfig = -1;
    std::unordered_map<hwc2_config_t, VrrConfig_t> mVrrConfigs;
    std::optional<int> mLastPresentFence;
    uint32_t mFrameRate = 0;

    std::shared_ptr<FileNode> mFileNode;

    DisplayContextProviderInterface mDisplayContextProviderInterface;
    std::unique_ptr<ExternalEventHandlerLoader> mPresentTimeoutEventHandlerLoader;
    ExternalEventHandler* mPresentTimeoutEventHandler = nullptr;
    std::optional<PresentTimeoutSettings> mVendorPresentTimeoutOverride;

    std::string mPanelName;

    // Refresh rate indicator.
    bool mRefreshRateCalculatorEnabled = false;

    std::shared_ptr<RefreshRateCalculator> mRefreshRateCalculator;
    int mLastRefreshRate = kDefaultInvalidRefreshRate;
    std::unordered_map<hwc2_config_t, std::vector<int>> mValidRefreshRates;

    std::shared_ptr<RefreshRateCalculator> mFrameRateReporter;

    // Power stats.
    std::shared_ptr<DisplayStateResidencyWatcher> mResidencyWatcher;
    std::shared_ptr<VariableRefreshRateStatistic> mVariableRefreshRateStatistic;

    std::shared_ptr<CommonDisplayContextProvider> mDisplayContextProvider;

    bool mEnabled = false;
    bool mThreadExit = false;

    PresentTimeoutControllerType mDefaultPresentTimeoutController =
            PresentTimeoutControllerType::kSoftware;
    PresentTimeoutControllerType mPresentTimeoutController =
            PresentTimeoutControllerType::kSoftware;

    // When |mMinimumRefreshRate| is 0 or equal to 1, we are in normal mode.
    // when |mMinimumRefreshRate| is greater than 1. we are in a special mode where the minimum idle
    // refresh rate is |mMinimumRefreshRate|.
    uint32_t mMinimumRefreshRate = 0;
    // |mMaximumRefreshRateTimeoutNs| sets the minimum duration for which we should maintain the
    // peak refresh rate when transitioning to idle. |mMaximumRefreshRateTimeoutNs| takes effect
    // only when |mMinimumRefreshRate| is greater than 1.
    uint64_t mMaximumRefreshRateTimeoutNs = 0;
    std::optional<TimedEvent> mMinimumRefreshRateTimeoutEvent;
    MinimumRefreshRatePresentStates mMinimumRefreshRatePresentState = kMinRefreshRateUnset;
    std::optional<uint32_t> mPendingMinimumRefreshRateRequest = std::nullopt;

    std::vector<std::shared_ptr<RefreshRateChangeListener>> mRefreshRateChangeListeners;

    PendingVendorRenderingTimeoutTasks mPendingVendorRenderingTimeoutTasks;

    // It stores the last present time as a cadence hint. Note that it does not update when
    // notifyExpectedPresent is called, as notifyExpectedPresent may not result in an actual
    // display.
    int64_t mLastExpectedPresentTimeNs = -1;

    std::mutex mMutex;
    std::condition_variable mCondition;
};

} // namespace android::hardware::graphics::composer
