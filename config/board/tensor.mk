HBM_SUPPORTED := true
HBM_NODE := /sys/class/backlight/panel0-backlight/hbm_mode
TORCH_STR_SUPPORTED := true
TARGET_NEEDS_DOZE_FIX := true
TARGET_ENABLES_IMS_OVERRIDES := true
TARGET_TOUCH_BOOST_SUPPORTED := true
TARGET_INCLUDE_AXFX := true
TARGET_PREBUILT_GOOGLE_CAMERA := true

INTERNAL_KERNEL_CMDLINE += irqaffinity=0-1 rcu_nocbs=0-7 cgroup.memory=nokmem,nosocket no-steal-acc can.stats_timer=0 kasan=off
