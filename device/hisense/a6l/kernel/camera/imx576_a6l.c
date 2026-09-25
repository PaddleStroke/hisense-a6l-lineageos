// SPDX-License-Identifier: GPL-2.0
/*
 * Sony IMX576 camera sensor driver for the Hisense A6L (SDM660), mainline V4L2.
 *
 * Register tables were extracted from the stock Hisense sensor library
 * (vendor/lib/libmmcamera_imx576_hmct.so, sensor_lib_t init/res arrays) and are
 * replayed verbatim. Controls follow the MIPI CCS-like register map
 * (0x0100 mode select, 0x0202 coarse integration, 0x0204 analogue gain,
 * 0x0340 frame length, 0x0342 line length).
 * Structure modelled on drivers/media/i2c/s5kjn1.c (Linaro).
 *
 * A6L port, 2026. UNTESTED ON HARDWARE.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/units.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#define SNS_MCLK_FREQ			(24 * HZ_PER_MHZ)
#define SNS_DATA_LANES			4

#define SNS_REG_CHIP_ID			CCI_REG16(0x0016)
#define SNS_CHIP_ID			0x0576

#define SNS_REG_CTRL_MODE		CCI_REG8(0x0100)
#define SNS_MODE_STREAMING		BIT(0)
#define SNS_REG_ORIENTATION		CCI_REG8(0x0101)
#define SNS_VFLIP			BIT(1)
#define SNS_HFLIP			BIT(0)
#define SNS_REG_EXPOSURE		CCI_REG16(0x0202)
#define SNS_EXPOSURE_MIN		8
#define SNS_REG_AGAIN			CCI_REG16(0x0204)
#define SNS_AGAIN_MIN			0
#define SNS_AGAIN_MAX			960
#define SNS_AGAIN_DEFAULT		0
#define SNS_REG_VTS			CCI_REG16(0x0340)
#define SNS_VTS_MAX			0xffff
#define SNS_REG_TEST_PATTERN		CCI_REG16(0x0600)

struct sns_seg {
	const struct cci_reg_sequence *regs;
	unsigned int num;
	unsigned int delay_us;		/* sleep after this segment */
};

struct sns_mode {
	u32 width, height, hts, vts, exposure, exposure_margin;
	u32 link_freq_index;
	u64 pixel_rate;			/* VT pixel rate = hts * vts * fps */
	const struct sns_seg *segs;
	unsigned int num_segs;
};

