// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * FocalTech touchscreen driver.
 *
 * Copyright (c) 2010-2017, FocalTech Systems, Ltd., all rights reserved.
 * Copyright (C) 2018 XiaoMi, Inc.
 * Copyright (c) 2021 Caleb Connolly <caleb@connolly.tech>
 * Copyright (c) 2022 Joel Selvaraj <jo@jsfamily.in>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#include <linux/regmap.h>
#include <linux/mutex.h>

#define FTS_REG_CHIP_ID_H 0xA3
#define FTS_REG_CHIP_ID_L 0x9F

#define FTS_MAX_POINTS_SUPPORT 10
#define FTS_ONE_TOUCH_LEN 6

#define FTS_TOUCH_X_H_OFFSET 3
#define FTS_TOUCH_X_L_OFFSET 4
#define FTS_TOUCH_Y_H_OFFSET 5
#define FTS_TOUCH_Y_L_OFFSET 6
#define FTS_TOUCH_PRESSURE_OFFSET 7
#define FTS_TOUCH_AREA_OFFSET 8
#define FTS_TOUCH_TYPE_OFFSET 3
#define FTS_TOUCH_ID_OFFSET 5

#define FTS_TOUCH_DOWN 0
#define FTS_TOUCH_UP 1
#define FTS_TOUCH_CONTACT 2

#define FTS_DRIVER_NAME "fts-i2c"
#define INTERVAL_READ_REG 100 /* unit:ms */
#define TIMEOUT_READ_REG 2000 /* unit:ms */

#define CHIP_TYPE_5452 0x5452
#define CHIP_TYPE_8719 0x8719

struct fts_ts_data {
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct touchscreen_properties prop;

	struct regmap *regmap;
	int irq;
	int resume_irq_counter;

	struct regulator_bulk_data regulators[2];

	/* Touch data*/
	u8 max_touch_number;
	u8 *point_buf;
	int point_buf_size;

	/* DT data */
	struct gpio_desc *reset_gpio;
};

static const struct regmap_config fts_ts_i2c_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static bool fts_chip_is_valid(struct fts_ts_data *data, u16 id)
{
	if (id != CHIP_TYPE_5452 && id != CHIP_TYPE_8719)
		return false;

	return true;
}

int fts_check_status(struct fts_ts_data *data)
{
	int count = 0;
	unsigned int val, id;

	do {
		regmap_read(data->regmap, FTS_REG_CHIP_ID_H, &val);
		id = val << 8;
		regmap_read(data->regmap, FTS_REG_CHIP_ID_L, &val);
		id |= val;

		if (fts_chip_is_valid(data, id)) {
			dev_dbg(&data->client->dev, "TS Ready: Chip ID = 0x%x", id);
			return 0;
		}

		count++;
		msleep(INTERVAL_READ_REG);
	} while ((count * INTERVAL_READ_REG) < TIMEOUT_READ_REG);

	return -EIO;
}

static void fts_report_touch(struct fts_ts_data *data)
{
	struct input_dev *input_dev = data->input_dev;
	int base;
	unsigned int x, y, z, maj;
	u8 slot, type;
	int error, i = 0;
	
	u8 *buf = data->point_buf;

	memset(buf, 0, data->point_buf_size);

	error = regmap_bulk_read(data->regmap, 0, buf, data->point_buf_size);
	if (error) {
		dev_err_ratelimited(&data->client->dev, "I2C read failed: %d\n",
				    error);
		return;
	}

	for (i = 0; i < data->max_touch_number; i++) {
		base = FTS_ONE_TOUCH_LEN * i;

		slot = (buf[base + FTS_TOUCH_ID_OFFSET]) >> 4;
		if (slot >= data->max_touch_number)
			break;

		x = ((buf[base + FTS_TOUCH_X_H_OFFSET] & 0x0F) << 8) +
			 (buf[base + FTS_TOUCH_X_L_OFFSET] & 0xFF);
		y = ((buf[base + FTS_TOUCH_Y_H_OFFSET] & 0x0F) << 8) +
			 (buf[base + FTS_TOUCH_Y_L_OFFSET] & 0xFF);

		z = buf[base + FTS_TOUCH_PRESSURE_OFFSET];
		if (z <= 0)
			z = 0x3f;
		
		maj = buf[base + FTS_TOUCH_AREA_OFFSET] >> 4;
		if (maj <= 0)
			maj = 0x09;
		
		type = buf[base + FTS_TOUCH_TYPE_OFFSET] >> 6;

		input_mt_slot(input_dev, slot);
		if (type == FTS_TOUCH_DOWN || type == FTS_TOUCH_CONTACT) {
			input_mt_report_slot_state(input_dev, MT_TOOL_FINGER, true);
			touchscreen_report_pos(data->input_dev, &data->prop, x, y, true);
			input_report_abs(input_dev, ABS_MT_PRESSURE, z);
			input_report_abs(input_dev, ABS_MT_TOUCH_MAJOR, maj);
			input_report_key(data->input_dev, BTN_TOUCH, 1);
		} else {
			input_report_key(data->input_dev, BTN_TOUCH, 0);
			input_mt_report_slot_inactive(input_dev);
		}
	}
	input_sync(input_dev);
}

