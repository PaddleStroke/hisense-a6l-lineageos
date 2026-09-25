#!/usr/bin/env bash
# Port of the NXP/CAF tfa98xx v6.7.14 out-of-tree ASoC driver (github.com/msm8916-mainline/tfa98xx, branch DIN_v6x,
# commit d2cd129) to Linux 7.2. usage: apply-a6l-patches.sh <driver checkout dir>   (idempotent enough for a fresh copy)
set -euo pipefail
T=$1; H=$(cd "$(dirname "$0")" && pwd); S=$T/src/tfa98xx.c
mkdir -p $T/inc/linux; cp $H/a6l_compat.h $T/inc/a6l_compat.h; cp $H/linux/of_gpio.h $T/inc/linux/of_gpio.h
# kbuild: EXTRA_CFLAGS is gone; no -Werror (upstream code has many enum-conversion warnings); no git describe
sed -i 's/^EXTRA_CFLAGS += -I\$(src)\/inc/ccflags-y += -I$(src)\/inc -include $(src)\/inc\/a6l_compat.h/; s/^EXTRA_CFLAGS += -Werror//; s/^EXTRA_CFLAGS += -DTFA98XX_GIT_VERSIONS.*/ccflags-y += -DTFA98XX_GIT_VERSIONS=\\"v6.7.14-a6l\\"/' $T/Makefile
sed -i 's/\.symmetric_rates/.symmetric_rate/; s/\.symmetric_samplebits/.symmetric_sample_bits/' $S
perl -0pi -e 's/(struct kobject \*kobj,\s*)struct bin_attribute \*/$1const struct bin_attribute */g' $S
sed -i 's/snd_soc_component_get_dapm/snd_soc_component_to_dapm/g' $S
perl -0pi -e 's/static int tfa98xx_i2c_probe\(struct i2c_client \*i2c,\s*const struct i2c_device_id \*id\)\s*\{/static int tfa98xx_i2c_probe(struct i2c_client *i2c)\n{\n\tconst struct i2c_device_id *id __maybe_unused = i2c_client_get_device_id(i2c);/' $S
python3 - $S <<'PY'
import sys;p=sys.argv[1];s=open(p).read()
i=s.index('static int tfa98xx_i2c_remove(struct i2c_client *i2c)');j=s.index('\n}\n',i)
b=s[i:j+3].replace('static int tfa98xx_i2c_remove','static void tfa98xx_i2c_remove',1);k=b.rfind('return 0;');b=b[:k]+'return;'+b[k+9:]
s=s[:i]+b+s[j+3:]
# stock A6L DT uses compatible "nxp,tfa98xx"
s=s.replace('\t{.compatible = "tfa,tfa98xx" },','\t{.compatible = "tfa,tfa98xx" },\n\t{.compatible = "nxp,tfa98xx" },',1)
open(p,'w').write(s)
PY
grep -c 'nxp,tfa98xx' $S
