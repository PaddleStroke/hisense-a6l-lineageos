// SPDX-License-Identifier: LGPL-2.1-or-later
// Host test of the A6L contrast AF search (af/libcamera/src/af_search.h) against simulated focus curves.
// Build: g++ -std=c++20 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -I../libcamera/src test_af_search.cpp
#include "af_search.h"

#include <cmath>
#include <cstdio>
#include <random>

using namespace a6l_af;

static int fails = 0, checks = 0;
#define CHECK(c, ...)                                                   \
	do {                                                            \
		checks++;                                               \
		if (!(c)) {                                             \
			fails++;                                        \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
			printf(__VA_ARGS__);                            \
			printf("\n");                                   \
		}                                                       \
	} while (0)

/* normalised sharpness of a scene focused at dac `peak`: Lorentzian-ish curve + floor + noise */
struct Scene {
	double peak, width, height, floor, noise;
	std::mt19937 rng{ 1234 };
	double at(int32_t dac)
	{
		double d = (dac - peak) / width;
		double v = floor + height / (1.0 + d * d);
		std::normal_distribution<double> n(0.0, noise * v);
		return std::max(0.0, v + n(rng));
	}
};

/* run a scan to completion; returns the number of measurements */
static int runScan(Search &s, Scene &sc, int maxMeas = 200)
{
	int n = 0;
	s.start();
	while (s.scanning() && n < maxMeas) {
		s.measure(sc.at(s.target()));
		n++;
	}
	return n;
}

