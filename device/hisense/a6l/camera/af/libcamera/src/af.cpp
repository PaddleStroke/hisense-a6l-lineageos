/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Hisense A6L: contrast-detect autofocus for the simple IPA.
 *
 * Statistic: SwStatsCpu accumulates, in the centre half x half of the stats window, the squared difference of
 * the averaged green of consecutive sampled 2x2 blocks (4 px apart) on the sampled rows (RAW10 CSI-2 packed only;
 * sharpnessCount == 0 means "not available" and AF stays manual). Stats are produced every
 * SwStatsCpu::kStatPerNumFrames (4) frames, which also covers the VCM settle time (GT9769 ringing control,
 * < 10 ms) and the sensor pipeline delay: a lens move requested while processing stats frame N is complete
 * before frame N + 4 starts exposing.
 *
 * Controls (only when the pipeline passed a lens with V4L2_CID_FOCUS_ABSOLUTE):
 *   AfMode Manual (default) / Auto / Continuous, AfTrigger Start / Cancel (Auto), LensPosition (dioptres, Manual;
 *   range 0 .. the widened macro end of the scan = the Android minimum focus distance)
 * Metadata: AfState, LensPosition.
 * The lens is moved by IPAActiveState::af (dac + moveRequested), emitted by IPASoftSimple::processStats() as
 * a setLensControls(V4L2_CID_FOCUS_ABSOLUTE) event; a valid stats frame less than settleFrames (default 3) after
 * that event is not used (a move requested from queueRequest() can land just before a stats frame).
 *
 * Log lines (category IPASoftAf): Debug "A6L_AF pos=<dac> sharp=<v> state=<s>" per measurement,
 * Info "A6L_AF_RESULT state=<focused|failed> dac=<n> dioptres=<d> peak=<v> coarse=<n> fine=<n>" per scan.
 */

#include "af.h"

#include <cmath>

#include <libcamera/base/log.h>

#include <libcamera/control_ids.h>

#include "libcamera/internal/software_isp/swisp_stats.h"

