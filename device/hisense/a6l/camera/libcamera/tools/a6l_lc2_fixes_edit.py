#!/usr/bin/env python3
# A6L libcam2 fixes (29 Sep 2026, after the first on-phone run t37/lc1; docs/libcamera-plan-20260929.md s.10).
# Usage: a6l_lc2_fixes_edit.py <libcamera-src with 0001-0005 applied> hi846|dmaheap   (idempotent)
#   hi846   -> patches/0006-a6l-hi846-sensor-helper-delays-tuning.patch
#   dmaheap -> patches/0007-dma_buf_allocator-a6l-system-heap-first.patch
import sys, os
src, what = sys.argv[1], sys.argv[2]

def rd(rel): return open(os.path.join(src, rel)).read()
def wr(rel, s): open(os.path.join(src, rel), 'w').write(s); print('edited', rel)

if what == 'hi846':
    rel = 'src/ipa/libipa/camera_sensor_helper.cpp'; s = rd(rel)
    if 'CameraSensorHelperHi846' not in s:
        anchor = 'REGISTER_CAMERA_SENSOR_HELPER("hm1246", CameraSensorHelperHm1246)\n'
        assert anchor in s
        s = s.replace(anchor, anchor + '''
/*
 * A6L: SK Hynix Hi-846 (8 MP wide, mainline hi846 driver, V4L2 gain = register 0x0077, 0..240).
 * Hynix analogue gain: gain = 1 + code / 16 (1x..16x). Black level 64 at 10 bits.
 */
class CameraSensorHelperHi846 : public CameraSensorHelper
{
public:
	CameraSensorHelperHi846()
	{
		blackLevel_ = 4096;
		gain_ = AnalogueGainLinear{ 1, 16, 0, 16 };
	}
};
REGISTER_CAMERA_SENSOR_HELPER("hi846", CameraSensorHelperHi846)
''', 1); wr(rel, s)
    rel = 'src/libcamera/sensor/camera_sensor_properties.cpp'; s = rd(rel)
    i = s.find('{ "hi846", {'); assert i > 0
    j = s.find('.sensorDelays = { },', i); k = s.find('} },', i)
    if 0 < j < k:
        s = s[:j] + '''/* A6L: conservative, no grouped hold (as imx576_a6l/s5k3t1) */
			.sensorDelays = {
				.exposureDelay = 2,
				.gainDelay = 2,
				.vblankDelay = 2,
				.hblankDelay = 2
			},''' + s[j + len('.sensorDelays = { },'):]
        wr(rel, s)
    y = os.path.join(src, 'src/ipa/simple/data/hi846.yaml')
    open(y, 'w').write(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'data', 'hi846.yaml')).read())
    rel = 'src/ipa/simple/data/meson.build'; s = rd(rel)
    if "'hi846.yaml'" not in s:
        s = s.replace("conf_files = files([\n", "conf_files = files([\n    'hi846.yaml',\n", 1); wr(rel, s)

elif what == 'dmaheap':
    rel = 'src/libcamera/dma_buf_allocator.cpp'; s = rd(rel)
    if 'LIBCAMERA_DMA_HEAP' in s: print('already', rel); sys.exit(0)
    old = s[s.find('static constexpr std::array<DmaBufAllocatorInfo, 4> providerInfos'):]
    old = old[:old.find('} };') + 4]
    assert old.startswith('static constexpr')
    new = '''static constexpr std::array<DmaBufAllocatorInfo, 5> providerInfos = { {
	/*
	 * A6L: prefer the system heap. The SoftISP output and the Android HAL
	 * internal buffers are only touched by the CPU (and by IOMMU-backed
	 * consumers), so they need no physically contiguous memory, and the
	 * default CMA area is small (32 MB, CONFIG_CMA_SIZE_MBYTES): upstream's
	 * CMA-first order picked /dev/dma_heap/reserved on the A6L and failed
	 * after 3 buffers at 2880x2156 ABGR8888 (t37, 29 Sep 2026).
	 *
	 * /dev/dma_heap/linux,cma is the CMA dma-heap. When the cma heap size is
	 * specified on the kernel command line, this gets renamed to "reserved".
	 * Linux >= 6.17 names the default CMA heap "default_cma_region" (and also
	 * exports the same area under its CMA name, "reserved").
	 */
	{ DmaBufAllocator::DmaBufAllocatorFlag::SystemHeap, "/dev/dma_heap/system" },
	{ DmaBufAllocator::DmaBufAllocatorFlag::CmaHeap, "/dev/dma_heap/linux,cma" },
	{ DmaBufAllocator::DmaBufAllocatorFlag::CmaHeap, "/dev/dma_heap/default_cma_region" },
	{ DmaBufAllocator::DmaBufAllocatorFlag::CmaHeap, "/dev/dma_heap/reserved" },
	{ DmaBufAllocator::DmaBufAllocatorFlag::UDmaBuf, "/dev/udmabuf" },
} };'''
    s = s.replace(old, new, 1)
    ctor = 'DmaBufAllocator::DmaBufAllocator(DmaBufAllocatorFlags type)\n{\n'
    assert ctor in s
    s = s.replace(ctor, ctor + '''	/*
	 * A6L: LIBCAMERA_DMA_HEAP=<heap name>|udmabuf|<absolute node path> tries
	 * that provider first (if its type is one of the requested types).
	 */
	const char *forced = utils::secure_getenv("LIBCAMERA_DMA_HEAP");
	if (forced && *forced) {
		std::string node = forced[0] == '/' ? std::string(forced)
			: std::string(forced) == "udmabuf" ? std::string("/dev/udmabuf")
			: std::string("/dev/dma_heap/") + forced;
		DmaBufAllocatorFlag ftype = node == "/dev/udmabuf" ? DmaBufAllocatorFlag::UDmaBuf
			: node.rfind("/dev/dma_heap/system", 0) == 0 ? DmaBufAllocatorFlag::SystemHeap
			: DmaBufAllocatorFlag::CmaHeap;
		if (type & ftype) {
			int fd = ::open(node.c_str(), O_RDWR | O_CLOEXEC, 0);
			if (fd < 0 && errno == EACCES)
				fd = ::open(node.c_str(), O_RDONLY | O_CLOEXEC, 0);
			if (fd >= 0) {
				LOG(DmaBufAllocator, Debug) << "Using " << node << " (LIBCAMERA_DMA_HEAP)";
				providerHandle_ = UniqueFD(fd);
				type_ = ftype;
				return;
			}
			LOG(DmaBufAllocator, Warning)
				<< "LIBCAMERA_DMA_HEAP: failed to open " << node << ": " << strerror(errno);
		}
	}

''', 1)
    s = s.replace('#include <libcamera/base/shared_fd.h>\n', '#include <libcamera/base/shared_fd.h>\n#include <libcamera/base/utils.h>\n', 1)
    if '#include <string>' not in s:
        s = s.replace('#include <array>\n', '#include <array>\n#include <string>\n', 1)
    wr(rel, s)