static const struct cci_reg_sequence sns_init_regs[] = {
	{ CCI_REG8(0x0136), 0x18 },
	{ CCI_REG8(0x0137), 0x00 },
	{ CCI_REG8(0x3c7e), 0x01 },
	{ CCI_REG8(0x3c7f), 0x02 },
	{ CCI_REG8(0xae09), 0x04 },
	{ CCI_REG8(0xae0a), 0x16 },
	{ CCI_REG8(0xaf05), 0x18 },
	{ CCI_REG8(0x380c), 0x00 },
	{ CCI_REG8(0x3c00), 0x10 },
	{ CCI_REG8(0x3c01), 0x10 },
	{ CCI_REG8(0x3c02), 0x10 },
	{ CCI_REG8(0x3c03), 0x10 },
	{ CCI_REG8(0x3c04), 0x10 },
	{ CCI_REG8(0x3c05), 0x01 },
	{ CCI_REG8(0x3c08), 0xff },
	{ CCI_REG8(0x3c09), 0xff },
	{ CCI_REG8(0x3c0a), 0x01 },
	{ CCI_REG8(0x3c0d), 0xff },
	{ CCI_REG8(0x3c0e), 0xff },
	{ CCI_REG8(0x3c0f), 0x20 },
	{ CCI_REG8(0x3f89), 0x01 },
	{ CCI_REG8(0x4430), 0x00 },
	{ CCI_REG8(0x4b8e), 0x18 },
	{ CCI_REG8(0x4b8f), 0x10 },
	{ CCI_REG8(0x4ba8), 0x08 },
	{ CCI_REG8(0x4baa), 0x08 },
	{ CCI_REG8(0x4bab), 0x08 },
	{ CCI_REG8(0x4bc9), 0x10 },
	{ CCI_REG8(0x5511), 0x01 },
	{ CCI_REG8(0x560b), 0x5b },
	{ CCI_REG8(0x56a7), 0x60 },
	{ CCI_REG8(0x5b3b), 0x60 },
	{ CCI_REG8(0x5ba7), 0x60 },
	{ CCI_REG8(0x6002), 0x00 },
	{ CCI_REG8(0x6014), 0x01 },
	{ CCI_REG8(0x6118), 0x0a },
	{ CCI_REG8(0x6122), 0x0a },
	{ CCI_REG8(0x6128), 0x0a },
	{ CCI_REG8(0x6132), 0x0a },
	{ CCI_REG8(0x6138), 0x0a },
	{ CCI_REG8(0x6142), 0x0a },
	{ CCI_REG8(0x6148), 0x0a },
	{ CCI_REG8(0x6152), 0x0a },
	{ CCI_REG8(0x617b), 0x04 },
	{ CCI_REG8(0x617e), 0x04 },
	{ CCI_REG8(0x6187), 0x04 },
	{ CCI_REG8(0x618a), 0x04 },
	{ CCI_REG8(0x6193), 0x04 },
	{ CCI_REG8(0x6196), 0x04 },
	{ CCI_REG8(0x619f), 0x04 },
	{ CCI_REG8(0x61a2), 0x04 },
	{ CCI_REG8(0x61ab), 0x04 },
	{ CCI_REG8(0x61ae), 0x04 },
	{ CCI_REG8(0x61b7), 0x04 },
	{ CCI_REG8(0x61ba), 0x04 },
	{ CCI_REG8(0x61c3), 0x04 },
	{ CCI_REG8(0x61c6), 0x04 },
	{ CCI_REG8(0x61cf), 0x04 },
	{ CCI_REG8(0x61d2), 0x04 },
	{ CCI_REG8(0x61db), 0x04 },
	{ CCI_REG8(0x61de), 0x04 },
	{ CCI_REG8(0x61e7), 0x04 },
	{ CCI_REG8(0x61ea), 0x04 },
	{ CCI_REG8(0x61f3), 0x04 },
	{ CCI_REG8(0x61f6), 0x04 },
	{ CCI_REG8(0x61ff), 0x04 },
	{ CCI_REG8(0x6202), 0x04 },
	{ CCI_REG8(0x620b), 0x04 },
	{ CCI_REG8(0x620e), 0x04 },
	{ CCI_REG8(0x6217), 0x04 },
	{ CCI_REG8(0x621a), 0x04 },
	{ CCI_REG8(0x6223), 0x04 },
	{ CCI_REG8(0x6226), 0x04 },
	{ CCI_REG8(0x671d), 0x00 },
	{ CCI_REG8(0x6725), 0x00 },
	{ CCI_REG8(0x6738), 0x03 },
	{ CCI_REG8(0x673b), 0x01 },
	{ CCI_REG8(0x6b0b), 0x02 },
	{ CCI_REG8(0x6b0c), 0x01 },
	{ CCI_REG8(0x6b0d), 0x05 },
	{ CCI_REG8(0x6b0f), 0x04 },
	{ CCI_REG8(0x6b10), 0x02 },
	{ CCI_REG8(0x6b11), 0x06 },
	{ CCI_REG8(0x6b12), 0x03 },
	{ CCI_REG8(0x6b13), 0x07 },
	{ CCI_REG8(0x6b14), 0x0d },
	{ CCI_REG8(0x6b15), 0x09 },
	{ CCI_REG8(0x6b16), 0x0c },
	{ CCI_REG8(0x6b17), 0x08 },
	{ CCI_REG8(0x6b18), 0x0e },
	{ CCI_REG8(0x6b19), 0x0a },
	{ CCI_REG8(0x6b1a), 0x0f },
	{ CCI_REG8(0x6b1b), 0x0b },
	{ CCI_REG8(0x6b1c), 0x01 },
	{ CCI_REG8(0x6b1d), 0x05 },
	{ CCI_REG8(0x6b1f), 0x04 },
	{ CCI_REG8(0x6b20), 0x02 },
	{ CCI_REG8(0x6b21), 0x06 },
	{ CCI_REG8(0x6b22), 0x03 },
	{ CCI_REG8(0x6b23), 0x07 },
	{ CCI_REG8(0x6b24), 0x0d },
	{ CCI_REG8(0x6b25), 0x09 },
	{ CCI_REG8(0x6b26), 0x0c },
	{ CCI_REG8(0x6b27), 0x08 },
	{ CCI_REG8(0x6b28), 0x0e },
	{ CCI_REG8(0x6b29), 0x0a },
	{ CCI_REG8(0x6b2a), 0x0f },
	{ CCI_REG8(0x6b2b), 0x0b },
	{ CCI_REG8(0x746e), 0x01 },
	{ CCI_REG8(0x7501), 0x1d },
	{ CCI_REG8(0x7505), 0x3d },
	{ CCI_REG8(0x7508), 0x49 },
	{ CCI_REG8(0x7509), 0x0d },
	{ CCI_REG8(0x750a), 0x8a },
	{ CCI_REG8(0x750b), 0x0c },
	{ CCI_REG8(0x750c), 0x4d },
	{ CCI_REG8(0x750d), 0x2d },
	{ CCI_REG8(0x750e), 0x8e },
	{ CCI_REG8(0x750f), 0x2c },
	{ CCI_REG8(0x7948), 0x01 },
	{ CCI_REG8(0x7949), 0x06 },
	{ CCI_REG8(0x794b), 0x04 },
	{ CCI_REG8(0x794c), 0x04 },
	{ CCI_REG8(0x794d), 0x3a },
	{ CCI_REG8(0x7951), 0x00 },
	{ CCI_REG8(0x7952), 0x01 },
	{ CCI_REG8(0x7955), 0x00 },
	{ CCI_REG8(0x9004), 0x10 },
	{ CCI_REG8(0x9200), 0xa0 },
	{ CCI_REG8(0x9201), 0xa7 },
	{ CCI_REG8(0x9202), 0xa0 },
	{ CCI_REG8(0x9203), 0xaa },
	{ CCI_REG8(0x9204), 0xa0 },
	{ CCI_REG8(0x9205), 0xad },
	{ CCI_REG8(0x9206), 0xa0 },
	{ CCI_REG8(0x9207), 0xb0 },
	{ CCI_REG8(0x9208), 0xa0 },
	{ CCI_REG8(0x9209), 0xb3 },
	{ CCI_REG8(0x920a), 0xb7 },
	{ CCI_REG8(0x920b), 0x34 },
	{ CCI_REG8(0x920c), 0xb7 },
	{ CCI_REG8(0x920d), 0x36 },
	{ CCI_REG8(0x920e), 0xb7 },
	{ CCI_REG8(0x920f), 0x37 },
	{ CCI_REG8(0x9210), 0xb7 },
	{ CCI_REG8(0x9211), 0x38 },
	{ CCI_REG8(0x9212), 0xb7 },
	{ CCI_REG8(0x9213), 0x39 },
	{ CCI_REG8(0x9214), 0xb7 },
	{ CCI_REG8(0x9215), 0x3a },
	{ CCI_REG8(0x9216), 0xb7 },
	{ CCI_REG8(0x9217), 0x3c },
	{ CCI_REG8(0x9218), 0xb7 },
	{ CCI_REG8(0x9219), 0x3d },
	{ CCI_REG8(0x921a), 0xb7 },
	{ CCI_REG8(0x921b), 0x3e },
	{ CCI_REG8(0x921c), 0xb7 },
	{ CCI_REG8(0x921d), 0x3f },
	{ CCI_REG8(0x921e), 0x7f },
	{ CCI_REG8(0x921f), 0x77 },
	{ CCI_REG8(0x9816), 0x14 },
	{ CCI_REG8(0x9865), 0x8c },
	{ CCI_REG8(0x9866), 0x64 },
	{ CCI_REG8(0x9867), 0x50 },
	{ CCI_REG8(0x9990), 0x0b },
	{ CCI_REG8(0x9991), 0x0b },
	{ CCI_REG8(0x9992), 0x0b },
	{ CCI_REG8(0x9993), 0x0b },
	{ CCI_REG8(0x9994), 0x0b },
	{ CCI_REG8(0x9995), 0x0d },
	{ CCI_REG8(0x9996), 0x0b },
	{ CCI_REG8(0x99af), 0x0f },
	{ CCI_REG8(0x99b0), 0x0f },
	{ CCI_REG8(0x99b1), 0x0f },
	{ CCI_REG8(0x99b2), 0x0f },
	{ CCI_REG8(0x99b3), 0x0f },
	{ CCI_REG8(0x99e1), 0x0f },
	{ CCI_REG8(0x99e2), 0x0f },
	{ CCI_REG8(0x99e3), 0x0f },
	{ CCI_REG8(0x99e4), 0x0f },
	{ CCI_REG8(0x99e5), 0x0f },
	{ CCI_REG8(0x99e6), 0x0f },
	{ CCI_REG8(0x99e7), 0x0f },
	{ CCI_REG8(0x99e8), 0x0f },
	{ CCI_REG8(0x99e9), 0x0f },
	{ CCI_REG8(0x99ea), 0x0f },
	{ CCI_REG8(0xae1b), 0x04 },
	{ CCI_REG8(0xae1c), 0x03 },
	{ CCI_REG8(0xae1d), 0x03 },
	{ CCI_REG8(0xe286), 0x31 },
	{ CCI_REG8(0xe2a6), 0x32 },
	{ CCI_REG8(0xe2c6), 0x33 },
	{ CCI_REG8(0x4038), 0x00 },
	{ CCI_REG8(0x9856), 0xa0 },
	{ CCI_REG8(0x9857), 0x78 },
	{ CCI_REG8(0x9858), 0x64 },
	{ CCI_REG8(0x986e), 0x64 },
	{ CCI_REG8(0x9870), 0x3c },
	{ CCI_REG8(0x993a), 0x0e },
	{ CCI_REG8(0x993b), 0x0e },
	{ CCI_REG8(0x9953), 0x08 },
	{ CCI_REG8(0x9954), 0x08 },
	{ CCI_REG8(0x996b), 0x0f },
	{ CCI_REG8(0x996d), 0x0f },
	{ CCI_REG8(0x996f), 0x0f },
	{ CCI_REG8(0x9981), 0x00 },
	{ CCI_REG8(0x9982), 0x00 },
	{ CCI_REG8(0x9986), 0x00 },
	{ CCI_REG8(0x9987), 0x00 },
	{ CCI_REG8(0x998e), 0x0f },
	{ CCI_REG8(0xa101), 0x01 },
	{ CCI_REG8(0xa103), 0x01 },
	{ CCI_REG8(0xa105), 0x01 },
	{ CCI_REG8(0xa107), 0x01 },
	{ CCI_REG8(0xa109), 0x01 },
	{ CCI_REG8(0xa10b), 0x01 },
	{ CCI_REG8(0xa10d), 0x01 },
	{ CCI_REG8(0xa10f), 0x01 },
	{ CCI_REG8(0xa111), 0x01 },
	{ CCI_REG8(0xa113), 0x01 },
	{ CCI_REG8(0xa115), 0x01 },
	{ CCI_REG8(0xa117), 0x01 },
	{ CCI_REG8(0xa119), 0x01 },
	{ CCI_REG8(0xa11b), 0x01 },
	{ CCI_REG8(0xa11d), 0x01 },
	{ CCI_REG8(0xa21e), 0x06 },
	{ CCI_REG8(0xa21f), 0x13 },
	{ CCI_REG8(0xa220), 0x13 },
	{ CCI_REG8(0xa31e), 0x00 },
	{ CCI_REG8(0xa31f), 0x20 },
	{ CCI_REG8(0xa6a9), 0x02 },
	{ CCI_REG8(0xa6ad), 0x02 },
	{ CCI_REG8(0xa75d), 0x00 },
	{ CCI_REG8(0xa75f), 0x00 },
	{ CCI_REG8(0xa763), 0x00 },
	{ CCI_REG8(0xa765), 0x00 },
	{ CCI_REG8(0xa831), 0x56 },
	{ CCI_REG8(0xa832), 0x2b },
	{ CCI_REG8(0xa833), 0x55 },
	{ CCI_REG8(0xa834), 0x55 },
	{ CCI_REG8(0xa835), 0x16 },
	{ CCI_REG8(0xa837), 0x51 },
	{ CCI_REG8(0xa838), 0x34 },
	{ CCI_REG8(0xa854), 0x58 },
	{ CCI_REG8(0xa855), 0x49 },
	{ CCI_REG8(0xa856), 0x45 },
	{ CCI_REG8(0xa857), 0x02 },
	{ CCI_REG8(0xa858), 0x02 },
	{ CCI_REG8(0xa85a), 0x32 },
	{ CCI_REG8(0xa85b), 0x19 },
	{ CCI_REG8(0xa85c), 0x12 },
	{ CCI_REG8(0xa85d), 0x02 },
	{ CCI_REG8(0xa85e), 0x02 },
	{ CCI_REG8(0xa931), 0x13 },
	{ CCI_REG8(0xa937), 0x04 },
	{ CCI_REG8(0xa93d), 0x92 },
	{ CCI_REG8(0xa943), 0x3f },
	{ CCI_REG8(0xa949), 0x64 },
	{ CCI_REG8(0xa94f), 0x12 },
	{ CCI_REG8(0xa955), 0x04 },
	{ CCI_REG8(0xa95b), 0x68 },
	{ CCI_REG8(0xaa58), 0x00 },
	{ CCI_REG8(0xaa59), 0x01 },
	{ CCI_REG8(0xab03), 0x10 },
	{ CCI_REG8(0xab04), 0x10 },
	{ CCI_REG8(0xab05), 0x10 },
	{ CCI_REG8(0xac72), 0x01 },
	{ CCI_REG8(0xac73), 0x26 },
	{ CCI_REG8(0xac74), 0x01 },
	{ CCI_REG8(0xac75), 0x26 },
	{ CCI_REG8(0xac76), 0x00 },
	{ CCI_REG8(0xac77), 0xc4 },
	{ CCI_REG8(0xad6a), 0x03 },
	{ CCI_REG8(0xad6b), 0xff },
	{ CCI_REG8(0xad77), 0x00 },
	{ CCI_REG8(0xad82), 0x03 },
	{ CCI_REG8(0xad83), 0xff },
	{ CCI_REG8(0xae06), 0x04 },
	{ CCI_REG8(0xae07), 0x16 },
	{ CCI_REG8(0xae08), 0xff },
	{ CCI_REG8(0xae0b), 0xff },
	{ CCI_REG8(0xaf01), 0x04 },
	{ CCI_REG8(0xaf03), 0x0a },
	{ CCI_REG8(0xb048), 0x0a },
	{ CCI_REG8(0xe8da), 0x00 },
	{ CCI_REG8(0xe8dd), 0x00 },
	{ CCI_REG8(0xe8e3), 0x00 },
	{ CCI_REG8(0xe8ec), 0x00 },
	{ CCI_REG8(0xe8ef), 0x00 },
	{ CCI_REG8(0xe8f0), 0x00 },
	{ CCI_REG8(0xe8f1), 0x00 },
	{ CCI_REG8(0xe8f2), 0x00 },
	{ CCI_REG8(0xe8f3), 0x05 },
	{ CCI_REG8(0xe918), 0x00 },
	{ CCI_REG8(0x38ac), 0x01 },
	{ CCI_REG8(0x38ad), 0x01 },
	{ CCI_REG8(0x38ae), 0x01 },
	{ CCI_REG8(0x38af), 0x01 },
	{ CCI_REG8(0x38b0), 0x01 },
	{ CCI_REG8(0x38b1), 0x01 },
	{ CCI_REG8(0x38b2), 0x01 },
	{ CCI_REG8(0x38b3), 0x01 },
};

