#!/usr/bin/env python3
# A6L lc2 (29 Sep 2026): libcamera Android HAL fixes for a single-output software-ISP pipeline
# (generates patches/0005-android-a6l-single-stream-dmabuf-allocator.patch).
#  1. camera_device.cpp: the simple pipeline + SoftISP produces ONE processed stream (+ raw). A typical Android session
#     (preview + JPEG, or preview + YUV analysis + JPEG) asks for 2-3 processed streams, so libcamera's configure fails.
#     All processed streams are merged onto one libcamera NV12 stream: the JPEG size when a JPEG stream exists (the JPEG
#     encoder needs input size == output size), else the largest requested size. The other streams become Mapped streams
#     produced by the YUV post-processor (libyuv NV12Scale). Mapped streams may now also hang off an Internal (JPEG-only)
#     source stream. LIBCAMERA_HAL_MULTI_STREAM=1 restores the upstream behaviour.
#  2. yuv/post_processor_yuv: centre-crop the source to the destination aspect ratio before scaling (a 16:9 preview mapped
#     on a 4:3 capture stream is not stretched).
#  3. mm/generic_frame_buffer_allocator.cpp: internal HAL buffers (JPEG source, mapped streams without their source in the
#     request) come from dma-buf heaps (DmaBufAllocator: CMA, system, udmabuf) instead of the legacy gralloc0 module that
#     the ROM does not ship (the NDK build has no libhardware: hw_get_module was the stub -> ASSERT at first allocation).
#  4. dma_buf_allocator.cpp: open the heap node O_RDONLY when O_RDWR is refused (Android ueventd: /dev/dma_heap/system
#     0444 system system; the allocation ioctl needs no write access).
# Usage: a6l_hal_edit.py <libcamera-src>
import sys, os
src = sys.argv[1]

def rep(rel, old, new, cnt=1):
    p = os.path.join(src, rel); s = open(p).read()
    n = s.count(old)
    if n != cnt: sys.exit('%s: anchor found %dx (want %d): %r' % (rel, n, cnt, old[:90]))
    open(p, 'w').write(s.replace(old, new)); print('edited', rel, '|', old.strip().splitlines()[0][:60])

# ---------------------------------------------------------------- 1. single processed stream
CD = 'src/android/camera_device.cpp'
rep(CD, '''	sortCamera3StreamConfigs(streamConfigs, jpegStream);
	for (const auto &streamConfig : streamConfigs) {''', '''	/*
	 * A6L: pipelines with a single processed output (simple pipeline +
	 * software ISP) cannot produce several processed streams. Merge all
	 * the processed streams onto one libcamera stream: the JPEG size if a
	 * JPEG stream is requested (the encoder needs identical input and
	 * output sizes), the largest requested size otherwise. The other
	 * streams become Mapped streams scaled by the YUV post-processor.
	 */
	if (!utils::secure_getenv("LIBCAMERA_HAL_MULTI_STREAM")) {
		std::vector<size_t> processed;
		for (size_t i = 0; i < streamConfigs.size(); ++i) {
			const PixelFormatInfo &info =
				PixelFormatInfo::info(streamConfigs[i].config.pixelFormat);
			if (info.colourEncoding != PixelFormatInfo::ColourEncodingRAW)
				processed.push_back(i);
		}

		if (processed.size() > 1) {
			size_t source = processed[0];
			bool hasJpeg = false;
			for (size_t i : processed) {
				for (const auto &s : streamConfigs[i].streams) {
					if (s.stream == jpegStream) {
						source = i;
						hasJpeg = true;
					}
				}
			}
			if (!hasJpeg) {
				for (size_t i : processed) {
					if (streamConfigs[i].config.size.width * streamConfigs[i].config.size.height >
					    streamConfigs[source].config.size.width * streamConfigs[source].config.size.height)
						source = i;
				}
			}

			Camera3StreamConfig &src = streamConfigs[source];
			camera3_stream_t *srcStream = src.streams[0].stream;
			for (size_t i : processed) {
				if (i == source)
					continue;
				for (const auto &s : streamConfigs[i].streams) {
					if (s.stream->width > src.config.size.width ||
					    s.stream->height > src.config.size.height) {
						LOG(HAL, Error)
							<< "Single-stream mode: " << s.stream->width
							<< "x" << s.stream->height
							<< " larger than the source stream "
							<< src.config.size;
						return -EINVAL;
					}
					srcStream->usage |= GRALLOC_USAGE_SW_READ_OFTEN;
					s.stream->usage |= GRALLOC_USAGE_SW_WRITE_OFTEN;
					src.streams.push_back({ s.stream, CameraStream::Type::Mapped });
				}
			}

			LOG(HAL, Info) << "Single-stream mode: " << src.streams.size()
				       << " Android streams on one " << src.config.toString()
				       << " libcamera stream";

			std::vector<Camera3StreamConfig> merged;
			for (size_t i = 0; i < streamConfigs.size(); ++i) {
				if (i == source ||
				    std::find(processed.begin(), processed.end(), i) == processed.end())
					merged.push_back(std::move(streamConfigs[i]));
			}
			streamConfigs = std::move(merged);
		}
	}

	sortCamera3StreamConfigs(streamConfigs, jpegStream);
	for (const auto &streamConfig : streamConfigs) {''')
