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

#define ATRACE_TAG (ATRACE_TAG_GRAPHICS | ATRACE_TAG_HAL)

#include "DisplayTe2Manager.h"

DisplayTe2Manager::DisplayTe2Manager(ExynosDisplay* display, int32_t panelIndex,
                                     int fixedTe2DefaultRateHz)
      : mDisplay(display),
        mPanelIndex(panelIndex),
        mMinRefreshRateForFixedTe2(0),
        mFixedTe2RateHz(fixedTe2DefaultRateHz),
        mIsOptionFixedTe2(true),
        mRefreshRateChangeListenerRegistered(false),
        mPendingOptionChangeableTe2(false),
        mPendingFixedTe2Rate(0) {
    mProximitySensorStateNotifierWorker =
            std::make_unique<ProximitySensorStateNotifierWorker>(this);
}

void DisplayTe2Manager::setTe2Option(bool fixedTe2) {
    int32_t ret = writeIntToFile(getPanelTe2OptionPath(), fixedTe2);
    if (!ret) {
        ALOGI("DisplayTe2Manager::%s writes te2_option(%d) to the sysfs node", __func__, fixedTe2);
        mIsOptionFixedTe2 = fixedTe2;
        if (fixedTe2) {
            setFixedTe2RateInternal(mFixedTe2RateHz, true);
        } else if (!mRefreshRateChangeListenerRegistered) {
            // register listener for changeable TE2
            if (!mDisplay) {
                ALOGW("DisplayTe2Manager::%s unable to register refresh rate change listener",
                      __func__);
            } else if (!mDisplay->registerRefreshRateChangeListener(
                               static_cast<std::shared_ptr<RefreshRateChangeListener>>(this))) {
                mRefreshRateChangeListenerRegistered = true;
            } else {
                ALOGW("DisplayTe2Manager::%s failed to register refresh rate change listener",
                      __func__);
            }
        }
    } else {
        ALOGW("DisplayTe2Manager::%s failed to write te2_option(%d) to the sysfs node", __func__,
              fixedTe2);
    }
}

int32_t DisplayTe2Manager::setTe2Rate(int targetTe2RateHz) {
    int32_t ret = writeIntToFile(getPanelTe2RatePath(), targetTe2RateHz);
    if (!ret) {
        ALOGI("DisplayTe2Manager::%s writes te2_rate_hz(%d) to the sysfs node", __func__,
              targetTe2RateHz);
    } else {
        ALOGW("DisplayTe2Manager::%s failed to write te2_rate_hz(%d) to the sysfs node", __func__,
              targetTe2RateHz);
    }
    return ret;
}

int32_t DisplayTe2Manager::setFixedTe2Rate(int targetTe2RateHz) {
    Mutex::Autolock lock(mTe2Mutex);
    return setFixedTe2RateInternal(targetTe2RateHz, false);
}

int32_t DisplayTe2Manager::setFixedTe2RateInternal(int targetTe2RateHz, bool enforce) {
    if (!mIsOptionFixedTe2) {
        ALOGW("DisplayTe2Manager::%s current option is not fixed TE2", __func__);
        return -EINVAL;
    }
    if (targetTe2RateHz == mFixedTe2RateHz && !enforce) {
        return NO_ERROR;
    } else {
        int32_t ret = setTe2Rate(targetTe2RateHz);
        if (!ret) mFixedTe2RateHz = targetTe2RateHz;
        return ret;
    }
}

int32_t DisplayTe2Manager::setChangeableTe2Rate(int targetTe2RateHz) {
    if (mIsOptionFixedTe2) {
        ALOGW("DisplayTe2Manager::%s current option is not changeable", __func__);
        return -EINVAL;
    }
    if (!mDisplay) {
        ALOGW("DisplayTe2Manager::%s unable to get peak refresh rate", __func__);
        return -EINVAL;
    }

    // While the proximity sensor is active, changeable TE2 should be used. In this case, it
    // should have the tolerance to receive only min (idle) and target (active) notifications of
    // refresh rate changes and ignore the intermediate values.
    if (targetTe2RateHz == mMinRefreshRateForFixedTe2 ||
        targetTe2RateHz == mDisplay->getRefreshRate(mDisplay->mActiveConfig)) {
        return setTe2Rate(targetTe2RateHz);
    } else {
        return NO_ERROR;
    }
}

void DisplayTe2Manager::updateTe2OptionForProximity(bool proximityActive, int minRefreshRate,
                                                    bool dozeMode) {
    Mutex::Autolock lock(mTe2Mutex);
    bool isOptionFixed = (!proximityActive || (proximityActive && dozeMode));
    // update the min refresh rate for changeable TE2 usage
    if (minRefreshRate) mMinRefreshRateForFixedTe2 = minRefreshRate;
    if (proximityActive && dozeMode) mPendingOptionChangeableTe2 = true;
    if (isOptionFixed == mIsOptionFixedTe2) return;

    setTe2Option(isOptionFixed);
}

