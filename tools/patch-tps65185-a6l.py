"""A6L patches for the upstream tps65185 regulator driver (hardware findings 21 Sep 2026). usage: patch-tps65185-a6l.py <file>
 1. PWR_GOOD is not readable on TLMM gpio0 on this board (stays low with all rails good, even with pull-up):
    use the PG status register (0x0f == 0xfa: VB, VDDH, VN, VPOS, VEE, VNEG good) and poll it instead of the IRQ.
 3. PG register marked volatile (it was served from the regmap cache).
 2. The chip NAKs until ~ms after WAKEUP goes high: wait before the first I2C access (first probe used to fail ENXIO)."""
import sys
p=sys.argv[1];s=open(p).read()
def rep(a,b):
    global s
    assert s.count(a)==1,a;s=s.replace(a,b)
rep("""	struct tps65185_data *data = rdev_get_drvdata(rdev);

	return gpiod_get_value_cansleep(data->pgood_gpio);
}""","""	struct tps65185_data *data = rdev_get_drvdata(rdev);
	unsigned int pg;

	if (regmap_read(data->regmap, 0x0f, &pg))
		return 0;
	return (pg & 0xfa) == 0xfa;	/* A6L: PG register instead of the unusable PWR_GOOD pin */
}""")
rep("""	wait_for_completion_timeout(&data->pgood_completion,
				    msecs_to_jiffies(PGOOD_TIMEOUT_MSECS));
	dev_dbg(data->dev, "turned on");
	if (gpiod_get_value_cansleep(data->pgood_gpio) != 1)
		return -ETIMEDOUT;
""","""	for (ret = 0; ret < PGOOD_TIMEOUT_MSECS / 5; ret++) {	/* A6L: poll the PG register */
		if (tps65185_check_powergood(rdev) == 1)
			return 0;
		usleep_range(5000, 6000);
	}
	dev_err(data->dev, "A6L: rails did not reach power good\\n");
	gpiod_set_value_cansleep(data->pwrup_gpio, 0);
	return -ETIMEDOUT;
""")
rep("""				     "failed to get wakeup gpio\\n");""","""				     "failed to get wakeup gpio\\n");
	msleep(20);	/* A6L: the chip NAKs right after WAKEUP */""")
rep('''	case TPS65185_REG_TMST1:
		return true;''','''	case TPS65185_REG_TMST1:
	case 0x0f:	/* A6L: PG status must never come from the cache */
		return true;''')
if '#include <linux/delay.h>' not in s:s=s.replace('#include <linux/','#include <linux/delay.h>\n#include <linux/',1)
open(p,'w').write(s)
