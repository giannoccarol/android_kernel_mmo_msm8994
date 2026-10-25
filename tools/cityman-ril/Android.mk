# SPDX-License-Identifier: GPL-2.0-only
# Include explicitly from a Cityman device tree; the kernel build does not
# install or enable these Android userspace services.
LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := libcityman_ds_target
LOCAL_SRC_FILES := cityman-ds-target.c
LOCAL_SHARED_LIBRARIES := libdl
LOCAL_VENDOR_MODULE := true
LOCAL_MULTILIB := 64
include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := cityman-uim-bootstrap
LOCAL_SRC_FILES := cityman-uim-bootstrap.c
LOCAL_SHARED_LIBRARIES := libdl liblog
LOCAL_INIT_RC := cityman-uim-bootstrap.rc
LOCAL_VENDOR_MODULE := true
LOCAL_MULTILIB := 64
include $(BUILD_EXECUTABLE)
