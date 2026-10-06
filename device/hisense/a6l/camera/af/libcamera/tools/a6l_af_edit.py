#!/usr/bin/env python3
# Hisense A6L camera phase 3 (AF): source edits on top of libcamera v0.7.2 + A6L patches 0001-0008.
# Usage: a6l_af_edit.py <libcamera-src> <step>     step = stats | lens | af | hal
#   stats: SwIspStats sharpness + sharpnessCount (centre window, RAW10 CSI-2 packed line functions)
#   lens : soft IPA <-> simple pipeline lens plumbing (init lensControls, setLensControls event -> CameraLens)
#   af   : Af algorithm (af.cpp/af.h/af_search.h from ../src), IPA context, lens move emission
#   hal  : Android HAL AF modes/trigger/state, LENS_FOCUS_DISTANCE, min focus distance (only if controls::AfMode)
# Every replacement must match exactly once (the script fails loudly otherwise). The generated commits are
# exported as device/hisense/a6l/camera/af/libcamera/patches/01xx-*.patch by build-libcamera-af.sh.
import os
import shutil
import sys

SRC = sys.argv[1]
STEP = sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))


def edit(path, old, new, count=1):
    p = os.path.join(SRC, path)
    s = open(p).read()
    n = s.count(old)
    if n != count:
        sys.exit(f"EDIT_FAIL {path}: expected {count} match(es), found {n}:\n{old}")
    s = s.replace(old, new)
    open(p, 'w').write(s)