rep(CD, '''#include "system/graphics.h"
''', '''#include "system/graphics.h"

#include "libcamera/internal/formats.h"
''')
rep(CD, '''			if (stream.type == CameraStream::Type::Direct)
				sourceStream = &streams_.back();''', '''			/* A6L: an Internal (JPEG) stream can be the source of Mapped streams too */
			if (stream.type != CameraStream::Type::Mapped)
				sourceStream = &streams_.back();''')

# ---------------------------------------------------------------- 2. YUV post-processor centre crop
YH = 'src/android/yuv/post_processor_yuv.h'
YC = 'src/android/yuv/post_processor_yuv.cpp'
rep(YH, '''	libcamera::Size sourceSize_;
	libcamera::Size destinationSize_;''', '''	libcamera::Size sourceSize_;
	libcamera::Size destinationSize_;
	libcamera::Rectangle crop_; /* A6L: source area with the destination aspect ratio */''')
rep(YC, '''	int ret = libyuv::NV12Scale(sourceMapped.planes()[0].data(),
				    sourceStride_[0],
				    sourceMapped.planes()[1].data(),
				    sourceStride_[1],
				    sourceSize_.width, sourceSize_.height,''', '''	/* A6L: centre crop (even offsets keep the CbCr pairs aligned) */
	const uint8_t *srcY = sourceMapped.planes()[0].data() +
			      crop_.y * sourceStride_[0] + crop_.x;
	const uint8_t *srcUV = sourceMapped.planes()[1].data() +
			       crop_.y / 2 * sourceStride_[1] + crop_.x;
	int ret = libyuv::NV12Scale(srcY,
				    sourceStride_[0],
				    srcUV,
				    sourceStride_[1],
				    crop_.width, crop_.height,''')
rep(YC, '''	sourceSize_ = inCfg.size;
	destinationSize_ = outCfg.size;
''', '''	sourceSize_ = inCfg.size;
	destinationSize_ = outCfg.size;

	/* A6L: largest centred source rectangle with the destination aspect ratio */
	crop_ = libcamera::Rectangle(sourceSize_);
	const uint64_t sw = sourceSize_.width, sh = sourceSize_.height;
	const uint64_t dw = destinationSize_.width, dh = destinationSize_.height;
	if (dw && dh) {
		if (sw * dh > dw * sh) {
			crop_.width = static_cast<unsigned int>(sh * dw / dh) & ~1u;
			crop_.x = static_cast<int>((sw - crop_.width) / 2) & ~1;
		} else if (sw * dh < dw * sh) {
			crop_.height = static_cast<unsigned int>(sw * dh / dw) & ~1u;
			crop_.y = static_cast<int>((sh - crop_.height) / 2) & ~1;
		}
	}
''')

