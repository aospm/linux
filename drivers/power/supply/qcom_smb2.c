// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021, Caleb Connolly <caleb.connolly@linaro.org>
 * 
 * This driver is for the switch-mode battery charger and boost
 * hardware found in pmi8998 and related PMICs.
 */

#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of_address.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/device.h>
#include <linux/iio/consumer.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/power_supply.h>
#include <linux/module.h>
#include <linux/math64.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>

#include "qcom_spmi_pmic.h"

/* Interrupt offsets */
#define INT_RT_STS					0x10
#define TYPE_C_CHANGE_RT_STS_BIT			BIT(7)
#define USBIN_ICL_CHANGE_RT_STS_BIT			BIT(6)
#define USBIN_SOURCE_CHANGE_RT_STS_BIT			BIT(5)
#define USBIN_PLUGIN_RT_STS_BIT				BIT(4)
#define USBIN_OV_RT_STS_BIT				BIT(3)
#define USBIN_UV_RT_STS_BIT				BIT(2)
#define USBIN_LT_3P6V_RT_STS_BIT			BIT(1)
#define USBIN_COLLAPSE_RT_STS_BIT			BIT(0)

#define INT_EN_CLR					0x16

#define BATTERY_CHARGER_STATUS_REG			0x06
#define BATTERY_HEALTH_STATUS_REG			0x07

#define BATTERY_CHARGER_STATUS_MASK 			GENMASK(2, 0)
#define POWER_PATH_STATUS_REG				(MISC_BASE + 0x0B)
#define P_PATH_INPUT_SS_DONE_BIT			BIT(7)
#define P_PATH_USBIN_SUSPEND_STS_BIT			BIT(6)
#define P_PATH_DCIN_SUSPEND_STS_BIT			BIT(5)
#define P_PATH_USE_USBIN_BIT				BIT(4)
#define P_PATH_USE_DCIN_BIT				BIT(3)
#define P_PATH_POWER_PATH_MASK				GENMASK(2, 1)
#define P_PATH_VALID_INPUT_POWER_SOURCE_STS_BIT		BIT(0)

enum charger_status{
	TRICKLE_CHARGE = 0,
	PRE_CHARGE,
	FAST_CHARGE,
	FULLON_CHARGE,
	TAPER_CHARGE,
	TERMINATE_CHARGE,
	INHIBIT_CHARGE,
	DISABLE_CHARGE,
};

struct smb_iio {
	struct iio_channel	*temp_chan;
	struct iio_channel	*temp_max_chan;
	struct iio_channel	*usbin_i_chan;
	struct iio_channel	*usbin_v_chan;
	struct iio_channel	*batt_i_chan;
	struct iio_channel	*connector_temp_chan;
	struct iio_channel	*connector_temp_thr1_chan;
	struct iio_channel	*connector_temp_thr2_chan;
	struct iio_channel	*connector_temp_thr3_chan;
};

struct smb2_chip {
	struct device *dev;
	unsigned int base;
	struct regmap *regmap;
	struct mutex lock;

	struct smb_iio iio;

	struct power_supply *chg_psy;
	struct power_supply *otg_psy;

	bool usb_present;
};

static enum power_supply_property smb2_properties[] = {
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_STATUS,
};

int smb2_get_prop_usb_online(struct smb2_chip *chip, int *val) {
	unsigned int stat;
	int rc;

	rc = regmap_read(chip->regmap, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0){
		dev_err(chip->dev, "Couldn't read POWER_PATH_STATUS! ret=%d\n", rc);
		return rc;
	}

	dev_info(chip->dev, "USB POWER_PATH_STATUS : 0x%02x\n", stat);
	*val = (stat & P_PATH_USE_USBIN_BIT)
		&& (stat & P_PATH_VALID_INPUT_POWER_SOURCE_STS_BIT);
	return rc;
}