static irqreturn_t fts_ts_interrupt(int irq, void *dev_id)
{
	struct fts_ts_data *data = dev_id;

	if (data->resume_irq_counter <= 0)
		fts_report_touch(data);
	else
		data->resume_irq_counter = data->resume_irq_counter - 1;

	return IRQ_HANDLED;
}

static void fts_power_off(void *d)
{
	struct fts_ts_data *data = d;

	regulator_bulk_disable(ARRAY_SIZE(data->regulators),
			       data->regulators);
}

static int fts_start(struct fts_ts_data *data)
{
	int error;

	error = regulator_bulk_enable(ARRAY_SIZE(data->regulators),
				      data->regulators);
	if (error) {
		dev_err(&data->client->dev, "failed to enable regulators\n");
		return error;
	}

	gpiod_set_value_cansleep(data->reset_gpio, 0);
	msleep(200);
	
	enable_irq(data->irq);

	return 0;
}

static int fts_stop(struct fts_ts_data *data)
{
	disable_irq(data->irq);
	gpiod_set_value_cansleep(data->reset_gpio, 1);
	fts_power_off(data);

	return 0;
}

static int fts_input_open(struct input_dev *dev)
{
	struct fts_ts_data *data = input_get_drvdata(dev);
	int error;

	error = fts_start(data);
	if (error)
		return error;

	error = fts_check_status(data);
	if (error) {
		dev_err(&data->client->dev, "Failed to start or unsupported chip");
		return error;
	}

	return 0;
}

static void fts_input_close(struct input_dev *dev)
{
	struct fts_ts_data *data = input_get_drvdata(dev);

	fts_stop(data);
}

static int fts_input_init(struct fts_ts_data *data)
{
	struct device *dev = &data->client->dev;
	struct input_dev *input_dev;
	int error = 0;

	input_dev = devm_input_allocate_device(dev);
	if (!input_dev)
		return -ENOMEM;

	data->input_dev = input_dev;

	/* Init and register Input device */
	input_dev->name = FTS_DRIVER_NAME;
	input_dev->id.bustype = BUS_I2C;
	input_dev->dev.parent = dev;
	input_dev->open = fts_input_open;
	input_dev->close = fts_input_close;

	input_set_drvdata(input_dev, data);

	__set_bit(EV_SYN, input_dev->evbit);
	__set_bit(EV_ABS, input_dev->evbit);
	__set_bit(INPUT_PROP_DIRECT, input_dev->propbit);

	input_mt_init_slots(input_dev, data->max_touch_number,
			    INPUT_MT_DIRECT);
	input_set_capability(input_dev, EV_ABS, ABS_MT_POSITION_X);
	input_set_capability(input_dev, EV_ABS, ABS_MT_POSITION_Y);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_PRESSURE, 0, 255, 0, 0);

	touchscreen_parse_properties(input_dev, true, &data->prop);
	if (!data->prop.max_x || !data->prop.max_y) {
		dev_err(dev,
			"touchscreen-size-x and/or touchscreen-size-y not set in dts\n");
		return -EINVAL;
	}

	data->point_buf_size = (data->max_touch_number * FTS_ONE_TOUCH_LEN) + 3;
	data->point_buf = devm_kzalloc(dev, data->point_buf_size, GFP_KERNEL);
	if (!data->point_buf) {
		dev_err(dev, "Failed to alloc memory for point buffer\n");
		return -ENOMEM;
	}

	error = input_register_device(input_dev);
	if (error) {
		dev_err(dev, "Failed to register input device\n");
		return error;
	}

	return 0;
}

