# Independent PM660 GPIO11 review

## Conclusion

Prepare a **single explicit PMIC pinctrl state** for PM660 GPIO11 and attach it
to `a6l-buttons`.  The proposed state reproduces the relevant enabled stock
configuration through the pinned mainline driver's supported binding.  It is a
bounded candidate, not a proven fix: V45 did create the correct IRQ, and the
pinned driver normally reads the live PMIC configuration before `gpio-keys`
requests the line.

Do not use `bias-disable` for this pin.  Stock raw `qcom,pull = <0>` means the
PMIC driver's pull value zero, which the pinned driver labels the **30 uA
pull-up**, rather than no pull (no pull is raw value 5).

## Established facts

* Both `stock-00-merged.dts` and `stock-01-merged.dts` enable PM660
  `gpio@ca00`: pin number 11, mode 0, pull 0, VIN/power source 0, source select
  0, and output-strength 1.  The alternate disabled `gpio@ca00` is the
  PM660L instance, not this PM660 key.
* Stock Android independently recorded thirteen complete code-616
  `KEY_LEFT_UP` press/release pairs from `gpio-keys`.  This establishes the
  physical control and intended Linux code.
* V45's `/a6l-buttons/eink-key` is exactly `<&pm660_gpios 11 1>` with code 616.
  The V45 PM660 controller is compatible with `qcom,pm660-gpio` and has
  `#gpio-cells = <2>`.  It has no GPIO11 child/pinctrl configuration state.
* V45 did register `epd_pwr` as `spmi-gpio 10` (the zero-based hwirq) and the
  counter stayed zero during the attended key presses.  The source-numbering
  relation is expected: the driver's GPIO and IRQ translators subtract one
  from the one-based DT specifier, while its debug group is named `gpio11`.
* The only `gpio-reserved-ranges = <8 4>` found in the candidate is beneath
  SDM660 TLMM.  It reserves TLMM GPIOs 8--11 and cannot reserve PM660 GPIO11.
  The PM660 GPIO controller itself has no reservation.

## Driver and binding interpretation

The pinned `pinctrl-spmi-gpio.c` reads mode, VIN, pull, output source/normal
function, and output strength from each PMIC peripheral in `pmic_gpio_populate`
before registering the gpiochip.  A later input request rewrites the complete
cached configuration.  Consequently, V45 normally inherits whatever PMIC
configuration firmware left behind; absence of pinctrl alone does **not** show
that the pin was misconfigured.

The same driver maps the stock raw values as follows:

| Stock raw property | Pinned-driver meaning | Candidate property |
|---|---|---|
| `qcom,mode = <0>` | digital input | `input-enable;` |
| `qcom,vin-sel = <0>` | power source/VIN 0 | `power-source = <0>;` |
| `qcom,pull = <0>` | 30 uA pull-up | `qcom,pull-up-strength = <0>;` |
| `qcom,src-sel = <0>` | normal function | `function = "normal";` |
| `qcom,out-strength = <1>` | raw low strength | `qcom,drive-strength = <3>;` |

`qcom,drive-strength = <3>` is the pinned driver's public value for LOW; it
maps back to raw hardware strength 1.  It is retained for a literal stock
match even though it has no intended electrical effect while the line is an
input.  `status = "ok"` maps to the driver's enabled state when the pinctrl
configuration is applied.

The PMIC GPIO binding permits the PM660 controller and `gpio-reserved-ranges`;
the pin configuration properties are parsed through the driver's generic
pinctrl path plus its declared Qualcomm custom properties.  The existing V45
DT already uses `pinctrl-names`/`pinctrl-0` successfully for the front-touch
device, so attaching a normal device pinctrl state is consistent with the
candidate architecture.

## Exact minimal DT proposal

Add one fragment to `device/hisense/a6l/kernel/a6l-buttons.dtso`, targeting
the existing PM660 GPIO controller, and add the two standard pinctrl references
to the already-created `a6l-buttons` node:

```dts
    fragment@11 {
        target = <&pm660_gpios>;
        __overlay__ {
            a6l_eink_key: a6l-eink-key-state {
                pins = "gpio11";
                function = "normal";
                input-enable;
                power-source = <0>;
                qcom,pull-up-strength = <0>;
                qcom,drive-strength = <3>;
            };
        };
    };
```

Within the existing `a6l-buttons` node, add:

```dts
                pinctrl-names = "default";
                pinctrl-0 = <&a6l_eink_key>;
```

No change is proposed to `gpios = <&pm660_gpios 11 1>`, `linux,code`, debounce,
wakeup, IRQ polarity, PM660 GPIO reservation, or the TLMM reservation.

## What this can and cannot establish

This candidate forces mode, pull, source, function and retained strength to
the observed stock values each time `a6l-buttons` probes.  A successful
post-change physical test with a positive GPIO11 IRQ/event count would support
the configuration hypothesis, but would not prove which individual field was
necessary.  A continued zero count would falsify this combined configuration
as a sufficient repair and point instead to a condition outside these DT
settings, such as a recovery-specific PMIC/power/reset path or a difference in
the physical control exercised.  The existing V45 IRQ allocation rules out a
missing gpiochip, wrong one-based specifier, or TLMM GPIO reservation as the
immediate explanation.

## Sources inspected

* `docs/controls-followup-20260918.md`
* `docs/combined-controls-v45-results-20260918.md`
* `research/controls-followup-20260918/stock-eink-key-result.json`
* `firmware/extracted/stock-dtbo-20260914/stock-00-merged.dts` and
  `stock-01-merged.dts`
* `firmware/extracted/recovery-controls-v45-20260917/base.dtb`, read with
  `tools/a6l_fdt.py`, plus V45 captured events/interrupts
* `device/hisense/a6l/kernel/a6l-buttons.dtso`
* pinned `/home/a6l/kernel/a6l-baseline-7.2` `pinctrl-spmi-gpio.c`,
  `qcom,pmic-gpio.yaml`, and `qcom,pmic-gpio.h`
