// SPDX-License-Identifier: GPL-2.0
/*
 * Reset driver for Siklu EH8010 that uses CPLD
 *
 * Copyright (C) 2022 Siklu Communication Ltd.
 */

#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/firmware/siklu-cpld.h>

#define MODEM_RESET	BIT(6)

/*
 * The following restart handler was an attempt to reboot the system in a clean
 * way. This does not work because SPI transactions require kernel thread
 * context switching, while restart handlers run in atomic context. There is
 * currently (as of v5.15) no support for SPI transactions in atomic context.
 * This is unlike I2C that recently added support for atomic transfers to
 * support this use case (commit 63b96983a5ddf: "i2c: core: introduce callbacks
 * for atomic transfers").
 */
#if 0
static int siklu_cpld_restart_handler(struct notifier_block *nb,
		unsigned long action, void *data)
{
	struct siklu_cpld_reset *cpld_reset =
		container_of(nb, struct siklu_cpld_reset, restart_nb);

	/* Assert all reset lines */
	siklu_cpld_reg_write(cpld_reset->cpld_dev, R_CPLD_LOGIC_RESET_CONTROL,
			0);
	mdelay(500);

	return NOTIFY_DONE;
}
#endif

static int siklu_cpld_reset_probe(struct platform_device *pdev)
{
	struct device *cpld_dev = pdev->dev.parent;
	int ret;
	u8 val;

	/* De-assert modem reset */
	ret = siklu_cpld_reg_read(cpld_dev, R_CPLD_LOGIC_RESET_CONTROL, &val);
	if (ret < 0)
		return ret;
	val |= MODEM_RESET;
	ret = siklu_cpld_reg_write(cpld_dev, R_CPLD_LOGIC_RESET_CONTROL, val);

	return ret;
}

static const struct of_device_id siklu_cpld_reset_of_match[] = {
	{ .compatible = "siklu,eh8010-cpld-reset", },
	{},
};

static struct platform_driver siklu_cpld_reset_driver = {
	.probe	= siklu_cpld_reset_probe,
	.driver	= {
		.name		= "siklu-cpld_reset",
		.of_match_table	= siklu_cpld_reset_of_match,
	},
};
builtin_platform_driver(siklu_cpld_reset_driver);
