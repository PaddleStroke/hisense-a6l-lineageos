#!/usr/bin/env python3
# A6L lc2 (29 Sep 2026): SoftISP CPU debayer NV12 output (generates patches/0004-software_isp-debayer_cpu-nv12-output.patch).
# Why: the libcamera Android HAL maps the mandatory Android formats YCbCr_420_888 and IMPLEMENTATION_DEFINED to NV12/NV21
# only (camera_capabilities.cpp camera3FormatsMap) and its JPEG/YUV post-processors read NV12. SoftISP 0.7.2 outputs
# RGB only, so without this every camera fails "Failed to map mandatory Android format". minigbm also resolves
# IMPLEMENTATION_DEFINED + camera usage to NV12.
# How: the debayer line functions of the RGB888 path (memory order B,G,R) write two lines into per-thread scratch lines,
# then each line pair is converted to two Y rows + one interleaved CbCr row (full-range BT.601 / JFIF, the colour space
# Android camera YUV and the HAL's libjpeg encoder use). Y stride = (width + 7) & ~7, UV plane directly after the Y plane
# (single-plane FrameBuffer) or plane 1 (two-plane FrameBuffer from gralloc / the HAL allocator).
# Usage: a6l_softisp_nv12_edit.py <libcamera-src>
import sys, os
src = sys.argv[1]
D = 'src/libcamera/software_isp/'

def rep(rel, old, new, cnt=1):
    p = os.path.join(src, rel); s = open(p).read()
    n = s.count(old)
    if n != cnt: sys.exit('%s: anchor found %dx (want %d): %r' % (rel, n, cnt, old[:90]))
    open(p, 'w').write(s.replace(old, new)); print('edited', rel, '|', old.strip().splitlines()[0][:60])

C = D + 'debayer_cpu.cpp'
H = D + 'debayer_cpu.h'

# --- thread class: scratch RGB lines, UV destination
rep(C, '''	void configure(unsigned int yStart, unsigned int yEnd);
	void process(uint32_t frame, const uint8_t *src, uint8_t *dst);
''', '''	void configure(unsigned int yStart, unsigned int yEnd);
	void process(uint32_t frame, const uint8_t *src, uint8_t *dst, uint8_t *dstUV);
''')
rep(C, '''	void process2(uint32_t frame, const uint8_t *src, uint8_t *dst);
	void process4(uint32_t frame, const uint8_t *src, uint8_t *dst);
''', '''	void process2(uint32_t frame, const uint8_t *src, uint8_t *dst, uint8_t *dstUV);
	void process4(uint32_t frame, const uint8_t *src, uint8_t *dst, uint8_t *dstUV);
	/* A6L: NV12 output = debayer to B,G,R scratch lines, then convert each line pair */
	uint8_t *lineOut(unsigned int i, uint8_t *dst)
	{
		return debayer_->yuvOutput_ ? rgbLines_[i].data() : dst;
	}
	void pairToNV12(uint8_t *y0, uint8_t *&uv);
''')
rep(C, '''	std::vector<uint8_t> lineBuffers_[kMaxLineBuffers];
	bool enableInputMemcpy_;
};
''', '''	std::vector<uint8_t> lineBuffers_[kMaxLineBuffers];
	std::vector<uint8_t> rgbLines_[2];
	bool enableInputMemcpy_;
};
''')

