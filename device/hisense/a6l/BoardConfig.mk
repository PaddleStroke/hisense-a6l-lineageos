# SPDX-License-Identifier: Apache-2.0
# Compile probe: upstream arm64 GSI board, not yet a native A6L board.
include build/make/target/board/generic_arm64/BoardConfig.mk

# Actual A6L system partition capacity from the verified GPT.
BOARD_SYSTEMIMAGE_PARTITION_RESERVED_SIZE :=
BOARD_SYSTEMIMAGE_PARTITION_SIZE := 6442450944

# No boot/recovery/kernel output from this initial system-only probe.
TARGET_NO_KERNEL := true
TARGET_NO_RECOVERY := true
