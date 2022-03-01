// SPDX-License-Identifier: GPL-2.0
/*
 * Watchdog driver for Siklu EH8010 CPLD
 *
 * Copyright (C) 2022 Siklu Communication Ltd.
 */

#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/delay.h>
#include <linux/watchdog.h>
#include <linux/firmware/siklu-cpld.h>

#define CPLD_WD_PING	BIT(4)

static int siklu_cpld_wdt_start(struct watchdog_device *wdt_dev)
{
	/* CPLD watchdog starts automatically on boot */
	return 0;
}

static int siklu_cpld_wdt_restart(struct watchdog_device *wdt_dev,
		unsigned long action, void *data)
{
	/*
	 * Write to CPLD over SPI is not allowed since we are in atomic
	 * context. So just wait long enough for the watchdog to trigger.
	 */
	mdelay(15*1000);

	return 0;
}

static int siklu_cpld_wdt_ping(struct watchdog_device *wdt_dev)
{
	struct device *cpld_dev = watchdog_get_drvdata(wdt_dev);
	u8 reg_val;

	siklu_cpld_reg_read(cpld_dev, R_CPLD_LOGIC_WD_RW, &reg_val);
	reg_val ^= CPLD_WD_PING;
	siklu_cpld_reg_write(cpld_dev, R_CPLD_LOGIC_WD_RW, reg_val);

	return 0;
}

static const struct watchdog_info siklu_cpld_wdt_info = {
	.options = WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE,
	.identity = "Siklu EH8010 CPLD Watchdog",
};

static const struct watchdog_ops siklu_cpld_wdt_ops = {
	.owner = THIS_MODULE,
	.start = siklu_cpld_wdt_start,
	.restart = siklu_cpld_wdt_restart,
	.ping = siklu_cpld_wdt_ping,
};

static struct watchdog_device siklu_cpld_wdt_wdd = {
	.info		= &siklu_cpld_wdt_info,
	.ops		= &siklu_cpld_wdt_ops,
	.min_timeout	= 8,
	.timeout	= 8,
	.max_hw_heartbeat_ms = 8*1000,
};

static int siklu_cpld_wdt_probe(struct platform_device *pdev)
{
	siklu_cpld_wdt_wdd.parent = &pdev->dev;
	set_bit(WDOG_HW_RUNNING, &siklu_cpld_wdt_wdd.status);

	watchdog_set_drvdata(&siklu_cpld_wdt_wdd, pdev->dev.parent);

	return devm_watchdog_register_device(&pdev->dev, &siklu_cpld_wdt_wdd);
}

static const struct of_device_id siklu_cpld_wdt_of_match[] = {
	{ .compatible = "siklu,eh8010-cpld-wdt", },
	{},
};

static struct platform_driver siklu_cpld_wdt_driver = {
	.probe	= siklu_cpld_wdt_probe,
	.driver	= {
		.name		= "siklu-cpld-wdt",
		.of_match_table	= siklu_cpld_wdt_of_match,
	},
};
builtin_platform_driver(siklu_cpld_wdt_driver);
