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

#include "HistogramDevice.h"

#include <drm/samsung_drm.h>

#include <sstream>
#include <string>

#include "ExynosDisplayDrmInterface.h"
#include "ExynosHWCHelper.h"
#include "android-base/macros.h"

/**
 * histogramOnBinderDied
 *
 * The binderdied callback function which is registered in registerHistogram and would trigger
 * unregisterHistogram to cleanup the resources.
 *
 * @cookie pointer to the TokenInfo of the binder object.
 */
static void histogramOnBinderDied(void* cookie) {
    HistogramDevice::HistogramErrorCode histogramErrorCode;
    HistogramDevice::TokenInfo* tokenInfo = (HistogramDevice::TokenInfo*)cookie;
    ATRACE_NAME(String8::format("%s pid=%d", __func__, tokenInfo->mPid).c_str());
    ALOGI("%s: process %d with token(%p) is died", __func__, tokenInfo->mPid,
          tokenInfo->mToken.get());

    // release the histogram resources
    tokenInfo->mHistogramDevice->unregisterHistogram(tokenInfo->mToken, &histogramErrorCode);
    if (histogramErrorCode != HistogramDevice::HistogramErrorCode::NONE) {
        ALOGW("%s: failed to unregisterHistogram, error(%s)", __func__,
              aidl::com::google::hardware::pixel::display::toString(histogramErrorCode).c_str());
    }
}

HistogramDevice::HistogramDevice(ExynosDisplay* const display, const uint8_t channelCount,
                                 const std::vector<uint8_t> reservedChannels)
      : mDisplay(display) {
    // TODO: b/295786065 - Get available channels from crtc property.
    initChannels(channelCount, reservedChannels);

    // Create the death recipient which will be deleted in the destructor
    mDeathRecipient = AIBinder_DeathRecipient_new(histogramOnBinderDied);
}

HistogramDevice::~HistogramDevice() {
    if (mDeathRecipient) {
        AIBinder_DeathRecipient_delete(mDeathRecipient);
    }
}

void HistogramDevice::initDrm(DrmDevice& device, const DrmCrtc& crtc) {
    // TODO: b/295786065 - Get available channels from crtc property.
    ATRACE_NAME("HistogramDevice::initDrm");

    {
        std::unique_lock<std::mutex> lock(mInitDrmDoneMutex);
        ::android::base::ScopedLockAssertion lock_assertion(mInitDrmDoneMutex);
        ATRACE_NAME("mInitDrmDoneMutex");
        if (mInitDrmDone) {
            HIST_LOG(W, "should be called only once, ignore!");
            return;
        }

        initHistogramCapability(crtc.histogram_channel_property(0).id() != 0);
        mDrmDevice = &device;
        mInitDrmDone = true;
        mInitDrmDone_cv.notify_all();
    }

    // print the histogram capability
    String8 logString;
    dumpHistogramCapability(logString);
    ALOGI("%s", logString.c_str());
    HIST_LOG(D, "successfully");
}

bool HistogramDevice::waitInitDrmDone() const {
    ATRACE_CALL();
    std::unique_lock<std::mutex> lock(mInitDrmDoneMutex);
    ::android::base::ScopedLockAssertion lock_assertion(mInitDrmDoneMutex);

    mInitDrmDone_cv.wait_for(lock, std::chrono::milliseconds(50), [this]() -> bool {
        ::android::base::ScopedLockAssertion lock_assertion(mInitDrmDoneMutex);
        return mInitDrmDone;
    });

    return mInitDrmDone;
}

ndk::ScopedAStatus HistogramDevice::getHistogramCapability(
        HistogramCapability* histogramCapability) const {
    ATRACE_CALL();

    if (!histogramCapability) {
        HIST_LOG(E, "binder error, histogramCapability is nullptr");
        return ndk::ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
    }

    if (waitInitDrmDone() == false) {
        HIST_LOG(E, "initDrm is not completed yet");
        return errorToStatus(HistogramErrorCode::TRY_AGAIN);
    }

    std::shared_lock lock(mHistogramCapabilityMutex);
    *histogramCapability = mHistogramCapability;

    return ndk::ScopedAStatus::ok();
}