namespace libcamera {

namespace ipa::soft::algorithms {

LOG_DEFINE_CATEGORY(IPASoftAf)

int Af::init(IPAContext &context, const ValueNode &tuningData)
{
	enabled_ = context.lens.present;
	if (!enabled_) {
		LOG(IPASoftAf, Info) << "No focus lens, AF disabled";
		return 0;
	}

	tuning_.dacMin = context.lens.min;
	tuning_.dacMax = context.lens.max;
	tuning_.infinityDac = tuningData["infinityDac"].get<int32_t>(tuning_.infinityDac);
	tuning_.macroDac = tuningData["macroDac"].get<int32_t>(tuning_.macroDac);
	tuning_.infinityMargin = tuningData["infinityMargin"].get<double>(tuning_.infinityMargin);
	tuning_.macroMargin = tuningData["macroMargin"].get<double>(tuning_.macroMargin);
	tuning_.coarseSteps = tuningData["coarseSteps"].get<uint32_t>(tuning_.coarseSteps);
	tuning_.fineSteps = tuningData["fineSteps"].get<uint32_t>(tuning_.fineSteps);
	tuning_.minContrast = tuningData["minContrast"].get<double>(tuning_.minContrast);
	tuning_.minPeakRatio = tuningData["minPeakRatio"].get<double>(tuning_.minPeakRatio);
	tuning_.stopRatio = tuningData["stopRatio"].get<double>(tuning_.stopRatio);
	tuning_.rescanDrop = tuningData["rescanDrop"].get<double>(tuning_.rescanDrop);
	tuning_.rescanRise = tuningData["rescanRise"].get<double>(tuning_.rescanRise);
	tuning_.rescanCount = tuningData["rescanCount"].get<uint32_t>(tuning_.rescanCount);
	tuning_.macroDioptres = tuningData["macroDioptres"].get<double>(tuning_.macroDioptres);
	tuning_.defaultDioptres = tuningData["defaultDioptres"].get<double>(tuning_.defaultDioptres);
	settleFrames_ = tuningData["settleFrames"].get<uint32_t>(settleFrames_);

	if (tuning_.macroDac <= tuning_.infinityDac || tuning_.macroDioptres <= 0) {
		LOG(IPASoftAf, Error) << "Invalid AF tuning: infinityDac " << tuning_.infinityDac
				      << " macroDac " << tuning_.macroDac;
		return -EINVAL;
	}
	search_.configure(tuning_);

	context.ctrlMap[&controls::AfMode] = ControlInfo(controls::AfModeValues,
							 ControlValue(static_cast<int32_t>(controls::AfModeManual)));
	context.ctrlMap[&controls::AfTrigger] = ControlInfo(controls::AfTriggerValues);
	context.ctrlMap[&controls::LensPosition] =
		ControlInfo(0.0f, static_cast<float>(search_.dacToDioptres(search_.rangeHigh())),
			    static_cast<float>(tuning_.defaultDioptres));

	LOG(IPASoftAf, Info) << "A6L_AF_INIT lens " << tuning_.dacMin << "-" << tuning_.dacMax
			     << " infinity " << tuning_.infinityDac << " macro " << tuning_.macroDac
			     << " scan " << search_.rangeLow() << "-" << search_.rangeHigh()
			     << " default " << search_.defaultDac();
	return 0;
}

int Af::configure(IPAContext &context, [[maybe_unused]] const IPAConfigInfo &configInfo)
{
	context.activeState.af = {};
	if (!enabled_)
		return 0;

	mode_ = controls::AfModeManual;
	triggered_ = false;
	measurements_ = 0;
	search_.configure(tuning_);
	/* known rest position at stream start */
	moveTo(context, search_.defaultDac());
	return 0;
}

void Af::moveTo(IPAContext &context, int32_t dac)
{
	dac = search_.clamp(dac);
	if (context.activeState.af.dac != dac || !context.activeState.af.valid) {
		context.activeState.af.dac = dac;
		context.activeState.af.moveRequested = true;
		context.activeState.af.valid = true;
	}
}

void Af::queueRequest(IPAContext &context, [[maybe_unused]] const uint32_t frame,
		      [[maybe_unused]] IPAFrameContext &frameContext,
		      const ControlList &controls)
{
	if (!enabled_)
		return;

	const auto &mode = controls.get(controls::AfMode);
	if (mode && *mode != mode_) {
		mode_ = *mode;
		triggered_ = false;
		LOG(IPASoftAf, Debug) << "AfMode " << mode_;
		if (mode_ == controls::AfModeContinuous) {
			search_.start();
			moveTo(context, search_.target());
		} else if (mode_ == controls::AfModeManual) {
			search_.hold(context.activeState.af.dac);
		}
		/*
		 * Auto: a running scan finishes and the focused/failed state is kept (the Android HAL
		 * switches Continuous -> Auto to lock after AF_TRIGGER_START).
		 */
	}

	const auto &lensPosition = controls.get(controls::LensPosition);
	if (lensPosition && mode_ == controls::AfModeManual) {
		int32_t dac = search_.dioptresToDac(*lensPosition);
		search_.hold(dac);
		moveTo(context, dac);
	}

	const auto &trigger = controls.get(controls::AfTrigger);
	if (trigger && mode_ == controls::AfModeAuto) {
		if (*trigger == controls::AfTriggerStart) {
			triggered_ = true;
			search_.start();
			moveTo(context, search_.target());
		} else if (*trigger == controls::AfTriggerCancel) {
			triggered_ = false;
			search_.cancel();
		}
	}
}

double Af::normalisedSharpness(const SwIspStats *stats, unsigned int blackLevel)
{
	if (!stats->sharpnessCount)
		return -1.0;

	uint64_t samples = 0;
	for (uint32_t v : stats->yHistogram)
		samples += v;
	if (!samples)
		return 0.0;

	double meanG = static_cast<double>(stats->sum_.g()) / samples - blackLevel;
	meanG = std::max(meanG, 4.0);
	double msd = static_cast<double>(stats->sharpness) / stats->sharpnessCount;
	return msd / (meanG * meanG);
}

int32_t Af::afStateControl() const
{
	switch (search_.state()) {
	case a6l_af::State::Coarse:
	case a6l_af::State::Fine:
		return controls::AfStateScanning;
	case a6l_af::State::Focused:
		return controls::AfStateFocused;
	case a6l_af::State::Failed:
		return controls::AfStateFailed;
	case a6l_af::State::Idle:
	default:
		return controls::AfStateIdle;
	}
}

void Af::process(IPAContext &context, [[maybe_unused]] const uint32_t frame,
		 [[maybe_unused]] IPAFrameContext &frameContext,
		 const SwIspStats *stats, ControlList &metadata)
{
	if (!enabled_)
		return;

	const auto &af = context.activeState.af;
	const bool settling = af.moveEmitted && frame < af.moveFrame + settleFrames_;
	if (stats->valid && settling) {
		LOG(IPASoftAf, Debug) << "A6L_AF skip frame " << frame << " (lens moved at " << af.moveFrame << ")";
	} else if (stats->valid) {
		double sharp = normalisedSharpness(stats, context.activeState.blc.level);
		if (sharp < 0) {
			if (!warnedNoStats_)
				LOG(IPASoftAf, Warning) << "No sharpness statistic for this format, AF manual only";
			warnedNoStats_ = true;
			if (search_.scanning()) {
				search_.hold(context.activeState.af.dac);
			}
		} else {
			/*
			 * The lens was moved while processing the previous valid stats (4 frames ago): this
			 * measurement belongs to search_.target() == activeState.af.dac.
			 */
			const bool wasScanning = search_.scanning();
			bool moved = false;
			if (wasScanning)
				moved = search_.measure(sharp);
			else if (mode_ == controls::AfModeContinuous)
				moved = search_.monitor(sharp);

			measurements_++;
			LOG(IPASoftAf, Debug) << "A6L_AF pos=" << context.activeState.af.dac
					      << " sharp=" << sharp
					      << " state=" << a6l_af::stateName(search_.state());

			if (wasScanning && !search_.scanning())
				LOG(IPASoftAf, Info)
					<< "A6L_AF_RESULT state=" << a6l_af::stateName(search_.state())
					<< " dac=" << search_.target()
					<< " dioptres=" << search_.dacToDioptres(search_.target())
					<< " peak=" << search_.focusedSharpness()
					<< " coarse=" << search_.coarseSamples().size()
					<< " fine=" << search_.fineSamples().size();
			if (moved)
				moveTo(context, search_.target());
		}
	}

	metadata.set(controls::AfState, afStateControl());
	metadata.set(controls::LensPosition,
		     static_cast<float>(search_.dacToDioptres(context.activeState.af.dac)));
}

REGISTER_IPA_ALGORITHM(Af, "Af")

} /* namespace ipa::soft::algorithms */

} /* namespace libcamera */
