/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Hisense A6L: contrast-detect autofocus for the simple IPA (SoftISP sharpness statistic + VCM lens).
 */

#pragma once

#include "af_search.h"
#include "algorithm.h"

namespace libcamera {

namespace ipa::soft::algorithms {

class Af : public Algorithm
{
public:
	Af() = default;
	~Af() = default;

	int init(IPAContext &context, const ValueNode &tuningData) override;
	int configure(IPAContext &context, const IPAConfigInfo &configInfo) override;
	void queueRequest(IPAContext &context, const uint32_t frame,
			  IPAFrameContext &frameContext,
			  const ControlList &controls) override;
	void process(IPAContext &context, const uint32_t frame,
		     IPAFrameContext &frameContext,
		     const SwIspStats *stats,
		     ControlList &metadata) override;

	/* normalised focus measure: mean squared green difference / (mean green - black)^2 */
	static double normalisedSharpness(const SwIspStats *stats, unsigned int blackLevel);

private:
	void moveTo(IPAContext &context, int32_t dac);
	int32_t afStateControl() const;

	a6l_af::Tuning tuning_;
	a6l_af::Search search_;
	bool enabled_ = false;
	int32_t mode_ = 0;		/* controls::AfModeManual */
	bool triggered_ = false;	/* Auto mode: a scan was triggered since the last AfModeAuto/Cancel */
	unsigned int measurements_ = 0;
	uint32_t settleFrames_ = 3;
	bool warnedNoStats_ = false;
};

} /* namespace ipa::soft::algorithms */

} /* namespace libcamera */
