/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2022 Siklu Communication Ltd.
 */

#ifndef __FIRMWARE_SIKLU_CPLD_H__
#define __FIRMWARE_SIKLU_CPLD_H__

struct device;

/* CPLD registers */
#define R_CPLD_LOGIC_MAJOR		0x00
#define R_CPLD_LOGIC_MINOR_BOARDTYPE	0x01
#define R_CPLD_LOGIC_RESET_CONTROL	0x02
#define R_CPLD_LOGIC_RESET_CAUSE	0x03
#define R_CPLD_LOGIC_MISC_STATUS	0x04
#define R_CPLD_LOGIC_WD_RW		0x05
#define R_CPLD_LOGIC_SFP_MODE		0x07
#define R_CPLD_LOGIC_DIP_MODE		0x08
#define R_CPLD_LOGIC_MODEM_LEDS_CTRL	0x09
#define R_CPLD_LOGIC_ETHERNET_LEDS_CTRL	0x0a
#define R_CPLD_LOGIC_POWER_LEDS_CTRL	0x0b
#define R_CPLD_LOGIC_POWER_STATUS	0x0c
#define R_CPLD_LOGIC_INT_HNDLR_0	0x0e
#define R_CPLD_LOGIC_INT_HNDLR_1	0x0f
#define R_CPLD_LOGIC_INT_HNDLR_0_MASK	0x10
#define R_CPLD_LOGIC_INT_HNDLR_1_MASK	0x11
#define R_CPLD_LOGIC_CPLD_INT_CAUSE_RO	0x12
#define R_CPLD_LOGIC_MISC_0		0x13
#define R_CPLD_LOGIC_MISC_1		0x14
#define R_CPLD_TEST			0x15
#define R_CPLD_LOGIC_GPIO		0x1c
#define R_CPLD_LOGIC_CFG_SEL_MISC	0x1d
#define R_CPLD_LOGIC_HW_ASM_VER		0x23
#define R_CPLD_LOGIC_MISC_2		0x24
#define R_CPLD_LOGIC_SER_EEPROM_IF	0x27
#define R_CPLD_LOGIC_SER_EEPROM_WRITE_DATA 0x2e
#define R_CPLD_LOGIC_SER_EEPROM_CMD	0x2f
#define R_CPLD_LOGIC_SER_EEPROM_ADDR_LSB	0x30
#define R_CPLD_LOGIC_SER_EEPROM_ADDR_MSB	0x31
#define R_CPLD_LOGIC_SER_EEPROM_READ_DATA	0x32
#define R_CPLD_LOGIC_SER_EEPROM_STATUS		0x33
#define R_CPLD_LOGIC_SER_EEPROM_CTL		0x34

extern int siklu_cpld_reg_write(struct device *dev, u8 reg, u8 val);
extern int siklu_cpld_reg_read(struct device *dev, u8 reg, u8 *val);

#endif /* __FIRMWARE_SIKLU_CPLD_H__ */
