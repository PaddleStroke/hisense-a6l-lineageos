/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Hisense A6L: contrast-detect autofocus search for the GT9769 VCM (pure logic, no libcamera dependency,
 * host-testable: device/hisense/a6l/camera/af/tests/test_af_search.cpp).
 *
 * The lens is driven in VCM DAC codes (V4L2_CID_FOCUS_ABSOLUTE, 0..1023). The search range comes from the stock
 * calibration: infinity/macro DAC (per-unit OTP in the imx576 module EEPROM, 0x708 flag / 0x709 macro BE16 /
 * 0x70b infinity BE16 / 0x720 checksum) widened by the stock margins (libmmcamera_imx576_hmct_eeprom.so:
 * infinity -0.25, macro +0.05 of the infinity..macro span). Without OTP data the stock actuator tuning
 * (libactuator_gt9769.so: initial_code 227, 400 steps x 1 code) gives 227..627.
 *
 * Search = coarse sweep from infinity to macro (early stop once the curve has clearly passed its peak), then a
 * fine sweep of +-1 coarse step around the best coarse position, then a parabolic fit on the best fine sample and
 * its neighbours. One measurement per call to measure(); the caller moves the lens to target() after each call and
 * must only feed measurements taken after the lens settled at the previous target.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <stdint.h>
#include <vector>

namespace a6l_af {

struct Tuning {
	int32_t dacMin = 0;
	int32_t dacMax = 1023;
	int32_t infinityDac = 227;
	int32_t macroDac = 627;
	double infinityMargin = -0.25;
	double macroMargin = 0.05;
	unsigned int coarseSteps = 12;	/* coarse intervals over the widened range */
	unsigned int fineSteps = 4;	/* fine samples per coarse step, each side */
	double minContrast = 1e-4;	/* normalised sharpness below this = no texture */
	double minPeakRatio = 1.10;	/* peak / worst coarse sample */
	double stopRatio = 0.70;	/* coarse early stop: 2 samples below stopRatio * peak */
	double rescanDrop = 0.65;	/* continuous: rescan below rescanDrop * focused sharpness */
	double rescanRise = 1.60;	/* ... or above rescanRise * focused sharpness (scene change) */
	unsigned int rescanCount = 3;	/* consecutive measurements before a rescan */
	double macroDioptres = 10.0;	/* dioptres at macroDac (stock OTP macro is measured at 10 cm) */
	double defaultDioptres = 0.2;	/* hyperfocal-ish rest position (f 3.95 mm, f/1.8) */
};

enum class State { Idle, Coarse, Fine, Focused, Failed };

inline const char *stateName(State s)
{
	switch (s) {
	case State::Idle: return "idle";
	case State::Coarse: return "coarse";
	case State::Fine: return "fine";
	case State::Focused: return "focused";
	case State::Failed: return "failed";
	}
	return "?";
}

class Search
{
public:
	Search() { configure(Tuning()); }

	void configure(const Tuning &t)
	{
		t_ = t;
		if (t_.dacMax < t_.dacMin)
			std::swap(t_.dacMin, t_.dacMax);
		if (t_.macroDac <= t_.infinityDac)
			t_.macroDac = t_.infinityDac + 1;
		if (t_.coarseSteps < 2)
			t_.coarseSteps = 2;
		if (t_.fineSteps < 1)
			t_.fineSteps = 1;
		const double span = t_.macroDac - t_.infinityDac;
		lo_ = clamp(static_cast<int32_t>(std::lround(t_.infinityDac + t_.infinityMargin * span)));
		hi_ = clamp(static_cast<int32_t>(std::lround(t_.macroDac + t_.macroMargin * span)));
		if (hi_ <= lo_)
			hi_ = std::min(t_.dacMax, lo_ + 1);
		state_ = State::Idle;
		target_ = defaultDac();
		focusedSharpness_ = 0.0;
		deviations_ = 0;
	}

	const Tuning &tuning() const { return t_; }
	int32_t rangeLow() const { return lo_; }
	int32_t rangeHigh() const { return hi_; }
	State state() const { return state_; }
	int32_t target() const { return target_; }
	bool scanning() const { return state_ == State::Coarse || state_ == State::Fine; }
	double focusedSharpness() const { return focusedSharpness_; }

	int32_t clamp(int32_t dac) const { return std::clamp(dac, t_.dacMin, t_.dacMax); }

	double dacToDioptres(int32_t dac) const
	{
		double d = (dac - t_.infinityDac) * t_.macroDioptres / (t_.macroDac - t_.infinityDac);
		return std::max(0.0, d);
	}

	int32_t dioptresToDac(double dioptres) const
	{
		double dac = t_.infinityDac + std::max(0.0, dioptres) * (t_.macroDac - t_.infinityDac) / t_.macroDioptres;
		return clamp(static_cast<int32_t>(std::lround(dac)));
	}

	double maxDioptres() const { return dacToDioptres(t_.dacMax); }
	int32_t defaultDac() const { return dioptresToDac(t_.defaultDioptres); }

	/* Start a scan; the lens must be moved to target() before the first measurement. */
	void start()
	{
		coarse_.clear();
		fine_.clear();
		coarseStep_ = std::max<int32_t>(1, (hi_ - lo_ + static_cast<int32_t>(t_.coarseSteps) - 1) /
						       static_cast<int32_t>(t_.coarseSteps));
		state_ = State::Coarse;
		target_ = lo_;
		deviations_ = 0;
	}

	/* Cancel a scan: keep the lens where it is, back to idle. */
	void cancel()
	{
		if (scanning())
			state_ = State::Idle;
	}