int main()
{
	Tuning t;
	Search s;
	s.configure(t);

	/* range from the stock margins: 227 - 0.25*400 = 127 .. 627 + 0.05*400 = 647 */
	CHECK(s.rangeLow() == 127 && s.rangeHigh() == 647, "range %d-%d", s.rangeLow(), s.rangeHigh());
	CHECK(s.dioptresToDac(0) == 227 && s.dioptresToDac(10) == 627, "dioptre mapping");
	CHECK(std::fabs(s.dacToDioptres(427) - 5.0) < 1e-9, "dacToDioptres");
	CHECK(s.dacToDioptres(100) == 0.0, "below infinity clamps to 0 dioptres");
	CHECK(s.defaultDac() == 235, "default dac %d", s.defaultDac());
	CHECK(s.state() == State::Idle && s.target() == s.defaultDac(), "idle at default");
	CHECK(std::fabs(s.maxDioptres() - (1023 - 227) * 10.0 / 400) < 1e-9, "max dioptres");

	/* sweep of focus positions across the range, low noise: final within 1/2 fine step */
	int worstErr = 0, maxMeas = 0;
	for (int p = 140; p <= 640; p += 25) {
		Scene sc{ double(p), 60, 0.02, 0.002, 0.01 };
		int n = runScan(s, sc);
		int err = std::abs(s.target() - p);
		worstErr = std::max(worstErr, err);
		maxMeas = std::max(maxMeas, n);
		CHECK(s.state() == State::Focused, "peak %d: state %s", p, stateName(s.state()));
		CHECK(err <= 8, "peak %d: target %d err %d", p, s.target(), err);
	}
	printf("sweep: worst error %d DAC, max measurements %d\n", worstErr, maxMeas);
	CHECK(maxMeas <= 13 + 7, "measurements %d", maxMeas);

	/* early stop: near-infinity subject must not sweep the whole range */
	{
		Scene sc{ 180, 40, 0.02, 0.001, 0.0 };
		s.start();
		int coarse = 0;
		while (s.state() == State::Coarse) {
			s.measure(sc.at(s.target()));
			coarse++;
		}
		CHECK(coarse < 8, "early stop after %d coarse samples", coarse);
		while (s.scanning())
			s.measure(sc.at(s.target()));
		CHECK(std::abs(s.target() - 180) <= 6, "early stop target %d", s.target());
	}

	/* noisy (low light) curve: still focused, within a coarse step */
	{
		Scene sc{ 400, 60, 0.01, 0.003, 0.08 };
		runScan(s, sc);
		CHECK(s.state() == State::Focused && std::abs(s.target() - 400) <= 44, "noisy: %s %d",
		      stateName(s.state()), s.target());
	}

	/* flat scene (no texture): failed, lens back to the default position */
	{
		Scene sc{ 400, 60, 0.0, 0.00005, 0.0 };
		runScan(s, sc);
		CHECK(s.state() == State::Failed && s.target() == s.defaultDac(), "flat: %s %d", stateName(s.state()),
		      s.target());
	}
	/* textured but no peak (monotonic, < 10 % variation): failed */
	{
		Tuning t2 = t;
		Search s2;
		s2.configure(t2);
		s2.start();
		int i = 0;
		while (s2.scanning())
			s2.measure(0.01 + 0.00001 * i++);
		CHECK(s2.state() == State::Failed, "monotonic flat-ish: %s", stateName(s2.state()));
	}

	/* peak at the macro end of the range */
	{
		Scene sc{ 647, 50, 0.02, 0.002, 0.0 };
		runScan(s, sc);
		CHECK(s.state() == State::Focused && std::abs(s.target() - 647) <= 8, "macro end %d", s.target());
	}

	/* continuous: no rescan while stable, rescan after 3 low measurements, and on a strong rise */
	{
		Scene sc{ 300, 60, 0.02, 0.002, 0.0 };
		runScan(s, sc);
		double f = s.focusedSharpness();
		CHECK(!s.monitor(f * 0.95) && !s.monitor(f * 1.02) && !s.monitor(f), "stable: no rescan");
		CHECK(!s.monitor(f * 0.5) && !s.monitor(f * 0.5), "2 drops: no rescan yet");
		CHECK(!s.monitor(f), "recovered resets the count");
		CHECK(!s.monitor(f * 0.5) && !s.monitor(f * 0.5) && s.monitor(f * 0.5), "3 drops: rescan");
		CHECK(s.state() == State::Coarse && s.target() == s.rangeLow(), "rescan starts at infinity");
		sc.peak = 500;
		while (s.scanning())
			s.measure(sc.at(s.target()));
		CHECK(std::abs(s.target() - 500) <= 8, "refocus %d", s.target());
		f = s.focusedSharpness();
		CHECK(!s.monitor(f * 2) && !s.monitor(f * 2) && s.monitor(f * 2), "rise: rescan");
	}

	/* cancel keeps the lens, hold() moves to manual position, measure() ignored when idle */
	{
		s.start();
		s.measure(0.01);
		int32_t at = s.target();
		s.cancel();
		CHECK(s.state() == State::Idle && s.target() == at, "cancel");
		CHECK(!s.measure(0.5) && s.target() == at, "measure ignored when idle");
		s.hold(5000);
		CHECK(s.target() == 1023, "hold clamps");
		CHECK(!s.monitor(1.0), "monitor ignored when idle");
	}

	/* tuning sanitising: macro <= infinity, tiny step counts, NaN input */
	{
		Tuning bad;
		bad.infinityDac = 500;
		bad.macroDac = 400;
		bad.coarseSteps = 0;
		bad.fineSteps = 0;
		Search s3;
		s3.configure(bad);
		CHECK(s3.tuning().macroDac == 501 && s3.tuning().coarseSteps == 2 && s3.tuning().fineSteps == 1,
		      "sanitised");
		s3.start();
		int n = 0;
		while (s3.scanning() && n < 1000) {
			s3.measure(NAN);
			n++;
		}
		CHECK(!s3.scanning() && n < 1000, "terminates on NaN input (%d)", n);
	}

	/* narrow lens range (dacMax below the widened macro) */
	{
		Tuning nr = t;
		nr.dacMax = 500;
		Search s4;
		s4.configure(nr);
		CHECK(s4.rangeHigh() == 500, "clamped range high %d", s4.rangeHigh());
		Scene sc{ 450, 50, 0.02, 0.002, 0.0 };
		runScan(s4, sc);
		CHECK(s4.state() == State::Focused && std::abs(s4.target() - 450) <= 8, "narrow %d", s4.target());
	}

	printf("%s %d/%d checks\n", fails ? "AF_SEARCH_FAIL" : "AF_SEARCH_PASS", checks - fails, checks);
	return fails ? 1 : 0;
}
