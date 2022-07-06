// SPDX-License-Identifier: GPL-2.0
/*
 * Provides interface for Siklu EH8010 CPLD registers over SPI bus.
 *
 * Copyright (C) 2022 Siklu Communication Ltd.
 */

#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/debugfs.h>
#include <linux/spi/spi.h>
#include <linux/firmware/siklu-cpld.h>

#define CPLD_WRITE	0x02
#define CPLD_READ	0x0b

struct debugfs_cpld_reg {
	char *name;
	int reg_id;
	struct device *cpld_dev;
};

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

static int cpld_reg_get(void *data, u64 *val)
{
	struct debugfs_cpld_reg *dc_reg = data;
	u8 reg_val;
	int ret;

	ret = siklu_cpld_reg_read(dc_reg->cpld_dev, dc_reg->reg_id, &reg_val);
	*val = reg_val;

	return ret;
}

static int cpld_reg_set(void *data, u64 val)
{
	struct debugfs_cpld_reg *dc_reg = data;

	return siklu_cpld_reg_write(dc_reg->cpld_dev, dc_reg->reg_id, (u8) val);
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_cpld_reg, cpld_reg_get, cpld_reg_set,
		"0x%02llx\n");

static struct debugfs_cpld_reg siklu_cpld_regs[] = {
	{ "reset_control",		R_CPLD_LOGIC_RESET_CONTROL },
	{ "cfg_sel_misc",		R_CPLD_LOGIC_CFG_SEL_MISC },
	{ "ser_eeprom_addr_lsb",	R_CPLD_LOGIC_SER_EEPROM_ADDR_LSB, },
	{ "ser_eeprom_addr_msb",	R_CPLD_LOGIC_SER_EEPROM_ADDR_MSB, },
	{ "ser_eeprom_status",		R_CPLD_LOGIC_SER_EEPROM_STATUS },
	{ "ser_eeprom_cmd",		R_CPLD_LOGIC_SER_EEPROM_CMD },
	{ "ser_eeprom_ctl",		R_CPLD_LOGIC_SER_EEPROM_CTL },
	{ "ser_eeprom_read_data",	R_CPLD_LOGIC_SER_EEPROM_READ_DATA },
};

static void siklu_cpld_debugfs_init(struct device *dev)
{
	struct dentry *debugfs;
	int i;

	debugfs = debugfs_create_dir("siklu_cpld", NULL);
	if (IS_ERR(debugfs))
		return;

	for (i = 0; i < ARRAY_SIZE(siklu_cpld_regs); i++) {
		siklu_cpld_regs[i].cpld_dev = dev;
		debugfs_create_file_unsafe(siklu_cpld_regs[i].name, 0600,
				debugfs, &siklu_cpld_regs[i], &fops_cpld_reg);
	}
}

static int siklu_cpld_probe(struct spi_device *spi)
{
	siklu_cpld_debugfs_init(&spi->dev);

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