# ---------------------------------------------------------------- 3. dma-buf internal allocator
GA = 'src/android/mm/generic_frame_buffer_allocator.cpp'
open(os.path.join(src, GA), 'w').write('''/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2021, Google Inc.
 *
 * Allocate FrameBuffer for the HAL internal streams.
 *
 * A6L: the buffers come from the dma-buf heaps (DmaBufAllocator: CMA,
 * system, udmabuf) instead of the legacy gralloc0 module, which neither
 * the NDK build (no libhardware) nor a minigbm/gralloc4 platform provides.
 * The planes are laid out contiguously in one dma-buf with the stride
 * aligned to 8 bytes, the layout the software ISP writes.
 */

#include <memory>
#include <vector>

#include <libcamera/base/log.h>
#include <libcamera/base/shared_fd.h>
#include <libcamera/base/unique_fd.h>
#include <libcamera/base/utils.h>

#include "libcamera/internal/dma_buf_allocator.h"
#include "libcamera/internal/formats.h"

#include <hardware/camera3.h>

#include "../camera_capabilities.h"
#include "../camera_device.h"
#include "../frame_buffer_allocator.h"
#include "../hal_framebuffer.h"

using namespace libcamera;

LOG_DECLARE_CATEGORY(HAL)

class PlatformFrameBufferAllocator::Private : public Extensible::Private
{
	LIBCAMERA_DECLARE_PUBLIC(PlatformFrameBufferAllocator)

public:
	Private(CameraDevice *const cameraDevice)
		: cameraDevice_(cameraDevice),
		  allocator_(DmaBufAllocator::DmaBufAllocatorFlag::CmaHeap |
			     DmaBufAllocator::DmaBufAllocatorFlag::SystemHeap |
			     DmaBufAllocator::DmaBufAllocatorFlag::UDmaBuf)
	{
		if (!allocator_.isValid())
			LOG(HAL, Error) << "No dma-buf heap or udmabuf for internal buffers";
	}

	~Private() override = default;

	std::unique_ptr<HALFrameBuffer>
	allocate(int halPixelFormat, const libcamera::Size &size, uint32_t usage);

private:
	const CameraDevice *const cameraDevice_;
	DmaBufAllocator allocator_;
};

std::unique_ptr<HALFrameBuffer>
PlatformFrameBufferAllocator::Private::allocate(int halPixelFormat,
						const libcamera::Size &size,
						[[maybe_unused]] uint32_t usage)
{
	const PixelFormat pixelFormat =
		cameraDevice_->capabilities()->toPixelFormat(halPixelFormat);
	const PixelFormatInfo &info = PixelFormatInfo::info(pixelFormat);
	if (!info.isValid()) {
		LOG(HAL, Error) << "Internal buffer: unsupported HAL format "
				<< halPixelFormat;
		return nullptr;
	}

	const unsigned int stride = info.stride(size.width, 0, 8);
	std::vector<unsigned int> planeSizes;
	size_t total = 0;
	for (unsigned int i = 0; i < info.numPlanes(); i++) {
		planeSizes.push_back(info.planeSize(size.height, i, stride));
		total += planeSizes.back();
	}

	UniqueFD fd = allocator_.alloc("libcamera-hal", total);
	if (!fd.isValid()) {
		LOG(HAL, Error) << "Internal buffer allocation failed ("
				<< total << " bytes)";
		return nullptr;
	}

	SharedFD sharedFd(std::move(fd));
	std::vector<FrameBuffer::Plane> planes(info.numPlanes());
	size_t offset = 0;
	for (auto [i, plane] : utils::enumerate(planes)) {
		plane.fd = sharedFd;
		plane.offset = offset;
		plane.length = planeSizes[i];
		offset += planeSizes[i];
	}

	return std::make_unique<HALFrameBuffer>(planes, nullptr);
}

PUBLIC_FRAME_BUFFER_ALLOCATOR_IMPLEMENTATION
''')
print('wrote', GA)

# ---------------------------------------------------------------- 4. read-only heap node
DA = 'src/libcamera/dma_buf_allocator.cpp'
rep(DA, '''		int ret = ::open(info.deviceNodeName, O_RDWR | O_CLOEXEC, 0);''',
        '''		int ret = ::open(info.deviceNodeName, O_RDWR | O_CLOEXEC, 0);
		/* A6L: Android ueventd makes /dev/dma_heap/system 0444, the ioctl needs no write access */
		if (ret < 0 && errno == EACCES)
			ret = ::open(info.deviceNodeName, O_RDONLY | O_CLOEXEC, 0);''')
print('A6L_HAL_EDIT_OK')