def stats():
    edit('include/libcamera/internal/software_isp/swisp_stats.h',
         """	Histogram yHistogram;
""", """	Histogram yHistogram;
	/**
	 * \\brief A6L focus measure: sum of the squared differences of the
	 * averaged green of consecutive sampled 2x2 blocks, centre half x half
	 * of the window (RAW10 CSI-2 packed only, 0 otherwise)
	 */
	uint64_t sharpness;
	/**
	 * \\brief Number of differences accumulated in sharpness
	 */
	uint32_t sharpnessCount;
""")
    h = 'include/libcamera/internal/software_isp/swstats_cpu.h'
    for fn, ptr in (('processLine0', 'stats0_'), ('processLine2', 'stats2_')):
        edit(h, f"""		    y >= (window_.y + window_.height))
			return;

		(this->*{ptr})(src, stats_[statsBufferIndex]);""",
             f"""		    y >= (window_.y + window_.height))
			return;

		setSharpRow(y, statsBufferIndex);
		(this->*{ptr})(src, stats_[statsBufferIndex]);""")
    edit(h, """	int setupStandardBayerOrder(BayerFormat::Order order);
""", """	int setupStandardBayerOrder(BayerFormat::Order order);

	/* A6L AF: the sharpness statistic only uses the centre half of the window rows */
	void setSharpRow(unsigned int y, unsigned int statsBufferIndex)
	{
		const unsigned int ry = y - window_.y;
		sharpRow_[statsBufferIndex] = ry >= window_.height / 4 && ry < window_.height * 3 / 4;
	}
	bool sharpRow(const SwIspStats &stats) const
	{
		return sharpRow_[&stats - stats_.data()];
	}
""")
    edit(h, """	std::vector<SwIspStats> stats_;
""", """	std::vector<SwIspStats> stats_;
	std::vector<uint8_t> sharpRow_;
""")

    c = 'src/libcamera/software_isp/swstats_cpu.cpp'
    # 10-bit packed line functions: accumulate the sharpness in the centre half of the columns
    for order, body in (('BGGR', """		/* BGGR */
		b = src0[x];
		g = src0[x + 1];
		g2 = src1[x];
		r = src1[x + 1];
		g = (g + g2) / 2;
"""), ('GBRG', """		/* GBRG */
		g = src0[x];
		b = src0[x + 1];
		r = src1[x];
		g2 = src1[x + 1];
		g = (g + g2) / 2;
""")):
        edit(c, f"""void SwStatsCpu::stats{order}10PLine0(const uint8_t *src[], SwIspStats &stats)
{{
	const uint8_t *src0 = src[1] + window_.x * 5 / 4;
	const uint8_t *src1 = src[2] + window_.x * 5 / 4;
	const unsigned int widthInBytes = window_.width * 5 / 4;

	if (swapLines_)
		std::swap(src0, src1);

	SWSTATS_START_LINE_STATS(uint8_t)

	/* x += 5 sample every other 2x2 block */
	for (unsigned int x = 0; x < widthInBytes; x += 5) {{
{body}		/* Data is already 8 bits, divide by 1 */
		SWSTATS_ACCUMULATE_LINE_STATS(1)
	}}

	SWSTATS_FINISH_LINE_STATS()
}}""", f"""void SwStatsCpu::stats{order}10PLine0(const uint8_t *src[], SwIspStats &stats)
{{
	const uint8_t *src0 = src[1] + window_.x * 5 / 4;
	const uint8_t *src1 = src[2] + window_.x * 5 / 4;
	const unsigned int widthInBytes = window_.width * 5 / 4;
	/* A6L AF: centre half of the columns, on the centre half of the rows */
	const bool sharp = sharpRow(stats);
	const unsigned int sx0 = widthInBytes / 4 / 5 * 5;
	const unsigned int sx1 = widthInBytes * 3 / 4 / 5 * 5;
	int prevG = -1;
	uint64_t sharpSum = 0;
	uint32_t sharpCount = 0;

	if (swapLines_)
		std::swap(src0, src1);

	SWSTATS_START_LINE_STATS(uint8_t)

	/* x += 5 sample every other 2x2 block */
	for (unsigned int x = 0; x < widthInBytes; x += 5) {{
{body}		if (sharp && x >= sx0 && x < sx1) {{
			if (prevG >= 0) {{
				const int d = static_cast<int>(g) - prevG;
				sharpSum += static_cast<uint64_t>(d * d);
				sharpCount++;
			}}
			prevG = g;
		}}
		/* Data is already 8 bits, divide by 1 */
		SWSTATS_ACCUMULATE_LINE_STATS(1)
	}}

	SWSTATS_FINISH_LINE_STATS()
	stats.sharpness += sharpSum;
	stats.sharpnessCount += sharpCount;
}}""")
    edit(c, """		s.sum_ = RGB<uint64_t>({ 0, 0, 0 });
		s.yHistogram.fill(0);
	}
""", """		s.sum_ = RGB<uint64_t>({ 0, 0, 0 });
		s.yHistogram.fill(0);
		s.sharpness = 0;
		s.sharpnessCount = 0;
	}
""")
    edit(c, """		sharedStats_->yHistogram.fill(0);
		for (const auto &s : stats_) {
			sharedStats_->sum_ += s.sum_;
""", """		sharedStats_->yHistogram.fill(0);
		sharedStats_->sharpness = 0;
		sharedStats_->sharpnessCount = 0;
		for (const auto &s : stats_) {
			sharedStats_->sum_ += s.sum_;
			sharedStats_->sharpness += s.sharpness;
			sharedStats_->sharpnessCount += s.sharpnessCount;
""")
    edit(c, """	stats_.resize(statsBufferCount);
""", """	stats_.resize(statsBufferCount);
	sharpRow_.assign(statsBufferCount, 0);
""")
    edit(c, """		/* linePointers[0] is not used by any stats0_ functions */
		linePointers[1] = src;
		linePointers[2] = src + stride_;
""", """		/* linePointers[0] is not used by any stats0_ functions */
		linePointers[1] = src;
		linePointers[2] = src + stride_;
		setSharpRow(window_.y + y, 0);
""")


