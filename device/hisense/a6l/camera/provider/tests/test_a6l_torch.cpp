// A6L (lc2, 29 Sep 2026): host test of the A6lTorch level <-> LED brightness mapping.
// build: clang++ -std=c++20 -DA6L_TORCH_HOST_TEST -I tests/stub -I device tests/test_a6l_torch.cpp device/A6lTorch.cpp
#include "A6lTorch.h"

#include <cstdio>

using android::hardware::camera::device::implementation::A6lTorch;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// leds-qcom-flash: current_ma = brightness * max_torch_current_ma / LED_FULL; register code = current_ua / 5000 (torch IRES)
static int kernelCurrentUa(int b, int maxUa) { return (b * (maxUa / 1000) / 255) * 1000; }

int main() {
    CHECK(A6lTorch::levelsForMaxUa(500000) == 100);
    CHECK(A6lTorch::levelsForMaxUa(100000) == 20);
    CHECK(A6lTorch::levelsForMaxUa(900000) == 100);  // kernel clamps at 500 mA
    CHECK(A6lTorch::levelsForMaxUa(0) == 1);
    CHECK(A6lTorch::defaultLevelFor(100) == 20);
    CHECK(A6lTorch::defaultLevelFor(20) == 20);
    CHECK(A6lTorch::defaultLevelFor(10) == 10);
    CHECK(A6lTorch::brightnessForLevel(0, 500000, 255) == 0);
    CHECK(A6lTorch::brightnessForLevel(20, 500000, 255) == 51);
    CHECK(A6lTorch::brightnessForLevel(100, 500000, 255) == 255);
    CHECK(A6lTorch::brightnessForLevel(20, 100000, 255) == 255);
    for (int maxUa : {100000, 200000, 300000, 500000}) {
        int prev = 0;
        for (int l = 1; l <= A6lTorch::levelsForMaxUa(maxUa); l++) {
            int b = A6lTorch::brightnessForLevel(l, maxUa, 255);
            int ua = kernelCurrentUa(b, maxUa);
            // every level lands on its own 5 mA step (never below nominal, never a full step above) and is monotonic
            CHECK(b >= 1 && b <= 255);
            CHECK(ua / 5000 == l);
            CHECK(b > prev);
            prev = b;
        }
    }
    printf("%s (%d failures)\n", fails ? "A6L_TORCH_TEST FAIL" : "A6L_TORCH_TEST PASS", fails);
    return fails ? 1 : 0;
}
