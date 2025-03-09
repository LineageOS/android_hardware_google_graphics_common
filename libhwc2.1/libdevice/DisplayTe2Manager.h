/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef _DISPLAY_TE2_MANAGER_H_
#define _DISPLAY_TE2_MANAGER_H_

#include "ExynosDisplay.h"
#include "ExynosHWCHelper.h"

enum class ProximitySensorState : uint32_t {
    NONE = 0,
    ACTIVE,
    INACTIVE,
};

// TODO: Rename this and integrate with refresh rate throttler related features into this class.
class DisplayTe2Manager : public RefreshRateChangeListener {
public:
    DisplayTe2Manager(ExynosDisplay* display, int32_t panelIndex, int fixedTe2DefaultRateHz);
    ~DisplayTe2Manager() = default;

    // Set the rate while option is fixed TE2. This should be set by the sensor.
    int32_t setFixedTe2Rate(int targetTe2RateHz);
    // Set the rate while option is changeable TE2. This should be set by the composer
    // while the display state is idle or active.
    int32_t setChangeableTe2Rate(int targetTe2RateHz);
    // Update TE2 option to either fixed or changeable according to the proximity sensor state.
    // Ideally we should use changeable TE2 if the proximity sensor is active. Also set the min
    // refresh rate of fixed TE2. It equals to the refresh rate while display is idle after
    // switching to changeable TE2, and we can use it for the notification of refresh rate
    // change.
    void updateTe2OptionForProximity(bool proximityActive, int minRefreshRate, bool dozeMode);
    bool isOptionFixedTe2() { return mIsOptionFixedTe2; }

    // By default we will continue the TE2 setting after entering doze mode. The ALSP may not
    // work properly if it's changeable TE2 with lower refresh rates, e.g. 1Hz. To avoid this
    // problem, we should update the setting to fixed TE2 no matter the proximity sensor is
    // active or not.
    void updateTe2ForDozeMode();
    // The TE2 might be enforced to different settings after entering doze mode. We should
    // restore the previous settings to keep the request from ALSP.
    void restoreTe2FromDozeMode();

    // Handle the notifications while the proximity sensor state is changed.
    void handleProximitySensorStateChange(bool active);

    void dump(String8& result) const;

private:
    static constexpr const char* kTe2RateFileNode =
            "/sys/devices/platform/exynos-drm/%s-panel/te2_rate_hz";
    static constexpr const char* kTe2OptionFileNode =
            "/sys/devices/platform/exynos-drm/%s-panel/te2_option";

    const char* getPanelString() {
        return (mPanelIndex == 0 ? "primary" : mPanelIndex == 1 ? "secondary" : "unknown");
    }

    const String8 getPanelTe2RatePath() {
        return String8::format(kTe2RateFileNode, getPanelString());
    }

    const String8 getPanelTe2OptionPath() {
        return String8::format(kTe2OptionFileNode, getPanelString());
    }

    void setTe2Option(bool fixedTe2);
    int32_t setTe2Rate(int targetTe2RateHz);
    int32_t setFixedTe2RateInternal(int targetTe2RateHz, bool enforce);

    virtual void onRefreshRateChange(int refreshRate) override;

    ExynosDisplay* mDisplay;
    int32_t mPanelIndex;
    // The min refresh rate of fixed TE2. For the refresh rates lower than this, the changeable
    // TE2 should be used.
    int mMinRefreshRateForFixedTe2;
    int mFixedTe2RateHz;
    // True when the current option is fixed TE2, otherwise it's changeable TE2.
    bool mIsOptionFixedTe2;
    // True when the refresh rate change listener of VariableRefreshRateController is
    // registered successfully. Thus we can receive the notification of refresh rate change
    // for changeable TE2 usage.
    bool mRefreshRateChangeListenerRegistered;

    // Indicates that TE2 was changed from changeable to fixed after entering doze mode. We
    // should restore the setting after exiting doze mode.
    bool mPendingOptionChangeableTe2;
    // After entering doze mode, the TE2 rate will be enforced to mFixedTe2RateForDozeMode.
    // We should save the previous rate as a pending value and restore it after exiting doze
    // mode.
    int mPendingFixedTe2Rate;
    // After entering doze mode, the TE2 will be enforced to fixed 30Hz.
    const int kFixedTe2RateForDozeMode = 30;

    Mutex mTe2Mutex;

    class ProximitySensorStateNotifierWorker : public Worker {
    public:
        explicit ProximitySensorStateNotifierWorker(DisplayTe2Manager* te2Manager);
        ~ProximitySensorStateNotifierWorker();

        void onStateChanged(bool active);

    protected:
        void Routine() override;

    private:
        static constexpr uint32_t kDebounceTimeMs = 500U;

        DisplayTe2Manager* mTe2Manager;
        bool mIsStateActive;
        bool mReceivedFirstStateAfterTimeout;
        enum ProximitySensorState mPendingState;
    };

    std::unique_ptr<ProximitySensorStateNotifierWorker> mProximitySensorStateNotifierWorker;
};

#endif // _DISPLAY_TE2_MANAGER_H_
