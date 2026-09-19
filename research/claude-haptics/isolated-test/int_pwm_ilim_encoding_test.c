/*
 * Isolated, hardware-free unit test for the candidate PM660 drive-config
 * encoding added by 0001-spmi-haptics-pm660-int-pwm-ilim.patch.
 *
 * It replicates ONLY the pure integer encoding the candidate patch performs:
 *   - internal PWM frequency (kHz) -> HAP_INT_PWM_REG / HAP_PWM_CAP_REG field
 *   - current limit (mA)          -> HAP_ILIM_CFG_REG field
 * and checks them against the A6L stock qpnp-haptic device-tree values.
 *
 * This does NOT touch hardware, a regmap, or a real register. It exists to
 * prove the field math the patch relies on is correct and bounded. Perceptible
 * vibration still requires physical confirmation on the phone.
 *
 * Build/run:  cc -Wall -Wextra -O2 -o t int_pwm_ilim_encoding_test.c && ./t
 */
#include <stdio.h>
#include <stdint.h>

/* --- constants mirrored from the candidate driver / stock binding --- */
#define HAP_INT_PWM_FREQ_253_KHZ 253
#define HAP_INT_PWM_FREQ_505_KHZ 505
#define HAP_INT_PWM_FREQ_739_KHZ 739
#define HAP_INT_PWM_FREQ_1076_KHZ 1076

#define HAP_ILIM_400_MA 0
#define HAP_ILIM_800_MA 1

/* Same table the downstream qpnp-haptic driver uses: nearest-supported freq
 * maps to a 2-bit selector written to both INT_PWM (0x56) and PWM_CAP (0x58). */
static int int_pwm_sel(uint32_t khz, uint8_t *sel)
{
    switch (khz) {
    case HAP_INT_PWM_FREQ_253_KHZ:  *sel = 0; return 0;
    case HAP_INT_PWM_FREQ_505_KHZ:  *sel = 1; return 0;
    case HAP_INT_PWM_FREQ_739_KHZ:  *sel = 2; return 0;
    case HAP_INT_PWM_FREQ_1076_KHZ: *sel = 3; return 0;
    default: return -1; /* reject unsupported values, do not silently coerce */
    }
}

/* ilim_ma -> selector, matching downstream (ilim/400 - 1), bounded to {0,1}. */
static int ilim_sel(uint32_t ma, uint8_t *sel)
{
    if (ma != 400 && ma != 800) return -1;
    *sel = (uint8_t)(ma / 400 - 1);
    return 0;
}

int main(void)
{
    int fails = 0;
    uint8_t s;

    /* Stock A6L: qcom,int-pwm-freq-khz = 0x1f9 = 505 kHz -> selector 1 */
    if (int_pwm_sel(505, &s) || s != 1) { printf("FAIL int_pwm 505->%u\n", s); fails++; }
    /* full table */
    if (int_pwm_sel(253, &s) || s != 0) { printf("FAIL int_pwm 253\n"); fails++; }
    if (int_pwm_sel(739, &s) || s != 2) { printf("FAIL int_pwm 739\n"); fails++; }
    if (int_pwm_sel(1076, &s) || s != 3) { printf("FAIL int_pwm 1076\n"); fails++; }
    /* unsupported values must be rejected, never coerced */
    if (int_pwm_sel(500, &s) != -1) { printf("FAIL int_pwm bad-accept 500\n"); fails++; }
    if (int_pwm_sel(0,   &s) != -1) { printf("FAIL int_pwm bad-accept 0\n"); fails++; }

    /* Stock A6L: qcom,ilim-ma = 0x320 = 800 mA -> selector 1 (HAP_ILIM_800_MA) */
    if (ilim_sel(800, &s) || s != HAP_ILIM_800_MA) { printf("FAIL ilim 800->%u\n", s); fails++; }
    if (ilim_sel(400, &s) || s != HAP_ILIM_400_MA) { printf("FAIL ilim 400\n"); fails++; }
    if (ilim_sel(600, &s) != -1) { printf("FAIL ilim bad-accept 600\n"); fails++; }

    if (fails) { printf("A6L_INT_PWM_ILIM_TEST_FAIL fails=%d\n", fails); return 1; }
    printf("A6L_INT_PWM_ILIM_TEST_PASS cases=9 stock_int_pwm_sel=1 stock_ilim_sel=1\n");
    return 0;
}