# --- formats / output config / stride
rep(C, '''						   formats::XBGR8888,
						   formats::ABGR8888 };
''', '''						   formats::XBGR8888,
						   formats::ABGR8888,
						   formats::NV12 };
''')
rep(C, '''	if (outputFormat == formats::XRGB8888 || outputFormat == formats::ARGB8888 ||
	    outputFormat == formats::XBGR8888 || outputFormat == formats::ABGR8888) {
		config.bpp = 32;
		return 0;
	}
''', '''	if (outputFormat == formats::XRGB8888 || outputFormat == formats::ARGB8888 ||
	    outputFormat == formats::XBGR8888 || outputFormat == formats::ABGR8888) {
		config.bpp = 32;
		return 0;
	}

	/* A6L: NV12, 8 bpp luma plane (the CbCr plane is handled in strideAndFrameSize()) */
	if (outputFormat == formats::NV12) {
		config.bpp = 8;
		return 0;
	}
''')
rep(C, '''	/* round up to multiple of 8 for 64 bits alignment */
	unsigned int stride = (size.width * config.bpp / 8 + 7) & ~7;

	return std::make_tuple(stride, stride * size.height);
''', '''	/* round up to multiple of 8 for 64 bits alignment */
	unsigned int stride = (size.width * config.bpp / 8 + 7) & ~7;

	/* A6L: NV12 = Y plane + interleaved CbCr plane of half the height, same stride */
	if (outputFormat == formats::NV12)
		return std::make_tuple(stride, stride * size.height * 3 / 2);

	return std::make_tuple(stride, stride * size.height);
''')
rep(C, '''	case formats::RGB888:
		break;
	case formats::XBGR8888:
''', '''	case formats::RGB888:
	case formats::NV12: /* A6L: B,G,R scratch lines converted to NV12 per line pair */
		break;
	case formats::XBGR8888:
''')
rep(C, '''	ccmEnabled_ = ccmEnabled;

	/*
	 * Lookup tables must be initialized''', '''	ccmEnabled_ = ccmEnabled;
	yuvOutput_ = outputCfg.pixelFormat == formats::NV12;

	/*
	 * Lookup tables must be initialized''')
rep(C, '''	if (enableInputMemcpy_) {
		for (unsigned int i = 0; i <= inputConfig.patternSize.height; i++)
			lineBuffers_[i].resize(lineBufferLength_);
	}
}
''', '''	if (enableInputMemcpy_) {
		for (unsigned int i = 0; i <= inputConfig.patternSize.height; i++)
			lineBuffers_[i].resize(lineBufferLength_);
	}

	/* A6L: B,G,R scratch lines for the NV12 output */
	for (unsigned int i = 0; i < 2; i++)
		rgbLines_[i].resize(debayer_->yuvOutput_ ? debayer_->window_.width * 3 : 0);
}

/*
 * A6L: convert the two B,G,R scratch lines to the luma rows at y0 and y0 + stride
 * and one interleaved CbCr row at uv (advanced by one stride). Full-range BT.601 (JFIF).
 */
void DebayerCpuThread::pairToNV12(uint8_t *y0, uint8_t *&uv)
{
	const unsigned int stride = debayer_->outputConfig_.stride;
	const unsigned int width = debayer_->window_.width;
	const uint8_t *p0 = rgbLines_[0].data();
	const uint8_t *p1 = rgbLines_[1].data();
	uint8_t *y1 = y0 + stride;
	auto luma = [](int r, int g, int b) {
		return static_cast<uint8_t>((77 * r + 150 * g + 29 * b + 128) >> 8);
	};
	auto clip = [](int v) {
		return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
	};

	for (unsigned int x = 0; x < width; x += 2) {
		const int b0 = p0[0], g0 = p0[1], r0 = p0[2];
		const int b1 = p0[3], g1 = p0[4], r1 = p0[5];
		const int b2 = p1[0], g2 = p1[1], r2 = p1[2];
		const int b3 = p1[3], g3 = p1[4], r3 = p1[5];
		y0[x] = luma(r0, g0, b0);
		y0[x + 1] = luma(r1, g1, b1);
		y1[x] = luma(r2, g2, b2);
		y1[x + 1] = luma(r3, g3, b3);
		const int r = r0 + r1 + r2 + r3, g = g0 + g1 + g2 + g3, b = b0 + b1 + b2 + b3;
		uv[x] = clip(((-43 * r - 85 * g + 128 * b + 512) >> 10) + 128);
		uv[x + 1] = clip(((128 * r - 107 * g - 21 * b + 512) >> 10) + 128);
		p0 += 6;
		p1 += 6;
	}
	uv += stride;
}
''')

