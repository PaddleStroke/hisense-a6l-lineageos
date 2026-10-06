#!/usr/bin/env python3
# Fake qcom-smgr IIO sysfs + chardev records for the sensors.a6l host tests (agent senshal, 26 Sep 2026).
# Layout = drivers/iio/*/qcom_smgr_*.c: x/y/z le:s32/32 (index 0..2), timestamp le:u32/64 (index 3), scale 1/65536,
# record 24 bytes. Values in the SMGR frame (NED): raw = (Android Y, Android X, -Android Z) = inverse of map +y+x-z.
# Magnetometer in gauss. DSP ticks at 32768 Hz starting 1 s before the u32 wrap. Records go to src/; the test feeds
# them into dev/ at the real rate (the kfifo stand-in), see test_sensors_a6l.c feeder().
# usage: mkfake_hal.py ROOT flat|rotate|planar|slowyaw [--no-mag] [--secs N] [--stk]
import math, os, random, struct, sys, json
root, mode = sys.argv[1], sys.argv[2]
args = sys.argv[3:]
secs = float(args[args.index('--secs') + 1]) if '--secs' in args else 20.0
random.seed(1234)
G = 9.80665
HARD = (8.0, -5.0, 3.0)            # uT, Android frame
BW = (0.0, 21.0, -42.0)            # world ENU (x east, y north, z up), uT
GBIAS = (0.01, -0.02, 0.005)       # rad/s
TICK0 = (1 << 32) - 32768          # wraps after 1 s
os.makedirs(root + '/sys', exist_ok=True)
os.makedirs(root + '/dev', exist_ok=True)
os.makedirs(root + '/data', exist_ok=True)
os.makedirs(root + '/src', exist_ok=True)
feed = open(root + '/src/feed.txt', 'w')

def rot(t):
    """device->world rotation matrix at time t"""
    if mode == 'flat':
        yaw, pitch, roll = 0.3, 0.0, 0.0
    elif mode == 'slowyaw':        # r5 F32: table turning slowly (0.04 rad/s about gravity)
        yaw, pitch, roll = 0.3 + 0.04 * t, 0.0, 0.0
    elif mode == 'planar':         # yaw only (table turn): not enough for a 3-D fit
        yaw, pitch, roll = 2 * math.pi * t / 5.0, 0.0, 0.0
    else:                          # tumble: covers the sphere
        yaw = 2 * math.pi * t / 2.0
        pitch = math.pi * math.sin(2 * math.pi * t / 5.0)
        roll = 0.8 * math.sin(2 * math.pi * t / 3.0)
    cz, sz = math.cos(yaw), math.sin(yaw)
    cx, sx = math.cos(pitch), math.sin(pitch)
    cy, sy = math.cos(roll), math.sin(roll)
    Rz = [[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]]
    Rx = [[1, 0, 0], [0, cx, -sx], [0, sx, cx]]
    Ry = [[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]
    return mm(mm(Rz, Rx), Ry)

def mm(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]

def tmv(R, v):  # R^T v (world -> device)
    return [sum(R[k][i] * v[k] for k in range(3)) for i in range(3)]

def omega(t, dt=1e-4):
    R0, R1 = rot(t), rot(t + dt)
    # body rate: skew = R0^T (R1 - R0)/dt
    Rt = [[R0[j][i] for j in range(3)] for i in range(3)]
    D = [[(R1[i][j] - R0[i][j]) / dt for j in range(3)] for i in range(3)]
    S = mm(Rt, D)
    return [(S[2][1] - S[1][2]) / 2, (S[0][2] - S[2][0]) / 2, (S[1][0] - S[0][1]) / 2]

devs = [('qcom-smgr-accel', 'accel', 200), ('qcom-smgr-gyro', 'anglvel', 200)]
if '--no-mag' not in args:
    devs.append(('qcom-smgr-mag', 'magn', 100))
for i, (nm, ty, hz) in enumerate(devs):
    d = f'{root}/sys/iio:device{i + 3}'
    os.makedirs(d + '/scan_elements', exist_ok=True)
    os.makedirs(d + '/buffer', exist_ok=True)
    open(d + '/name', 'w').write(nm + '\n')
    open(d + f'/in_{ty}_scale', 'w').write('0.000015258\n')
    open(d + f'/in_{ty}_sampling_frequency', 'w').write(f'{hz}\n')
    for f in ('enable', 'length', 'watermark'):
        open(d + '/buffer/' + f, 'w').write('0\n')
    for k, a in enumerate('xyz'):
        p = d + f'/scan_elements/in_{ty}_{a}'
        open(p + '_en', 'w').write('0')
        open(p + '_index', 'w').write(str(k))
        open(p + '_type', 'w').write('le:s32/32>>0\n')
    p = d + '/scan_elements/in_timestamp'
    open(p + '_en', 'w').write('0')
    open(p + '_index', 'w').write('3')
    open(p + '_type', 'w').write('le:u32/64>>0\n')
    open(f'{root}/dev/iio:device{i + 3}', 'wb').close()      # the HAL reads this one, the test feeds it in real time
    feed.write(f'iio:device{i + 3} {hz} 24\n')
    out = open(f'{root}/src/iio:device{i + 3}', 'wb')
    n = int(secs * hz)
    for s in range(n):
        t = s / hz
        R = rot(t)
        if ty == 'accel':
            A = tmv(R, [0, 0, G])
            A = [a + random.gauss(0, 0.02) for a in A]
        elif ty == 'anglvel':
            w = omega(t)
            A = [w[k] + GBIAS[k] + random.gauss(0, 0.002) for k in range(3)]
        else:
            b = tmv(R, BW)
            A = [(b[k] + HARD[k] + random.gauss(0, 0.3)) / 100.0 for k in range(3)]   # gauss
        raw = (A[1], A[0], -A[2])
        tick = (TICK0 + int(round(t * 32768))) & 0xffffffff
        out.write(struct.pack('<iii', *[int(round(v * 65536)) for v in raw]) + b'\0' * 4 + struct.pack('<Q', tick))
    out.close()
if '--stk' in args:
    d = f'{root}/sys/iio:device7'
    os.makedirs(d + '/events', exist_ok=True)
    open(d + '/name', 'w').write('stk3338\n')
    open(d + '/in_illuminance_raw', 'w').write('100\n')
    open(d + '/in_illuminance_scale', 'w').write('0.5\n')
    open(d + '/in_proximity_raw', 'w').write('40\n')
    open(d + '/events/in_proximity_thresh_rising_value', 'w').write('600\n')
    for e in ('rising', 'falling'):   # r5 F3: the HAL drives the PS interrupt enable; driver default = enabled
        open(d + f'/events/in_proximity_thresh_{e}_en', 'w').write('1\n')
feed.close()
json.dump({'hard_iron_uT': HARD, 'field_world_uT': BW, 'gyro_bias': GBIAS, 'secs': secs, 'mode': mode},
          open(root + '/expected.json', 'w'))
print('FAKE_OK', root, mode, [d[0] for d in devs])
