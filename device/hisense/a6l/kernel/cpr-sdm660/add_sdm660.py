#!/usr/bin/env python3
"""Add SDM660 CPRh + OSM data to a tree that already carries SoMainline topic/cpr3hh.
Numbers: stock A6L DT (cprh-ctrl@179c8000/179c4000) + stock kernel fuse tables."""
import sys, os
K = sys.argv[1]
cpr3 = os.path.join(K, "drivers/pmdomain/qcom/cpr3.c")
s = open(cpr3).read()
if "sdm660_cpr_desc" not in s:
    fc = lambda ref, mx, mn, rng, cl, ol, mvs, mqs: f"""		{{
			.ref_uV = {ref}, .max_uV = {mx}, .min_uV = {mn}, .range_uV = {rng},
			.volt_cloop_adjust = {cl}, .volt_oloop_adjust = {ol},
			.max_volt_scale = {mvs}, .max_quot_scale = {mqs},
			.quot_offset = 0, .quot_scale = 1, .quot_adjust = 0,
			.quot_offset_scale = 5, .quot_offset_adjust = 0,
		}},
"""
    # max_volt_scale/max_quot_scale: NOT in stock DT/ELF symbols -> sdm630 gold values (placeholders, closed-loop only)
    scales = [(10, 300), (320, 275), (350, 800), (868, 980), (868, 980)]
    pwr = [(644000, 724000, 588000, 32000, -32000, -4000),
           (724000, 724000, 596000, 32000, -30000, 4000),
           (788000, 788000, 652000, 40000, -29000, 7000),
           (868000, 868000, 712000, 44000, -23000, 19000),
           (1068000, 1068000, 844000, 40000, -21000, -8000)]
    perf = [(724000, 724000, 596000, 40000, -22000, 16000),
            (788000, 788000, 652000, 40000, -9000, 27000),
            (868000, 868000, 712000, 40000, -7000, 39000),
            (988000, 988000, 784000, 66000, -2000, 39000),
            (1068000, 1068000, 844000, 40000, 11000, 20000)]
    def thread(name, ctrl, ro, rows):
        body = "".join(fc(*r, *scales[i]) for i, r in enumerate(rows))
        return f"""
static const int {name}_scaling_factor[][CPR3_RO_COUNT] = {{
	{{ {", ".join(map(str, ro))} }}
}};

static const struct cpr_thread_desc {name} = {{
	.controller_id = {ctrl},
	.hw_tid = 0,
	.ro_scaling_factor = {name}_scaling_factor,
	.ro_scaling_factor_common = true,
	.sensor_range_start = 0,
	.sensor_range_end = 6,	/* UNVERIFIED for SDM660 (sdm630 value) */
	.init_voltage_step = 10000,
	.init_voltage_width = 6,
	.step_quot_init_min = 12,
	.step_quot_init_max = 14,
	.num_fuse_corners = 5,
	.fuse_corner_data = (struct fuse_corner_data[]){{
{body}	}},
}};
"""
    add = "\n/* ---- SDM660 (Hisense A6L), from stock DT + stock kernel fuse tables ---- */"
    add += thread("sdm660_thread_pwrcl", 0, [3600,3600,3830,2430,2520,2700,1790,1760,1970,1880,2110,2010,2510,4900,4370,4780], pwr)
    add += thread("sdm660_thread_perfcl", 1, [4040,4230,0,2210,2560,2450,2230,2220,2410,2300,2560,2470,1600,3120,2620,2280], perf)
    add += """
static const struct cpr_desc sdm660_cpr_desc = {
	.cpr_type = CTRL_TYPE_CPRH,
	.num_threads = 2,
	.apm_threshold = 872000,
	.apm_crossover = 872000,
	.apm_hysteresis = 20000,
	.cpr_base_voltage = 400000,
	.cpr_max_voltage = 1300000,
	.timer_delay_us = 5000,
	.timer_cons_up = 0,
	.timer_cons_down = 2,
	.up_threshold = 2,
	.down_threshold = 2,
	.idle_clocks = 15,
	.count_mode = CPR3_CPR_CTL_COUNT_MODE_ALL_AT_ONCE_MIN,
	.count_repeat = 14,
	.gcnt_us = 1,
	.vreg_step_fixed = 4000,
	.vreg_step_up_limit = 1,
	.vreg_step_down_limit = 1,
	.vdd_settle_time_us = 34,
	.corner_settle_time_us = 5,
	.reduce_to_corner_uV = true,
	/* OPEN-LOOP first: closed loop stays off until the open-loop step is validated */
	.hw_closed_loop_en = false,
	.threads = (const struct cpr_thread_desc *[]) {
		&sdm660_thread_pwrcl,
		&sdm660_thread_perfcl,
	},
};

static const struct cpr_acc_desc sdm660_cpr_acc_desc = {
	.cpr_desc = &sdm660_cpr_desc,
};
"""
    anchor = "static int cpr_power_off(struct generic_pm_domain *domain)"
    assert s.count(anchor) == 1
    s = s.replace(anchor, add + "\n" + anchor)
    m = '\t{ .compatible = "qcom,sdm630-cprh", .data = &sdm630_cpr_acc_desc },'
    assert s.count(m) == 1
    s = s.replace(m, m + '\n\t{ .compatible = "qcom,sdm660-cprh", .data = &sdm660_cpr_acc_desc },')
    open(cpr3, "w").write(s)
    print("cpr3.c: sdm660 added")

cf = os.path.join(K, "drivers/cpufreq/qcom-cpufreq-hw.c")
s = open(cf).read()
if "sdm660_soc_data" not in s:
    i = s.index("static const struct qcom_cpufreq_soc_data msm8998_soc_data = {")
    j = s.index("\t.acd_data = {", i)
    k = s.index("\n};\n", j)
    block = s[i:j].replace("msm8998_soc_data", "sdm660_soc_data") + "};\n"
    block = "/* SDM660: OSM v1 like MSM8998 (stock clk-cpu-osm programs the same LUT format); no ACD in stock DT */\n" + block
    s = s[:k + 4] + "\n" + block + s[k + 4:]
    m = '\t{ .compatible = "qcom,msm8998-cpufreq-hw", .data = &msm8998_soc_data },'
    assert s.count(m) == 1
    s = s.replace(m, m + '\n\t{ .compatible = "qcom,sdm660-cpufreq-hw", .data = &sdm660_soc_data },')
    open(cf, "w").write(s)
    print("qcom-cpufreq-hw.c: sdm660 added")