def lens():
    edit('include/libcamera/ipa/soft.mojom', """	     libcamera.ControlInfoMap sensorControls)
		=> (int32 ret, libcamera.ControlInfoMap ipaControls, bool ccmEnabled);""",
         """	     libcamera.ControlInfoMap sensorControls,
	     libcamera.ControlInfoMap lensControls)
		=> (int32 ret, libcamera.ControlInfoMap ipaControls, bool ccmEnabled);""")
    edit('include/libcamera/ipa/soft.mojom', """	setSensorControls(libcamera.ControlList sensorControls);
""", """	setSensorControls(libcamera.ControlList sensorControls);
	setLensControls(libcamera.ControlList lensControls);
""")

    h = 'include/libcamera/internal/software_isp/software_isp.h'
    edit(h, """	Signal<const ControlList &> setSensorControls;
""", """	Signal<const ControlList &> setSensorControls;
	Signal<const ControlList &> setLensControls;
""")
    c = 'src/libcamera/software_isp/software_isp.cpp'
    edit(c, """#include "libcamera/internal/bayer_format.h"
""", """#include "libcamera/internal/bayer_format.h"
#include "libcamera/internal/camera_lens.h"
#include "libcamera/internal/camera_sensor.h"
""")
    edit(c, """	ret = ipa_->init(IPASettings{ ipaTuningFile, sensor->model() },
			 debayer_->getStatsFD(),
			 sharedParams_.fd(),
			 sensorInfo,
			 sensor->controls(),
			 ipaControls,""", """	/*
	 * A6L AF: the focus lens (DT lens-focus ancillary link), empty map if
	 * none. focusLens() is not const but only returns the lens pointer.
	 */
	CameraLens *lens = const_cast<CameraSensor *>(sensor)->focusLens();
	const ControlInfoMap lensControls = lens ? lens->controls() : ControlInfoMap();
	if (lens)
		LOG(SoftwareIsp, Info) << "Focus lens " << lens->model();

	ret = ipa_->init(IPASettings{ ipaTuningFile, sensor->model() },
			 debayer_->getStatsFD(),
			 sharedParams_.fd(),
			 sensorInfo,
			 sensor->controls(),
			 lensControls,
			 ipaControls,""")
    edit(c, """	ipa_->setSensorControls.connect(this, &SoftwareIsp::setSensorCtrls);
""", """	ipa_->setSensorControls.connect(this, &SoftwareIsp::setSensorCtrls);
	ipa_->setLensControls.connect(this,
				      [this](const ControlList &lensControls) {
					      setLensControls.emit(lensControls);
				      });
""")
    edit(c, """ * \\var SoftwareIsp::setSensorControls
""", """ * \\var SoftwareIsp::setLensControls
 * \\brief Signal emitted when the focus lens position shall be changed
 * (V4L2_CID_FOCUS_ABSOLUTE)
 */

/**
 * \\var SoftwareIsp::setSensorControls
""")

    p = 'src/libcamera/pipeline/simple/simple.cpp'
    edit(p, """#include "libcamera/internal/camera.h"
""", """#include "libcamera/internal/camera.h"
#include "libcamera/internal/camera_lens.h"
""")
    edit(p, """	void setSensorControls(const ControlList &sensorControls);
};
""", """	void setSensorControls(const ControlList &sensorControls);
	void setLensControls(const ControlList &lensControls);
};
""")
    edit(p, """			swIsp_->setSensorControls.connect(this, &SimpleCameraData::setSensorControls);
""", """			swIsp_->setSensorControls.connect(this, &SimpleCameraData::setSensorControls);
			swIsp_->setLensControls.connect(this, &SimpleCameraData::setLensControls);
""")
    edit(p, """/* Retrieve all source pads connected to a sink pad through active routes. */
""", """/* A6L AF: the soft IPA moves the VCM directly (no frame alignment, the IPA skips settling frames). */
void SimpleCameraData::setLensControls(const ControlList &lensControls)
{
	CameraLens *lens = sensor_->focusLens();
	if (!lens || !lensControls.contains(V4L2_CID_FOCUS_ABSOLUTE))
		return;

	const ControlValue &focus = lensControls.get(V4L2_CID_FOCUS_ABSOLUTE);
	lens->setFocusPosition(focus.get<int32_t>());
}

/* Retrieve all source pads connected to a sink pad through active routes. */
""")

    ctx = 'src/ipa/simple/ipa_context.h'
    edit(ctx, """	ControlInfoMap::Map ctrlMap;
	bool ccmEnabled = false;
""", """	ControlInfoMap::Map ctrlMap;
	bool ccmEnabled = false;

	/* A6L AF: focus lens limits (V4L2_CID_FOCUS_ABSOLUTE), from init() */
	struct {
		bool present = false;
		int32_t min = 0;
		int32_t max = 0;
	} lens;
""")
    s = 'src/ipa/simple/soft_simple.cpp'
    for old, new in ((
            """		 const ControlInfoMap &sensorControls,
		 ControlInfoMap *ipaControls,
		 bool *ccmEnabled) override;""",
            """		 const ControlInfoMap &sensorControls,
		 const ControlInfoMap &lensControls,
		 ControlInfoMap *ipaControls,
		 bool *ccmEnabled) override;"""), (
            """			const ControlInfoMap &sensorControls,
			ControlInfoMap *ipaControls,
			bool *ccmEnabled)
{""",
            """			const ControlInfoMap &sensorControls,
			const ControlInfoMap &lensControls,
			ControlInfoMap *ipaControls,
			bool *ccmEnabled)
{"""), (
            """	ControlInfoMap sensorInfoMap_;
""", """	ControlInfoMap sensorInfoMap_;
	ControlInfoMap lensInfoMap_;
"""), (
            """	context_.sensorInfo = sensorInfo;
""", """	context_.sensorInfo = sensorInfo;

	/* A6L AF: must be known before the algorithms are created */
	lensInfoMap_ = lensControls;
	const auto lensIt = lensControls.find(V4L2_CID_FOCUS_ABSOLUTE);
	if (lensIt != lensControls.end()) {
		context_.lens.present = true;
		context_.lens.min = lensIt->second.min().get<int32_t>();
		context_.lens.max = lensIt->second.max().get<int32_t>();
	}
""")):
        edit(s, old, new)