static const struct sns_seg sns_init[] = {
	{ sns_init_regs, ARRAY_SIZE(sns_init_regs), 0 },
};

static const struct cci_reg_sequence sns_mode_2880x2156[] = {
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x03 },
	{ CCI_REG8(0x0342), 0x15 },
	{ CCI_REG8(0x0343), 0xa8 },
	{ CCI_REG8(0x0340), 0x08 },
	{ CCI_REG8(0x0341), 0xa7 },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x00 },
	{ CCI_REG8(0x0348), 0x16 },
	{ CCI_REG8(0x0349), 0x7f },
	{ CCI_REG8(0x034a), 0x10 },
	{ CCI_REG8(0x034b), 0xd7 },
	{ CCI_REG8(0x0220), 0x62 },
	{ CCI_REG8(0x0900), 0x01 },
	{ CCI_REG8(0x0901), 0x22 },
	{ CCI_REG8(0x0902), 0x08 },
	{ CCI_REG8(0x3140), 0x00 },
	{ CCI_REG8(0x3246), 0x81 },
	{ CCI_REG8(0x3247), 0x81 },
	{ CCI_REG8(0x0401), 0x00 },
	{ CCI_REG8(0x0404), 0x00 },
	{ CCI_REG8(0x0405), 0x10 },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x00 },
	{ CCI_REG8(0x040c), 0x0b },
	{ CCI_REG8(0x040d), 0x40 },
	{ CCI_REG8(0x040e), 0x08 },
	{ CCI_REG8(0x040f), 0x6c },
	{ CCI_REG8(0x034c), 0x0b },
	{ CCI_REG8(0x034d), 0x40 },
	{ CCI_REG8(0x034e), 0x08 },
	{ CCI_REG8(0x034f), 0x6c },
	{ CCI_REG8(0x0301), 0x05 },
	{ CCI_REG8(0x0303), 0x04 },
	{ CCI_REG8(0x0305), 0x04 },
	{ CCI_REG8(0x0306), 0x01 },
	{ CCI_REG8(0x0307), 0x33 },
	{ CCI_REG8(0x030b), 0x04 },
	{ CCI_REG8(0x030d), 0x04 },
	{ CCI_REG8(0x030e), 0x01 },
	{ CCI_REG8(0x030f), 0x54 },
	{ CCI_REG8(0x0310), 0x01 },
	{ CCI_REG8(0x0b06), 0x01 },
	{ CCI_REG8(0x3620), 0x00 },
	{ CCI_REG8(0x3f0c), 0x01 },
	{ CCI_REG8(0x3f14), 0x00 },
	{ CCI_REG8(0x3f80), 0x05 },
	{ CCI_REG8(0x3f81), 0x00 },
	{ CCI_REG8(0x3ffc), 0x04 },
	{ CCI_REG8(0x3ffd), 0x60 },
	{ CCI_REG8(0x7995), 0x01 },
	{ CCI_REG8(0x0202), 0x07 },
	{ CCI_REG8(0x0203), 0xd0 },
	{ CCI_REG8(0x0224), 0x01 },
	{ CCI_REG8(0x0225), 0xf4 },
	{ CCI_REG8(0x3fe0), 0x03 },
	{ CCI_REG8(0x3fe1), 0xe8 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x0216), 0x00 },
	{ CCI_REG8(0x0217), 0x00 },
	{ CCI_REG8(0x0218), 0x01 },
	{ CCI_REG8(0x0219), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x3fe2), 0x00 },
	{ CCI_REG8(0x3fe3), 0x00 },
	{ CCI_REG8(0x3fe4), 0x01 },
	{ CCI_REG8(0x3fe5), 0x00 },
	{ CCI_REG8(0x3e20), 0x01 },
	{ CCI_REG8(0x3e37), 0x01 },
	{ CCI_REG8(0x38a4), 0x00 },
	{ CCI_REG8(0x38a5), 0x0d },
	{ CCI_REG8(0x38a6), 0x00 },
	{ CCI_REG8(0x38a7), 0x10 },
	{ CCI_REG8(0x38a8), 0x01 },
	{ CCI_REG8(0x38a9), 0xaf },
	{ CCI_REG8(0x38aa), 0x01 },
	{ CCI_REG8(0x38ab), 0xac },
	{ CCI_REG8(0x38a3), 0x02 },
	{ CCI_REG8(0x38b4), 0x03 },
	{ CCI_REG8(0x38b5), 0x75 },
	{ CCI_REG8(0x38b6), 0x02 },
	{ CCI_REG8(0x38b7), 0x57 },
	{ CCI_REG8(0x38b8), 0x04 },
	{ CCI_REG8(0x38b9), 0xd1 },
	{ CCI_REG8(0x38ba), 0x03 },
	{ CCI_REG8(0x38bb), 0xb7 },
	{ CCI_REG8(0x38bc), 0x04 },
	{ CCI_REG8(0x38bd), 0x45 },
	{ CCI_REG8(0x38be), 0x02 },
	{ CCI_REG8(0x38bf), 0x57 },
	{ CCI_REG8(0x38c0), 0x05 },
	{ CCI_REG8(0x38c1), 0xa2 },
	{ CCI_REG8(0x38c2), 0x03 },
	{ CCI_REG8(0x38c3), 0xb7 },
	{ CCI_REG8(0x38c4), 0x03 },
	{ CCI_REG8(0x38c5), 0x75 },
	{ CCI_REG8(0x38c6), 0x03 },
	{ CCI_REG8(0x38c7), 0x29 },
	{ CCI_REG8(0x38c8), 0x04 },
	{ CCI_REG8(0x38c9), 0xd1 },
	{ CCI_REG8(0x38ca), 0x04 },
	{ CCI_REG8(0x38cb), 0x89 },
	{ CCI_REG8(0x38cc), 0x04 },
	{ CCI_REG8(0x38cd), 0x45 },
	{ CCI_REG8(0x38ce), 0x03 },
	{ CCI_REG8(0x38cf), 0x29 },
	{ CCI_REG8(0x38d0), 0x05 },
	{ CCI_REG8(0x38d1), 0xa2 },
	{ CCI_REG8(0x38d2), 0x04 },
	{ CCI_REG8(0x38d3), 0x89 },
	{ CCI_REG8(0x38d4), 0x03 },
	{ CCI_REG8(0x38d5), 0x75 },
	{ CCI_REG8(0x38d6), 0x02 },
	{ CCI_REG8(0x38d7), 0x57 },
	{ CCI_REG8(0x38d8), 0x05 },
	{ CCI_REG8(0x38d9), 0xa2 },
	{ CCI_REG8(0x38da), 0x04 },
	{ CCI_REG8(0x38db), 0x89 },
	{ CCI_REG8(0x38dc), 0x02 },
	{ CCI_REG8(0x38dd), 0xba },
	{ CCI_REG8(0x38de), 0x02 },
	{ CCI_REG8(0x38df), 0x10 },
	{ CCI_REG8(0x38e0), 0x06 },
	{ CCI_REG8(0x38e1), 0x5c },
	{ CCI_REG8(0x38e2), 0x04 },
	{ CCI_REG8(0x38e3), 0xd0 },
};