	/* Force idle at a given position (manual mode). */
	void hold(int32_t dac)
	{
		state_ = State::Idle;
		target_ = clamp(dac);
	}

	/*
	 * Feed the sharpness measured at target(). Returns true when target() changed (the caller moves the lens).
	 * Ignored outside a scan.
	 */
	bool measure(double sharpness)
	{
		if (!std::isfinite(sharpness) || sharpness < 0)
			sharpness = 0;

		if (state_ == State::Coarse) {
			coarse_.push_back({ target_, sharpness });
			if (target_ < hi_ && !coarsePassedPeak()) {
				target_ = std::min(hi_, target_ + coarseStep_);
				return true;
			}
			return startFine();
		}

		if (state_ == State::Fine) {
			fine_.push_back({ target_, sharpness });
			if (fineIndex_ + 1 < finePositions_.size()) {
				target_ = finePositions_[++fineIndex_];
				return true;
			}
			return finish();
		}

		return false;
	}

	/*
	 * Continuous AF: feed sharpness while focused. Returns true when a rescan was started (target() = scan start).
	 */
	bool monitor(double sharpness)
	{
		if (state_ != State::Focused && state_ != State::Failed)
			return false;
		const double ref = state_ == State::Focused ? focusedSharpness_ : failedSharpness_;
		bool deviates = ref <= 0 ? sharpness > t_.minContrast
					 : (sharpness < ref * t_.rescanDrop || sharpness > ref * t_.rescanRise);
		deviations_ = deviates ? deviations_ + 1 : 0;
		if (deviations_ < t_.rescanCount)
			return false;
		start();
		return true;
	}

	struct Sample {
		int32_t dac;
		double sharpness;
	};
	const std::vector<Sample> &coarseSamples() const { return coarse_; }
	const std::vector<Sample> &fineSamples() const { return fine_; }

private:
	bool coarsePassedPeak() const
	{
		if (coarse_.size() < 4)
			return false;
		double peak = 0;
		size_t peakIdx = 0;
		for (size_t i = 0; i < coarse_.size(); i++)
			if (coarse_[i].sharpness > peak) {
				peak = coarse_[i].sharpness;
				peakIdx = i;
			}
		if (peak < t_.minContrast || peakIdx + 2 >= coarse_.size())
			return false;
		const size_t n = coarse_.size();
		return coarse_[n - 1].sharpness < t_.stopRatio * peak &&
		       coarse_[n - 2].sharpness < t_.stopRatio * peak;
	}

	bool startFine()
	{
		size_t best = 0;
		for (size_t i = 1; i < coarse_.size(); i++)
			if (coarse_[i].sharpness > coarse_[best].sharpness)
				best = i;

		double worst = coarse_[0].sharpness;
		for (const auto &s : coarse_)
			worst = std::min(worst, s.sharpness);

		const double peak = coarse_[best].sharpness;
		if (peak < t_.minContrast || peak < worst * t_.minPeakRatio)
			return fail(peak);

		const int32_t centre = coarse_[best].dac;
		const int32_t fstep = std::max<int32_t>(1, coarseStep_ / static_cast<int32_t>(t_.fineSteps));
		finePositions_.clear();
		for (int32_t p = centre - coarseStep_ + fstep; p < centre + coarseStep_; p += fstep) {
			int32_t c = std::clamp(p, lo_, hi_);
			if (finePositions_.empty() || finePositions_.back() != c)
				finePositions_.push_back(c);
		}
		fine_.clear();
		/* the coarse peak itself is re-measured in the fine pass (same direction of approach) */
		fineIndex_ = 0;
		state_ = State::Fine;
		target_ = finePositions_[0];
		return true;
	}

	bool finish()
	{
		size_t best = 0;
		for (size_t i = 1; i < fine_.size(); i++)
			if (fine_[i].sharpness > fine_[best].sharpness)
				best = i;
		const double peak = fine_[best].sharpness;
		if (peak < t_.minContrast)
			return fail(peak);

		double pos = fine_[best].dac;
		if (best > 0 && best + 1 < fine_.size()) {
			/* parabola through (x-1, a) (x, b) (x+1, c) on a uniform grid */
			const double a = fine_[best - 1].sharpness, b = peak, c = fine_[best + 1].sharpness;
			const double h = (fine_[best + 1].dac - fine_[best - 1].dac) / 2.0;
			const double den = a - 2 * b + c;
			if (den < 0) {
				double off = 0.5 * (a - c) / den * h;
				pos += std::clamp(off, -h, h);
			}
		}
		const int32_t old = target_;
		target_ = clamp(static_cast<int32_t>(std::lround(pos)));
		state_ = State::Focused;
		focusedSharpness_ = peak;
		deviations_ = 0;
		return target_ != old;
	}

	bool fail(double peak)
	{
		const int32_t old = target_;
		target_ = defaultDac();
		state_ = State::Failed;
		failedSharpness_ = peak;
		focusedSharpness_ = 0;
		deviations_ = 0;
		return target_ != old;
	}

	Tuning t_;
	int32_t lo_ = 0, hi_ = 0;
	int32_t coarseStep_ = 1;
	State state_ = State::Idle;
	int32_t target_ = 0;
	double focusedSharpness_ = 0;
	double failedSharpness_ = 0;
	unsigned int deviations_ = 0;
	std::vector<Sample> coarse_, fine_;
	std::vector<int32_t> finePositions_;
	size_t fineIndex_ = 0;
};

} /* namespace a6l_af */
