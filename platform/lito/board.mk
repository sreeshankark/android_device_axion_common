PRODUCT_SOONG_NAMESPACES += device/axion/common/platform/lito

TARGET_NEEDS_VULKAN_MEDIA_FIX := true
TARGET_SUPPORTS_KERNEL_MANAGER := true

TARGET_PRODUCT_PROP += device/axion/common/platform/lito/props/ax_lito.prop

PRODUCT_COPY_FILES += \
    device/axion/common/init/init.axion.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.axion-common.rc \
    device/axion/common/init/ax_init_lito.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/ax_init_lito.rc \
    device/axion/common/prebuilts/ax_kernel_manager_lito.xml:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/ax_kernel_manager.xml \
    device/axion/common/prebuilts/ax_perf_thermal_lito.xml:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/ax_perf_thermal.xml