def af():
    alg = os.path.join(SRC, 'src/ipa/simple/algorithms')
    for f in ('af.cpp', 'af.h', 'af_search.h'):
        shutil.copy(os.path.join(HERE, '..', 'src', f), os.path.join(alg, f))
    edit('src/ipa/simple/algorithms/meson.build', """    'adjust.cpp',
""", """    'adjust.cpp',
    'af.cpp',
""")
    ctx = 'src/ipa/simple/ipa_context.h'
    edit(ctx, """	Matrix<float, 3, 3> combinedMatrix;

	struct {
		float gamma;""", """	Matrix<float, 3, 3> combinedMatrix;

	/* A6L AF: lens position in VCM DAC codes */
	struct {
		int32_t dac;
		bool valid;
		bool moveRequested;
		bool moveEmitted;
		uint32_t moveFrame;
	} af;

	struct {
		float gamma;""")
    s = 'src/ipa/simple/soft_simple.cpp'
    edit(s, """	setSensorControls.emit(ctrls);
}
""", """	setSensorControls.emit(ctrls);

	/* A6L AF: lens move requested by the Af algorithm (from process() or queueRequest()) */
	auto &af = context_.activeState.af;
	if (context_.lens.present && af.moveRequested) {
		ControlList lensCtrls(lensInfoMap_);
		lensCtrls.set(V4L2_CID_FOCUS_ABSOLUTE, af.dac);
		setLensControls.emit(lensCtrls);
		af.moveRequested = false;
		af.moveEmitted = true;
		af.moveFrame = frame;
	}
}
""")
    y = 'src/ipa/simple/data/imx576_a6l.yaml'
    edit(y, """  - Agc:
""", open(os.path.join(HERE, '..', 'data', 'af-block.yaml')).read())


