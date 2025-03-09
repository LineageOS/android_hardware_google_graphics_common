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

#include <aidl/android/hardware/graphics/common/Rect.h>
#include <aidl/com/google/hardware/pixel/display/HistogramCapability.h>
#include <aidl/com/google/hardware/pixel/display/HistogramConfig.h>
#include <aidl/com/google/hardware/pixel/display/HistogramErrorCode.h>
#include <aidl/com/google/hardware/pixel/display/HistogramSamplePos.h>
#include <aidl/com/google/hardware/pixel/display/Weight.h>
#include <android-base/thread_annotations.h>
#include <drm/samsung_drm.h>
#include <utils/String8.h>

#include <condition_variable>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "ExynosDisplay.h"
#include "ExynosDisplayDrmInterface.h"
#include "drmcrtc.h"

#define HIST_BLOB_CH_LOG(LEVEL, blobId, channelId, msg, ...)              \
    ALOG##LEVEL("[%s,pid=%d,blob#%u,chan#%d] HistogramDevice::%s: " msg,  \
                ((mDisplay) ? (mDisplay->mDisplayName.c_str()) : "NULL"), \
                AIBinder_getCallingPid(), blobId, channelId, __func__, ##__VA_ARGS__)

#define HIST_BLOB_LOG(LEVEL, blobId, msg, ...)                            \
    ALOG##LEVEL("[%s,pid=%d,blob#%u] HistogramDevice::%s: " msg,          \
                ((mDisplay) ? (mDisplay->mDisplayName.c_str()) : "NULL"), \
                AIBinder_getCallingPid(), blobId, __func__, ##__VA_ARGS__)

#define HIST_CH_LOG(LEVEL, channelId, msg, ...)                           \
    ALOG##LEVEL("[%s,pid=%d,chan#%d] HistogramDevice::%s: " msg,          \
                ((mDisplay) ? (mDisplay->mDisplayName.c_str()) : "NULL"), \
                AIBinder_getCallingPid(), channelId, __func__, ##__VA_ARGS__)

#define HIST_LOG(LEVEL, msg, ...)                                         \
    ALOG##LEVEL("[%s,pid=%d] HistogramDevice::%s: " msg,                  \
                ((mDisplay) ? (mDisplay->mDisplayName.c_str()) : "NULL"), \
                AIBinder_getCallingPid(), __func__, ##__VA_ARGS__)