# --- thread process(): UV destination offset for yStart_, pass through
rep(C, '''void DebayerCpuThread::process(uint32_t frame, const uint8_t *src, uint8_t *dst)
{''', '''void DebayerCpuThread::process(uint32_t frame, const uint8_t *src, uint8_t *dst, uint8_t *dstUV)
{''')
rep(C, '''	/* Adjust dst for yStart_ */
	dst += yStart_ * debayer_->outputConfig_.stride;

	if (debayer_->inputConfig_.patternSize.height == 2)
		process2(frame, src, dst);
	else
		process4(frame, src, dst);
''', '''	/* Adjust dst for yStart_ */
	dst += yStart_ * debayer_->outputConfig_.stride;
	/* A6L: one CbCr row per line pair (yStart_ is a multiple of the pattern height) */
	if (dstUV)
		dstUV += yStart_ / 2 * debayer_->outputConfig_.stride;

	if (debayer_->inputConfig_.patternSize.height == 2)
		process2(frame, src, dst, dstUV);
	else
		process4(frame, src, dst, dstUV);
''')
rep(C, 'void DebayerCpuThread::process2(uint32_t frame, const uint8_t *src, uint8_t *dst)\n',
       'void DebayerCpuThread::process2(uint32_t frame, const uint8_t *src, uint8_t *dst, uint8_t *dstUV)\n')
rep(C, 'void DebayerCpuThread::process4(uint32_t frame, const uint8_t *src, uint8_t *dst)\n',
       'void DebayerCpuThread::process4(uint32_t frame, const uint8_t *src, uint8_t *dst, uint8_t *dstUV)\n')

# process2 main loop + tail, process4 loop: route line output through lineOut() and convert after each odd line
rep(C, '''		debayer_->stats_->processLine0(frame, y, linePointers, threadIndex_);
		debayer_->debayer0(dst, linePointers);
		src += inputStride;
		dst += outputStride;

		shiftLinePointers(linePointers, src);
		memcpyNextLine(linePointers);
		debayer_->debayer1(dst, linePointers);
		src += inputStride;
		dst += outputStride;
	}

	if (window.y == 0 && yEnd_ == window.height) {''', '''		debayer_->stats_->processLine0(frame, y, linePointers, threadIndex_);
		debayer_->debayer0(lineOut(0, dst), linePointers);
		src += inputStride;
		dst += outputStride;

		shiftLinePointers(linePointers, src);
		memcpyNextLine(linePointers);
		debayer_->debayer1(lineOut(1, dst), linePointers);
		if (debayer_->yuvOutput_)
			pairToNV12(dst - outputStride, dstUV);
		src += inputStride;
		dst += outputStride;
	}

	if (window.y == 0 && yEnd_ == window.height) {''')
rep(C, '''		debayer_->stats_->processLine0(frame, yEnd, linePointers, threadIndex_);
		debayer_->debayer0(dst, linePointers);
		src += inputStride;
		dst += outputStride;

		shiftLinePointers(linePointers, src);
		/* next line may point outside of src, use prev. */
		linePointers[2] = linePointers[0];
		debayer_->debayer1(dst, linePointers);
		src += inputStride;
		dst += outputStride;
	}''', '''		debayer_->stats_->processLine0(frame, yEnd, linePointers, threadIndex_);
		debayer_->debayer0(lineOut(0, dst), linePointers);
		src += inputStride;
		dst += outputStride;

		shiftLinePointers(linePointers, src);
		/* next line may point outside of src, use prev. */
		linePointers[2] = linePointers[0];
		debayer_->debayer1(lineOut(1, dst), linePointers);
		if (debayer_->yuvOutput_)
			pairToNV12(dst - outputStride, dstUV);
		src += inputStride;
		dst += outputStride;
	}''')
