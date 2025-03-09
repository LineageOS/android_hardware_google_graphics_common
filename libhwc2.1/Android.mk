# Copyright (C) 2012 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# TODO(b/186905324): Switch soc_ver with TARGET_BOARD_PLATFORM
soc_ver := $(TARGET_BOARD_PLATFORM)

LOCAL_PATH:= $(call my-dir)
# HAL module implemenation, not prelinked and stored in
# hw/<COPYPIX_HARDWARE_MODULE_ID>.<ro.product.board>.so

ifeq ($(BOARD_USES_HWC_SERVICES),true)

include $(CLEAR_VARS)

LOCAL_HEADER_LIBRARIES := libhardware_legacy_headers libbinder_headers google_hal_headers
LOCAL_HEADER_LIBRARIES += libgralloc_headers
LOCAL_SHARED_LIBRARIES := liblog libcutils libutils libbinder libexynosdisplay libacryl \
	android.hardware.graphics.composer@2.4 \
	android.hardware.graphics.allocator@2.0 \
	android.hardware.graphics.mapper@2.0 \
	android.hardware.graphics.composer3-V4-ndk \
	android.hardware.drm-V1-ndk

LOCAL_SHARED_LIBRARIES += com.google.hardware.pixel.display-V13-ndk \
                          android.frameworks.stats-V2-ndk \
                          libpixelatoms_defs \
                          pixelatoms-cpp \
                          libbinder_ndk \
                          libbase

LOCAL_STATIC_LIBRARIES += libVendorVideoApi
LOCAL_PROPRIETARY_MODULE := true

LOCAL_C_INCLUDES += \
	$(TOP)/hardware/google/graphics/common/include \
	$(TOP)/hardware/google/graphics/common/libhwc2.1/libdevice \
	$(TOP)/hardware/google/graphics/common/libhwc2.1/libmaindisplay \
	$(TOP)/hardware/google/graphics/common/libhwc2.1/libexternaldisplay \
	$(TOP)/hardware/google/graphics/common/libhwc2.1/libvirtualdisplay \
	$(TOP)/hardware/google/graphics/common/libhwc2.1/libhwchelper \
	$(TOP)/hardware/google/graphics/common/libhwc2.1/libresource \
	$(TOP)/hardware/google/graphics/$(soc_ver)/libhwc2.1 \
	$(TOP)/hardware/google/graphics/$(soc_ver)/libhwc2.1/libmaindisplay \
	$(TOP)/hardware/google/graphics/$(soc_ver)/libhwc2.1/libexternaldisplay \
	$(TOP)/hardware/google/graphics/$(soc_ver)/libhwc2.1/libvirtualdisplay \
	$(TOP)/hardware/google/graphics/$(soc_ver)/libhwc2.1/libresource \
	$(TOP)/hardware/google/graphics/$(soc_ver)/libhwc2.1/libcolormanager \
	$(TOP)/hardware/google/graphics/$(soc_ver)/libhwc2.1/libdevice \
	$(TOP)/hardware/google/graphics/$(soc_ver)/libhwc2.1/libresource \
	$(TOP)/hardware/google/graphics/common/libhwc2.1/libhwcService \
	$(TOP)/hardware/google/graphics/common/libhwc2.1/libdisplayinterface \
	$(TOP)/hardware/google/graphics/common/libhwc2.1/libdrmresource/include

LOCAL_EXPORT_SHARED_LIBRARY_HEADERS += libdrm
LOCAL_EXPORT_C_INCLUDE_DIRS := $(LOCAL_C_INCLUDES)

LOCAL_CFLAGS := -DHLOG_CODE=0
LOCAL_CFLAGS += -DLOG_TAG=\"hwc-service\"
LOCAL_CFLAGS += -DSOC_VERSION=$(soc_ver)
LOCAL_CFLAGS += -Wthread-safety

ifeq ($(CLANG_COVERAGE),true)
# enable code coverage (these flags are copied from build/soong/cc/coverage.go)
LOCAL_CFLAGS += -fprofile-instr-generate -fcoverage-mapping
LOCAL_CFLAGS += -Wno-frame-larger-than=
LOCAL_WHOLE_STATIC_LIBRARIES += libprofile-clang-extras_ndk
LOCAL_LDFLAGS += -fprofile-instr-generate
LOCAL_LDFLAGS += -Wl,--wrap,open

ifeq ($(CLANG_COVERAGE_CONTINUOUS_MODE),true)
LOCAL_CFLAGS += -mllvm -runtime-counter-relocation
LOCAL_LDFLAGS += -Wl,-mllvm=-runtime-counter-relocation
endif
endif

LOCAL_SRC_FILES := \
	libhwcService/IExynosHWC.cpp \
	libhwcService/ExynosHWCService.cpp

LOCAL_MODULE := libExynosHWCService
LOCAL_LICENSE_KINDS := SPDX-license-identifier-Apache-2.0
LOCAL_LICENSE_CONDITIONS := notice
LOCAL_NOTICE_FILE := $(LOCAL_PATH)/NOTICE
LOCAL_MODULE_TAGS := optional