static const struct sns_seg sns_mode_2880x2156_segs[] = {
	{ sns_mode_2880x2156, ARRAY_SIZE(sns_mode_2880x2156), 0 },
};

static const struct cci_reg_sequence sns_mode_5760x4312[] = {
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x03 },
	{ CCI_REG8(0x0342), 0x18 },
	{ CCI_REG8(0x0343), 0x00 },
	{ CCI_REG8(0x0340), 0x11 },
	{ CCI_REG8(0x0341), 0x31 },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x00 },
	{ CCI_REG8(0x0348), 0x16 },
	{ CCI_REG8(0x0349), 0x7f },
	{ CCI_REG8(0x034a), 0x10 },
	{ CCI_REG8(0x034b), 0xd7 },
	{ CCI_REG8(0x0220), 0x62 },
	{ CCI_REG8(0x0900), 0x00 },
	{ CCI_REG8(0x0901), 0x11 },
	{ CCI_REG8(0x0902), 0x0a },
	{ CCI_REG8(0x3140), 0x00 },
	{ CCI_REG8(0x3246), 0x01 },
	{ CCI_REG8(0x3247), 0x01 },
	{ CCI_REG8(0x0401), 0x00 },
	{ CCI_REG8(0x0404), 0x00 },
	{ CCI_REG8(0x0405), 0x10 },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x00 },
	{ CCI_REG8(0x040c), 0x16 },
	{ CCI_REG8(0x040d), 0x80 },
	{ CCI_REG8(0x040e), 0x10 },
	{ CCI_REG8(0x040f), 0xd8 },
	{ CCI_REG8(0x034c), 0x16 },
	{ CCI_REG8(0x034d), 0x80 },
	{ CCI_REG8(0x034e), 0x10 },
	{ CCI_REG8(0x034f), 0xd8 },
	{ CCI_REG8(0x0301), 0x05 },
	{ CCI_REG8(0x0303), 0x02 },
	{ CCI_REG8(0x0305), 0x04 },
	{ CCI_REG8(0x0306), 0x01 },
	{ CCI_REG8(0x0307), 0x52 },
	{ CCI_REG8(0x030b), 0x01 },
	{ CCI_REG8(0x030d), 0x04 },
	{ CCI_REG8(0x030e), 0x01 },
	{ CCI_REG8(0x030f), 0x51 },
	{ CCI_REG8(0x0310), 0x01 },
	{ CCI_REG8(0x0b06), 0x01 },
	{ CCI_REG8(0x3620), 0x01 },
	{ CCI_REG8(0x3f0c), 0x00 },
	{ CCI_REG8(0x3f14), 0x01 },
	{ CCI_REG8(0x3f80), 0x01 },
	{ CCI_REG8(0x3f81), 0x72 },
	{ CCI_REG8(0x3ffc), 0x00 },
	{ CCI_REG8(0x3ffd), 0x3c },
	{ CCI_REG8(0x7995), 0x02 },
	{ CCI_REG8(0x0202), 0x07 },
	{ CCI_REG8(0x0203), 0xd0 },
	{ CCI_REG8(0x0224), 0x01 },
	{ CCI_REG8(0x0225), 0xf4 },
	{ CCI_REG8(0x3fe0), 0x03 },
	{ CCI_REG8(0x3fe1), 0xe8 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x0216), 0x00 },
	{ CCI_REG8(0x0217), 0x00 },
	{ CCI_REG8(0x0218), 0x01 },
	{ CCI_REG8(0x0219), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x3fe2), 0x00 },
	{ CCI_REG8(0x3fe3), 0x00 },
	{ CCI_REG8(0x3fe4), 0x01 },
	{ CCI_REG8(0x3fe5), 0x00 },
	{ CCI_REG8(0x3e20), 0x01 },
	{ CCI_REG8(0x3e37), 0x01 },
	{ CCI_REG8(0x3621), 0x01 },
	{ CCI_REG8(0x38a4), 0x00 },
	{ CCI_REG8(0x38a5), 0x22 },
	{ CCI_REG8(0x38a6), 0x00 },
	{ CCI_REG8(0x38a7), 0x28 },
	{ CCI_REG8(0x38a8), 0x04 },
	{ CCI_REG8(0x38a9), 0x2c },
	{ CCI_REG8(0x38aa), 0x04 },
	{ CCI_REG8(0x38ab), 0x20 },
	{ CCI_REG8(0x38a3), 0x02 },
	{ CCI_REG8(0x38b4), 0x08 },
	{ CCI_REG8(0x38b5), 0x8e },
	{ CCI_REG8(0x38b6), 0x05 },
	{ CCI_REG8(0x38b7), 0xc8 },
	{ CCI_REG8(0x38b8), 0x0b },
	{ CCI_REG8(0x38b9), 0xec },
	{ CCI_REG8(0x38ba), 0x09 },
	{ CCI_REG8(0x38bb), 0x2a },
	{ CCI_REG8(0x38bc), 0x0a },
	{ CCI_REG8(0x38bd), 0x92 },
	{ CCI_REG8(0x38be), 0x05 },
	{ CCI_REG8(0x38bf), 0xc8 },
	{ CCI_REG8(0x38c0), 0x0d },
	{ CCI_REG8(0x38c1), 0xf1 },
	{ CCI_REG8(0x38c2), 0x09 },
	{ CCI_REG8(0x38c3), 0x2a },
	{ CCI_REG8(0x38c4), 0x08 },
	{ CCI_REG8(0x38c5), 0x8e },
	{ CCI_REG8(0x38c6), 0x07 },
	{ CCI_REG8(0x38c7), 0xce },
	{ CCI_REG8(0x38c8), 0x0b },
	{ CCI_REG8(0x38c9), 0xec },
	{ CCI_REG8(0x38ca), 0x0b },
	{ CCI_REG8(0x38cb), 0x30 },
	{ CCI_REG8(0x38cc), 0x0a },
	{ CCI_REG8(0x38cd), 0x92 },
	{ CCI_REG8(0x38ce), 0x07 },
	{ CCI_REG8(0x38cf), 0xce },
	{ CCI_REG8(0x38d0), 0x0d },
	{ CCI_REG8(0x38d1), 0xf1 },
	{ CCI_REG8(0x38d2), 0x0b },
	{ CCI_REG8(0x38d3), 0x30 },
	{ CCI_REG8(0x38d4), 0x08 },
	{ CCI_REG8(0x38d5), 0x8e },
	{ CCI_REG8(0x38d6), 0x05 },
	{ CCI_REG8(0x38d7), 0xc8 },
	{ CCI_REG8(0x38d8), 0x0d },
	{ CCI_REG8(0x38d9), 0xf1 },
	{ CCI_REG8(0x38da), 0x0b },
	{ CCI_REG8(0x38db), 0x30 },
	{ CCI_REG8(0x38dc), 0x06 },
	{ CCI_REG8(0x38dd), 0xc0 },
	{ CCI_REG8(0x38de), 0x05 },
	{ CCI_REG8(0x38df), 0x17 },
	{ CCI_REG8(0x38e0), 0x0f },
	{ CCI_REG8(0x38e1), 0xbe },
	{ CCI_REG8(0x38e2), 0x0b },
	{ CCI_REG8(0x38e3), 0xe0 },
};