#if defined(EXYNOS_HISTOGRAM_CHANNEL_REQUEST)
ndk::ScopedAStatus HistogramDevice::registerHistogram(const ndk::SpAIBinder& token,
                                                      const HistogramConfig& histogramConfig,
                                                      HistogramErrorCode* histogramErrorCode) {
    ATRACE_CALL();

    if (waitInitDrmDone() == false) {
        HIST_LOG(E, "initDrm is not completed yet");
        // TODO: b/323158344 - add retry error in HistogramErrorCode and return here.
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    {
        std::shared_lock lock(mHistogramCapabilityMutex);
        if (UNLIKELY(!mHistogramCapability.supportMultiChannel)) {
            HIST_LOG(E, "multi-channel interface is not supported");
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        }
    }

    ndk::ScopedAStatus binderStatus =
            validateHistogramRequest(token, histogramConfig, histogramErrorCode);
    if (!binderStatus.isOk() || *histogramErrorCode != HistogramErrorCode::NONE) {
        HIST_LOG(E, "validateHistogramRequest failed");
        return binderStatus;
    }

    // Create the histogram config blob if possible, early creation can reduce critical section
    int ret;
    const auto [displayActiveH, displayActiveV] = snapDisplayActiveSize();
    std::shared_ptr<PropertyBlob> drmConfigBlob;
    if ((ret = createDrmConfigBlob(histogramConfig, displayActiveH, displayActiveV, drmConfigBlob)))
        HIST_LOG(D, "createDrmConfigBlob failed, skip creation, ret(%d)", ret);

    bool needRefresh = false;

    {
        // Insert new client's token into mTokenInfoMap
        SCOPED_HIST_LOCK(mHistogramMutex);
        auto [it, emplaceResult] =
                mTokenInfoMap.try_emplace(token.get(), this, token, AIBinder_getCallingPid());
        if (!emplaceResult) {
            HIST_LOG(E, "BAD_TOKEN, token(%p) is already registered", token.get());
            *histogramErrorCode = HistogramErrorCode::BAD_TOKEN;
            return ndk::ScopedAStatus::ok();
        }
        auto tokenInfo = &it->second;

        /* In previous design, histogram client is attached to the histogram channel directly. Now
         * we use struct ConfigInfo to maintain the config metadata. We can benefit from this
         * design:
         * 1. More elegantly to change the applied config of the histogram channels (basics of
         *    virtualization).
         * 2. We may be able to share the same struct ConfigInfo for different histogram clients
         *    when the histogramConfigs via registerHistogram are the same. */
        auto& configInfo = tokenInfo->mConfigInfo;
        replaceConfigInfo(configInfo, &histogramConfig);

        // Attach the histogram drmConfigBlob to the configInfo
        if (drmConfigBlob)
            configInfo->mBlobsList.emplace_front(displayActiveH, displayActiveV, drmConfigBlob);

        needRefresh = scheduler();

        /* link the binder object (token) to the death recipient. When the binder object is
         * destructed, the callback function histogramOnBinderDied can release the histogram
         * resources automatically. */
        binder_status_t status;
        if ((status = AIBinder_linkToDeath(token.get(), mDeathRecipient, tokenInfo))) {
            /* Not return error due to the AIBinder_linkToDeath because histogram function can
             * still work */
            HIST_CH_LOG(E, configInfo->mChannelId, "token(%p): AIBinder_linkToDeath error, ret(%d)",
                        token.get(), status);
        }
    }

    if (needRefresh) {
        ATRACE_NAME("HistogramOnRefresh");
        mDisplay->mDevice->onRefresh(mDisplay->mDisplayId);
    }

    HIST_LOG(D, "register client successfully");

    return ndk::ScopedAStatus::ok();
}
#else
ndk::ScopedAStatus HistogramDevice::registerHistogram(const ndk::SpAIBinder& token,
                                                      const HistogramConfig& histogramConfig,
                                                      HistogramErrorCode* histogramErrorCode) {
    ATRACE_CALL();
    HIST_LOG(E, "multi-channel interface is not supported");
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
#endif

ndk::ScopedAStatus HistogramDevice::queryHistogram(const ndk::SpAIBinder& token,
                                                   std::vector<char16_t>* histogramBuffer,
                                                   HistogramErrorCode* histogramErrorCode) {
    ATRACE_CALL();

    {
        std::shared_lock lock(mHistogramCapabilityMutex);
        if (UNLIKELY(!mHistogramCapability.supportMultiChannel)) {
            HIST_LOG(E, "multi-channel interface is not supported");
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        }
    }

    // validate the argument (histogramBuffer)
    if (!histogramBuffer) {
        HIST_LOG(E, "binder error, histogramBuffer is nullptr");
        return ndk::ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
    }

    // validate the argument (histogramErrorCode)
    if (!histogramErrorCode) {
        HIST_LOG(E, "binder error, histogramErrorCode is nullptr");
        return ndk::ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
    }

    getHistogramData(token, histogramBuffer, histogramErrorCode);

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus HistogramDevice::reconfigHistogram(const ndk::SpAIBinder& token,
                                                      const HistogramConfig& histogramConfig,
                                                      HistogramErrorCode* histogramErrorCode) {
    ATRACE_CALL();

    {
        std::shared_lock lock(mHistogramCapabilityMutex);
        if (UNLIKELY(!mHistogramCapability.supportMultiChannel)) {
            HIST_LOG(E, "multi-channel interface is not supported");
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        }
    }

    ndk::ScopedAStatus binderStatus =
            validateHistogramRequest(token, histogramConfig, histogramErrorCode);
    if (!binderStatus.isOk() || *histogramErrorCode != HistogramErrorCode::NONE) {
        HIST_LOG(E, "validateHistogramRequest failed");
        return binderStatus;
    }

    // Create the histogram config blob if possible, early creation can reduce critical section
    int ret;
    const auto [displayActiveH, displayActiveV] = snapDisplayActiveSize();
    std::shared_ptr<PropertyBlob> drmConfigBlob;
    if ((ret = createDrmConfigBlob(histogramConfig, displayActiveH, displayActiveV, drmConfigBlob)))
        HIST_LOG(D, "createDrmConfigBlob failed, skip creation, ret(%d)", ret);

    bool needRefresh = false;

    {
        // Search the registered tokenInfo
        TokenInfo* tokenInfo = nullptr;
        SCOPED_HIST_LOCK(mHistogramMutex);
        if ((*histogramErrorCode = searchTokenInfo(token, tokenInfo)) != HistogramErrorCode::NONE) {
            HIST_LOG(E, "searchTokenInfo failed, error(%s)",
                     aidl::com::google::hardware::pixel::display::toString(*histogramErrorCode)
                             .c_str());
            return ndk::ScopedAStatus::ok();
        }

        // Change the histogram configInfo
        auto& configInfo = tokenInfo->mConfigInfo;
        replaceConfigInfo(configInfo, &histogramConfig);

        // Attach the histogram drmConfigBlob to the configInfo
        if (drmConfigBlob)
            configInfo->mBlobsList.emplace_front(displayActiveH, displayActiveV, drmConfigBlob);

        if (configInfo->mStatus == ConfigInfo::Status_t::HAS_CHANNEL_ASSIGNED) needRefresh = true;
    }

    if (needRefresh) {
        ATRACE_NAME("HistogramOnRefresh");
        mDisplay->mDevice->onRefresh(mDisplay->mDisplayId);
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus HistogramDevice::unregisterHistogram(const ndk::SpAIBinder& token,
                                                        HistogramErrorCode* histogramErrorCode) {
    ATRACE_CALL();

    {
        std::shared_lock lock(mHistogramCapabilityMutex);
        if (UNLIKELY(!mHistogramCapability.supportMultiChannel)) {
            HIST_LOG(E, "multi-channel interface is not supported");
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        }
    }

    // validate the argument (histogramErrorCode)
    if (!histogramErrorCode) {
        HIST_LOG(E, "binder error, histogramErrorCode is nullptr");
        return ndk::ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
    }

    // default histogramErrorCode: no error
    *histogramErrorCode = HistogramErrorCode::NONE;

    bool needRefresh = false;

    {
        // Search the registered tokenInfo
        TokenInfo* tokenInfo = nullptr;
        SCOPED_HIST_LOCK(mHistogramMutex);
        if ((*histogramErrorCode = searchTokenInfo(token, tokenInfo)) != HistogramErrorCode::NONE) {
            HIST_LOG(E, "searchTokenInfo failed, error(%s)",
                     aidl::com::google::hardware::pixel::display::toString(*histogramErrorCode)
                             .c_str());
            return ndk::ScopedAStatus::ok();
        }

        // Clear the histogram configInfo
        replaceConfigInfo(tokenInfo->mConfigInfo, nullptr);

        /*
         * If AIBinder is alive, the unregisterHistogram is triggered from the histogram client, and
         * we need to unlink the binder object from death notification. If AIBinder is already dead,
         * the unregisterHistogram is triggered from binderdied callback, no need to unlink here.
         */
        if (LIKELY(AIBinder_isAlive(token.get()))) {
            binder_status_t status;
            if ((status = AIBinder_unlinkToDeath(token.get(), mDeathRecipient, tokenInfo))) {
                // Not return error due to the AIBinder_unlinkToDeath
                HIST_LOG(E, "AIBinder_unlinkToDeath error for token(%p), ret(%d)", token.get(),
                         status);
            }
        }

        // Delete the corresponding TokenInfo after the binder object is already unlinked.
        mTokenInfoMap.erase(token.get());
        tokenInfo = nullptr;

        needRefresh = scheduler();
    }

    if (needRefresh) {
        ATRACE_NAME("HistogramOnRefresh");
        mDisplay->mDevice->onRefresh(mDisplay->mDisplayId);
    }

    HIST_LOG(D, "unregister client successfully");

    return ndk::ScopedAStatus::ok();
}

void HistogramDevice::_handleDrmEvent(void* event, uint32_t blobId, char16_t* buffer) {
    ATRACE_NAME(String8::format("handleHistogramEvent(blob#%u)", blobId).c_str());

    std::shared_ptr<BlobIdData> blobIdData;
    searchOrCreateBlobIdData(blobId, false, blobIdData);
    if (!blobIdData) {
        HIST_BLOB_LOG(W, blobId, "no condition var allocated, ignore the event(%p)", event);
        return;
    }

    std::unique_lock<std::mutex> lock(blobIdData->mDataCollectingMutex);
    ::android::base::ScopedLockAssertion lock_assertion(blobIdData->mDataCollectingMutex);
    ATRACE_NAME(String8::format("mDataCollectingMutex(blob#%u)", blobId));
    // Check if the histogram blob is collecting the histogram data
    if (UNLIKELY(blobIdData->mCollectStatus == CollectStatus_t::NOT_STARTED)) {
        HIST_BLOB_LOG(W, blobId, "ignore the event(%p), collectStatus is NOT_STARTED", event);
    } else {
        std::memcpy(blobIdData->mData, buffer, HISTOGRAM_BIN_COUNT * sizeof(char16_t));
        blobIdData->mCollectStatus = CollectStatus_t::COLLECTED;
        blobIdData->mDataCollecting_cv.notify_all();
    }
}

void HistogramDevice::handleDrmEvent(void* event) {
    int ret = NO_ERROR;
    uint32_t blobId = 0, channelId = 0;
    char16_t* buffer;

    if ((ret = parseDrmEvent(event, channelId, buffer))) {
        HIST_LOG(E, "parseDrmEvent failed, ret(%d)", ret);
        return;
    }

    // For the old kernel without blob id query supports, fake the blobId with channelId.
    // In this hack way can prevent some duplicate codes just for channel id as well.
    // In the future, all kernel will support blob id query. And can remove the hack.
    blobId = channelId;
    _handleDrmEvent(event, blobId, buffer);
}

void HistogramDevice::handleContextDrmEvent(void* event) {
    int ret = NO_ERROR;
    uint32_t blobId = 0;
    char16_t* buffer;

    if ((ret = parseContextDrmEvent(event, blobId, buffer))) {
        HIST_LOG(E, "parseContextDrmEvent failed, ret(%d)", ret);
        return;
    }

    _handleDrmEvent(event, blobId, buffer);
}

void HistogramDevice::prepareAtomicCommit(ExynosDisplayDrmInterface::DrmModeAtomicReq& drmReq) {
    {
        std::shared_lock lock(mHistogramCapabilityMutex);
        // Skip multi channel histogram commit if not supported.
        if (!mHistogramCapability.supportMultiChannel) return;
    }

    ATRACE_NAME("HistogramAtomicCommit");

    ExynosDisplayDrmInterface* moduleDisplayInterface =
            static_cast<ExynosDisplayDrmInterface*>(mDisplay->mDisplayInterface.get());
    if (!moduleDisplayInterface) {
        HIST_LOG(E, "failed to send atomic commit, moduleDisplayInterface is NULL");
        return;
    }

    const auto [displayActiveH, displayActiveV] = snapDisplayActiveSize();
    SCOPED_HIST_LOCK(mHistogramMutex);

    // Loop through every used channel and set config blob if needed.
    for (auto it = mUsedChannels.begin(); it != mUsedChannels.end();) {
        uint8_t channelId = *it;
        ChannelInfo& channel = mChannels[channelId];

        if (channel.mStatus == ChannelStatus_t::CONFIG_COMMITTED ||
            channel.mStatus == ChannelStatus_t::CONFIG_PENDING) {
            std::shared_ptr<ConfigInfo> configInfo = channel.mConfigInfo.lock();
            if (!configInfo) {
                HIST_CH_LOG(E, channelId, "expired configInfo, review code!");
                it = cleanupChannelInfo(channelId);
                continue;
            }
            setChannelConfigBlob(drmReq, channelId, moduleDisplayInterface, displayActiveH,
                                 displayActiveV, configInfo);
        }

        ++it;
    }

    // Loop through every free channels and disable channel if needed
    for (auto it = mFreeChannels.begin(); it != mFreeChannels.end(); ++it) {
        uint8_t channelId = *it;
        ChannelInfo& channel = mChannels[channelId];
        if (channel.mStatus == ChannelStatus_t::DISABLE_PENDING)
            clearChannelConfigBlob(drmReq, channelId, moduleDisplayInterface);
    }
}

void HistogramDevice::postAtomicCommit() {
    {
        std::shared_lock lock(mHistogramCapabilityMutex);
        // Skip multi channel histogram commit if not supported.
        if (!mHistogramCapability.supportMultiChannel) return;
    }

    ATRACE_CALL();

    {
        SCOPED_HIST_LOCK(mHistogramMutex);

        // Atomic commit is success, loop through every channel and update the channel status
        for (uint8_t channelId = 0; channelId < mChannels.size(); ++channelId) {
            ChannelInfo& channel = mChannels[channelId];

            switch (channel.mStatus) {
                case ChannelStatus_t::CONFIG_BLOB_ADDED:
                    channel.mStatus = ChannelStatus_t::CONFIG_COMMITTED;
                    break;
                case ChannelStatus_t::DISABLE_BLOB_ADDED:
                    channel.mStatus = ChannelStatus_t::DISABLED;
                    break;
                default:
                    break;
            }
        }
    }

    postAtomicCommitCleanup();
}

void HistogramDevice::dump(String8& result) const {
    {
        std::shared_lock lock(mHistogramCapabilityMutex);
        // Do not dump the Histogram Device if it is not supported.
        if (!mHistogramCapability.supportMultiChannel) {
            return;
        }
    }

    ATRACE_NAME("HistogramDump");

    // print the histogram capability
    dumpHistogramCapability(result);
    result.append("\n");

    SCOPED_HIST_LOCK(mHistogramMutex);

    // print the tokens and the requested configs
    for (const auto& [_, tokenInfo] : mTokenInfoMap) {
        tokenInfo.dump(result);
        if (tokenInfo.mConfigInfo) {
            tokenInfo.mConfigInfo->dump(result, "\t");
        }
    }
    dumpInternalConfigs(result);
    result.append("\n");

    // print the histogram channel info
    result.append("Histogram channel info (applied to kernel):\n");
    for (uint8_t channelId = 0; channelId < mChannels.size(); ++channelId) {
        // TODO: b/294489887 - Use buildForMiniDump can eliminate the redundant rows.
        TableBuilder tb;
        dumpChannel(tb, channelId);
        result.append(tb.build().c_str());
    }
    result.append("\n");

    // print the inactive list
    result.append("Histogram inactive list:");
    if (mInactiveConfigItList.empty()) {
        result.append(" none\n");
    } else {
        result.append("\n");
        int i = 1;
        for (const auto& configInfo : mInactiveConfigItList)
            result.appendFormat("\t%d. configInfo: %p\n", i++, configInfo.lock().get());
    }
    result.append("\n");
    result.append("-----End of Histogram dump-----\n");
}

void HistogramDevice::initChannels(const uint8_t channelCount,
                                   const std::vector<uint8_t>& reservedChannels) {
    ATRACE_CALL();
    HIST_LOG(I, "init with %u channels", channelCount);

    SCOPED_HIST_LOCK(mHistogramMutex);
    mChannels.resize(channelCount);

    for (const uint8_t reservedChannelId : reservedChannels) {
        if (reservedChannelId < mChannels.size()) {
            mChannels[reservedChannelId].mStatus = ChannelStatus_t::RESERVED;
        } else {
            HIST_CH_LOG(W, reservedChannelId,
                        "invalid channel cannot be reserved (channelCount: %u)", channelCount);
        }
    }

    for (uint8_t channelId = 0; channelId < channelCount; ++channelId) {
        if (mChannels[channelId].mStatus == ChannelStatus_t::RESERVED) {
            HIST_CH_LOG(D, channelId, "channel reserved for driver");
            continue;
        }

        mFreeChannels.push_back(channelId);
    }
}

void HistogramDevice::initHistogramCapability(const bool supportMultiChannel) {
    ATRACE_CALL();
    uint8_t channelCount = 0;
    {
        SCOPED_HIST_LOCK(mHistogramMutex);
        channelCount = mChannels.size();
    }

    ExynosDisplayDrmInterface* moduleDisplayInterface =
            static_cast<ExynosDisplayDrmInterface*>(mDisplay->mDisplayInterface.get());

    SCOPED_HIST_LOCK(mHistogramCapabilityMutex);
    if (!moduleDisplayInterface) {
        HIST_LOG(E, "failed to get panel full resolution, moduleDisplayInterface is NULL");
        mHistogramCapability.fullResolutionWidth = 0;
        mHistogramCapability.fullResolutionHeight = 0;
    } else {
        mHistogramCapability.fullResolutionWidth =
                moduleDisplayInterface->getPanelFullResolutionHSize();
        mHistogramCapability.fullResolutionHeight =
                moduleDisplayInterface->getPanelFullResolutionVSize();
    }
    mHistogramCapability.channelCount = channelCount;
    mHistogramCapability.supportMultiChannel = supportMultiChannel;
    mHistogramCapability.supportSamplePosList.push_back(HistogramSamplePos::POST_POSTPROC);
    mHistogramCapability.supportBlockingRoi = false;
    mHistogramCapability.supportQueryOpr = false;
    initPlatformHistogramCapability();
}

void HistogramDevice::replaceConfigInfo(std::shared_ptr<ConfigInfo>& configInfo,
                                        const HistogramConfig* histogramConfig) {
    ATRACE_CALL();

    // Capture the old ConfigInfo reference
    std::shared_ptr<ConfigInfo> oldConfigInfo = configInfo;

    // Populate the new ConfigInfo object based on the histogramConfig pointer
    configInfo = (histogramConfig) ? std::make_shared<ConfigInfo>(*histogramConfig) : nullptr;

    if (!oldConfigInfo && !configInfo) {
        return;
    } else if (!oldConfigInfo && configInfo) { // Case #1: registerHistogram
        addConfigToInactiveList(configInfo);
    } else if (oldConfigInfo && configInfo) { // Case #2: reconfigHistogram
        if (oldConfigInfo->mStatus == ConfigInfo::Status_t::HAS_CHANNEL_ASSIGNED) {
            configInfo->mStatus = ConfigInfo::Status_t::HAS_CHANNEL_ASSIGNED;
            configInfo->mChannelId = oldConfigInfo->mChannelId;
            mChannels[configInfo->mChannelId].mStatus = ChannelStatus_t::CONFIG_PENDING;
            mChannels[configInfo->mChannelId].mConfigInfo = configInfo;
        } else if (oldConfigInfo->mStatus == ConfigInfo::Status_t::IN_INACTIVE_LIST) {
            configInfo->mStatus = ConfigInfo::Status_t::IN_INACTIVE_LIST;
            configInfo->mInactiveListIt = oldConfigInfo->mInactiveListIt;
            *(configInfo->mInactiveListIt) = configInfo;
        } else {
            addConfigToInactiveList(configInfo);
        }
    } else if (oldConfigInfo && !configInfo) { // Case #3: unregisterHistogram
        if (oldConfigInfo->mStatus == ConfigInfo::Status_t::HAS_CHANNEL_ASSIGNED)
            cleanupChannelInfo(oldConfigInfo->mChannelId);
        else if (oldConfigInfo->mStatus == ConfigInfo::Status_t::IN_INACTIVE_LIST)
            mInactiveConfigItList.erase(oldConfigInfo->mInactiveListIt);

        oldConfigInfo->mStatus = ConfigInfo::Status_t::INITIALIZED;
    }

    // Cleanup the blobIdData
    if (oldConfigInfo) {
        SCOPED_HIST_LOCK(mBlobIdDataMutex);
        for (const auto& blobInfo : oldConfigInfo->mBlobsList)
            mBlobIdDataMap.erase(blobInfo.mBlob->getId());
    }
}

HistogramDevice::HistogramErrorCode HistogramDevice::searchTokenInfo(const ndk::SpAIBinder& token,
                                                                     TokenInfo*& tokenInfo) {
    auto it = mTokenInfoMap.find(token.get());

    if (it == mTokenInfoMap.end()) {
        HIST_LOG(E, "BAD_TOKEN, token(%p) is not registered", token.get());
        tokenInfo = nullptr;
        return HistogramErrorCode::BAD_TOKEN;
    }

    tokenInfo = &it->second;
    return HistogramErrorCode::NONE;
}

std::list<std::weak_ptr<HistogramDevice::ConfigInfo>>::iterator HistogramDevice::swapInConfigInfo(
        std::shared_ptr<ConfigInfo>& configInfo) {
    // Acquire a free histogram channel, pdate used and free channels
    const uint8_t channelId = mFreeChannels.front();
    mFreeChannels.pop_front();
    mUsedChannels.insert(channelId);

    // update the ChannelInfo
    ChannelInfo& channel = mChannels[channelId];
    channel.mStatus = ChannelStatus_t::CONFIG_PENDING;
    channel.mConfigInfo = configInfo;

    // update the configInfo and the inactive list
    configInfo->mStatus = ConfigInfo::Status_t::HAS_CHANNEL_ASSIGNED;
    configInfo->mChannelId = channelId;
    auto it = mInactiveConfigItList.erase(configInfo->mInactiveListIt);
    configInfo->mInactiveListIt = mInactiveConfigItList.end();

    return it;
}

void HistogramDevice::swapOutConfigInfo(uint8_t channelId) {
    // Update used and free channels
    mFreeChannels.push_back(channelId);
    mUsedChannels.erase(channelId);

    // update the ChannelInfo
    ChannelInfo& channel = mChannels[channelId];
    std::shared_ptr<ConfigInfo> configInfo = channel.mConfigInfo.lock();
    channel.mStatus = ChannelStatus_t::DISABLE_PENDING;
    channel.mConfigInfo.reset();

    // update the configInfo and the inactive list
    if (configInfo) {
        uint32_t blobId = getActiveBlobId(configInfo->mBlobsList);
        HIST_BLOB_CH_LOG(I, blobId, channelId, "configInfo(%p) is swapped out", configInfo.get());
        addConfigToInactiveList(configInfo);
    } else
        HIST_CH_LOG(E, channelId, "expired configInfo, review code!");
}

void HistogramDevice::addConfigToInactiveList(const std::shared_ptr<ConfigInfo>& configInfo,
                                              bool addToFront) {
    configInfo->mChannelId = -1;
    configInfo->mStatus = ConfigInfo::Status_t::IN_INACTIVE_LIST;
    if (addToFront) {
        configInfo->mInactiveListIt =
                mInactiveConfigItList.emplace(mInactiveConfigItList.begin(), configInfo);
    } else {
        configInfo->mInactiveListIt =
                mInactiveConfigItList.emplace(mInactiveConfigItList.end(), configInfo);
    }
}

bool HistogramDevice::scheduler() {
    ATRACE_CALL();

    bool needRefresh = false;

    for (auto it = mInactiveConfigItList.begin(); it != mInactiveConfigItList.end();) {
        if (mFreeChannels.empty()) break;

        auto configInfo = it->lock();
        if (!configInfo) {
            HIST_LOG(W, "find expired configInfo ptr in mInactiveConfigItList, review code!");
            it = mInactiveConfigItList.erase(it);
            continue;
        }

        // Requires an onRefresh call to apply the config change of the channel
        needRefresh = true;

        // Swap in the config
        it = swapInConfigInfo(configInfo);
    }

    return needRefresh;
}

void HistogramDevice::searchOrCreateBlobIdData(uint32_t blobId, bool create,
                                               std::shared_ptr<BlobIdData>& blobIdData) {
    ATRACE_CALL();
    blobIdData = nullptr;
    SCOPED_HIST_LOCK(mBlobIdDataMutex);

    auto it = mBlobIdDataMap.find(blobId);
    if (it != mBlobIdDataMap.end()) {
        blobIdData = it->second;
    } else if (create) {
        std::shared_ptr<BlobIdData> blobIdDataTmp = std::make_shared<BlobIdData>();
        mBlobIdDataMap.emplace(blobId, blobIdDataTmp);
        blobIdData = blobIdDataTmp;
    }
}

void HistogramDevice::getChanIdBlobId(const ndk::SpAIBinder& token,
                                      HistogramErrorCode* histogramErrorCode, int& channelId,
                                      uint32_t& blobId) {
    ATRACE_CALL();
    TokenInfo* tokenInfo = nullptr;
    channelId = -1;
    blobId = 0;

    SCOPED_HIST_LOCK(mHistogramMutex);
    if ((*histogramErrorCode = searchTokenInfo(token, tokenInfo)) != HistogramErrorCode::NONE) {
        HIST_LOG(E, "searchTokenInfo failed, ret(%s)",
                 aidl::com::google::hardware::pixel::display::toString(*histogramErrorCode)
                         .c_str());
        return;
    }

    std::shared_ptr<ConfigInfo>& configInfo = tokenInfo->mConfigInfo;
    if (configInfo->mStatus == ConfigInfo::Status_t::HAS_CHANNEL_ASSIGNED)
        channelId = configInfo->mChannelId;
    else
        channelId = -1;
#if defined(EXYNOS_CONTEXT_HISTOGRAM_EVENT_REQUEST)
    blobId = getActiveBlobId(configInfo->mBlobsList);

    if (!blobId) {
        HIST_BLOB_CH_LOG(E, blobId, channelId, "CONFIG_HIST_ERROR, blob is not created yet");
        *histogramErrorCode = HistogramErrorCode::CONFIG_HIST_ERROR;
        return;
    }
#else
    // For the old kernel without blob id query supports, fake the blobId with channelId.
    // In this hack way can prevent some duplicate codes just for channel id as well.
    // In the future, all kernel will support blob id query. And can remove the hack.
    blobId = channelId;
    if (channelId < 0) {
        HIST_BLOB_CH_LOG(E, blobId, channelId, "CONFIG_HIST_ERROR, no channel executes config");
        *histogramErrorCode = HistogramErrorCode::CONFIG_HIST_ERROR;
        return;
    }
#endif
}

void HistogramDevice::getHistogramData(const ndk::SpAIBinder& token,
                                       std::vector<char16_t>* histogramBuffer,
                                       HistogramErrorCode* histogramErrorCode) {
    ATRACE_CALL();

    // default histogramErrorCode: no error
    *histogramErrorCode = HistogramErrorCode::NONE;

    // Get the current channelId and active blobId
    int channelId;
    uint32_t blobId;
    getChanIdBlobId(token, histogramErrorCode, channelId, blobId);
    if (*histogramErrorCode != HistogramErrorCode::NONE) return;

    std::cv_status cv_status;

    {
        // Get the moduleDisplayInterface pointer
        ExynosDisplayDrmInterface* moduleDisplayInterface =
                static_cast<ExynosDisplayDrmInterface*>(mDisplay->mDisplayInterface.get());
        if (!moduleDisplayInterface) {
            *histogramErrorCode = HistogramErrorCode::ENABLE_HIST_ERROR;
            HIST_BLOB_CH_LOG(E, blobId, channelId,
                             "ENABLE_HIST_ERROR, moduleDisplayInterface is NULL");
            return;
        }

        // Use shared_ptr to keep the blobIdData which will be used by receiveBlobIdData and
        // receiveBlobIdData.
        std::shared_ptr<BlobIdData> blobIdData;
        searchOrCreateBlobIdData(blobId, true, blobIdData);

        std::unique_lock<std::mutex> lock(blobIdData->mDataCollectingMutex);
        ::android::base::ScopedLockAssertion lock_assertion(blobIdData->mDataCollectingMutex);
        ATRACE_NAME(String8::format("mDataCollectingMutex(blob#%u)", blobId));

        // Request the drmEvent of the blobId (with mDataCollectingMutex held)
        requestBlobIdData(moduleDisplayInterface, histogramErrorCode, channelId, blobId,
                          blobIdData);
        if (*histogramErrorCode != HistogramErrorCode::NONE) return;

        // Receive the drmEvent of the blobId (with mDataCollectingMutex held)
        cv_status = receiveBlobIdData(moduleDisplayInterface, histogramBuffer, histogramErrorCode,
                                      channelId, blobId, blobIdData, lock);
    }

    // Check the query result and clear the buffer if needed (no lock is held now)
    checkQueryResult(histogramBuffer, histogramErrorCode, channelId, blobId, cv_status);
}

void HistogramDevice::requestBlobIdData(ExynosDisplayDrmInterface* const moduleDisplayInterface,
                                        HistogramErrorCode* histogramErrorCode, const int channelId,
                                        const uint32_t blobId,
                                        const std::shared_ptr<BlobIdData>& blobIdData) {
    ATRACE_CALL();
    int ret;

#if defined(EXYNOS_CONTEXT_HISTOGRAM_EVENT_REQUEST)
    /* Send the ioctl request (histogram_event_request_ioctl) which increases the ref_cnt of the
     * blobId request. The drm event is sent back with data when available. Must call
     * sendContextHistogramIoctl(CANCEL) to decrease the ref_cnt after the request. */
    if ((ret = moduleDisplayInterface->sendContextHistogramIoctl(ContextHistogramIoctl_t::REQUEST,
                                                                 blobId)) != NO_ERROR) {
        *histogramErrorCode = HistogramErrorCode::ENABLE_HIST_ERROR;
        HIST_BLOB_CH_LOG(E, blobId, channelId,
                         "ENABLE_HIST_ERROR, sendContextHistogramIoctl(REQUEST) failed, ret(%d)",
                         ret);
        return;
    }
#else
    /* Send the ioctl request (histogram_channel_request_ioctl) which increases the ref_cnt of the
     * blobId request. The drm event is sent back with data when available. Must call
     * sendHistogramChannelIoctl(CANCEL) to decrease the ref_cnt after the request. */
    if ((ret = moduleDisplayInterface->sendHistogramChannelIoctl(HistogramChannelIoctl_t::REQUEST,
                                                                 blobId)) != NO_ERROR) {
        *histogramErrorCode = HistogramErrorCode::ENABLE_HIST_ERROR;
        HIST_BLOB_CH_LOG(E, blobId, channelId,
                         "ENABLE_HIST_ERROR, sendHistogramChannelIoctl(REQUEST) failed, ret(%d)",
                         ret);
        return;
    }
#endif
    blobIdData->mCollectStatus = CollectStatus_t::COLLECTING;
}

std::cv_status HistogramDevice::receiveBlobIdData(
        ExynosDisplayDrmInterface* const moduleDisplayInterface,
        std::vector<char16_t>* histogramBuffer, HistogramErrorCode* histogramErrorCode,
        const int channelId, const uint32_t blobId, const std::shared_ptr<BlobIdData>& blobIdData,
        std::unique_lock<std::mutex>& lock) {
    ATRACE_CALL();

    // Wait until the condition variable is notified or timeout.
    std::cv_status cv_status = std::cv_status::no_timeout;
    if (blobIdData->mCollectStatus != CollectStatus_t::COLLECTED) {
        ATRACE_NAME(String8::format("waitDrmEvent(noMutex,blob#%u)", blobId).c_str());
        cv_status = blobIdData->mDataCollecting_cv.wait_for(lock, std::chrono::milliseconds(50));
    }

    // Wait for the drm event is finished, decrease ref_cnt.
    int ret;
#if defined(EXYNOS_CONTEXT_HISTOGRAM_EVENT_REQUEST)
    if ((ret = moduleDisplayInterface->sendContextHistogramIoctl(ContextHistogramIoctl_t::CANCEL,
                                                                 blobId)) != NO_ERROR) {
        HIST_BLOB_CH_LOG(W, blobId, channelId, "sendContextHistogramIoctl(CANCEL) failed, ret(%d)",
                         ret);
    }
#else
    if ((ret = moduleDisplayInterface->sendHistogramChannelIoctl(HistogramChannelIoctl_t::CANCEL,
                                                                 blobId)) != NO_ERROR) {
        HIST_BLOB_CH_LOG(W, blobId, channelId, "sendHistogramChannelIoctl(CANCEL) failed, ret(%d)",
                         ret);
    }
#endif

    /*
     * Case #1: timeout occurs, status is not COLLECTED
     * Case #2: no timeout, status is not COLLECTED
     * Case #3: timeout occurs, status is COLLECTED
     * Case #4: no timeout, status is COLLECTED
     */
    if (blobIdData->mCollectStatus == CollectStatus_t::COLLECTED) {
        cv_status = std::cv_status::no_timeout; // ignore the timeout in Case #3
        // Copy the histogram data from histogram info to histogramBuffer
        histogramBuffer->assign(blobIdData->mData, blobIdData->mData + HISTOGRAM_BIN_COUNT);
    } else {
        // Case #1 and Case #2 will be checked by checkQueryResult
        *histogramErrorCode = HistogramErrorCode::BAD_HIST_DATA;
        blobIdData->mCollectStatus = CollectStatus_t::NOT_STARTED;
    }

    return cv_status;
}

void HistogramDevice::checkQueryResult(std::vector<char16_t>* histogramBuffer,
                                       HistogramErrorCode* histogramErrorCode, const int channelId,
                                       const uint32_t blobId,
                                       const std::cv_status cv_status) const {
    ATRACE_CALL();

    /*
     * It may take for a while in isSecureContentPresenting() and isPowerModeOff().
     * We should not hold any lock from the caller.
     */
    if (mDisplay->isSecureContentPresenting()) {
        HIST_BLOB_CH_LOG(V, blobId, channelId,
                         "DRM_PLAYING, data is not available when secure content is presenting");
        *histogramErrorCode = HistogramErrorCode::DRM_PLAYING;
    } else if (*histogramErrorCode != HistogramErrorCode::NONE) {
        if (cv_status == std::cv_status::timeout) {
            if (mDisplay->isPowerModeOff()) {
                HIST_BLOB_CH_LOG(W, blobId, channelId, "DISPLAY_POWEROFF, data is not available");
                *histogramErrorCode = HistogramErrorCode::DISPLAY_POWEROFF;
            } else {
                HIST_BLOB_CH_LOG(E, blobId, channelId, "BAD_HIST_DATA, no event is handled");
                *histogramErrorCode = HistogramErrorCode::BAD_HIST_DATA;
            }
        } else {
            HIST_BLOB_CH_LOG(I, blobId, channelId, "RRS detected, cv is notified without data");
        }
    }

    if (*histogramErrorCode != HistogramErrorCode::NONE) {
        // Clear the histogramBuffer when error occurs
        histogramBuffer->assign(HISTOGRAM_BIN_COUNT, 0);
    }

    ATRACE_NAME(aidl::com::google::hardware::pixel::display::toString(*histogramErrorCode).c_str());
}

// TODO: b/295990513 - Remove the if defined after kernel prebuilts are merged.
#if defined(EXYNOS_HISTOGRAM_CHANNEL_REQUEST)
int HistogramDevice::parseDrmEvent(const void* const event, uint32_t& channelId,
                                   char16_t*& buffer) const {
    ATRACE_NAME(String8::format("parseHistogramDrmEvent(%p)", event).c_str());
    if (!event) {
        HIST_LOG(E, "event is NULL");
        return BAD_VALUE;
    }

    const struct exynos_drm_histogram_channel_event* const histogram_channel_event =
            (struct exynos_drm_histogram_channel_event*)event;
    channelId = histogram_channel_event->hist_id;
    buffer = (char16_t*)&histogram_channel_event->bins;
    return NO_ERROR;
}
#else
int HistogramDevice::parseDrmEvent(const void* const event, uint32_t& channelId,
                                   char16_t*& buffer) const {
    HIST_LOG(E,
             "not supported by kernel, struct exynos_drm_histogram_channel_event is not defined");
    channelId = 0;
    buffer = nullptr;
    return INVALID_OPERATION;
}
#endif

#if defined(EXYNOS_CONTEXT_HISTOGRAM_EVENT_REQUEST)
int HistogramDevice::parseContextDrmEvent(const void* const event, uint32_t& blobId,
                                          char16_t*& buffer) const {
    ATRACE_NAME(String8::format("parseHistogramDrmEvent(%p)", event).c_str());
    if (!event) {
        HIST_LOG(E, "event is NULL");
        return BAD_VALUE;
    }

    const struct exynos_drm_context_histogram_event* const context_histogram_event =
            (struct exynos_drm_context_histogram_event*)event;
    blobId = context_histogram_event->user_handle;
    buffer = (char16_t*)&context_histogram_event->bins;
    return NO_ERROR;
}
#else
int HistogramDevice::parseContextDrmEvent(const void* const event, uint32_t& blobId,
                                          char16_t*& buffer) const {
    HIST_LOG(E,
             "not supported by kernel, struct exynos_drm_context_histogram_event is not defined");
    blobId = 0;
    buffer = nullptr;
    return INVALID_OPERATION;
}
#endif

std::set<uint8_t>::iterator HistogramDevice::cleanupChannelInfo(const uint8_t channelId) {
    mChannels[channelId].mStatus = ChannelStatus_t::DISABLE_PENDING;
    mChannels[channelId].mConfigInfo.reset();
    mFreeChannels.push_back(channelId);
    return mUsedChannels.erase(mUsedChannels.find(channelId));
}

void HistogramDevice::setChannelConfigBlob(ExynosDisplayDrmInterface::DrmModeAtomicReq& drmReq,
                                           const uint8_t channelId,
                                           ExynosDisplayDrmInterface* const moduleDisplayInterface,
                                           const int displayActiveH, const int displayActiveV,
                                           const std::shared_ptr<ConfigInfo>& configInfo) {
    ATRACE_NAME(String8::format("%s(chan#%u)", __func__, channelId));
    ChannelInfo& channel = mChannels[channelId];
    int ret;
    bool isRRS = false;
    uint32_t blobId = getMatchBlobId(configInfo->mBlobsList, displayActiveH, displayActiveV, isRRS);

    // Early return for config blob already committed and no RRS occurs.
    if (channel.mStatus == ChannelStatus_t::CONFIG_COMMITTED && blobId && !isRRS) return;

    // Create the histogram config blob when no matched blob found.
    if (!blobId) {
        std::shared_ptr<PropertyBlob> drmConfigBlob;
        ret = createDrmConfigBlob(configInfo->mRequestedConfig, displayActiveH, displayActiveV,
                                  drmConfigBlob);
        if (ret || !drmConfigBlob) {
            if (ret == NO_INIT) {
                HIST_CH_LOG(D, channelId, "skip channel config");
                channel.mStatus = ChannelStatus_t::CONFIG_PENDING;
            } else {
                HIST_CH_LOG(E, channelId, "createDrmConfigBlob failed, ret(%d)", ret);
                channel.mStatus = ChannelStatus_t::CONFIG_ERROR;
            }
            return;
        }

        // Attach the histogram drmConfigBlob to the configInfo
        if (!configInfo->mBlobsList.empty()) isRRS = true;
        configInfo->mBlobsList.emplace_front(displayActiveH, displayActiveV, drmConfigBlob);
        blobId = drmConfigBlob->getId();
    }

    if (channel.mStatus == ChannelStatus_t::CONFIG_COMMITTED && isRRS) {
        HIST_BLOB_CH_LOG(I, blobId, channelId, "RRS (%dx%d) detected", displayActiveH,
                         displayActiveV);
    }

    // Add histogram config blob to atomic commit
    if ((ret = moduleDisplayInterface->setHistogramChannelConfigBlob(drmReq, channelId, blobId))) {
        HIST_BLOB_CH_LOG(E, blobId, channelId, "setHistogramChannelConfigBlob failed, ret(%d)",
                         ret);
        channel.mStatus = ChannelStatus_t::CONFIG_ERROR;
    } else {
        channel.mStatus = ChannelStatus_t::CONFIG_BLOB_ADDED;
    }
}

void HistogramDevice::clearChannelConfigBlob(
        ExynosDisplayDrmInterface::DrmModeAtomicReq& drmReq, const uint8_t channelId,
        ExynosDisplayDrmInterface* const moduleDisplayInterface) {
    ATRACE_NAME(String8::format("%s(chan#%u)", __func__, channelId));
    ChannelInfo& channel = mChannels[channelId];
    int ret;

    if ((ret = moduleDisplayInterface->clearHistogramChannelConfigBlob(drmReq, channelId))) {
        HIST_CH_LOG(E, channelId, "clearHistogramChannelConfigBlob failed, ret(%d)", ret);
        channel.mStatus = ChannelStatus_t::DISABLE_ERROR;
    } else {
        channel.mStatus = ChannelStatus_t::DISABLE_BLOB_ADDED;
    }
}

uint32_t HistogramDevice::getMatchBlobId(std::list<BlobInfo>& blobsList, const int displayActiveH,
                                         const int displayActiveV, bool& isPositionChanged) const {
    auto resultIt = blobsList.end();

    for (auto it = blobsList.begin(); it != blobsList.end(); ++it) {
        if (it->mDisplayActiveH == displayActiveH && it->mDisplayActiveV == displayActiveV) {
            resultIt = it;
            break;
        }
    }

    if (resultIt == blobsList.end()) return 0;

    // Move the matched config blob to the front
    if (resultIt != blobsList.begin()) {
        isPositionChanged = true;
        blobsList.splice(blobsList.begin(), blobsList, resultIt, std::next(resultIt));
    }

    return blobsList.begin()->mBlob->getId();
}

uint32_t HistogramDevice::getActiveBlobId(const std::list<BlobInfo>& blobsList) const {
    return blobsList.empty() ? 0 : blobsList.begin()->mBlob->getId();
}

// TODO: b/295990513 - Remove the if defined after kernel prebuilts are merged.
#if defined(EXYNOS_HISTOGRAM_CHANNEL_REQUEST)
int HistogramDevice::createDrmConfig(const HistogramConfig& histogramConfig,
                                     const int displayActiveH, const int displayActiveV,
                                     std::shared_ptr<void>& drmConfig,
                                     size_t& drmConfigLength) const {
    int ret = NO_ERROR;

    if (UNLIKELY(!displayActiveH || !displayActiveV)) {
        HIST_LOG(I, "active mode (%dx%d) is not initialized, skip creation", displayActiveH,
                 displayActiveV);
        return NO_INIT;
    }

    HistogramRoiRect drmRoi, drmBlockingRoi;
    if (UNLIKELY(ret = convertRoi(histogramConfig.roi, drmRoi, displayActiveH, displayActiveV,
                                  ""))) {
        HIST_LOG(E, "failed to convert roi, ret(%d)", ret);
        return ret;
    }
    if (UNLIKELY(ret = convertRoi(histogramConfig.blockingRoi.value_or(DISABLED_ROI),
                                  drmBlockingRoi, displayActiveH, displayActiveV, "blocking "))) {
        HIST_LOG(E, "failed to convert blocking roi, ret(%d)", ret);
        return ret;
    }

    drmConfig = std::make_shared<struct histogram_channel_config>();
    struct histogram_channel_config* config = (struct histogram_channel_config*)drmConfig.get();
    config->roi.start_x = drmRoi.left;
    config->roi.start_y = drmRoi.top;
    config->roi.hsize = drmRoi.right - drmRoi.left;
    config->roi.vsize = drmRoi.bottom - drmRoi.top;
    if (drmBlockingRoi != DISABLED_ROI) {
        config->flags |= HISTOGRAM_FLAGS_BLOCKED_ROI;
        config->blocked_roi.start_x = drmBlockingRoi.left;
        config->blocked_roi.start_y = drmBlockingRoi.top;
        config->blocked_roi.hsize = drmBlockingRoi.right - drmBlockingRoi.left;
        config->blocked_roi.vsize = drmBlockingRoi.bottom - drmBlockingRoi.top;
    } else {
        config->flags &= ~HISTOGRAM_FLAGS_BLOCKED_ROI;
    }
    config->weights.weight_r = histogramConfig.weights.weightR;
    config->weights.weight_g = histogramConfig.weights.weightG;
    config->weights.weight_b = histogramConfig.weights.weightB;
    config->pos =
            (histogramConfig.samplePos == HistogramSamplePos::POST_POSTPROC) ? POST_DQE : PRE_DQE;
    config->threshold = calculateThreshold(drmRoi, displayActiveH, displayActiveV);

    drmConfigLength = sizeof(struct histogram_channel_config);

    return NO_ERROR;
}
#else
int HistogramDevice::createDrmConfig(const HistogramConfig& histogramConfig,
                                     const int displayActiveH, const int displayActiveV,
                                     std::shared_ptr<void>& drmConfig,
                                     size_t& drmConfigLength) const {
    HIST_LOG(E, "not supported by kernel, struct histogram_channel_config is not defined");
    drmConfig = nullptr;
    drmConfigLength = 0;
    return INVALID_OPERATION;
}
#endif

int HistogramDevice::createDrmConfigBlob(const HistogramConfig& histogramConfig,
                                         const int displayActiveH, const int displayActiveV,
                                         std::shared_ptr<PropertyBlob>& drmConfigBlob) const {
    int ret = NO_ERROR;
    std::shared_ptr<void> drmConfig;
    size_t drmConfigLength = 0;

    if ((ret = createDrmConfig(histogramConfig, displayActiveH, displayActiveV, drmConfig,
                               drmConfigLength)))
        return ret;

    std::shared_ptr<PropertyBlob> drmConfigBlobTmp =
            std::make_shared<PropertyBlob>(mDrmDevice, drmConfig.get(), drmConfigLength);
    if ((ret = drmConfigBlobTmp->getError())) {
        HIST_LOG(E, "failed to create property blob, ret(%d)", ret);
        return ret;
    }

    // If success, drmConfigBlobTmp must contain a non-zero blobId
    drmConfigBlob = drmConfigBlobTmp;

    return NO_ERROR;
}

void HistogramDevice::resetConfigInfoStatus(std::shared_ptr<ConfigInfo>& configInfo) {
    if (configInfo->mStatus == ConfigInfo::Status_t::HAS_CHANNEL_ASSIGNED)
        cleanupChannelInfo(configInfo->mChannelId);
    else if (configInfo->mStatus == ConfigInfo::Status_t::IN_INACTIVE_LIST) {
        mInactiveConfigItList.erase(configInfo->mInactiveListIt);
        configInfo->mInactiveListIt = mInactiveConfigItList.end();
    }

    configInfo->mStatus = ConfigInfo::Status_t::INITIALIZED;
}

std::pair<int, int> HistogramDevice::snapDisplayActiveSize() const {
    ExynosDisplayDrmInterface* moduleDisplayInterface =
            static_cast<ExynosDisplayDrmInterface*>(mDisplay->mDisplayInterface.get());
    if (!moduleDisplayInterface) {
        HIST_LOG(E, "failed to get active size, moduleDisplayInterface is NULL");
        return std::make_pair(0, 0);
    }

    return std::make_pair(moduleDisplayInterface->getActiveModeHDisplay(),
                          moduleDisplayInterface->getActiveModeVDisplay());
}

int HistogramDevice::convertRoi(const HistogramRoiRect& requestedRoi,
                                HistogramRoiRect& convertedRoi, const int displayActiveH,
                                const int displayActiveV, const char* roiType) const {
    int32_t panelH, panelV;

    {
        std::shared_lock lock(mHistogramCapabilityMutex);
        panelH = mHistogramCapability.fullResolutionWidth;
        panelV = mHistogramCapability.fullResolutionHeight;
    }

    HIST_LOG(V, "active: (%dx%d), panel: (%dx%d)", displayActiveH, displayActiveV, panelH, panelV);

    if (panelH < displayActiveH || displayActiveH < 0 || panelV < displayActiveV ||
        displayActiveV < 0) {
        HIST_LOG(E, "failed to convert %sroi, active: (%dx%d), panel: (%dx%d)", roiType,
                 displayActiveH, displayActiveV, panelH, panelV);
        return -EINVAL;
    }

    // Linear transform from full resolution to active resolution
    convertedRoi.left = requestedRoi.left * displayActiveH / panelH;
    convertedRoi.top = requestedRoi.top * displayActiveV / panelV;
    convertedRoi.right = requestedRoi.right * displayActiveH / panelH;
    convertedRoi.bottom = requestedRoi.bottom * displayActiveV / panelV;

    HIST_LOG(V, "working %sroi: %s", roiType, toString(convertedRoi).c_str());

    return NO_ERROR;
}

void HistogramDevice::dumpHistogramCapability(String8& result) const {
    std::shared_lock lock(mHistogramCapabilityMutex);
    // Append the histogram capability info to the dump string
    result.appendFormat("Histogram capability(%s):\n",
                        (mDisplay) ? (mDisplay->mDisplayName.c_str()) : "NULL");
    result.appendFormat("\tsupportMultiChannel: %s, ",
                        mHistogramCapability.supportMultiChannel ? "true" : "false");
    result.appendFormat("supportBlockingRoi: %s, ",
                        mHistogramCapability.supportBlockingRoi ? "true" : "false");
    result.appendFormat("supportQueryOpr: %s, ",
                        mHistogramCapability.supportQueryOpr ? "true" : "false");
    result.appendFormat("supportSamplePosList:");
    for (HistogramSamplePos samplePos : mHistogramCapability.supportSamplePosList) {
        result.appendFormat(" %s",
                            aidl::com::google::hardware::pixel::display::toString(samplePos)
                                    .c_str());
    }
    result.appendFormat("\n");
    result.appendFormat("\tchannelCount: %d, ", mHistogramCapability.channelCount);
    result.appendFormat("fullscreen roi: (0,0)x(%dx%d)\n", mHistogramCapability.fullResolutionWidth,
                        mHistogramCapability.fullResolutionHeight);
}

// TODO: b/295990513 - Remove the if defined after kernel prebuilts are merged.
#if defined(EXYNOS_HISTOGRAM_CHANNEL_REQUEST)
void HistogramDevice::dumpChannel(TableBuilder& tb, const uint8_t channelId) const {
    const ChannelInfo& channel = mChannels[channelId];
    auto configInfo = channel.mConfigInfo.lock();
    uint32_t blobId = configInfo ? getActiveBlobId(configInfo->mBlobsList) : 0;
    drmModePropertyBlobPtr blob = nullptr;

    // Get the histogram config blob
    if (blobId && mDrmDevice) {
        if ((blob = drmModeGetPropertyBlob(mDrmDevice->fd(), blobId)) == nullptr) {
            HIST_BLOB_CH_LOG(E, blobId, channelId,
                             "drmModeGetPropertyBlob failed, blob is nullptr");
        }
    }

    tb.add("ID", (int)channelId);
    tb.add("status", toString(channel.mStatus));
    tb.add("configInfo", configInfo.get());

    if (!blob) {
        if (blobId)
            tb.add("blobId", String8::format("%u (Get failed)", blobId));
        else
            tb.add("blobId", "N/A");
        tb.add("workingRoi", "N/A");
        tb.add("workingBlockRoi", "N/A");
        tb.add("threshold", "N/A");
        tb.add("weightRGB", "N/A");
        tb.add("samplePos", "N/A");
        return;
    }

    const struct histogram_channel_config* config =
            reinterpret_cast<struct histogram_channel_config*>(blob->data);
    HistogramRoiRect workingRoi = {config->roi.start_x, config->roi.start_y,
                                   config->roi.start_x + config->roi.hsize,
                                   config->roi.start_y + config->roi.vsize};
    HistogramRoiRect workingBlockRoi = {config->blocked_roi.start_x, config->blocked_roi.start_y,
                                        config->blocked_roi.start_x + config->blocked_roi.hsize,
                                        config->blocked_roi.start_y + config->blocked_roi.vsize};
    tb.add("blobId", blobId);
    tb.add("workingRoi", toString(workingRoi));
    tb.add("workingBlockRoi", toString(workingBlockRoi));
    tb.add("threshold", config->threshold);
    tb.add("weightRGB",
           String8::format("(%" PRIu16 ",%" PRIu16 ",%" PRIu16 ")", config->weights.weight_r,
                           config->weights.weight_g, config->weights.weight_b));
    tb.add("samplePos", config->pos == POST_DQE ? "POST_DQE" : "PRE_DQE");
    drmModeFreePropertyBlob(blob);
}
#else
void HistogramDevice::dumpChannel(TableBuilder& tb, const uint8_t channelId) const {}
#endif

ndk::ScopedAStatus HistogramDevice::validateHistogramRequest(
        const ndk::SpAIBinder& token, const HistogramConfig& histogramConfig,
        HistogramErrorCode* histogramErrorCode) const {
    // validate the argument (histogramErrorCode)
    if (!histogramErrorCode) {
        HIST_LOG(E, "binder error, histogramErrorCode is nullptr");
        return ndk::ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
    }

    // default histogramErrorCode: no error
    *histogramErrorCode = HistogramErrorCode::NONE;

    // validate the argument (token)
    if (token.get() == nullptr) {
        HIST_LOG(E, "BAD_TOKEN, token is nullptr");
        *histogramErrorCode = HistogramErrorCode::BAD_TOKEN;
        return ndk::ScopedAStatus::ok();
    }

    // validate the argument (histogramConfig)
    if ((*histogramErrorCode = validateHistogramConfig(histogramConfig)) !=
        HistogramErrorCode::NONE) {
        return ndk::ScopedAStatus::ok();
    }

    return ndk::ScopedAStatus::ok();
}

HistogramDevice::HistogramErrorCode HistogramDevice::validateHistogramConfig(
        const HistogramConfig& histogramConfig) const {
    HistogramErrorCode ret;

    std::shared_lock lock(mHistogramCapabilityMutex);

    if ((ret = validateHistogramRoi(histogramConfig.roi, "")) != HistogramErrorCode::NONE ||
        (ret = validateHistogramWeights(histogramConfig.weights)) != HistogramErrorCode::NONE ||
        (ret = validateHistogramSamplePos(histogramConfig.samplePos)) != HistogramErrorCode::NONE ||
        (ret = validateHistogramBlockingRoi(histogramConfig.blockingRoi)) !=
                HistogramErrorCode::NONE) {
        return ret;
    }

    return HistogramErrorCode::NONE;
}

HistogramDevice::HistogramErrorCode HistogramDevice::validateHistogramRoi(
        const HistogramRoiRect& roi, const char* roiType) const {
    if (roi == DISABLED_ROI) return HistogramErrorCode::NONE;

    if ((roi.left < 0) || (roi.top < 0) || (roi.right - roi.left <= 0) ||
        (roi.bottom - roi.top <= 0) || (roi.right > mHistogramCapability.fullResolutionWidth) ||
        (roi.bottom > mHistogramCapability.fullResolutionHeight)) {
        HIST_LOG(E, "BAD_ROI, %sroi: %s, full screen roi: (0,0)x(%dx%d)", roiType,
                 toString(roi).c_str(), mHistogramCapability.fullResolutionWidth,
                 mHistogramCapability.fullResolutionHeight);
        return HistogramErrorCode::BAD_ROI;
    }

    return HistogramErrorCode::NONE;
}

HistogramDevice::HistogramErrorCode HistogramDevice::validateHistogramWeights(
        const HistogramWeights& weights) const {
    if ((weights.weightR + weights.weightG + weights.weightB) != WEIGHT_SUM) {
        HIST_LOG(E, "BAD_WEIGHT, weights%s", toString(weights).c_str());
        return HistogramErrorCode::BAD_WEIGHT;
    }

    return HistogramErrorCode::NONE;
}

HistogramDevice::HistogramErrorCode HistogramDevice::validateHistogramSamplePos(
        const HistogramSamplePos& samplePos) const {
    for (HistogramSamplePos mSamplePos : mHistogramCapability.supportSamplePosList) {
        if (samplePos == mSamplePos) {
            return HistogramErrorCode::NONE;
        }
    }

    HIST_LOG(E, "BAD_POSITION, samplePos is %s",
             aidl::com::google::hardware::pixel::display::toString(samplePos).c_str());
    return HistogramErrorCode::BAD_POSITION;
}

HistogramDevice::HistogramErrorCode HistogramDevice::validateHistogramBlockingRoi(
        const std::optional<HistogramRoiRect>& blockingRoi) const {
    // If the platform doesn't support blockingRoi, client should not enable blockingRoi
    if (mHistogramCapability.supportBlockingRoi == false) {
        if (blockingRoi.has_value() && blockingRoi.value() != DISABLED_ROI) {
            HIST_LOG(E, "BAD_ROI, platform doesn't support blockingRoi, requested: %s",
                     toString(blockingRoi.value()).c_str());
            return HistogramErrorCode::BAD_ROI;
        }
        return HistogramErrorCode::NONE;
    }

    // For the platform that supports blockingRoi, use the same validate rule as roi
    return validateHistogramRoi(blockingRoi.value_or(DISABLED_ROI), "blocking ");
}

int HistogramDevice::calculateThreshold(const HistogramRoiRect& roi, const int displayActiveH,
                                        const int displayActiveV) const {
    // If roi is disabled, the targeted region is entire screen.
    int32_t roiH = (roi != DISABLED_ROI) ? (roi.right - roi.left) : displayActiveH;
    int32_t roiV = (roi != DISABLED_ROI) ? (roi.bottom - roi.top) : displayActiveV;
    int threshold = (roiV * roiH) >> 16;
    // TODO: b/294491895 - Check if the threshold plus one really need it
    return threshold + 1;
}

std::string HistogramDevice::toString(const ChannelStatus_t& status) {
    switch (status) {
        case ChannelStatus_t::RESERVED:
            return "RESERVED";
        case ChannelStatus_t::DISABLED:
            return "DISABLED";
        case ChannelStatus_t::CONFIG_PENDING:
            return "CONFIG_PENDING";
        case ChannelStatus_t::CONFIG_BLOB_ADDED:
            return "CONFIG_BLOB_ADDED";
        case ChannelStatus_t::CONFIG_COMMITTED:
            return "CONFIG_COMMITTED";
        case ChannelStatus_t::CONFIG_ERROR:
            return "CONFIG_ERROR";
        case ChannelStatus_t::DISABLE_PENDING:
            return "DISABLE_PENDING";
        case ChannelStatus_t::DISABLE_BLOB_ADDED:
            return "DISABLE_BLOB_ADDED";
        case ChannelStatus_t::DISABLE_ERROR:
            return "DISABLE_ERROR";
    }

    return "UNDEFINED";
}

std::string HistogramDevice::toString(const HistogramRoiRect& roi) {
    if (roi == DISABLED_ROI) return "OFF";

    std::ostringstream os;
    os << "(" << roi.left << "," << roi.top << ")";
    os << "x";
    os << "(" << roi.right << "," << roi.bottom << ")";
    return os.str();
}

std::string HistogramDevice::toString(const HistogramWeights& weights) {
    std::ostringstream os;
    os << "(";
    os << (int)weights.weightR << ",";
    os << (int)weights.weightG << ",";
    os << (int)weights.weightB;
    os << ")";
    return os.str();
}

std::string HistogramDevice::toString(const HistogramConfig& config) {
    std::ostringstream os;
    os << "roi:" << toString(config.roi) << ", ";
    os << "blockRoi:" << toString(config.blockingRoi.value_or(DISABLED_ROI)) << ", ";
    os << "weightRGB:" << toString(config.weights) << ", ";
    os << "samplePos:" << aidl::com::google::hardware::pixel::display::toString(config.samplePos);
    return os.str();
}

void HistogramDevice::TokenInfo::dump(String8& result, const char* prefix) const {
    result.appendFormat("%sHistogram token %p:\n", prefix, mToken.get());
    result.appendFormat("%s\tpid: %d\n", prefix, mPid);
    if (!mConfigInfo) {
        result.append("%s\tconfigInfo: (nullptr)\n");
    }
}

void HistogramDevice::ConfigInfo::dump(String8& result, const char* prefix) const {
    result.appendFormat("%sconfigInfo: %p -> ", prefix, this);
    if (mStatus == Status_t::HAS_CHANNEL_ASSIGNED)
        result.appendFormat("channelId: %d\n", mChannelId);
    else if (mStatus == Status_t::IN_INACTIVE_LIST)
        result.appendFormat("inactive list: queued\n");
    else
        result.appendFormat("inactive list: N/A\n");
    result.appendFormat("%s\trequestedConfig: %s\n", prefix, toString(mRequestedConfig).c_str());
    result.appendFormat("%s\tblobsList: ", prefix);
    if (!mBlobsList.empty()) {
        result.append("*");
        for (auto it = mBlobsList.begin(); it != mBlobsList.end(); ++it) {
            result.appendFormat("blob#%u(%dx%d) ", it->mBlob->getId(), it->mDisplayActiveH,
                                it->mDisplayActiveV);
        }
    } else {
        result.append("none");
    }
    result.append("\n");
}

HistogramDevice::PropertyBlob::PropertyBlob(DrmDevice* const drmDevice, const void* const blobData,
                                            const size_t blobLength)
      : mDrmDevice(drmDevice) {
    if (!mDrmDevice) {
        ALOGE("%s: mDrmDevice is nullptr", __func__);
        mError = BAD_VALUE;
        return;
    }

    if ((mError = mDrmDevice->CreatePropertyBlob(blobData, blobLength, &mBlobId))) {
        mBlobId = 0;
        ALOGE("%s: failed to create histogram config blob, ret(%d)", __func__, mError);
    } else if (!mBlobId) {
        mError = BAD_VALUE;
        ALOGE("%s: create histogram config blob successful, but blobId is 0", __func__);
    }
}

HistogramDevice::PropertyBlob::~PropertyBlob() {
    if (mError) return;

    int ret = mDrmDevice->DestroyPropertyBlob(mBlobId);
    if (ret)
        ALOGE("%s: failed to destroy histogram config blob %d, ret(%d)", __func__, mBlobId, ret);
}

uint32_t HistogramDevice::PropertyBlob::getId() const {
    return mBlobId;
}

int HistogramDevice::PropertyBlob::getError() const {
    return mError;
}
