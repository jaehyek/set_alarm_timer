LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := set_alarm_timer
LOCAL_SRC_FILES := set_alarm_timer.cpp
LOCAL_CFLAGS += -DPOSIX -D_POSIX_C_SOURCE=200809L -DTEST_NARROW_WRITES
include $(BUILD_EXECUTABLE)