static const struct sns_seg sns_mode_5760x4312_segs[] = {
	{ sns_mode_5760x4312, ARRAY_SIZE(sns_mode_5760x4312), 0 },
};

static const struct cci_reg_sequence sns_mode_2880x1620[] = {
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x03 },
	{ CCI_REG8(0x0342), 0x15 },
	{ CCI_REG8(0x0343), 0xa8 },
	{ CCI_REG8(0x0340), 0x06 },
	{ CCI_REG8(0x0341), 0x8e },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x02 },
	{ CCI_REG8(0x0347), 0x18 },
	{ CCI_REG8(0x0348), 0x16 },
	{ CCI_REG8(0x0349), 0x7f },
	{ CCI_REG8(0x034a), 0x0e },
	{ CCI_REG8(0x034b), 0xbf },
	{ CCI_REG8(0x0220), 0x62 },
	{ CCI_REG8(0x0900), 0x01 },
	{ CCI_REG8(0x0901), 0x22 },
	{ CCI_REG8(0x0902), 0x08 },
	{ CCI_REG8(0x3140), 0x00 },
	{ CCI_REG8(0x3246), 0x81 },
	{ CCI_REG8(0x3247), 0x81 },
	{ CCI_REG8(0x0401), 0x00 },
	{ CCI_REG8(0x0404), 0x00 },
	{ CCI_REG8(0x0405), 0x10 },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x00 },
	{ CCI_REG8(0x040c), 0x0b },
	{ CCI_REG8(0x040d), 0x40 },
	{ CCI_REG8(0x040e), 0x06 },
	{ CCI_REG8(0x040f), 0x54 },
	{ CCI_REG8(0x034c), 0x0b },
	{ CCI_REG8(0x034d), 0x40 },
	{ CCI_REG8(0x034e), 0x06 },
	{ CCI_REG8(0x034f), 0x54 },
	{ CCI_REG8(0x0301), 0x05 },
	{ CCI_REG8(0x0303), 0x02 },
	{ CCI_REG8(0x0305), 0x04 },
	{ CCI_REG8(0x0306), 0x01 },
	{ CCI_REG8(0x0307), 0x5d },
	{ CCI_REG8(0x030b), 0x01 },
	{ CCI_REG8(0x030d), 0x04 },
	{ CCI_REG8(0x030e), 0x00 },
	{ CCI_REG8(0x030f), 0xd1 },
	{ CCI_REG8(0x0310), 0x01 },
	{ CCI_REG8(0x0b06), 0x01 },
	{ CCI_REG8(0x3620), 0x00 },
	{ CCI_REG8(0x3f0c), 0x01 },
	{ CCI_REG8(0x3f14), 0x00 },
	{ CCI_REG8(0x3f80), 0x05 },
	{ CCI_REG8(0x3f81), 0x00 },
	{ CCI_REG8(0x3ffc), 0x04 },
	{ CCI_REG8(0x3ffd), 0x60 },
	{ CCI_REG8(0x7995), 0x01 },
	{ CCI_REG8(0x0202), 0x06 },
	{ CCI_REG8(0x0203), 0x50 },
	{ CCI_REG8(0x0224), 0x01 },
	{ CCI_REG8(0x0225), 0x94 },
	{ CCI_REG8(0x3fe0), 0x03 },
	{ CCI_REG8(0x3fe1), 0x28 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x0216), 0x00 },
	{ CCI_REG8(0x0217), 0x00 },
	{ CCI_REG8(0x0218), 0x01 },
	{ CCI_REG8(0x0219), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x3fe2), 0x00 },
	{ CCI_REG8(0x3fe3), 0x00 },
	{ CCI_REG8(0x3fe4), 0x01 },
	{ CCI_REG8(0x3fe5), 0x00 },
	{ CCI_REG8(0x3e20), 0x01 },
	{ CCI_REG8(0x3e37), 0x01 },
};

