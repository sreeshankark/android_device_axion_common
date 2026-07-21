PRODUCT_SOONG_NAMESPACES += device/axion/common/platform/lito

include device/axion/common/config/board/common.mk

TARGET_NEEDS_VULKAN_MEDIA_FIX := true
TARGET_SHIPS_AXION_KERNEL_MODULES := false
