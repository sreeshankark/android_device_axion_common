SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS += device/axion/common/sepolicy/private
SYSTEM_EXT_PUBLIC_SEPOLICY_DIRS += device/axion/common/sepolicy/public
ifeq ($(BOARD_USES_QCOM_HARDWARE),true)
BOARD_VENDOR_SEPOLICY_DIRS += device/axion/common/sepolicy/vendor/qcom
endif