include $(TOP)/hardware/google/graphics/common/BoardConfigCFlags.mk
include $(BUILD_SHARED_LIBRARY)

endif

################################################################################

include $(CLEAR_VARS)

LOCAL_PRELINK_MODULE := false
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_SHARED_LIBRARIES := liblog libcutils libutils libexynosdisplay libacryl \
	android.hardware.graphics.composer@2.4 \
	android.hardware.graphics.allocator@2.0 \
	android.hardware.graphics.mapper@2.0 \
	libui

LOCAL_SHARED_LIBRARIES += android.hardware.graphics.composer3-V4-ndk \
                          android.hardware.drm-V1-ndk \
                          com.google.hardware.pixel.display-V13-ndk \
                          android.frameworks.stats-V2-ndk \
                          libpixelatoms_defs \
                          pixelatoms-cpp \
                          libbinder_ndk \
                          libbase

LOCAL_PROPRIETARY_MODULE := true
LOCAL_HEADER_LIBRARIES := libhardware_legacy_headers libbinder_headers google_hal_headers
LOCAL_HEADER_LIBRARIES += libgralloc_headers

LOCAL_CFLAGS := -DHLOG_CODE=0
LOCAL_CFLAGS += -DLOG_TAG=\"hwc-2\"
LOCAL_CFLAGS += -DSOC_VERSION=$(soc_ver)
LOCAL_CFLAGS += -Wthread-safety

ifeq ($(CLANG_COVERAGE),true)
# enable code coverage (these flags are copied from build/soong/cc/coverage.go)
LOCAL_CFLAGS += -fprofile-instr-generate -fcoverage-mapping
LOCAL_CFLAGS += -Wno-frame-larger-than=
LOCAL_WHOLE_STATIC_LIBRARIES += libprofile-clang-extras_ndk
LOCAL_LDFLAGS += -fprofile-instr-generate
LOCAL_LDFLAGS += -Wl,--wrap,open

ifeq ($(CLANG_COVERAGE_CONTINUOUS_MODE),true)
LOCAL_CFLAGS += -mllvm -runtime-counter-relocation
LOCAL_LDFLAGS += -Wl,-mllvm=-runtime-counter-relocation
endif
endif

ifeq ($(BOARD_USES_HWC_SERVICES),true)
LOCAL_CFLAGS += -DUSES_HWC_SERVICES
LOCAL_SHARED_LIBRARIES += libExynosHWCService
endif
LOCAL_STATIC_LIBRARIES += libVendorVideoApi

LOCAL_C_INCLUDES += \
	$(TOP)/hardware/google/graphics/common/include \
	$(TOP)/hardware/google/graphics/common/libhwc2.1/libdevice \
	$(TOP)/hardware/google/graphics/common/libhwc2.1/libmaindisplay \
	$(TOP)/hardware/google/graphics/common/libhwc2.1/libexternaldisplay \
	$(TOP)/hardware/google/graphics/common/libhwc2.1/libvirtualdisplay \
	$(TOP)/hardware/google/graphics/common/libhwc2.1/libhwchelper \
	$(TOP)/hardware/google/graphics/common/libhwc2.1/libresource \
	$(TOP)/hardware/google/graphics/$(soc_ver)/libhwc2.1 \
	$(TOP)/hardware/google/graphics/$(soc_ver)/libhwc2.1/libmaindisplay \
	$(TOP)/hardware/google/graphics/$(soc_ver)/libhwc2.1/libexternaldisplay \
	$(TOP)/hardware/google/graphics/$(soc_ver)/libhwc2.1/libvirtualdisplay \
	$(TOP)/hardware/google/graphics/$(soc_ver)/libhwc2.1/libcolormanager \
	$(TOP)/hardware/google/graphics/$(soc_ver)/libhwc2.1/libresource \
	$(TOP)/hardware/google/graphics/$(soc_ver)/libhwc2.1/libdevice \
	$(TOP)/hardware/google/graphics/$(soc_ver)/libhwc2.1/libresource \
	$(TOP)/hardware/google/graphics/$(soc_ver)/include \
	$(TOP)/hardware/google/graphics/common/libhwc2.1/libhwcService \
	$(TOP)/hardware/google/graphics/common/libhwc2.1/libdisplayinterface

LOCAL_SRC_FILES := \
	ExynosHWC.cpp

LOCAL_MODULE := hwcomposer.$(TARGET_BOARD_PLATFORM)
LOCAL_LICENSE_KINDS := SPDX-license-identifier-Apache-2.0
LOCAL_LICENSE_CONDITIONS := notice
LOCAL_NOTICE_FILE := $(LOCAL_PATH)/NOTICE
LOCAL_MODULE_TAGS := optional

include $(TOP)/hardware/google/graphics/common/BoardConfigCFlags.mk
include $(BUILD_SHARED_LIBRARY)