#define SCOPED_HIST_LOCK(mutex)   \
    std::scoped_lock lock(mutex); \
    ATRACE_NAME(#mutex);

using namespace android;

class HistogramDevice {
public:
    using HistogramCapability = aidl::com::google::hardware::pixel::display::HistogramCapability;
    using HistogramConfig = aidl::com::google::hardware::pixel::display::HistogramConfig;
    using HistogramErrorCode = aidl::com::google::hardware::pixel::display::HistogramErrorCode;
    using HistogramRoiRect = aidl::android::hardware::graphics::common::Rect;
    using HistogramSamplePos = aidl::com::google::hardware::pixel::display::HistogramSamplePos;
    using HistogramWeights = aidl::com::google::hardware::pixel::display::Weight;
    using ContextHistogramIoctl_t = ExynosDisplayDrmInterface::ContextHistogramIoctl_t;
    using HistogramChannelIoctl_t = ExynosDisplayDrmInterface::HistogramChannelIoctl_t;

    class PropertyBlob;

    /* For blocking roi and roi, (0, 0, 0, 0) means disabled */
    static constexpr HistogramRoiRect DISABLED_ROI = {0, 0, 0, 0};

    /* Histogram weight constraint: weightR + weightG + weightB = WEIGHT_SUM */
    static constexpr size_t WEIGHT_SUM = 1024;

    static constexpr size_t HISTOGRAM_BINS_SIZE = 256;

    /* OPR_R, OPR_G, OPR_B */
    static constexpr int kOPRConfigsCount = 3;

    struct BlobInfo {
        const int mDisplayActiveH, mDisplayActiveV;
        const std::shared_ptr<PropertyBlob> mBlob;
        BlobInfo(const int displayActiveH, const int displayActiveV,
                 const std::shared_ptr<PropertyBlob>& drmConfigBlob)
              : mDisplayActiveH(displayActiveH),
                mDisplayActiveV(displayActiveV),
                mBlob(drmConfigBlob) {}
    };

    struct ConfigInfo {
        enum class Status_t : uint8_t {
            INITIALIZED, // Not in the inactive list and no channel assigned
            IN_INACTIVE_LIST,
            HAS_CHANNEL_ASSIGNED,
        };

        const HistogramConfig mRequestedConfig;
        Status_t mStatus = Status_t::INITIALIZED;
        int mChannelId = -1;
        std::list<BlobInfo> mBlobsList;
        std::list<std::weak_ptr<ConfigInfo>>::iterator mInactiveListIt;
        ConfigInfo(const HistogramConfig& histogramConfig) : mRequestedConfig(histogramConfig) {}
        void dump(String8& result, const char* prefix = "") const;
    };

    /* Histogram channel status */
    enum class ChannelStatus_t : uint32_t {
        /* occupied by the driver for specific usage such as LHBM */
        RESERVED = 0,

        /* channel is off */
        DISABLED,

        /* channel config is ready and requires to be added into an atomic commit */
        CONFIG_PENDING,

        /* channel config (blob) is added to an atomic commit but not committed yet */
        CONFIG_BLOB_ADDED,

        /* channel config is committed to drm driver successfully */
        CONFIG_COMMITTED,

        /* channel config has error */
        CONFIG_ERROR,

        /* channel is released and requires an atomic commit to cleanup completely */
        DISABLE_PENDING,

        /* channel is released and the cleanup blob is added but not committed yet */
        DISABLE_BLOB_ADDED,

        /* channel disable has error */
        DISABLE_ERROR,
    };

    struct ChannelInfo {
        /* track the channel status */
        ChannelStatus_t mStatus;
        std::weak_ptr<ConfigInfo> mConfigInfo;

        ChannelInfo() : mStatus(ChannelStatus_t::DISABLED) {}
        ChannelInfo(const ChannelInfo& other) = default;
    };

    struct TokenInfo {
        /* The binderdied callback would call unregisterHistogram (member function of this object)
         * to release resource. */
        HistogramDevice* const mHistogramDevice;

        /* The binderdied callback would call unregisterHistogram with this token to release
         * resource. */
        const ndk::SpAIBinder mToken;

        /* The process id of the client that calls registerHistogram. */
        const pid_t mPid;

        /* The shared pointer to the ConfigInfo. */
        std::shared_ptr<ConfigInfo> mConfigInfo;

        TokenInfo(HistogramDevice* histogramDevice, const ndk::SpAIBinder& token, pid_t pid)
              : mHistogramDevice(histogramDevice), mToken(token), mPid(pid) {}
        void dump(String8& result, const char* prefix = "") const;
    };

    enum class CollectStatus_t : uint8_t {
        NOT_STARTED = 0,
        COLLECTING,
        COLLECTED,
    };

    struct BlobIdData {
        mutable std::mutex mDataCollectingMutex;
        uint16_t mData[HISTOGRAM_BIN_COUNT] GUARDED_BY(mDataCollectingMutex);
        CollectStatus_t mCollectStatus GUARDED_BY(mDataCollectingMutex) =
                CollectStatus_t::NOT_STARTED;
        std::condition_variable mDataCollecting_cv GUARDED_BY(mDataCollectingMutex);
    };

    /**
     * HistogramDevice
     *
     * Construct the HistogramDevice to mange histogram channel.
     *
     * @display display pointer which would be stored in mDisplay.
     * @channelCount number of the histogram channels in the system.
     * @reservedChannels a list of channel id that are reserved by the driver.
     */
    explicit HistogramDevice(ExynosDisplay* const display, const uint8_t channelCount,
                             const std::vector<uint8_t> reservedChannels);

    /**
     * ~HistogramDevice
     *
     * Destruct the HistogramDevice.
     */
    virtual ~HistogramDevice();

    /**
     * initDrm
     *
     * Get histogram info from crtc property and initialize the mHistogramCapability.
     *     1. The available histogram channel bitmask.
     *     2. Determine kernel support multi channel property or not.
     *
     * @device drm device object which will be used to create the config blob.
     * @crtc drm crtc object which would contain histogram related information.
     */
    void initDrm(DrmDevice& device, const DrmCrtc& crtc)
            EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

    /**
     * getHistogramCapability
     *
     * Return the histogram capability for the system.
     *
     * @histogramCapability: describe the histogram capability for the system.
     * @return ok() when the interface is supported and arguments are valid, else otherwise.
     */
    ndk::ScopedAStatus getHistogramCapability(HistogramCapability* histogramCapability) const
            EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

    /**
     * registerHistogram
     *
     * Register the histogram sampling config, and queue into the mInactiveConfigItList. Scheduler
     * will help to apply the config if possible. If the display is not turned on, just store the
     * histogram config. Otherwise, trigger the onRefresh call to force the config take effect, and
     * then the DPU hardware will continuously sample the histogram data.
     *
     * @token binder object created by the client whose lifetime should be equal to the client. When
     * the binder object is destructed, the unregisterHistogram would be called automatically. Token
     * serves as the handle in every histogram operation.
     * @histogramConfig histogram config from the client.
     * @histogramErrorCode NONE when no error, or else otherwise. Client should retry when failed.
     * @return ok() when the interface is supported, or EX_UNSUPPORTED_OPERATION when the interface
     * is not supported yet.
     */
    ndk::ScopedAStatus registerHistogram(const ndk::SpAIBinder& token,
                                         const HistogramConfig& histogramConfig,
                                         HistogramErrorCode* histogramErrorCode)
            EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

    /**
     * queryHistogram
     *
     * Query the histogram data from the corresponding channel of the token.
     *
     * @token is the handle registered via registerHistogram which would be used to identify the
     * channel.
     * @histogramBuffer 256 * 16 bits buffer to store the luma counts return by the histogram
     * hardware.
     * @histogramErrorCode NONE when no error, or else otherwise. Client should examine this
     * errorcode.
     * @return ok() when the interface is supported, or EX_UNSUPPORTED_OPERATION when the interface
     * is not supported yet.
     */
    ndk::ScopedAStatus queryHistogram(const ndk::SpAIBinder& token,
                                      std::vector<char16_t>* histogramBuffer,
                                      HistogramErrorCode* histogramErrorCode)
            EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

    /**
     * reconfigHistogram
     *
     * Change the histogram config for the corresponding channel of the token.
     *
     * @token is the handle registered via registerHistogram which would be used to identify the
     * channel.
     * @histogramConfig histogram config from the client.
     * @histogramErrorCode NONE when no error, or else otherwise. Client should examine this
     * errorcode.
     * @return ok() when the interface is supported, or EX_UNSUPPORTED_OPERATION when the interface
     * is not supported yet.
     */
    ndk::ScopedAStatus reconfigHistogram(const ndk::SpAIBinder& token,
                                         const HistogramConfig& histogramConfig,
                                         HistogramErrorCode* histogramErrorCode)
            EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

    /**
     * unregisterHistogram
     *
     * Release the corresponding channel of the token and add the channel id to free channel list.
     *
     * @token is the handle registered via registerHistogram which would be used to identify the
     * channel.
     * @histogramErrorCode NONE when no error, or else otherwise. Client should examine this
     * errorcode.
     * @return ok() when the interface is supported, or EX_UNSUPPORTED_OPERATION when the interface
     * is not supported yet.
     */
    ndk::ScopedAStatus unregisterHistogram(const ndk::SpAIBinder& token,
                                           HistogramErrorCode* histogramErrorCode)
            EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

    /**
     * queryOPR
     *
     * Query the linear space OPR.
     *
     * @oprVals will store the [OPR_R, OPR_G, OPR_B], 0 <= OPR_R, OPR_G, OPR_B <= 1
     */
    virtual ndk::ScopedAStatus queryOPR(std::array<double, kOPRConfigsCount>& oprVals)
            EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    /**
     * handleDrmEvent
     *
     * Handle the histogram blob drm event (EXYNOS_DRM_HISTOGRAM_CHANNEL_EVENT) and copy the
     * histogram data from event struct to channel info.
     *
     * @event histogram blob drm event pointer (struct exynos_drm_histogram_channel_event *)
     */
    void handleDrmEvent(void* event) EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

    /**
     * handleContextDrmEvent
     *
     * Handle the histogram blob drm event (EXYNOS_DRM_CONTEXT_HISTOGRAM_EVENT) and copy the
     * histogram data from event struct to blobIdData.
     *
     * @event context histogram event pointer (struct exynos_drm_context_histogram_event *)
     */
    void handleContextDrmEvent(void* event)
            EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

    /**
     * prepareAtomicCommit
     *
     * Prepare the histogram atomic commit for each channel (see prepareChannelCommit).
     *
     * @drmReq drm atomic request object
     */
    void prepareAtomicCommit(ExynosDisplayDrmInterface::DrmModeAtomicReq& drmReq)
            EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

    /**
     * postAtomicCommit
     *
     * After the atomic commit is done, update the channel status as below.
     * Channel_Status:
     *     CONFIG_BLOB_ADDED  -> CONFIG_COMMITTED
     *     DISABLE_BLOB_ADDED -> DISABLED
     */
    void postAtomicCommit() EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

    virtual void postAtomicCommitCleanup()
            EXCLUDES(mHistogramMutex, mInitDrmDoneMutex, mBlobIdDataMutex) {}

    inline ndk::ScopedAStatus errorToStatus(const HistogramErrorCode histogramErrorCode) const {
        return ndk::ScopedAStatus::
                fromServiceSpecificErrorWithMessage(static_cast<int>(histogramErrorCode),
                                                    aidl::com::google::hardware::pixel::display::
                                                            toString(histogramErrorCode)
                                                                    .c_str());
    }

    /**
     * dump
     *
     * Dump histogram information.
     *
     * @result histogram dump information would be appended to this string
     */
    void dump(String8& result) const EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

protected:
    mutable std::shared_mutex mHistogramCapabilityMutex;
    HistogramCapability mHistogramCapability;

    ExynosDisplay* const mDisplay;
    DrmDevice* mDrmDevice = nullptr;

    mutable std::mutex mHistogramMutex;
    std::unordered_map<AIBinder*, TokenInfo> mTokenInfoMap GUARDED_BY(mHistogramMutex);
    std::list<uint8_t> mFreeChannels GUARDED_BY(mHistogramMutex); // free channel list
    std::set<uint8_t> mUsedChannels GUARDED_BY(mHistogramMutex);  // all - free - reserved
    std::vector<ChannelInfo> mChannels GUARDED_BY(mHistogramMutex);
    std::list<std::weak_ptr<ConfigInfo>> mInactiveConfigItList GUARDED_BY(mHistogramMutex);

    mutable std::mutex mBlobIdDataMutex;
    std::unordered_map<uint32_t, const std::shared_ptr<BlobIdData>> mBlobIdDataMap
            GUARDED_BY(mBlobIdDataMutex);

    mutable std::mutex mInitDrmDoneMutex;
    bool mInitDrmDone GUARDED_BY(mInitDrmDoneMutex) = false;
    mutable std::condition_variable mInitDrmDone_cv GUARDED_BY(mInitDrmDoneMutex);

    /* Death recipient for the binderdied callback, would be deleted in the destructor */
    AIBinder_DeathRecipient* mDeathRecipient = nullptr;

    /**
     * initChannels
     *
     * Allocate channelCount channels and initialize the channel status for every channel.
     *
     * @channelCount number of channels in the system including the reserved channels.
     * @reservedChannels a list of channel id that are reserved by the driver.
     */
    void initChannels(const uint8_t channelCount, const std::vector<uint8_t>& reservedChannels)
            EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

    /**
     * initHistogramCapability
     *
     * Initialize the histogramCapability which would be queried by the client (see
     * getHistogramCapability).
     *
     * @supportMultiChannel true if the kernel support multi channel property, false otherwise.
     */
    void initHistogramCapability(const bool supportMultiChannel)
            EXCLUDES(mHistogramMutex, mBlobIdDataMutex);

    /**
     * initPlatformHistogramCapability
     *
     * Initialize platform specific histogram capability.
     */
    virtual void initPlatformHistogramCapability() {}

    /**
     * waitInitDrmDone
     *
     * Wait until the initDrm is finished, or when the timeout expires.
     *
     * @return true if initDrm is finished, or false when the timeout expires.
     */
    bool waitInitDrmDone() const EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

    /**
     * replaceConfigInfo
     *
     * If histogramConfig is not nullptr, the configInfo pointer will point to the generated
     * ConfigInfo of the histogramConfig. Otherwise, histogramConfig is reset to the nullptr.
     * For the original ConfigInfo, every created blob will be released.
     *
     * @configInfo is the reference to the shared_ptr of ConfigInfo that will be updated depends on
     * histogramConfig.
     * @histogramConfig is the new requested config or nullptr to clear the configInfo ptr.
     */
    void replaceConfigInfo(std::shared_ptr<ConfigInfo>& configInfo,
                           const HistogramConfig* histogramConfig) REQUIRES(mHistogramMutex)
            EXCLUDES(mInitDrmDoneMutex, mBlobIdDataMutex);

    /**
     * searchTokenInfo
     *
     * Search the corresponding TokenInfo of the token object.
     *
     * @token is the key to be searched.
     * @tokenInfo is the result pointer to the corresponding TokenInfo.
     */
    HistogramErrorCode searchTokenInfo(const ndk::SpAIBinder& token, TokenInfo*& tokenInfo)
            REQUIRES(mHistogramMutex) EXCLUDES(mInitDrmDoneMutex, mBlobIdDataMutex);

    /**
     * swapInConfigInfo
     *
     * Move the configInfo from the mInactiveConfigItList to the first free channel. Caller should
     * ensure mFreeChannels is not empty.
     *
     * @configInfo is the config to be moved from inactive list to the histogram channel.
     * @return is the iterator of the next object in the mInactiveConfigItList after deletion.
     */
    std::list<std::weak_ptr<ConfigInfo>>::iterator swapInConfigInfo(
            std::shared_ptr<ConfigInfo>& configInfo) REQUIRES(mHistogramMutex)
            EXCLUDES(mInitDrmDoneMutex, mBlobIdDataMutex);

    /**
     * swapOutConfigInfo
     *
     * Swap out the configInfo from the specified histogram channel to mInactiveConfigItList.
     *
     * @channelId histogram channel to be swapped out
     */
    void swapOutConfigInfo(uint8_t channelId) REQUIRES(mHistogramMutex)
            EXCLUDES(mInitDrmDoneMutex, mBlobIdDataMutex);

    /**
     * addConfigToInactiveList
     *
     * Add the configInfo (status is NOT_READY) into mInactiveConfigItList.
     *
     * @configInfo operated configino
     */
    void addConfigToInactiveList(const std::shared_ptr<ConfigInfo>& configInfo,
                                 bool addToFront = false) REQUIRES(mHistogramMutex)
            EXCLUDES(mInitDrmDoneMutex, mBlobIdDataMutex);

    /**
     * scheduler
     *
     * Move every configInfo from the mInactiveConfigItList to the idle histogram channel until no
     * idle channel exists.
     *
     * @return true if there is any configInfo moved to histogram channel, false otherwise.
     */
    bool scheduler() REQUIRES(mHistogramMutex) EXCLUDES(mInitDrmDoneMutex, mBlobIdDataMutex);

    /**
     * searchOrCreateBlobIdData
     *
     * Search the corresponding blobIdData of blobId from mBlobIdDataMap.
     *
     * @blobId is the blob id to be searched in mBlobIdDataMap.
     * @create is true if the caller would like to create blobIdData when doesn't exist.
     * @blobIdData stores the pointer to the blobIdData if any, else point to nullptr.
     */
    void searchOrCreateBlobIdData(uint32_t blobId, bool create,
                                  std::shared_ptr<BlobIdData>& blobIdData)
            EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

    /**
     * getChanIdBlobId
     *
     * Convert the token into the channelId and the blobId. ChannelId means the current applied
     * channel (-1 if config is inactive) of the config registered by token. BlobId is the first
     * blob id in the mBlobsList. Caller should check the histogram before using the channelId and
     * blobId.
     *
     * @token is the token object registered by registerHistogram.
     * @histogramErrorCode stores any error during query.
     * @channelId is the channel id.
     * @blobId is the blob id.
     */
    void getChanIdBlobId(const ndk::SpAIBinder& token, HistogramErrorCode* histogramErrorCode,
                         int& channelId, uint32_t& blobId)
            EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

    /**
     * getHistogramData
     *
     * Get the histogram data by sending ioctl request which will allocate the drm event for
     * histogram, and wait on the condition variable until the drm event is handled or timeout. Copy
     * the histogram data from blobIdData->data to histogramBuffer.
     *
     * @token is the handle registered by the registerHistogram.
     * @histogramBuffer AIDL created buffer which will be sent back to the client.
     * @histogramErrorCode::NONE when success, or else otherwise.
     */
    void getHistogramData(const ndk::SpAIBinder& token, std::vector<char16_t>* histogramBuffer,
                          HistogramErrorCode* histogramErrorCode)
            EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

    /**
     * requestBlobIdData
     *
     * Request the drm event of the blobId via sending the ioctl which increases the ref_cnt of the
     * blobId event request. Set the query status of blobIdData to COLLECTING.
     *
     * @moduleDisplayInterface display drm interface pointer
     * @histogramErrorCode::NONE when success, or else otherwise.
     * @channelId is the channel id of the request
     * @blobId is the blob id of the request
     * @blobIdData is the histogram data query related struct of the blobId
     */
    void requestBlobIdData(ExynosDisplayDrmInterface* const moduleDisplayInterface,
                           HistogramErrorCode* histogramErrorCode, const int channelId,
                           const uint32_t blobId, const std::shared_ptr<BlobIdData>& blobIdData)
            REQUIRES(blobIdData->mDataCollectingMutex)
                    EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

    /**
     * receiveBlobIdData
     *
     * Wait for the drm event of the blobId, and copy the data into histogramBuffer if no error.
     * Note: It may take for a while, this function should be called without any mutex held except
     * the mDataCollectingMutex.
     *
     * @moduleDisplayInterface display drm interface pointer
     * @histogramBuffer AIDL created buffer which will be sent back to the client.
     * @histogramErrorCode::NONE when success, or else otherwise.
     * @channelId is the channel id of the request
     * @blobId is the blob id of the request
     * @blobIdData is the histogram data query related struct of the blobId
     * @lock is the unique lock of the data query request.
     */
    std::cv_status receiveBlobIdData(ExynosDisplayDrmInterface* const moduleDisplayInterface,
                                     std::vector<char16_t>* histogramBuffer,
                                     HistogramErrorCode* histogramErrorCode, const int channelId,
                                     const uint32_t blobId,
                                     const std::shared_ptr<BlobIdData>& blobIdData,
                                     std::unique_lock<std::mutex>& lock)
            REQUIRES(blobIdData->mDataCollectingMutex)
                    EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

    /**
     * checkQueryResult
     *
     * Check the query result of the receiveBlobIdData. If there is any error, store to the
     * histogramErrorCode and clear the histogramBuffer.
     * Note: It may take for a while, no mutex should be held from the caller.
     *
     * @histogramBuffer AIDL created buffer which will be sent back to the client.
     * @histogramErrorCode::NONE when success, or else otherwise.
     * @channelId is the channel id of the request
     * @blobId is the blob id of the request
     * @cv_status represents if the timeout occurs in receiveBlobIdData
     */
    void checkQueryResult(std::vector<char16_t>* histogramBuffer,
                          HistogramErrorCode* histogramErrorCode, const int channelId,
                          const uint32_t blobId, const std::cv_status cv_status) const
            EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

    /**
     * _handleDrmEvent
     *
     * Internal function to handle the drm event, notify all the threads that wait on the specific
     * drm event with blob id.
     *
     * @event histogram event get from kernel
     * @blobId blob id of the histogram event
     * @buffer buffer that contains histogram data
     */
    void _handleDrmEvent(void* event, uint32_t blobId, char16_t* buffer)
            EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

    /**
     * parseDrmEvent
     *
     * Parse the histogram drm event. This function should get the histogram channel id
     * and the histogram buffer address from the event struct.
     *
     * @event histogram drm event struct.
     * @channelId stores the extracted channel id from the event.
     * @buffer stores the extracted buffer address from the event.
     * @return NO_ERROR on success, else otherwise.
     */
    int parseDrmEvent(const void* const event, uint32_t& channelId, char16_t*& buffer) const
            EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

    /**
     * parseContextDrmEvent
     *
     * Parse the context histogram drm event. This function should get the histogram blob id
     * and the histogram buffer address from the event struct.
     *
     * @event histogram drm event struct.
     * @blobId stores the extracted blob id from the event.
     * @buffer stores the extracted buffer address from the event.
     * @return NO_ERROR on success, else otherwise.
     */
    int parseContextDrmEvent(const void* const event, uint32_t& blobId, char16_t*& buffer) const
            EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

    /**
     * cleanupChannelInfo
     *
     * Cleanup the channel info and set status to DISABLE_PENDING which means need to wait
     * for the atomic commit to release the kernel and hardware channel resources.
     *
     * @channelId the channel id to be cleanup.
     * @return next iterator of mUsedChannels after deletion.
     */
    std::set<uint8_t>::iterator cleanupChannelInfo(const uint8_t channelId)
            REQUIRES(mHistogramMutex) EXCLUDES(mInitDrmDoneMutex, mBlobIdDataMutex);

    /**
     * setChannelConfigBlob
     *
     * Check and detect if the histogram channel config blob needs change due to RRS. Send the
     * config blob commit by setHistogramChannelConfigBlob.
     *
     * Case RRS detected:
     *        CONFIG_COMMITTED / CONFIG_PENDING -> CONFIG_BLOB_ADDED
     * Case RRS not detected:
     *        CONFIG_COMMITTED -> CONFIG_COMMITTED
     *        CONFIG_PENDING -> CONFIG_BLOB_ADDED
     *
     * @drmReq drm atomic request object
     * @channelId histogram channel id
     * @moduleDisplayInterface display drm interface pointer
     * @displayActiveH current display active horizontal size (in pixel)
     * @displayActiveV current display active vertical size (in pixel)
     * @configInfo is the reference to the shared_ptr of ConfigInfo that will be updated depends on
     * histogramConfig.
     */
    void setChannelConfigBlob(ExynosDisplayDrmInterface::DrmModeAtomicReq& drmReq,
                              const uint8_t channelId,
                              ExynosDisplayDrmInterface* const moduleDisplayInterface,
                              const int displayActiveH, const int displayActiveV,
                              const std::shared_ptr<ConfigInfo>& configInfo)
            REQUIRES(mHistogramMutex) EXCLUDES(mInitDrmDoneMutex, mBlobIdDataMutex);

    /**
     * clearChannelConfigBlob
     *
     * Call clearHistogramChannelConfigBlob to disable the histogram channel.
     *     Case #1: success, channel status: DISABLE_PENDING -> DISABLE_BLOB_ADDED.
     *     Case #2: failed, channel status: DISABLE_PENDING -> DISABLE_ERROR.
     *
     * @drmReq drm atomic request object
     * @channelId histogram channel id
     * @moduleDisplayInterface display drm interface pointer
     */
    void clearChannelConfigBlob(ExynosDisplayDrmInterface::DrmModeAtomicReq& drmReq,
                                const uint8_t channelId,
                                ExynosDisplayDrmInterface* const moduleDisplayInterface)
            REQUIRES(mHistogramMutex) EXCLUDES(mInitDrmDoneMutex, mBlobIdDataMutex);

    /**
     * getMatchBlobId
     *
     * Traverse the blobsList to find any BlobInfo matched the active size
     * (displayActiveHxdisplayActiveV). Once found, move the BlobInfo to the front of the list which
     * means the active blob.
     *
     * @blobsList contains the BlobInfo list to be searched from.
     * @displayActiveH current display active horizontal size (in pixel)
     * @displayActiveV current display active vertical size (in pixel)
     * @return the blob id if found, 0 otherwise.
     */
    uint32_t getMatchBlobId(std::list<BlobInfo>& blobsList, const int displayActiveH,
                            const int displayActiveV, bool& isPositionChanged) const
            REQUIRES(mHistogramMutex) EXCLUDES(mInitDrmDoneMutex, mBlobIdDataMutex);

    /**
     * getActiveBlobId
     *
     * Get the current active blob id from the blobsList. The active blob is the first blob in the
     * list.
     *
     * @return the first blod id from the blobsList if any, else return 0.
     */
    uint32_t getActiveBlobId(const std::list<BlobInfo>& blobsList) const REQUIRES(mHistogramMutex)
            EXCLUDES(mInitDrmDoneMutex, mBlobIdDataMutex);

    /**
     * createDrmConfig
     *
     * Allocate and initialize the histogram config for the drm driver. PropertyBlob class will use
     * this config to createPropertyBlob.
     *
     * @histogramConfig HistogramConfig that is requested from the client
     * @displayActiveH current display active horizontal size (in pixel)
     * @displayActiveV current display active vertical size (in pixel)
     * @drmConfig shared pointer to the allocated histogram config struct.
     * @drmConfigLength size of the histogram config.
     * @return NO_ERROR on success, else otherwise
     */
    int createDrmConfig(const HistogramConfig& histogramConfig, const int displayActiveH,
                        const int displayActiveV, std::shared_ptr<void>& drmConfig,
                        size_t& drmConfigLength) const
            EXCLUDES(mInitDrmDoneMutex, mBlobIdDataMutex);

    /**
     * createDrmConfigBlob
     *
     * Create the drmConfigBlob for the requeste histogramConfig.
     *
     * @histogramConfig HistogramConfig that is requested from the client
     * @displayActiveH current display active horizontal size (in pixel)
     * @displayActiveV current display active vertical size (in pixel)
     * @drmConfigBlob shared pointer to the created drmConfigBlob.
     */
    int createDrmConfigBlob(const HistogramConfig& histogramConfig, const int displayActiveH,
                            const int displayActiveV,
                            std::shared_ptr<PropertyBlob>& drmConfigBlob) const
            EXCLUDES(mInitDrmDoneMutex, mBlobIdDataMutex);

    void resetConfigInfoStatus(std::shared_ptr<ConfigInfo>& configInfo) REQUIRES(mHistogramMutex)
            EXCLUDES(mInitDrmDoneMutex, mBlobIdDataMutex);

    std::pair<int, int> snapDisplayActiveSize() const
            EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

    /**
     * convertRoi
     *
     * Linear transform the requested roi (based on panel full resolution) into the working roi
     * (active resolution).
     *
     * @moduleDisplayInterface the displayInterface which contains the full resolution info
     * @requestedRoi requested roi
     * @workingRoi converted roi from the requested roi
     * @return NO_ERROR on success, else otherwise
     */
    int convertRoi(const HistogramRoiRect& requestedRoi, HistogramRoiRect& workingRoi,
                   const int displayActiveH, const int displayActiveV, const char* roiType) const
            EXCLUDES(mInitDrmDoneMutex, mBlobIdDataMutex);

    void dumpHistogramCapability(String8& result) const
            EXCLUDES(mInitDrmDoneMutex, mHistogramMutex, mBlobIdDataMutex);

    virtual void dumpInternalConfigs(String8& result) const REQUIRES(mHistogramMutex)
            EXCLUDES(mInitDrmDoneMutex, mBlobIdDataMutex) {}

    void dumpChannel(TableBuilder& tb, const uint8_t channelId) const REQUIRES(mHistogramMutex)
            EXCLUDES(mInitDrmDoneMutex, mBlobIdDataMutex);

    ndk::ScopedAStatus validateHistogramRequest(const ndk::SpAIBinder& token,
                                                const HistogramConfig& histogramConfig,
                                                HistogramErrorCode* histogramErrorCode) const;
    HistogramErrorCode validateHistogramConfig(const HistogramConfig& histogramConfig) const;
    HistogramErrorCode validateHistogramRoi(const HistogramRoiRect& roi, const char* roiType) const;
    HistogramErrorCode validateHistogramWeights(const HistogramWeights& weights) const;
    HistogramErrorCode validateHistogramSamplePos(const HistogramSamplePos& samplePos) const;
    HistogramErrorCode validateHistogramBlockingRoi(
            const std::optional<HistogramRoiRect>& blockingRoi) const;

    int calculateThreshold(const HistogramRoiRect& roi, const int displayActiveH,
                           const int displayActiveV) const;

    static std::string toString(const ChannelStatus_t& status);
    static std::string toString(const HistogramRoiRect& roi);
    static std::string toString(const HistogramWeights& weights);
    static std::string toString(const HistogramConfig& config);
};

// PropertyBlob is the RAII class to manage the histogram PropertyBlob creation and deletion.
class HistogramDevice::PropertyBlob {
public:
    /**
     * PropertyBlob
     *
     * Construct the PropertyBlob to mange the histogram PropertyBlob.
     *
     * @drmDevice the object to call the CreatePropertyBlob.
     * @blobData pointer to the buffer that contains requested property blob data to be created.
     * @blobLength size of the buffer pointed by blobData.
     */
    PropertyBlob(DrmDevice* const drmDevice, const void* const blobData, const size_t blobLength);

    /**
     * ~PropertyBlob
     *
     * Destruct the PropertyBlob and release the allocated blob in constructor.
     */
    ~PropertyBlob();

    /**
     * getId
     *
     * @return blobId of this PropertyBlob
     */
    uint32_t getId() const;

    /**
     * getError
     *
     * @return any error in the constructor if any, otherwise return 0.
     */
    int getError() const;

private:
    DrmDevice* const mDrmDevice;
    uint32_t mBlobId = 0;
    int mError = NO_ERROR;
};