int smb2_get_prop_batt_status(struct smb2_chip *chip, int *val) {
	int usb_online_val;
	unsigned int stat;
	int rc;
	bool usb_online;

	rc = smb2_get_prop_usb_online(chip, &usb_online_val);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't get usb online property rc=%d\n", rc);
		return rc;
	}
	dev_info(chip->dev, "USB ONLINE val : %d\n", usb_online_val);
	usb_online = (bool)usb_online_val;

	if (!usb_online) {
		*val = POWER_SUPPLY_STATUS_DISCHARGING;
		return rc;
	}

	rc = regmap_read(chip->regmap, chip->base + BATTERY_CHARGER_STATUS_REG, &stat);
	if (rc < 0){
		dev_err(chip->dev, "Charging status REGMAP read failed! ret=%d\n", rc);
		return rc;
	}

	stat = stat & BATTERY_CHARGER_STATUS_MASK;
	dev_info(chip->dev, "Charging status : %d!\n", stat);

	switch (stat) {
		case TRICKLE_CHARGE:
		case PRE_CHARGE:
		case FAST_CHARGE:
		case FULLON_CHARGE:
		case TAPER_CHARGE:
		case TERMINATE_CHARGE:
		case INHIBIT_CHARGE:
			*val = POWER_SUPPLY_STATUS_CHARGING;
			break;
		case DISABLE_CHARGE:
			*val = POWER_SUPPLY_STATUS_NOT_CHARGING;
			break;
		default:  
			*val = POWER_SUPPLY_STATUS_UNKNOWN;
			break;
	}

	return rc;
}

int smb2_get_current(struct smb2_chip *chip, int *val) {
	int rc = 0;
	union power_supply_propval status;

	rc = power_supply_get_property(chip->chg_psy, POWER_SUPPLY_PROP_STATUS, &status);
	if (rc < 0 || status.intval != POWER_SUPPLY_STATUS_CHARGING) {
		return 0;
	}

	if (!chip->iio.usbin_i_chan ||
		PTR_ERR(chip->iio.usbin_i_chan) == -EPROBE_DEFER)
		chip->iio.usbin_i_chan = iio_channel_get(chip->dev, "usbin_i");

	if (IS_ERR(chip->iio.usbin_i_chan)) {
		dev_err(chip->dev, "Failed to get usbin_i_chan, err = %li", PTR_ERR(chip->iio.usbin_i_chan));
		return PTR_ERR(chip->iio.usbin_i_chan);
	}

	rc = iio_read_channel_processed(chip->iio.usbin_i_chan, val);
	dev_info(chip->dev, "%s(): iio_read_channel_processed rc = %d, val = %d", __func__, rc, *val);
	return rc;
}

int smb2_get_voltage(struct smb2_chip *chip, int *val) {
	int rc;
	union power_supply_propval status;

	rc = power_supply_get_property(chip->chg_psy, POWER_SUPPLY_PROP_STATUS, &status);
	if (rc < 0 || status.intval != POWER_SUPPLY_STATUS_CHARGING) {
		return 0;
	}

	if (!chip->iio.usbin_v_chan ||
		PTR_ERR(chip->iio.usbin_v_chan) == -EPROBE_DEFER)
		chip->iio.usbin_v_chan = iio_channel_get(chip->dev, "usbin_v");
	if (IS_ERR(chip->iio.usbin_v_chan))
		return PTR_ERR(chip->iio.usbin_v_chan);

	rc = iio_read_channel_processed(chip->iio.usbin_v_chan, val);
	*val = *val / 1000;
	return rc;
}

static int smb2_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct smb2_chip *chip = power_supply_get_drvdata(psy);
	int error = 0;

	dev_info(chip->dev, "Getting property: %d", psp);

	switch (psp) {
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "Qualcomm";
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "SMB2 Charger";
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		error = smb2_get_current(chip, &val->intval);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		error = smb2_get_voltage(chip, &val->intval);
		break;
	case POWER_SUPPLY_PROP_STATUS:
		error = smb2_get_prop_batt_status(chip, &val->intval);
		break;
	default:
		dev_err(chip->dev, "invalid property: %d\n", psp);
		return -EINVAL;
	}
	return error;
}

