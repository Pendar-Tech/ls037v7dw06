/*
 * Copyright (C) 2018, Pendar Technologies LLC.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <linux/i2c.h>
#include <linux/of_platform.h>
#include <linux/backlight.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>

#define LS037V7DW06_SLEEP_OFF 0x10
#define LS037V7DW06_SLEEP_ON  0x11
#define LS037V7DW06_DISP_OFF  0x28
#define LS037V7DW06_DISP_ON   0x29

struct panel_ls037v7dw06 {
	bool enabled;

	unsigned long driver_data;

	struct i2c_client *client;

	struct regulator *supply;

	struct gpio_desc *resb_gpio;
	struct gpio_desc *clk_gpio;
	struct gpio_desc *i2ciso_gpio;
	struct backlight_device *backlight;
};

static const struct i2c_device_id panel_id[] = {
	{"sharp,ls037v7dw06", 0, },
	{ }
};

static int sharp_ls_enable(struct panel_ls037v7dw06 *panel)
{
	int retval;
	// Start by setting RESB to low.
	gpiod_set_value_cansleep(panel->resb_gpio, 0);

	gpiod_set_value_cansleep(panel->i2ciso_gpio, 0);

	// Enable VDD.
	retval = regulator_enable(panel->supply);
	if(retval != 0)
		return retval;

	// Wait for the panel to power up.
	msleep(10);

	// Set RESB to high.
	gpiod_set_value_cansleep(panel->resb_gpio, 1);

	// Wait for the panel to act on the RESB state change.
	msleep(1);

	// Exit Sleep mode.
	retval = i2c_smbus_write_byte_data(panel->client, 
	                                   LS037V7DW06_SLEEP_ON, 0x00);
	if(retval != 0)
		return retval;

	// Let the panel wake up.
	msleep(100);

	// Turn on the display.
	retval = i2c_smbus_write_byte_data(panel->client, 
	                                   LS037V7DW06_DISP_ON, 0x00);
	if(retval != 0)
		return retval;

	// Start transmitting all signals.
	gpiod_set_value_cansleep(panel->clk_gpio, 1);

	// Turn on the backlight.
	panel->backlight->props.state &= ~BL_CORE_FBBLANK;
	panel->backlight->props.power = FB_BLANK_UNBLANK;
	panel->backlight->props.brightness = panel->backlight->props.max_brightness;
	backlight_update_status(panel->backlight);

	// Mark the panel as enabled.
	panel->enabled = true;

	return retval;
}

static int sharp_ls_disable(struct panel_ls037v7dw06 *panel)
{
	int retval;

	// Turn off the backlight.
	panel->backlight->props.power = FB_BLANK_POWERDOWN;
	panel->backlight->props.state |= BL_CORE_FBBLANK;
	panel->backlight->props.brightness = 0;
	backlight_update_status(panel->backlight);

	// Stop transmitting all signals.
	gpiod_set_value_cansleep(panel->clk_gpio, 0);

	// Wait 1ms before continuing.
	msleep(1);

	// Turn off the display.
	retval = i2c_smbus_write_byte_data(panel->client, 
	                                   LS037V7DW06_DISP_OFF, 0x00);
	if(retval != 0)
		return retval;

	// Wait >1 frame period before continuing.
	msleep(20);

	retval = i2c_smbus_write_byte_data(panel->client, 
	                                   LS037V7DW06_SLEEP_OFF, 0x00);
	if(retval != 0)
		return retval;

	// Let the panel shut down.
	msleep(100);

	// Set RESB to low.
	gpiod_set_value_cansleep(panel->resb_gpio, 0);

	// Wait 1ms before continuing.
	msleep(1);

	// Disable VDD.
	retval = regulator_disable(panel->supply);
	if(retval != 0)
		return retval;

	return retval;
}

static const struct of_device_id sharp_ls_dt_ids[] = {
	{ .compatible = "sharp,ls037v7dw06", },
	{ }
};

MODULE_DEVICE_TABLE(of, sharp_ls_dt_ids);

static int sharp_ls_probe(struct i2c_client *client,
		   const struct i2c_device_id *i2c_id)
{
	struct panel_ls037v7dw06 *panel;
	struct device_node *backlight;
	int err;

	if (!i2c_check_functionality(client->adapter,
	    I2C_FUNC_I2C))
		return -EIO;

	panel = devm_kzalloc(&client->dev, sizeof(*panel), GFP_KERNEL);
	if (!panel)
		return -ENOMEM;

	panel->enabled = false;

	panel->client = client;

	panel->supply = devm_regulator_get(&client->dev, "power");
	if (IS_ERR(panel->supply))
		return PTR_ERR(panel->supply);

	panel->resb_gpio = devm_gpiod_get(&client->dev, "resb",
	                                  GPIOD_OUT_LOW);
	if (IS_ERR(panel->resb_gpio)) {
		err = PTR_ERR(panel->resb_gpio);
		dev_err(&client->dev, "failed to request GPIO: %d\n", err);
		return err;
	}

	panel->clk_gpio = devm_gpiod_get(&client->dev, "clock",
	                                 GPIOD_OUT_LOW);
	if (IS_ERR(panel->clk_gpio)) {
		err = PTR_ERR(panel->clk_gpio);
		dev_err(&client->dev, "failed to request GPIO: %d\n", err);
		return err;
	}

	panel->i2ciso_gpio = devm_gpiod_get(&client->dev, "i2c-iso",
	                                 GPIOD_OUT_LOW);
	if (IS_ERR(panel->i2ciso_gpio)) {
		err = PTR_ERR(panel->i2ciso_gpio);
		dev_err(&client->dev, "failed to request GPIO: %d\n", err);
		return err;
	}
	 
	struct device *dev = &client->dev;

	backlight = of_parse_phandle(dev->of_node, "backlight", 0);
	if (backlight) {
		panel->backlight = of_find_backlight_by_node(backlight);
		of_node_put(backlight);

		if (!panel->backlight)
		return -EPROBE_DEFER;
	}

	if (i2c_id) {
		panel->driver_data = i2c_id->driver_data;
	} else {
		const struct of_device_id *match;

		match = of_match_device(sharp_ls_dt_ids, &client->dev);
		if (match) {
			panel->driver_data = (int)(uintptr_t)match->data;
		}
	}

	i2c_set_clientdata(client, panel);

	err = sharp_ls_enable(panel);

	if(!err)
		dev_info(&client->dev, "Driver Initialized.\n");

	return err;
}

static int sharp_ls_remove(struct i2c_client *client)
{
	struct panel_ls037v7dw06 *panel = i2c_get_clientdata(client);
	int err = sharp_ls_disable(panel);

	if(!err)
		dev_info(&client->dev, "Driver Unloaded.\n");

	return err;
}

static struct i2c_driver sharp_ls_driver = {
	.driver = {
		.name   = "ls037v7dw06",
		.of_match_table = sharp_ls_dt_ids,
	},
	.probe      = sharp_ls_probe,
	.remove     = sharp_ls_remove,
	.id_table   = panel_id,
};

static int __init sharp_ls_init(void)
{
	return i2c_add_driver(&sharp_ls_driver);
}

subsys_initcall(sharp_ls_init);

static void __exit sharp_ls_exit(void)
{
	i2c_del_driver(&sharp_ls_driver);
}
module_exit(sharp_ls_exit);

MODULE_AUTHOR("David Lockhart <dlockhart@pendar.com>");
MODULE_DESCRIPTION("Sharp LS037v7DW06 LCD panel driver");
MODULE_LICENSE("GPL");
