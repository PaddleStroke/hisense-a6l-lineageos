#!/usr/bin/env bash
# audio3 (24 Sep 2026): make the TFA9894 codec "optional" for the sound card. Run AFTER apply-a6l-patches.sh on the same
# checkout: apply-a6l-optional-v75.sh <driver checkout dir>
# The sm8250 machine driver binds the card only when EVERY dai-link codec is registered, so a TFA98xx probe failure (amp not
# answering on I2C, unknown revision, IRQ request failure) used to take the headset/mics down with it. Now:
#   * any hardware-side probe failure (not -EPROBE_DEFER/-ENOMEM) registers a silent stub codec DAI instead
#     ("tfa98xx-aif-stub", playback only, no controls) and logs "A6L_TFA_STUB <err>": the card binds, the speaker is mute;
#   * an IRQ request failure is no longer fatal (interrupts are simply skipped, as for the TFA989x parts);
#   * remove() copes with the stub (no driver data).
# What this does NOT cover: the module missing entirely, or the LPI pinctrl without the ter_mi2s functions (pinctrl-0 of the
# amp node then fails before probe). Both are build-time items: ship snd-soc-tfa98xx.ko in the same module set and build the
# kernel with a6l-lpi-ter-mi2s-v74.patch (see docs/audio3-20260924.md).
set -euo pipefail
S=$1/src/tfa98xx.c
python3 - "$S" <<'PY'
import sys, re
p = sys.argv[1]; s = open(p).read()
if 'A6L_TFA_STUB' in s:
    print('already applied'); sys.exit(0)
# 1. rename the real probe
old = 'static int tfa98xx_i2c_probe(struct i2c_client *i2c)\n{'
assert s.count(old) == 1, 'probe signature (run apply-a6l-patches.sh first)'
s = s.replace(old, 'static int tfa98xx_i2c_probe_hw(struct i2c_client *i2c)\n{', 1)
# 2. IRQ request failure -> warning, interrupts skipped
irq_old = re.compile(r'dev_err\(&i2c->dev, "Failed to request IRQ %d: %d\\n",\s*gpio_to_irq\(tfa98xx->irq_gpio\), ret\);\s*return ret;')
assert len(irq_old.findall(s)) == 1, 'irq block'
irq_new = ('dev_warn(&i2c->dev, "A6L: IRQ %d request failed (%d), interrupts skipped\\n",\n'
           '\t\t\t\tgpio_to_irq(tfa98xx->irq_gpio), ret);\n\t\t\ttfa98xx->flags |= TFA98XX_FLAG_SKIP_INTERRUPTS;')
s = irq_old.sub(lambda m: irq_new, s, 1)
# 3. stub + wrapper, inserted right before the remove function
rm = 'static void tfa98xx_i2c_remove(struct i2c_client *i2c)\n{\n\tstruct tfa98xx *tfa98xx = i2c_get_clientdata(i2c);\n'
assert s.count(rm) == 1, 'remove (run apply-a6l-patches.sh first)'
stub = r'''/* A6L (audio3): silent stand-in so that the sound card binds even when the amplifier is unusable. */
static struct snd_soc_dai_driver a6l_tfa_stub_dai[] = {
	{
		.name = "tfa98xx-aif-stub",
		.playback = {
			.stream_name = "AIF Playback",
			.channels_min = 1,
			.channels_max = 4,
			.rates = TFA98XX_RATES,
			.formats = TFA98XX_FORMATS,
		},
	},
};

static const struct snd_soc_component_driver a6l_tfa_stub_component = {
	.name = "tfa98xx-stub",
};

static int tfa98xx_i2c_probe(struct i2c_client *i2c)
{
	int ret = tfa98xx_i2c_probe_hw(i2c);

	if (ret >= 0 || ret == -EPROBE_DEFER || ret == -ENOMEM)
		return ret;
	dev_warn(&i2c->dev, "A6L_TFA_STUB %d: amplifier not usable, registering a silent stub DAI so the card still binds\n", ret);
	i2c_set_clientdata(i2c, NULL);
	return devm_snd_soc_register_component(&i2c->dev, &a6l_tfa_stub_component,
					       a6l_tfa_stub_dai, ARRAY_SIZE(a6l_tfa_stub_dai));
}

'''
s = s.replace(rm, stub + rm + '\n\tif (!tfa98xx)\t/* A6L stub: nothing but the devm-registered component */\n\t\treturn;\n', 1)
open(p, 'w').write(s)
print('A6L optional patch applied')
PY
grep -c "A6L_TFA_STUB" "$S"
