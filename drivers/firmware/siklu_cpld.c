// SPDX-License-Identifier: GPL-2.0
/*
 * Provides interface for Siklu EH8010 CPLD registers over SPI bus.
 *
 * Copyright (C) 2022 Siklu Communication Ltd.
 */

#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/firmware/siklu-cpld.h>

#define CPLD_WRITE	0x02
#define CPLD_READ	0x0b

int siklu_cpld_reg_write(struct device *dev, u8 reg, u8 val)
{
	struct spi_device *spi = to_spi_device(dev);
	uint8_t tx_buf[3];

	tx_buf[0] = CPLD_WRITE;
	tx_buf[1] = reg;
	tx_buf[2] = val;

	return spi_write(spi, tx_buf, sizeof(tx_buf));
}

int siklu_cpld_reg_read(struct device *dev, u8 reg, u8 *val)
{
	struct spi_device *spi = to_spi_device(dev);
	uint8_t tx_buf[3];

	tx_buf[0] = CPLD_READ;
	tx_buf[1] = reg;

	return spi_write_then_read(spi, tx_buf, sizeof(tx_buf), val, 1);
}

static int siklu_cpld_probe(struct spi_device *spi)
{
	return devm_of_platform_populate(&spi->dev);
}

static const struct of_device_id siklu_cpld_of_match[] = {
	{ .compatible = "siklu,eh8010-cpld", },
	{},
};

static struct spi_driver siklu_cpld_driver = {
	.driver = {
		.name = "eh8010-cpld",
		.of_match_table = siklu_cpld_of_match,
	},
	.probe	= siklu_cpld_probe,
};
builtin_driver(siklu_cpld_driver, spi_register_driver);
