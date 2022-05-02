// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/gpio/driver.h>
#include <linux/firmware/siklu-cpld.h>

#define DRV_NAME "siklu-cpld-gpio"

static DEFINE_MUTEX(siklu_cpld_gpio_mutex);

static int siklu_cpld_gpio_get(struct gpio_chip *gc, unsigned offset)
{
	struct device *cpld_dev = gpiochip_get_data(gc);
	u8 reg_val;

	siklu_cpld_reg_read(cpld_dev, R_CPLD_LOGIC_GPIO, &reg_val);

	return !!(reg_val & (1 << offset));
}

static void siklu_cpld_gpio_set(struct gpio_chip *gc, unsigned offset, int value)
{
	struct device *cpld_dev = gpiochip_get_data(gc);
	u8 reg_val;

	mutex_lock(&siklu_cpld_gpio_mutex);
	siklu_cpld_reg_read(cpld_dev, R_CPLD_LOGIC_GPIO, &reg_val);
	if (value)
		reg_val |= (1 << offset);
	else
		reg_val &= ~(1 << offset);
	siklu_cpld_reg_write(cpld_dev, R_CPLD_LOGIC_GPIO, reg_val);
	mutex_unlock(&siklu_cpld_gpio_mutex);
}

static int siklu_cpld_gpio_get_dir(struct gpio_chip *gc, unsigned int gpio)
{
	return GPIO_LINE_DIRECTION_OUT;
}

static int siklu_cpld_gpio_probe(struct platform_device *pdev)
{
	struct device *cpld_dev = pdev->dev.parent;
	struct gpio_chip *gc;

	gc = devm_kzalloc(&pdev->dev, sizeof(*gc), GFP_KERNEL);
	if (gc == NULL)
		return -ENOMEM;

	gc->ngpio = 8;
	gc->base = -1;
	gc->label = DRV_NAME;
	gc->owner = THIS_MODULE;
	gc->get_direction = siklu_cpld_gpio_get_dir;
	gc->get = siklu_cpld_gpio_get;
	gc->set = siklu_cpld_gpio_set;

	return devm_gpiochip_add_data(&pdev->dev, gc, cpld_dev);
}

static const struct of_device_id siklu_cpld_gpio_of_match[] = {
	{ .compatible = "siklu,eh8010-cpld-gpio", },
	{},
};
MODULE_DEVICE_TABLE(of, siklu_cpld_gpio_of_match);

static struct platform_driver siklu_cpld_gpio_driver = {
	.probe	= siklu_cpld_gpio_probe,
	.driver	= {
		.name		= DRV_NAME,
		.of_match_table	= siklu_cpld_gpio_of_match,
	},
};
module_platform_driver(siklu_cpld_gpio_driver);