static int fts_parse_dt(struct fts_ts_data *data)
{
	int error = 0;
	struct device *dev = &data->client->dev;
	struct device_node *np = dev->of_node;
	u32 val;

	error = of_property_read_u32(np, "focaltech,max-touch-number", &val);
	if (error) {
		dev_err(dev, "Unable to read property 'focaltech,max-touch-number'");
		return -EINVAL;
	}
	if (val < 2 || val > FTS_MAX_POINTS_SUPPORT) {
		dev_err(dev, "'focaltech,max-touch-number' out of range [2, %d]",
			FTS_MAX_POINTS_SUPPORT);
		return -EINVAL;
	}
	data->max_touch_number = val;

	data->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(data->reset_gpio)) {
		error = PTR_ERR(data->reset_gpio);
		dev_err(dev, "Failed to request reset gpio, error %d\n", error);
		return error;
	}

	return 0;
}

static int fts_ts_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	int error = 0;
	struct fts_ts_data *data;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "I2C not supported");
		return -ENODEV;
	}

	if (!client->irq) {
		dev_err(&client->dev, "No irq specified\n");
		return -EINVAL;
	}

	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	data->resume_irq_counter = 0;

	error = fts_parse_dt(data);
	if (error)
		return error;

	i2c_set_clientdata(client, data);

	data->regmap = devm_regmap_init_i2c(client, &fts_ts_i2c_regmap_config);
	if (IS_ERR(data->regmap)) {
		dev_err(&client->dev, "regmap allocation failed\n");
		return PTR_ERR(data->regmap);
	}

	/* 
	 * AVDD is the analog voltage supply (2.6V to 3.3V)
	 * VDDIO is the digital voltage supply (1.8V)
	 */
	data->regulators[0].supply = "avdd";
	data->regulators[1].supply = "vddio";
	error = devm_regulator_bulk_get(&client->dev, ARRAY_SIZE(data->regulators),
				      data->regulators);
	if (error) {
		dev_err(&client->dev, "Failed to get regulators %d\n", error);
		return error;
	}

	error = devm_add_action_or_reset(&client->dev, fts_power_off, data);
	if (error) {
		dev_err(&client->dev, "failed to install power off handler\n");
		return error;
	}

	error = devm_request_threaded_irq(&client->dev, client->irq, NULL,
					fts_ts_interrupt, IRQF_ONESHOT,
					client->name, data);
	if (error) {
		dev_err(&client->dev, "Failed to request IRQ: %d\n", error);
		return error;
	}

	error = fts_input_init(data);
	if (error)
		return error;

	return 0;
}

static int fts_pm_suspend(struct device *dev)
{
	struct fts_ts_data *data = dev_get_drvdata(dev);

	mutex_lock(&data->input_dev->mutex);

	if (input_device_enabled(data->input_dev))
		fts_stop(data);

	mutex_unlock(&data->input_dev->mutex);

	return 0;
}

static int fts_pm_resume(struct device *dev)
{
	struct fts_ts_data *data = dev_get_drvdata(dev);
	int error = 0;

	mutex_lock(&data->input_dev->mutex);

	data->resume_irq_counter = 3;
	
	if (input_device_enabled(data->input_dev))
		error = fts_start(data);

	mutex_unlock(&data->input_dev->mutex);

	return error;
}

static const struct dev_pm_ops fts_dev_pm_ops = {
	.suspend = fts_pm_suspend,
	.resume = fts_pm_resume,
};

static const struct of_device_id fts_match_table[] = {
	{ .compatible = "focaltech,fts5452", },
	{ .compatible = "focaltech,fts8719", },
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(of, fts_match_table);

static struct i2c_driver fts_ts_driver = {
	.probe = fts_ts_probe,
	.driver = {
		.name = FTS_DRIVER_NAME,
		.pm = &fts_dev_pm_ops,
		.of_match_table = fts_match_table,
	},
};
module_i2c_driver(fts_ts_driver);

MODULE_AUTHOR("Caleb Connolly <caleb@connolly.tech>");
MODULE_AUTHOR("Joel Selvaraj <jo@jsfamily.in>");
MODULE_DESCRIPTION("FocalTech touchscreen Driver");
MODULE_LICENSE("GPL v2");