static const struct sns_seg sns_mode_2880x1620_segs[] = {
	{ sns_mode_2880x1620, ARRAY_SIZE(sns_mode_2880x1620), 0 },
};



static const s64 sns_link_freq_menu[] = { 255000000LL, 1011000000LL, 627000000LL };

static const struct sns_mode sns_modes[] = {
	{
		/* stock res1: 2x2 binned 30 fps, OP 510 Mbps/lane */
		.width = 2880, .height = 2156, .hts = 5544, .vts = 2215,
		.exposure = 2146, .exposure_margin = 61,
		.link_freq_index = 0, .pixel_rate = 368398800ULL,
		.segs = sns_mode_2880x2156_segs, .num_segs = ARRAY_SIZE(sns_mode_2880x2156_segs),
	},
	{
		/* stock res0: full 30 fps, OP 2022 Mbps/lane (= upstream v4 1011 MHz) */
		.width = 5760, .height = 4312, .hts = 6144, .vts = 4401,
		.exposure = 4332, .exposure_margin = 61,
		.link_freq_index = 1, .pixel_rate = 811238400ULL,
		.segs = sns_mode_5760x4312_segs, .num_segs = ARRAY_SIZE(sns_mode_5760x4312_segs),
	},
	{
		/* stock res3: binned 16:9 90 fps, OP 1254 Mbps/lane */
		.width = 2880, .height = 1620, .hts = 5544, .vts = 1678,
		.exposure = 1609, .exposure_margin = 61,
		.link_freq_index = 2, .pixel_rate = 837254880ULL,
		.segs = sns_mode_2880x1620_segs, .num_segs = ARRAY_SIZE(sns_mode_2880x1620_segs),
	},
};

/* index = vflip << 1 | hflip */
static const u32 sns_mbus_formats[] = { MEDIA_BUS_FMT_SRGGB10_1X10, MEDIA_BUS_FMT_SGRBG10_1X10, MEDIA_BUS_FMT_SGBRG10_1X10, MEDIA_BUS_FMT_SBGGR10_1X10 };

static const char * const sns_test_pattern_menu[] = {
	"Disabled", "Solid color", "Color bars", "Fade to grey color bars", "PN9",
};

struct sns {
	struct device *dev;
	struct regmap *regmap;
	struct clk *mclk;
	struct gpio_desc *reset_gpio;
	struct regulator *vdda, *vddd, *vddio;

	struct v4l2_subdev sd;
	struct media_pad pad;

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq, *pixel_rate, *hblank, *vblank;
	struct v4l2_ctrl *exposure, *vflip, *hflip;

	const struct sns_mode *mode;
};

#define to_sns(_sd) container_of(_sd, struct sns, sd)

static int sns_write_segs(struct sns *s, const struct sns_seg *segs,
			  unsigned int n)
{
	unsigned int i;
	int ret = 0;

	for (i = 0; i < n && !ret; i++) {
		cci_multi_reg_write(s->regmap, segs[i].regs, segs[i].num, &ret);
		if (segs[i].delay_us)
			fsleep(segs[i].delay_us);
	}
	return ret;
}

static int sns_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sns *s = container_of(ctrl->handler, struct sns, ctrl_handler);
	const struct sns_mode *mode = s->mode;
	int ret;

	if (ctrl->id == V4L2_CID_VBLANK) {
		s64 max = mode->height + ctrl->val - mode->exposure_margin;

		__v4l2_ctrl_modify_range(s->exposure, s->exposure->minimum, max,
					 1, min_t(s64, s->exposure->default_value, max));
	}

	if (!pm_runtime_get_if_active(s->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(s->regmap, SNS_REG_AGAIN, ctrl->val, NULL);
		break;
	case V4L2_CID_EXPOSURE:
		ret = cci_write(s->regmap, SNS_REG_EXPOSURE, ctrl->val, NULL);
		break;
	case V4L2_CID_VBLANK:
		ret = cci_write(s->regmap, SNS_REG_VTS, ctrl->val + mode->height, NULL);
		break;
	case V4L2_CID_VFLIP:
	case V4L2_CID_HFLIP:
		ret = cci_write(s->regmap, SNS_REG_ORIENTATION,
				(s->vflip->val ? SNS_VFLIP : 0) |
				(s->hflip->val ? SNS_HFLIP : 0), NULL);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = cci_write(s->regmap, SNS_REG_TEST_PATTERN, ctrl->val, NULL);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(s->dev);
	return ret;
}

static const struct v4l2_ctrl_ops sns_ctrl_ops = {
	.s_ctrl = sns_set_ctrl,
};

static u64 sns_max_pixel_rate(void)
{
	u64 m = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sns_modes); i++)
		m = max(m, sns_modes[i].pixel_rate);
	return m;
}