void DisplayTe2Manager::updateTe2ForDozeMode() {
    Mutex::Autolock lock(mTe2Mutex);
    mPendingFixedTe2Rate = mFixedTe2RateHz;
    mFixedTe2RateHz = kFixedTe2RateForDozeMode;

    if (!mIsOptionFixedTe2) {
        mPendingOptionChangeableTe2 = true;
        setTe2Option(true);
    }
}

void DisplayTe2Manager::restoreTe2FromDozeMode() {
    Mutex::Autolock lock(mTe2Mutex);
    if (mPendingFixedTe2Rate) mFixedTe2RateHz = mPendingFixedTe2Rate;

    if (mPendingOptionChangeableTe2) {
        setTe2Option(false);
        mPendingOptionChangeableTe2 = false;
    }
}

void DisplayTe2Manager::onRefreshRateChange(int refreshRate) {
    Mutex::Autolock lock(mTe2Mutex);
    if (!mIsOptionFixedTe2 && refreshRate) setChangeableTe2Rate(refreshRate);
}

void DisplayTe2Manager::handleProximitySensorStateChange(bool active) {
    if (mProximitySensorStateNotifierWorker) {
        mProximitySensorStateNotifierWorker->onStateChanged(active);
    } else {
        ALOGW("DisplayTe2Manager::%s unable to handle the state change", __func__);
    }
}

void DisplayTe2Manager::dump(String8& result) const {
    result.appendFormat("DisplayTe2Manager:\n");
    result.appendFormat("\tmin refresh rate for fixed TE2: %d\n", mMinRefreshRateForFixedTe2);
    if (!mIsOptionFixedTe2) {
        result.appendFormat("\tcurrent TE2: changeable\n");
    } else {
        result.appendFormat("\tcurrent TE2: fixed %d Hz\n", mFixedTe2RateHz);
    }
    result.appendFormat("\n");
}

DisplayTe2Manager::ProximitySensorStateNotifierWorker::ProximitySensorStateNotifierWorker(
        DisplayTe2Manager* te2Manager)
      : Worker("ProximitySensorStateNotifierWorker", HAL_PRIORITY_URGENT_DISPLAY),
        mTe2Manager(te2Manager),
        mIsStateActive(false),
        mReceivedFirstStateAfterTimeout(false),
        mPendingState(ProximitySensorState::NONE) {
    InitWorker();
}

DisplayTe2Manager::ProximitySensorStateNotifierWorker::~ProximitySensorStateNotifierWorker() {
    Exit();
}

void DisplayTe2Manager::ProximitySensorStateNotifierWorker::onStateChanged(bool active) {
    ATRACE_INT("proximitySensorState(HAL)", active);
    Lock();
    mIsStateActive = active;
    Unlock();
    Signal();
}

void DisplayTe2Manager::ProximitySensorStateNotifierWorker::Routine() {
    ATRACE_NAME("StateNotifierWorker");
    int ret;
    Lock();
    ret = WaitForSignalOrExitLocked(ms2ns(kDebounceTimeMs));
    if (ret == -EINTR) {
        ALOGE("ProximitySensorStateNotifierWorker: failed to wait for signal");
        mReceivedFirstStateAfterTimeout = false;
        Unlock();
        return;
    }

    if (!mReceivedFirstStateAfterTimeout) {
        if (ret != -ETIMEDOUT) {
            // the 1st signal after timeout, send the notification immediately
            ALOGI("ProximitySensorStateNotifierWorker: %s: notify state (%d)", __func__,
                  mIsStateActive);
            mTe2Manager->mDisplay->onProximitySensorStateChanged(mIsStateActive);
            mReceivedFirstStateAfterTimeout = true;
        }
    } else {
        if (ret != -ETIMEDOUT) {
            if (!mIsStateActive) {
                // inactive within kDebounceTimeMs, update the pending state
                mPendingState = ProximitySensorState::INACTIVE;
            } else {
                // notify immediately if active
                ALOGI("ProximitySensorStateNotifierWorker: %s: notify state (1)", __func__);
                mTe2Manager->mDisplay->onProximitySensorStateChanged(true);
                mPendingState = ProximitySensorState::NONE;
            }
        } else {
            // no signal within kDebounceTimeMs, notify the pending state if it exists
            if (mPendingState != ProximitySensorState::NONE) {
                mIsStateActive = (mPendingState == ProximitySensorState::ACTIVE);
                ALOGI("ProximitySensorStateNotifierWorker: %s: notify pending state (%d)", __func__,
                      mIsStateActive);
                mTe2Manager->mDisplay->onProximitySensorStateChanged(mIsStateActive);
                mPendingState = ProximitySensorState::NONE;
            } else {
                mReceivedFirstStateAfterTimeout = false;
            }
        }
        ATRACE_INT("proximitySensorPendingState", static_cast<uint32_t>(mPendingState));
    }
    Unlock();
}