irqreturn_t smb2_handle_usb_plugin(int irq, void *data){
	struct smb2_chip *chip = data;
	int rc;
	unsigned int stat; 

	rc = regmap_read(chip->regmap, USBIN_BASE + INT_RT_STS, &stat);
	if (rc < 0){
		dev_err(chip->dev, "Couldn't read USB status from reg! ret=%d\n", rc);
		return rc;
	}
	chip->usb_present = (bool)(stat & USBIN_PLUGIN_RT_STS_BIT);

	dev_info(chip->dev, "USB IRQ: %s\n", chip->usb_present ? "attached" : "detached");
	power_supply_changed(chip->chg_psy);
	return IRQ_HANDLED;
}

static const struct power_supply_desc smb2_psy_desc = {
	.name = "usb",
	.type = POWER_SUPPLY_TYPE_USB_PD_DRP,
	.properties = smb2_properties,
	.num_properties = ARRAY_SIZE(smb2_properties),
	.get_property = smb2_get_property,
};

// static const struct regulator_ops smb2_chg_otg_ops = {
// 	.enable = NULL,
// 	.disable = NULL,
// 	.is_enabled = NULL,
// };

// static const struct regulator_desc otg_reg_desc = {
// 	.name = "otg-vbus",
// 	.ops = &smb2_chg_otg_ops,
// 	.owner = THIS_MODULE,
// 	.type = REGULATOR_VOLTAGE,
// 	.supply_name = "usb-otg-in",
// 	.of_match = "otg-vbus",
// };

static int smb2_probe(struct platform_device *pdev)
{
	struct power_supply_config supply_config = {};
	struct smb2_chip *chip;
	const __be32 *prop_addr;
	int rc = 0, irq;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip) {
		return -ENOMEM;
	}

	chip->dev = &pdev->dev;
	mutex_init(&chip->lock);

	chip->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!chip->regmap) {
		dev_err(chip->dev, "failed to locate the regmap\n");
		return -ENODEV;
	}

	// Get base address
	prop_addr = of_get_address(pdev->dev.of_node, 0, NULL, NULL);
	if (!prop_addr) {
		dev_err(chip->dev, "Couldn't read SOC base address from dt\n");
		return -EINVAL;
	}
	chip->base = be32_to_cpu(*prop_addr);

	supply_config.drv_data = chip;
	supply_config.of_node = pdev->dev.of_node;

	chip->chg_psy = devm_power_supply_register(chip->dev,
			&smb2_psy_desc, &supply_config);
	if (IS_ERR(chip->chg_psy)) {
		dev_err(&pdev->dev, "failed to register battery\n");
		return PTR_ERR(chip->chg_psy);
	}

	platform_set_drvdata(pdev, chip);

	irq = of_irq_get_byname(pdev->dev.of_node, "usb-plugin");
	if (irq < 0) {
		dev_err(&pdev->dev, "Couldn't get irq usb-plugin byname\n");
		return irq;
	}

	rc = devm_request_threaded_irq(chip->dev, irq, NULL,
					smb2_handle_usb_plugin,
					IRQF_ONESHOT, "usb-plugin", chip);
	if (rc < 0) {
		pr_err("Couldn't request irq %d\n", irq);
		return rc;
	}

	return 0;
}

static int smb2_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id fg_match_id_table[] = {
	{ .compatible = "qcom,pmi8994-smb2" },
	{ .compatible = "qcom,pmi8998-smb2" },
	{ /* sentinal */ }
};
MODULE_DEVICE_TABLE(of, fg_match_id_table);

static struct platform_driver qcom_fg_driver = {
	.probe = smb2_probe,
	.remove = smb2_remove,
	.driver = {
		.name = "qcom-spmi-fg",
		.of_match_table = fg_match_id_table,
	},
};

module_platform_driver(qcom_fg_driver);

MODULE_AUTHOR("Caleb Connolly <caleb.connolly@linaro.org>");
MODULE_DESCRIPTION("Qualcomm SMB2 Charger Driver");
MODULE_LICENSE("GPL v2");