static int sns_init_controls(struct sns *s)
{
	struct v4l2_ctrl_handler *h = &s->ctrl_handler;
	const struct sns_mode *mode = s->mode;
	struct v4l2_fwnode_device_properties props;
	s64 hblank, vblank;
	int ret;

	v4l2_ctrl_handler_init(h, 12);

	s->link_freq = v4l2_ctrl_new_int_menu(h, &sns_ctrl_ops, V4L2_CID_LINK_FREQ,
					      ARRAY_SIZE(sns_link_freq_menu) - 1,
					      mode->link_freq_index,
					      sns_link_freq_menu);
	if (s->link_freq)
		s->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	s->pixel_rate = v4l2_ctrl_new_std(h, &sns_ctrl_ops, V4L2_CID_PIXEL_RATE,
					  1, sns_max_pixel_rate(), 1,
					  mode->pixel_rate);

	hblank = mode->hts - mode->width;
	s->hblank = v4l2_ctrl_new_std(h, &sns_ctrl_ops, V4L2_CID_HBLANK,
				      hblank, hblank, 1, hblank);
	if (s->hblank)
		s->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank = mode->vts - mode->height;
	s->vblank = v4l2_ctrl_new_std(h, &sns_ctrl_ops, V4L2_CID_VBLANK, vblank,
				      SNS_VTS_MAX - mode->height, 1, vblank);

	v4l2_ctrl_new_std(h, &sns_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  SNS_AGAIN_MIN, SNS_AGAIN_MAX, 1, SNS_AGAIN_DEFAULT);

	s->exposure = v4l2_ctrl_new_std(h, &sns_ctrl_ops, V4L2_CID_EXPOSURE,
					SNS_EXPOSURE_MIN,
					mode->vts - mode->exposure_margin, 1,
					mode->exposure);

	v4l2_ctrl_new_std_menu_items(h, &sns_ctrl_ops, V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(sns_test_pattern_menu) - 1,
				     0, 0, sns_test_pattern_menu);

	s->hflip = v4l2_ctrl_new_std(h, &sns_ctrl_ops, V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (s->hflip)
		s->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;
	s->vflip = v4l2_ctrl_new_std(h, &sns_ctrl_ops, V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (s->vflip)
		s->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	ret = v4l2_fwnode_device_parse(s->dev, &props);
	if (ret)
		goto err;
	ret = v4l2_ctrl_new_fwnode_properties(h, &sns_ctrl_ops, &props);
	if (ret)
		goto err;
	if (h->error) {
		ret = h->error;
		goto err;
	}

	s->sd.ctrl_handler = h;
	return 0;
err:
	v4l2_ctrl_handler_free(h);
	return ret;
}

/*
 * A6L camfix debug: prove whether the sensor transmits. CCS FRAME_COUNT (0x0005, 8 bit) increments on every
 * frame the sensor outputs, independently of the receiver. Logged as A6L_SNS_FC; disable with dbg_fc=0.
 */
static bool dbg_fc = true;
module_param(dbg_fc, bool, 0644);
MODULE_PARM_DESC(dbg_fc, "log frame counter (reg 0x0005) 200 ms after stream on");

#define SNS_REG_FRAME_COUNT		CCI_REG8(0x0005)

static void sns_log_frame_count(struct sns *s)
{
	u64 fc0 = 0, fc1 = 0, ms = 0;
	int ret = 0;

	cci_read(s->regmap, SNS_REG_FRAME_COUNT, &fc0, &ret);
	msleep(200);
	cci_read(s->regmap, SNS_REG_FRAME_COUNT, &fc1, &ret);
	cci_read(s->regmap, SNS_REG_CTRL_MODE, &ms, &ret);
	dev_info(s->dev, "A6L_SNS_FC %ux%u mode_select=0x%llx frame_count %llu -> %llu in 200 ms: %s (i2c %d)\n",
		 s->mode->width, s->mode->height, ms, fc0, fc1,
		 ret ? "READ_ERROR" : (fc1 != fc0 ? "SENSOR_STREAMING" : "SENSOR_NOT_COUNTING"), ret);
}

static int sns_enable_streams(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state, u32 pad,
			      u64 streams_mask)
{
	struct sns *s = to_sns(sd);
	int ret;

	ret = pm_runtime_resume_and_get(s->dev);
	if (ret)
		return ret;

	ret = sns_write_segs(s, sns_init, ARRAY_SIZE(sns_init));
	if (!ret)
		ret = sns_write_segs(s, s->mode->segs, s->mode->num_segs);
	if (ret)
		goto error;

	ret = __v4l2_ctrl_handler_setup(s->sd.ctrl_handler);
	if (ret)
		goto error;

	ret = cci_write(s->regmap, SNS_REG_CTRL_MODE, SNS_MODE_STREAMING, NULL);
	if (ret)
		goto error;

	if (dbg_fc)
		sns_log_frame_count(s);

	return 0;

error:
	dev_err(s->dev, "failed to start streaming: %d\n", ret);
	pm_runtime_put_autosuspend(s->dev);
	return ret;
}

static int sns_disable_streams(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state, u32 pad,
			       u64 streams_mask)
{
	struct sns *s = to_sns(sd);
	int ret;

	if (dbg_fc) {
		u64 fc = 0;

		ret = cci_read(s->regmap, SNS_REG_FRAME_COUNT, &fc, NULL);
		dev_info(s->dev, "A6L_SNS_FC_STOP frame_count %llu (i2c %d)\n", fc, ret);
	}

	ret = cci_write(s->regmap, SNS_REG_CTRL_MODE, 0, NULL);
	if (ret)
		dev_err(s->dev, "failed to stop streaming: %d\n", ret);

	pm_runtime_put_autosuspend(s->dev);
	return ret;
}

static u32 sns_get_format_code(struct sns *s)
{
	return sns_mbus_formats[(s->vflip->val ? 2 : 0) | (s->hflip->val ? 1 : 0)];
}

static void sns_update_pad_format(struct sns *s, const struct sns_mode *mode,
				  struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = sns_get_format_code(s);
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int sns_set_pad_format(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state,
			      struct v4l2_subdev_format *fmt)
{
	struct sns *s = to_sns(sd);
	const struct sns_mode *mode;
	s64 hblank, vblank;

	mode = v4l2_find_nearest_size(sns_modes, ARRAY_SIZE(sns_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);

	sns_update_pad_format(s, mode, &fmt->format);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY || s->mode == mode)
		goto set_format;

	s->mode = mode;

	__v4l2_ctrl_s_ctrl(s->link_freq, mode->link_freq_index);
	__v4l2_ctrl_s_ctrl_int64(s->pixel_rate, mode->pixel_rate);

	hblank = mode->hts - mode->width;
	__v4l2_ctrl_modify_range(s->hblank, hblank, hblank, 1, hblank);

	vblank = mode->vts - mode->height;
	__v4l2_ctrl_modify_range(s->vblank, vblank, SNS_VTS_MAX - mode->height,
				 1, vblank);
	__v4l2_ctrl_s_ctrl(s->vblank, vblank);

	__v4l2_ctrl_modify_range(s->exposure, SNS_EXPOSURE_MIN,
				 mode->vts - mode->exposure_margin, 1,
				 mode->exposure);
	__v4l2_ctrl_s_ctrl(s->exposure, mode->exposure);

	if (s->sd.ctrl_handler->error)
		return s->sd.ctrl_handler->error;

set_format:
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;
	return 0;
}

static int sns_enum_mbus_code(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *sd_state,
			      struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;
	code->code = sns_get_format_code(to_sns(sd));
	return 0;
}

static int sns_enum_frame_size(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *sd_state,
			       struct v4l2_subdev_frame_size_enum *fse)
{
	struct sns *s = to_sns(sd);

	if (fse->index >= ARRAY_SIZE(sns_modes))
		return -EINVAL;
	if (fse->code != sns_get_format_code(s))
		return -EINVAL;

	fse->min_width = fse->max_width = sns_modes[fse->index].width;
	fse->min_height = fse->max_height = sns_modes[fse->index].height;
	return 0;
}

static int sns_get_selection(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state,
			     struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = 5760;
		sel->r.height = 4312;
		return 0;
	default:
		return -EINVAL;
	}
}

static int sns_init_state(struct v4l2_subdev *sd, struct v4l2_subdev_state *state)
{
	struct sns *s = to_sns(sd);
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.width = s->mode->width,
			.height = s->mode->height,
		},
	};

	sns_set_pad_format(sd, state, &fmt);
	return 0;
}

static const struct v4l2_subdev_video_ops sns_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops sns_pad_ops = {
	.set_fmt = sns_set_pad_format,
	.get_fmt = v4l2_subdev_get_fmt,
	.get_selection = sns_get_selection,
	.enum_mbus_code = sns_enum_mbus_code,
	.enum_frame_size = sns_enum_frame_size,
	.enable_streams = sns_enable_streams,
	.disable_streams = sns_disable_streams,
};

static const struct v4l2_subdev_ops sns_subdev_ops = {
	.video = &sns_video_ops,
	.pad = &sns_pad_ops,
};

static const struct v4l2_subdev_internal_ops sns_internal_ops = {
	.init_state = sns_init_state,
};

