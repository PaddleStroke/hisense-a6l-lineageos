#!/usr/bin/env python3
# A6L: add imx576 (imx576_a6l) and s5k3t1 sensor support to libcamera v0.7.x source tree.
# Usage: a6l_sensor_edit.py <libcamera-src>   (idempotent; used once to generate patches/0001-*.patch)
import sys, os, re
src = sys.argv[1]

def edit(rel, anchor, text, before=True):
    p = os.path.join(src, rel); s = open(p).read()
    if text in s:
        print('already', rel); return
    i = s.find(anchor)
    if i < 0: sys.exit('anchor not found in %s: %r' % (rel, anchor))
    s = s[:i] + text + s[i:] if before else s[:i+len(anchor)] + text + s[i+len(anchor):]
    open(p, 'w').write(s); print('edited', rel)

# 1. camera sensor properties (unit cell, test pattern menu index, sensor delays)
props = '''\t\t/* A6L: Sony IMX576 (Hisense A6L driver imx576_a6l, CCS-like 0x0600 test pattern menu). */
\t\t{ "imx576", {
\t\t\t.unitCellSize = { 900, 900 },
\t\t\t.testPatternModes = {
\t\t\t\t{ controls::draft::TestPatternModeOff, 0 },
\t\t\t\t{ controls::draft::TestPatternModeSolidColor, 1 },
\t\t\t\t{ controls::draft::TestPatternModeColorBars, 2 },
\t\t\t\t{ controls::draft::TestPatternModeColorBarsFadeToGray, 3 },
\t\t\t\t{ controls::draft::TestPatternModePn9, 4 },
\t\t\t},
\t\t\t.sensorDelays = {
\t\t\t\t.exposureDelay = 2,
\t\t\t\t.gainDelay = 2,
\t\t\t\t.vblankDelay = 2,
\t\t\t\t.hblankDelay = 2
\t\t\t},
\t\t} },
\t\t{ "imx576_a6l", {
\t\t\t.unitCellSize = { 900, 900 },
\t\t\t.testPatternModes = {
\t\t\t\t{ controls::draft::TestPatternModeOff, 0 },
\t\t\t\t{ controls::draft::TestPatternModeSolidColor, 1 },
\t\t\t\t{ controls::draft::TestPatternModeColorBars, 2 },
\t\t\t\t{ controls::draft::TestPatternModeColorBarsFadeToGray, 3 },
\t\t\t\t{ controls::draft::TestPatternModePn9, 4 },
\t\t\t},
\t\t\t.sensorDelays = {
\t\t\t\t.exposureDelay = 2,
\t\t\t\t.gainDelay = 2,
\t\t\t\t.vblankDelay = 2,
\t\t\t\t.hblankDelay = 2
\t\t\t},
\t\t} },
'''
edit('src/libcamera/sensor/camera_sensor_properties.cpp', '\t\t{ "imx708", {', props)
props2 = '''\t\t/* A6L: Samsung S5K3T1 (Hisense A6L front, 5184x3880 0.8 um, CCS-like test pattern menu). */
\t\t{ "s5k3t1", {
\t\t\t.unitCellSize = { 800, 800 },
\t\t\t.testPatternModes = {
\t\t\t\t{ controls::draft::TestPatternModeOff, 0 },
\t\t\t\t{ controls::draft::TestPatternModeSolidColor, 1 },
\t\t\t\t{ controls::draft::TestPatternModeColorBars, 2 },
\t\t\t\t{ controls::draft::TestPatternModeColorBarsFadeToGray, 3 },
\t\t\t\t{ controls::draft::TestPatternModePn9, 4 },
\t\t\t},
\t\t\t.sensorDelays = {
\t\t\t\t.exposureDelay = 2,
\t\t\t\t.gainDelay = 2,
\t\t\t\t.vblankDelay = 2,
\t\t\t\t.hblankDelay = 2
\t\t\t},
\t\t} },
'''
# insert s5k3t1 before the first entry whose key sorts after it
p = os.path.join(src, 'src/libcamera/sensor/camera_sensor_properties.cpp'); s = open(p).read()
keys = re.findall(r'\n\t\t\{ "([^"]+)", \{', s)
nxt = next(k for k in keys if k > 's5k3t1')
edit('src/libcamera/sensor/camera_sensor_properties.cpp', '\t\t{ "%s", {' % nxt, props2)

# 2. camera sensor helpers (gain model + black level)
helper = '''/*
 * A6L: Sony IMX576. SMIA analogue gain: gain = 1024 / (1024 - code), code 0..960 (16x).
 * Black level 64 at 10 bits (stock chromatix BLC 64).
 */
class CameraSensorHelperImx576 : public CameraSensorHelper
{
public:
\tCameraSensorHelperImx576()
\t{
\t\tblackLevel_ = 4096;
\t\tgain_ = AnalogueGainLinear{ 0, 1024, -1, 1024 };
\t}
};
REGISTER_CAMERA_SENSOR_HELPER("imx576", CameraSensorHelperImx576)
/* The A6L kernel driver is named imx576_a6l (entity "imx576_a6l <bus>-001a"). */
class CameraSensorHelperImx576A6l : public CameraSensorHelperImx576
{
};
REGISTER_CAMERA_SENSOR_HELPER("imx576_a6l", CameraSensorHelperImx576A6l)

'''
edit('src/ipa/libipa/camera_sensor_helper.cpp', 'class CameraSensorHelperImx678', helper)
helper2 = '''/*
 * A6L: Samsung S5K3T1. Analogue gain: gain = code / 32, code 32..512 (1x..16x).
 * Black level 64 at 10 bits.
 */
class CameraSensorHelperS5k3t1 : public CameraSensorHelper
{
public:
\tCameraSensorHelperS5k3t1()
\t{
\t\tblackLevel_ = 4096;
\t\tgain_ = AnalogueGainLinear{ 1, 0, 0, 32 };
\t}
};
REGISTER_CAMERA_SENSOR_HELPER("s5k3t1", CameraSensorHelperS5k3t1)

'''
edit('src/ipa/libipa/camera_sensor_helper.cpp', 'class CameraSensorHelperVd55g1', helper2)

# 3. tuning files for the simple IPA
edit('src/ipa/simple/data/meson.build', "    'uncalibrated.yaml',", "    'imx576_a6l.yaml',\n    's5k3t1.yaml',\n")