rep(C, '''		debayer_->stats_->processLine0(frame, y, linePointers, threadIndex_);
		debayer_->debayer0(dst, linePointers);
		src += inputStride;
		dst += outputStride;

		shiftLinePointers(linePointers, src);
		memcpyNextLine(linePointers);
		debayer_->debayer1(dst, linePointers);
		src += inputStride;
		dst += outputStride;

		shiftLinePointers(linePointers, src);
		memcpyNextLine(linePointers);
		debayer_->stats_->processLine2(frame, y, linePointers, threadIndex_);
		debayer_->debayer2(dst, linePointers);
		src += inputStride;
		dst += outputStride;

		shiftLinePointers(linePointers, src);
		memcpyNextLine(linePointers);
		debayer_->debayer3(dst, linePointers);
		src += inputStride;
		dst += outputStride;''', '''		debayer_->stats_->processLine0(frame, y, linePointers, threadIndex_);
		debayer_->debayer0(lineOut(0, dst), linePointers);
		src += inputStride;
		dst += outputStride;

		shiftLinePointers(linePointers, src);
		memcpyNextLine(linePointers);
		debayer_->debayer1(lineOut(1, dst), linePointers);
		if (debayer_->yuvOutput_)
			pairToNV12(dst - outputStride, dstUV);
		src += inputStride;
		dst += outputStride;

		shiftLinePointers(linePointers, src);
		memcpyNextLine(linePointers);
		debayer_->stats_->processLine2(frame, y, linePointers, threadIndex_);
		debayer_->debayer2(lineOut(0, dst), linePointers);
		src += inputStride;
		dst += outputStride;

		shiftLinePointers(linePointers, src);
		memcpyNextLine(linePointers);
		debayer_->debayer3(lineOut(1, dst), linePointers);
		if (debayer_->yuvOutput_)
			pairToNV12(dst - outputStride, dstUV);
		src += inputStride;
		dst += outputStride;''')

# --- DebayerCpu::process(): UV plane pointer, bytesused for every plane
rep(C, '''	for (auto &thread : threads_)
		thread->invokeMethod(&DebayerCpuThread::process,
				     ConnectionTypeQueued, frame,
				     in.planes()[0].data(), out.planes()[0].data());
''', '''	/* A6L: NV12 CbCr plane = plane 1, or directly after the luma plane of a single-plane buffer */
	uint8_t *outUV = nullptr;
	if (yuvOutput_) {
		const size_t lumaSize = static_cast<size_t>(outputConfig_.stride) * window_.height;
		if (out.planes().size() >= 2) {
			outUV = out.planes()[1].data();
		} else if (out.planes()[0].size() >= lumaSize * 3 / 2) {
			outUV = out.planes()[0].data() + lumaSize;
		} else {
			LOG(Debayer, Error) << "NV12 output buffer too small";
			metadata.status = FrameMetadata::FrameError;
			return;
		}
	}

	for (auto &thread : threads_)
		thread->invokeMethod(&DebayerCpuThread::process,
				     ConnectionTypeQueued, frame,
				     in.planes()[0].data(), out.planes()[0].data(), outUV);
''')
rep(C, '''	metadata.planes()[0].bytesused = out.planes()[0].size();
''', '''	for (unsigned int i = 0; i < metadata.planes().size() && i < out.planes().size(); i++)
		metadata.planes()[i].bytesused = out.planes()[i].size();
''')

# --- header: flag
rep(H, '''	bool ccmEnabled_;
	DebayerParams params_;
''', '''	bool ccmEnabled_;
	bool yuvOutput_ = false; /* A6L: NV12 output (RGB888 debayer + per line-pair conversion) */
	DebayerParams params_;
''')
print('A6L_NV12_EDIT_OK')
