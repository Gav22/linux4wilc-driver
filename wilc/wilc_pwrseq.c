/*
 *  Copyright (C) 2014 Linaro Ltd
 *
 * Author: Ulf Hansson <ulf.hansson@linaro.org>
 * Author: Michael Walton <mike@farsouthnet.com>
 *
 * License terms: GNU General Public License (GPL) version 2
 *
 *  MMC power sequence for WILC
 */
#include <linux/clk.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/property.h>

#include <linux/mmc/host.h>

#include "pwrseq.h"

struct mmc_pwrseq_wilc {
	struct mmc_pwrseq pwrseq;
	bool clk_enabled;
	u32 post_power_on_delay_ms;
	u32 power_off_delay_us;
	struct clk *ext_clk;
	struct gpio_descs *reset_gpios;
};

#define to_pwrseq_wilc(p) container_of(p, struct mmc_pwrseq_wilc, pwrseq)

static void mmc_pwrseq_wilc_set_gpios_value(struct mmc_pwrseq_wilc *pwrseq,
					      int value, int delay)
{
	struct gpio_descs *reset_gpios = pwrseq->reset_gpios;

	if (!IS_ERR(reset_gpios)) {
		int i;
		int values[reset_gpios->ndescs];

		if (delay) {
			for (i = 0; i < reset_gpios->ndescs; i++)
				values[i] = !value;
		}

		for (i = 0; i < reset_gpios->ndescs; i++) {
			values[i] = value;

			if (delay) {
				dev_info(pwrseq->pwrseq.dev, "item %d -> %d\n", i, value);
				gpiod_set_array_value_cansleep(
					reset_gpios->ndescs, reset_gpios->desc, values);
				mdelay(delay);
			}
		}
		if (!delay) {
			gpiod_set_array_value_cansleep(
				reset_gpios->ndescs, reset_gpios->desc, values);
		}
	}
}

static void mmc_pwrseq_wilc_pre_power_on(struct mmc_host *host)
{
	struct mmc_pwrseq_wilc *pwrseq = to_pwrseq_wilc(host->pwrseq);

	if (!IS_ERR(pwrseq->ext_clk) && !pwrseq->clk_enabled) {
		clk_prepare_enable(pwrseq->ext_clk);
		pwrseq->clk_enabled = true;
	}

	mmc_pwrseq_wilc_set_gpios_value(pwrseq, 1, 0);
}

static void mmc_pwrseq_wilc_post_power_on(struct mmc_host *host)
{
	struct mmc_pwrseq_wilc *pwrseq = to_pwrseq_wilc(host->pwrseq);

	mmc_pwrseq_wilc_set_gpios_value(pwrseq, 0, 5); /* with 5ms delay between activating */

	if (pwrseq->post_power_on_delay_ms)
		msleep(pwrseq->post_power_on_delay_ms);
	else
		msleep(5);
}

static void mmc_pwrseq_wilc_power_off(struct mmc_host *host)
{
	struct mmc_pwrseq_wilc *pwrseq = to_pwrseq_wilc(host->pwrseq);

	mmc_pwrseq_wilc_set_gpios_value(pwrseq, 1, 0);

	if (pwrseq->power_off_delay_us)
		usleep_range(pwrseq->power_off_delay_us,
			2 * pwrseq->power_off_delay_us);

	if (!IS_ERR(pwrseq->ext_clk) && pwrseq->clk_enabled) {
		clk_disable_unprepare(pwrseq->ext_clk);
		pwrseq->clk_enabled = false;
	}
}

static const struct mmc_pwrseq_ops mmc_pwrseq_wilc_ops = {
	.pre_power_on = mmc_pwrseq_wilc_pre_power_on,
	.post_power_on = mmc_pwrseq_wilc_post_power_on,
	.power_off = mmc_pwrseq_wilc_power_off,
};

static const struct of_device_id mmc_pwrseq_wilc_of_match[] = {
	{ .compatible = "mmc-pwrseq-wilc",},
	{/* sentinel */},
};
MODULE_DEVICE_TABLE(of, mmc_pwrseq_wilc_of_match);

static int mmc_pwrseq_wilc_probe(struct platform_device *pdev)
{
	struct mmc_pwrseq_wilc *pwrseq;
	struct device *dev = &pdev->dev;

	pwrseq = devm_kzalloc(dev, sizeof(*pwrseq), GFP_KERNEL);
	if (!pwrseq)
		return -ENOMEM;

	pwrseq->ext_clk = devm_clk_get(dev, "ext_clock");
	if (IS_ERR(pwrseq->ext_clk) && PTR_ERR(pwrseq->ext_clk) != -ENOENT)
		return PTR_ERR(pwrseq->ext_clk);

	pwrseq->reset_gpios = devm_gpiod_get_array(dev, "reset",
							GPIOD_OUT_HIGH);
	if (IS_ERR(pwrseq->reset_gpios) &&
	    PTR_ERR(pwrseq->reset_gpios) != -ENOENT &&
	    PTR_ERR(pwrseq->reset_gpios) != -ENOSYS) {
		return PTR_ERR(pwrseq->reset_gpios);
	}

	device_property_read_u32(dev, "post-power-on-delay-ms",
				 &pwrseq->post_power_on_delay_ms);
	device_property_read_u32(dev, "power-off-delay-us",
				 &pwrseq->power_off_delay_us);

	pwrseq->pwrseq.dev = dev;
	pwrseq->pwrseq.ops = &mmc_pwrseq_wilc_ops;
	pwrseq->pwrseq.owner = THIS_MODULE;
	platform_set_drvdata(pdev, pwrseq);

	return mmc_pwrseq_register(&pwrseq->pwrseq);
}

static int mmc_pwrseq_wilc_remove(struct platform_device *pdev)
{
	struct mmc_pwrseq_wilc *pwrseq = platform_get_drvdata(pdev);

	mmc_pwrseq_unregister(&pwrseq->pwrseq);

	return 0;
}

static struct platform_driver mmc_pwrseq_wilc_driver = {
	.probe = mmc_pwrseq_wilc_probe,
	.remove = mmc_pwrseq_wilc_remove,
	.driver = {
		.name = "pwrseq_wilc",
		.of_match_table = mmc_pwrseq_wilc_of_match,
	},
};

module_platform_driver(mmc_pwrseq_wilc_driver);
MODULE_LICENSE("GPL v2");