static const struct media_entity_operations sns_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int sns_identify(struct sns *s)
{
	u64 val;
	int ret;

	ret = cci_read(s->regmap, SNS_REG_CHIP_ID, &val, NULL);
	if (ret)
		return dev_err_probe(s->dev, ret, "failed to read chip id\n");

	if (val != SNS_CHIP_ID)
		return dev_err_probe(s->dev, -ENODEV,
				     "chip id mismatch: 0x%x != 0x%llx\n",
				     SNS_CHIP_ID, val);

	dev_info(s->dev, "Sony IMX576 chip id 0x%llx\n", val);
	return 0;
}

static int sns_check_hwcfg(struct sns *s)
{
	struct fwnode_handle *fwnode = dev_fwnode(s->dev), *ep;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	unsigned long freq_bitmap;
	int ret;

	if (!fwnode)
		return -ENODEV;

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep)
		return -EINVAL;

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != SNS_DATA_LANES) {
		dev_err(s->dev, "only 4 data lanes supported (got %u)\n",
			bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto out;
	}

	ret = v4l2_link_freq_to_bitmap(s->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       sns_link_freq_menu,
				       ARRAY_SIZE(sns_link_freq_menu),
				       &freq_bitmap);
out:
	v4l2_fwnode_endpoint_free(&bus_cfg);
	return ret;
}

/* Stock power-up (sensor_lib_t power_setting_array): RESET low 1ms, VANA 1ms, MCLK 24MHz 2ms, RESET high 3ms */
static int sns_power_on(struct device *dev)
{
	struct sns *s = to_sns(dev_get_drvdata(dev));
	int ret;

	gpiod_set_value_cansleep(s->reset_gpio, 1);
	usleep_range(1000, 1500);

	if (s->vddio) {
		ret = regulator_enable(s->vddio);
		if (ret)
			return ret;
	}
	if (s->vddd) {
		ret = regulator_enable(s->vddd);
		if (ret)
			goto off_vddio;
	}
	ret = regulator_enable(s->vdda);
	if (ret)
		goto off_vddd;
	usleep_range(5000, 6000);

	ret = clk_prepare_enable(s->mclk);
	if (ret)
		goto off_vdda;
	usleep_range(2000, 3000);

	gpiod_set_value_cansleep(s->reset_gpio, 0);
	usleep_range(8000, 10000);
	return 0;

off_vdda:
	regulator_disable(s->vdda);
off_vddd:
	if (s->vddd)
		regulator_disable(s->vddd);
off_vddio:
	if (s->vddio)
		regulator_disable(s->vddio);
	return ret;
}

static int sns_power_off(struct device *dev)
{
	struct sns *s = to_sns(dev_get_drvdata(dev));

	gpiod_set_value_cansleep(s->reset_gpio, 1);
	clk_disable_unprepare(s->mclk);
	regulator_disable(s->vdda);
	if (s->vddd)
		regulator_disable(s->vddd);
	if (s->vddio)
		regulator_disable(s->vddio);
	return 0;
}

static struct regulator *sns_get_opt_reg(struct sns *s, const char *name)
{
	struct regulator *r = devm_regulator_get_optional(s->dev, name);

	if (IS_ERR(r) && PTR_ERR(r) == -ENODEV)
		return NULL;
	return r;
}

static int sns_probe(struct i2c_client *client)
{
	struct sns *s;
	unsigned long freq;
	int ret;

	s = devm_kzalloc(&client->dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	s->dev = &client->dev;
	v4l2_i2c_subdev_init(&s->sd, client, &sns_subdev_ops);

	s->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(s->regmap))
		return dev_err_probe(s->dev, PTR_ERR(s->regmap), "failed to init CCI\n");

	s->mclk = devm_v4l2_sensor_clk_get(s->dev, NULL);
	if (IS_ERR(s->mclk))
		return dev_err_probe(s->dev, PTR_ERR(s->mclk), "failed to get MCLK\n");

	freq = clk_get_rate(s->mclk);
	if (freq != SNS_MCLK_FREQ)
		return dev_err_probe(s->dev, -EINVAL, "MCLK %lu Hz not supported\n", freq);

	ret = sns_check_hwcfg(s);
	if (ret)
		return dev_err_probe(s->dev, ret, "failed to check HW configuration\n");

	s->reset_gpio = devm_gpiod_get_optional(s->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(s->reset_gpio))
		return dev_err_probe(s->dev, PTR_ERR(s->reset_gpio), "cannot get reset GPIO\n");

	s->vdda = devm_regulator_get(s->dev, "vdda");
	if (IS_ERR(s->vdda))
		return dev_err_probe(s->dev, PTR_ERR(s->vdda), "vdda\n");
	s->vddd = sns_get_opt_reg(s, "vddd");
	if (IS_ERR(s->vddd))
		return dev_err_probe(s->dev, PTR_ERR(s->vddd), "vddd\n");
	s->vddio = sns_get_opt_reg(s, "vddio");
	if (IS_ERR(s->vddio))
		return dev_err_probe(s->dev, PTR_ERR(s->vddio), "vddio\n");

	ret = sns_power_on(s->dev);
	if (ret)
		return ret;

	ret = sns_identify(s);
	if (ret)
		goto power_off;

	s->mode = &sns_modes[0];
	ret = sns_init_controls(s);
	if (ret) {
		dev_err_probe(s->dev, ret, "failed to init controls\n");
		goto power_off;
	}

	s->sd.state_lock = s->ctrl_handler.lock;
	s->sd.internal_ops = &sns_internal_ops;
	s->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	s->sd.entity.ops = &sns_entity_ops;
	s->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	s->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&s->sd.entity, 1, &s->pad);
	if (ret)
		goto free_ctrls;

	ret = v4l2_subdev_init_finalize(&s->sd);
	if (ret)
		goto entity_cleanup;

	pm_runtime_set_active(s->dev);
	pm_runtime_enable(s->dev);

	ret = v4l2_async_register_subdev_sensor(&s->sd);
	if (ret)
		goto subdev_cleanup;

	pm_runtime_set_autosuspend_delay(s->dev, 1000);
	pm_runtime_use_autosuspend(s->dev);
	pm_runtime_idle(s->dev);
	return 0;

subdev_cleanup:
	v4l2_subdev_cleanup(&s->sd);
	pm_runtime_disable(s->dev);
	pm_runtime_set_suspended(s->dev);
entity_cleanup:
	media_entity_cleanup(&s->sd.entity);
free_ctrls:
	v4l2_ctrl_handler_free(s->sd.ctrl_handler);
power_off:
	sns_power_off(s->dev);
	return ret;
}

static void sns_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct sns *s = to_sns(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
	pm_runtime_disable(s->dev);
	if (!pm_runtime_status_suspended(s->dev)) {
		sns_power_off(s->dev);
		pm_runtime_set_suspended(s->dev);
	}
}

static const struct dev_pm_ops sns_pm_ops = {
	SET_RUNTIME_PM_OPS(sns_power_off, sns_power_on, NULL)
};

static const struct of_device_id sns_of_match[] = {
	{ .compatible = "sony,imx576" },
	{ }
};
MODULE_DEVICE_TABLE(of, sns_of_match);

static struct i2c_driver sns_i2c_driver = {
	.driver = {
		.name = "imx576_a6l",
		.pm = &sns_pm_ops,
		.of_match_table = sns_of_match,
	},
	.probe = sns_probe,
	.remove = sns_remove,
};
module_i2c_driver(sns_i2c_driver);

MODULE_DESCRIPTION("Sony IMX576 sensor driver (A6L, stock register tables)");
MODULE_LICENSE("GPL");