def hal():
    # Android HAL: AF modes/trigger/state + lens focus distance when the camera exposes controls::AfMode
    c = 'src/android/camera_capabilities.cpp'
    edit(c, """	std::vector<uint8_t> availableAfModes = {
		ANDROID_CONTROL_AF_MODE_OFF,
	};
	staticMetadata_->addEntry(ANDROID_CONTROL_AF_AVAILABLE_MODES,
				  availableAfModes);
""", """	std::vector<uint8_t> availableAfModes = {
		ANDROID_CONTROL_AF_MODE_OFF,
	};
	/* A6L AF: the soft IPA Af algorithm (VCM lens) */
	const auto &afModeInfo = controlsInfo.find(&controls::AfMode);
	const auto &lensPosInfo = controlsInfo.find(&controls::LensPosition);
	const bool hasAf = afModeInfo != controlsInfo.end() && lensPosInfo != controlsInfo.end();
	if (hasAf) {
		availableAfModes.push_back(ANDROID_CONTROL_AF_MODE_AUTO);
		availableAfModes.push_back(ANDROID_CONTROL_AF_MODE_MACRO);
		availableAfModes.push_back(ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO);
		availableAfModes.push_back(ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE);
		availableRequestKeys_.insert(ANDROID_LENS_FOCUS_DISTANCE);
		availableResultKeys_.insert(ANDROID_LENS_FOCUS_DISTANCE);
		availableCharacteristicsKeys_.insert(ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION);
		uint8_t calibration = ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_APPROXIMATE;
		staticMetadata_->addEntry(ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION,
					  calibration);
	}
	staticMetadata_->addEntry(ANDROID_CONTROL_AF_AVAILABLE_MODES,
				  availableAfModes);
""")
    edit(c, """	float hypeFocalDistance = 0;
	staticMetadata_->addEntry(ANDROID_LENS_INFO_HYPERFOCAL_DISTANCE,
				  hypeFocalDistance);

	float minFocusDistance = 0;
	staticMetadata_->addEntry(ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE,
				  minFocusDistance);
""", """	/* A6L AF: dioptres from the IPA LensPosition range (0 = fixed focus) */
	float hypeFocalDistance = 0;
	float minFocusDistance = 0;
	{
		const auto &lensPos = controlsInfo.find(&controls::LensPosition);
		if (lensPos != controlsInfo.end() &&
		    controlsInfo.find(&controls::AfMode) != controlsInfo.end()) {
			minFocusDistance = lensPos->second.max().get<float>();
			hypeFocalDistance = lensPos->second.def().get<float>();
		}
	}
	staticMetadata_->addEntry(ANDROID_LENS_INFO_HYPERFOCAL_DISTANCE,
				  hypeFocalDistance);

	staticMetadata_->addEntry(ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE,
				  minFocusDistance);
""")
    edit(c, """	uint8_t afMode = ANDROID_CONTROL_AF_MODE_OFF;
	requestTemplate->addEntry(ANDROID_CONTROL_AF_MODE, afMode);
""", """	/* A6L AF: continuous picture AF by default when the camera has AF */
	uint8_t afMode = ANDROID_CONTROL_AF_MODE_OFF;
	camera_metadata_ro_entry_t afModesEntry;
	if (staticMetadata_->getEntry(ANDROID_CONTROL_AF_AVAILABLE_MODES, &afModesEntry) &&
	    afModesEntry.count > 1)
		afMode = ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
	requestTemplate->addEntry(ANDROID_CONTROL_AF_MODE, afMode);
""")
    edit(c, """	previewTemplate->updateEntry(ANDROID_CONTROL_AE_TARGET_FPS_RANGE,
				     entry.data.i32 + 2, 2);

	return previewTemplate;
""", """	previewTemplate->updateEntry(ANDROID_CONTROL_AE_TARGET_FPS_RANGE,
				     entry.data.i32 + 2, 2);

	/* A6L AF: continuous video AF in the video template */
	camera_metadata_ro_entry_t afEntry;
	if (previewTemplate->getEntry(ANDROID_CONTROL_AF_MODE, &afEntry) &&
	    *afEntry.data.u8 == ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE) {
		uint8_t afVideo = ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO;
		previewTemplate->updateEntry(ANDROID_CONTROL_AF_MODE, afVideo);
	}

	return previewTemplate;
""")

    h = 'src/android/camera_device.h'
    edit(h, """#include <map>
""", """#include <atomic>
#include <map>
""")
    edit(h, """	unsigned int id_;
	camera3_device_t camera3Device_;
""", """	unsigned int id_;
	camera3_device_t camera3Device_;

	/* A6L AF: AF_TRIGGER_START received in a continuous AF mode (lock until CANCEL) */
	std::atomic<bool> afLocked_{ false };
""")
    d = 'src/android/camera_device.cpp'
    edit(d, """	if (settings.getEntry(ANDROID_SENSOR_TEST_PATTERN_MODE, &entry)) {""", """	/*
	 * A6L AF: OFF -> AfModeManual (+ LENS_FOCUS_DISTANCE, dioptres in both APIs), AUTO/MACRO -> AfModeAuto with
	 * AfTrigger, CONTINUOUS_* -> AfModeContinuous. A trigger START in a continuous mode locks: the IPA is switched
	 * to AfModeAuto (it finishes a running scan, then keeps the lens) until CANCEL or a mode change.
	 */
	if (camera_->controls().find(&controls::AfMode) != camera_->controls().end() &&
	    settings.getEntry(ANDROID_CONTROL_AF_MODE, &entry)) {
		const uint8_t afMode = *entry.data.u8;
		uint8_t trigger = ANDROID_CONTROL_AF_TRIGGER_IDLE;
		camera_metadata_ro_entry_t trig;
		if (settings.getEntry(ANDROID_CONTROL_AF_TRIGGER, &trig))
			trigger = *trig.data.u8;

		switch (afMode) {
		case ANDROID_CONTROL_AF_MODE_AUTO:
		case ANDROID_CONTROL_AF_MODE_MACRO:
			afLocked_ = false;
			controls.set(controls::AfMode, controls::AfModeAuto);
			if (trigger == ANDROID_CONTROL_AF_TRIGGER_START)
				controls.set(controls::AfTrigger, controls::AfTriggerStart);
			else if (trigger == ANDROID_CONTROL_AF_TRIGGER_CANCEL)
				controls.set(controls::AfTrigger, controls::AfTriggerCancel);
			break;
		case ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO:
		case ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE:
			if (trigger == ANDROID_CONTROL_AF_TRIGGER_START)
				afLocked_ = true;
			else if (trigger == ANDROID_CONTROL_AF_TRIGGER_CANCEL)
				afLocked_ = false;
			controls.set(controls::AfMode, afLocked_ ? controls::AfModeAuto
								 : controls::AfModeContinuous);
			break;
		default: {
			afLocked_ = false;
			controls.set(controls::AfMode, controls::AfModeManual);
			camera_metadata_ro_entry_t dist;
			if (settings.getEntry(ANDROID_LENS_FOCUS_DISTANCE, &dist))
				controls.set(controls::LensPosition, *dist.data.f);
			break;
		}
		}
	}

	if (settings.getEntry(ANDROID_SENSOR_TEST_PATTERN_MODE, &entry)) {""")
    edit(d, """	value = ANDROID_CONTROL_AF_MODE_OFF;
	resultMetadata->addEntry(ANDROID_CONTROL_AF_MODE, value);

	value = ANDROID_CONTROL_AF_STATE_INACTIVE;
	resultMetadata->addEntry(ANDROID_CONTROL_AF_STATE, value);

	value = ANDROID_CONTROL_AF_TRIGGER_IDLE;
	resultMetadata->addEntry(ANDROID_CONTROL_AF_TRIGGER, value);
""", """	/* A6L AF: report the libcamera AfState / LensPosition in Android terms */
	uint8_t a6lLensState = ANDROID_LENS_STATE_STATIONARY;
	{
		const auto &libAfState = metadata.get(controls::AfState);
		const auto &libLensPos = metadata.get(controls::LensPosition);
		uint8_t afMode = ANDROID_CONTROL_AF_MODE_OFF;
		uint8_t afTrigger = ANDROID_CONTROL_AF_TRIGGER_IDLE;
		uint8_t afState = ANDROID_CONTROL_AF_STATE_INACTIVE;
		if (libAfState && settings.getEntry(ANDROID_CONTROL_AF_MODE, &entry))
			afMode = *entry.data.u8;
		if (libAfState && settings.getEntry(ANDROID_CONTROL_AF_TRIGGER, &entry))
			afTrigger = *entry.data.u8;
		const int32_t s = libAfState.value_or(controls::AfStateIdle);
		const bool continuous = afMode == ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO ||
					afMode == ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
		if (!libAfState || afMode == ANDROID_CONTROL_AF_MODE_OFF ||
		    afMode == ANDROID_CONTROL_AF_MODE_EDOF) {
			afState = ANDROID_CONTROL_AF_STATE_INACTIVE;
		} else if (continuous && !afLocked_) {
			afState = s == controls::AfStateScanning ? ANDROID_CONTROL_AF_STATE_PASSIVE_SCAN
				: s == controls::AfStateFocused ? ANDROID_CONTROL_AF_STATE_PASSIVE_FOCUSED
				: s == controls::AfStateFailed ? ANDROID_CONTROL_AF_STATE_PASSIVE_UNFOCUSED
				: ANDROID_CONTROL_AF_STATE_INACTIVE;
		} else if (continuous) {
			afState = s == controls::AfStateScanning ? ANDROID_CONTROL_AF_STATE_PASSIVE_SCAN
				: s == controls::AfStateFocused ? ANDROID_CONTROL_AF_STATE_FOCUSED_LOCKED
				: ANDROID_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED;
		} else {
			afState = s == controls::AfStateScanning ? ANDROID_CONTROL_AF_STATE_ACTIVE_SCAN
				: s == controls::AfStateFocused ? ANDROID_CONTROL_AF_STATE_FOCUSED_LOCKED
				: s == controls::AfStateFailed ? ANDROID_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED
				: ANDROID_CONTROL_AF_STATE_INACTIVE;
		}
		if (libAfState && s == controls::AfStateScanning)
			a6lLensState = ANDROID_LENS_STATE_MOVING;

		resultMetadata->addEntry(ANDROID_CONTROL_AF_MODE, afMode);
		resultMetadata->addEntry(ANDROID_CONTROL_AF_STATE, afState);
		resultMetadata->addEntry(ANDROID_CONTROL_AF_TRIGGER, afTrigger);
		if (libLensPos)
			resultMetadata->addEntry(ANDROID_LENS_FOCUS_DISTANCE, *libLensPos);
	}
""")
    edit(d, """	value = ANDROID_LENS_STATE_STATIONARY;
	resultMetadata->addEntry(ANDROID_LENS_STATE, value);
""", """	resultMetadata->addEntry(ANDROID_LENS_STATE, a6lLensState);
""")


{'stats': stats, 'lens': lens, 'af': af, 'hal': hal}[STEP]()
print(f"A6L_AF_EDIT_OK {STEP}")
