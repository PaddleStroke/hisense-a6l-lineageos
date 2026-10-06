# SPDX-License-Identifier: Apache-2.0
# A6L charger UI board flag (selinux-release, 29 Sep 2026). Included from rom/BoardConfig-rom.mk.
# TARGET_SCREEN_DENSITY picks the Lineage charger image bucket (vendor/lineage/config/BoardConfigSoong.mk: 400 -> xxhdpi,
# unset -> mdpi = a 160 px battery on the 1080 px LCD) and the recovery image density. It also emits ro.sf.lcd_density=400,
# the value rom/rom.mk already sets (post_process_props accepts duplicates with the same value). Keep them equal.
TARGET_SCREEN_DENSITY ?= 400
