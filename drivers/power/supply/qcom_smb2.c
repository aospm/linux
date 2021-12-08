// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021, Caleb Connolly <caleb.connolly@linaro.org>
 * 
 * This driver is for the switch-mode battery charger and boost
 * hardware found in pmi8998 and related PMICs.
 */

#include <linux/bits.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
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
#include <linux/spmi.h>
#include <linux/workqueue.h>

#include "qcom_spmi_pmic.h"

/*
 * All registers are relative to the smb2 base which is 0x1000 aka CHGR_BASE in downstream
 */

#define CHARGING_ENABLE_CMD_REG				0x42
#define CHARGING_ENABLE_CMD_BIT				BIT(0)

#define CHGR_CFG2_REG					0x51
#define CHG_EN_SRC_BIT					BIT(7)
#define CHG_EN_POLARITY_BIT				BIT(6)
#define PRETOFAST_TRANSITION_CFG_BIT			BIT(5)
#define BAT_OV_ECC_BIT					BIT(4)
#define I_TERM_BIT					BIT(3)
#define AUTO_RECHG_BIT					BIT(2)
#define EN_ANALOG_DROP_IN_VBATT_BIT			BIT(1)
#define CHARGER_INHIBIT_BIT				BIT(0)

#define FAST_CHARGE_CURRENT_CFG_REG			0x61
#define FAST_CHARGE_CURRENT_SETTING_MASK		GENMASK(7, 0)

#define FLOAT_VOLTAGE_CFG_REG				0x70
#define FLOAT_VOLTAGE_SETTING_MASK			GENMASK(7, 0)

#define FG_UPDATE_CFG_2_SEL_REG				0x7D
#define SOC_LT_OTG_THRESH_SEL_BIT			BIT(3)
#define SOC_LT_CHG_RECHARGE_THRESH_SEL_BIT		BIT(2)
#define VBT_LT_CHG_RECHARGE_THRESH_SEL_BIT		BIT(1)
#define IBT_LT_CHG_TERM_THRESH_SEL_BIT			BIT(0)

#define JEITA_EN_CFG_REG				0x90
#define JEITA_EN_HARDLIMIT_BIT				BIT(4)
#define JEITA_EN_HOT_SL_FCV_BIT				BIT(3)
#define JEITA_EN_COLD_SL_FCV_BIT			BIT(2)
#define JEITA_EN_HOT_SL_CCC_BIT				BIT(1)
#define JEITA_EN_COLD_SL_CCC_BIT			BIT(0)

#define INT_RT_STS					0x310
#define TYPE_C_CHANGE_RT_STS_BIT			BIT(7)
#define USBIN_ICL_CHANGE_RT_STS_BIT			BIT(6)
#define USBIN_SOURCE_CHANGE_RT_STS_BIT			BIT(5)
#define USBIN_PLUGIN_RT_STS_BIT				BIT(4)
#define USBIN_OV_RT_STS_BIT				BIT(3)
#define USBIN_UV_RT_STS_BIT				BIT(2)
#define USBIN_LT_3P6V_RT_STS_BIT			BIT(1)
#define USBIN_COLLAPSE_RT_STS_BIT			BIT(0)

#define BATTERY_CHARGER_STATUS_1_REG			0x06
#define BVR_INITIAL_RAMP_BIT				BIT(7)
#define CC_SOFT_TERMINATE_BIT				BIT(6)
#define STEP_CHARGING_STATUS_SHIFT			3
#define STEP_CHARGING_STATUS_MASK			GENMASK(5, 3)
#define BATTERY_CHARGER_STATUS_MASK			GENMASK(2, 0)

#define BATTERY_HEALTH_STATUS_REG			0x07


#define OTG_CFG_REG					0x153
#define OTG_RESERVED_MASK				GENMASK(7, 6)
#define DIS_OTG_ON_TLIM_BIT				BIT(5)
#define QUICKSTART_OTG_FASTROLESWAP_BIT			BIT(4)
#define INCREASE_DFP_TIME_BIT				BIT(3)
#define ENABLE_OTG_IN_DEBUG_MODE_BIT			BIT(2)
#define OTG_EN_SRC_CFG_BIT				BIT(1)
#define CONCURRENT_MODE_CFG_BIT				BIT(0)

#define OTG_ENG_OTG_CFG_REG				0x1C0
#define ENG_BUCKBOOST_HALT1_8_MODE_BIT			BIT(0)

#define APSD_STATUS_REG					0x307
#define APSD_STATUS_7_BIT				BIT(7)
#define HVDCP_CHECK_TIMEOUT_BIT				BIT(6)
#define SLOW_PLUGIN_TIMEOUT_BIT				BIT(5)
#define ENUMERATION_DONE_BIT				BIT(4)
#define VADP_CHANGE_DONE_AFTER_AUTH_BIT			BIT(3)
#define QC_AUTH_DONE_STATUS_BIT				BIT(2)
#define QC_CHARGER_BIT					BIT(1)
#define APSD_DTC_STATUS_DONE_BIT			BIT(0)

#define APSD_RESULT_STATUS_REG				0x308
#define ICL_OVERRIDE_LATCH_BIT				BIT(7)
#define APSD_RESULT_STATUS_MASK				GENMASK(6, 0)
#define QC_3P0_BIT					BIT(6)
#define QC_2P0_BIT					BIT(5)
#define FLOAT_CHARGER_BIT				BIT(4)
#define DCP_CHARGER_BIT					BIT(3)
#define CDP_CHARGER_BIT					BIT(2)
#define OCP_CHARGER_BIT					BIT(1)
#define SDP_CHARGER_BIT					BIT(0)

#define TYPE_C_STATUS_1_REG				0x30B
#define UFP_TYPEC_MASK					GENMASK(7, 5)
#define UFP_TYPEC_RDSTD_BIT				BIT(7)
#define UFP_TYPEC_RD1P5_BIT				BIT(6)
#define UFP_TYPEC_RD3P0_BIT				BIT(5)
#define UFP_TYPEC_FMB_255K_BIT				BIT(4)
#define UFP_TYPEC_FMB_301K_BIT				BIT(3)
#define UFP_TYPEC_FMB_523K_BIT				BIT(2)
#define UFP_TYPEC_FMB_619K_BIT				BIT(1)
#define UFP_TYPEC_OPEN_OPEN_BIT				BIT(0)

#define TYPE_C_STATUS_2_REG				0x30C
#define DFP_RA_OPEN_BIT					BIT(7)
#define TIMER_STAGE_BIT					BIT(6)
#define EXIT_UFP_MODE_BIT				BIT(5)
#define EXIT_DFP_MODE_BIT				BIT(4)
#define DFP_TYPEC_MASK					GENMASK(3, 0)
#define DFP_RD_OPEN_BIT					BIT(3)
#define DFP_RD_RA_VCONN_BIT				BIT(2)
#define DFP_RD_RD_BIT					BIT(1)
#define DFP_RA_RA_BIT					BIT(0)

#define TYPE_C_STATUS_3_REG				0x30D
#define ENABLE_BANDGAP_BIT				BIT(7)
#define U_USB_GND_NOVBUS_BIT				BIT(6)
#define U_USB_FLOAT_NOVBUS_BIT				BIT(5)
#define U_USB_GND_BIT					BIT(4)
#define U_USB_FMB1_BIT					BIT(3)
#define U_USB_FLOAT1_BIT				BIT(2)
#define U_USB_FMB2_BIT					BIT(1)
#define U_USB_FLOAT2_BIT				BIT(0)

#define TYPE_C_STATUS_4_REG				0x30E
#define UFP_DFP_MODE_STATUS_BIT				BIT(7)
#define TYPEC_VBUS_STATUS_BIT				BIT(6)
#define TYPEC_VBUS_ERROR_STATUS_BIT			BIT(5)
#define TYPEC_DEBOUNCE_DONE_STATUS_BIT			BIT(4)
#define TYPEC_UFP_AUDIO_ADAPT_STATUS_BIT		BIT(3)
#define TYPEC_VCONN_OVERCURR_STATUS_BIT			BIT(2)
#define CC_ORIENTATION_BIT				BIT(1)
#define CC_ATTACHED_BIT					BIT(0)

#define TYPE_C_STATUS_5_REG				0x30F
#define TRY_SOURCE_FAILED_BIT				BIT(6)
#define TRY_SINK_FAILED_BIT				BIT(5)
#define TIMER_STAGE_2_BIT				BIT(4)
#define TYPEC_LEGACY_CABLE_STATUS_BIT			BIT(3)
#define TYPEC_NONCOMP_LEGACY_CABLE_STATUS_BIT		BIT(2)
#define TYPEC_TRYSOURCE_DETECT_STATUS_BIT		BIT(1)
#define TYPEC_TRYSINK_DETECT_STATUS_BIT			BIT(0)

#define CMD_APSD_REG					0x341
#define ICL_OVERRIDE_BIT				BIT(1)
#define APSD_RERUN_BIT					BIT(0)

#define TYPE_C_CFG_REG					0x358
#define APSD_START_ON_CC_BIT				BIT(7)
#define WAIT_FOR_APSD_BIT				BIT(6)
#define FACTORY_MODE_DETECTION_EN_BIT			BIT(5)
#define FACTORY_MODE_ICL_3A_4A_BIT			BIT(4)
#define FACTORY_MODE_DIS_CHGING_CFG_BIT			BIT(3)
#define SUSPEND_NON_COMPLIANT_CFG_BIT			BIT(2)
#define VCONN_OC_CFG_BIT				BIT(1)
#define TYPE_C_OR_U_USB_BIT				BIT(0)

#define TYPE_C_CFG_2_REG				0x359
#define TYPE_C_DFP_CURRSRC_MODE_BIT			BIT(7)
#define DFP_CC_1P4V_OR_1P6V_BIT				BIT(6)
#define VCONN_SOFTSTART_CFG_MASK			GENMASK(5, 4)
#define EN_TRY_SOURCE_MODE_BIT				BIT(3)
#define USB_FACTORY_MODE_ENABLE_BIT			BIT(2)
#define TYPE_C_UFP_MODE_BIT				BIT(1)
#define EN_80UA_180UA_CUR_SOURCE_BIT			BIT(0)

#define TYPE_C_CFG_3_REG				0x35A
#define TVBUS_DEBOUNCE_BIT				BIT(7)
#define TYPEC_LEGACY_CABLE_INT_EN_BIT			BIT(6)
#define TYPEC_NONCOMPLIANT_LEGACY_CABLE_INT_EN_BIT	BIT(5)
#define TYPEC_TRYSOURCE_DETECT_INT_EN_BIT		BIT(4)
#define TYPEC_TRYSINK_DETECT_INT_EN_BIT			BIT(3)
#define EN_TRYSINK_MODE_BIT				BIT(2)
#define EN_LEGACY_CABLE_DETECTION_BIT			BIT(1)
#define ALLOW_PD_DRING_UFP_TCCDB_BIT			BIT(0)

#define USBIN_OPTIONS_1_CFG_REG				0x362
#define CABLE_R_SEL_BIT					BIT(7)
#define HVDCP_AUTH_ALG_EN_CFG_BIT			BIT(6)
#define HVDCP_AUTONOMOUS_MODE_EN_CFG_BIT		BIT(5)
#define INPUT_PRIORITY_BIT				BIT(4)
#define AUTO_SRC_DETECT_BIT				BIT(3)
#define HVDCP_EN_BIT					BIT(2)
#define VADP_INCREMENT_VOLTAGE_LIMIT_BIT		BIT(1)
#define VADP_TAPER_TIMER_EN_BIT				BIT(0)

#define USBIN_OPTIONS_2_CFG_REG				0x363
#define WIPWR_RST_EUD_CFG_BIT				BIT(7)
#define SWITCHER_START_CFG_BIT				BIT(6)
#define DCD_TIMEOUT_SEL_BIT				BIT(5)
#define OCD_CURRENT_SEL_BIT				BIT(4)
#define SLOW_PLUGIN_TIMER_EN_CFG_BIT			BIT(3)
#define FLOAT_OPTIONS_MASK				GENMASK(2, 0)
#define FLOAT_DIS_CHGING_CFG_BIT			BIT(2)
#define SUSPEND_FLOAT_CFG_BIT				BIT(1)
#define FORCE_FLOAT_SDP_CFG_BIT				BIT(0)

#define TAPER_TIMER_SEL_CFG_REG				0x364
#define TYPEC_SPARE_CFG_BIT				BIT(7)
#define TYPEC_DRP_DFP_TIME_CFG_BIT			BIT(5)
#define TAPER_TIMER_SEL_MASK				GENMASK(1, 0)

#define USBIN_LOAD_CFG_REG				0x365
#define USBIN_OV_CH_LOAD_OPTION_BIT			BIT(7)
#define ICL_OVERRIDE_AFTER_APSD_BIT			BIT(4)

#define USBIN_ICL_OPTIONS_REG				0x366
#define CFG_USB3P0_SEL_BIT				BIT(2)
#define USB51_MODE_BIT					BIT(1)
#define USBIN_MODE_CHG_BIT				BIT(0)

#define TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG		0x368
#define EXIT_SNK_BASED_ON_CC_BIT			BIT(7)
#define VCONN_EN_ORIENTATION_BIT			BIT(6)
#define TYPEC_VCONN_OVERCURR_INT_EN_BIT			BIT(5)
#define VCONN_EN_SRC_BIT				BIT(4)
#define VCONN_EN_VALUE_BIT				BIT(3)
#define TYPEC_POWER_ROLE_CMD_MASK			GENMASK(2, 0)
#define UFP_EN_CMD_BIT					BIT(2)
#define DFP_EN_CMD_BIT					BIT(1)
#define TYPEC_DISABLE_CMD_BIT				BIT(0)

#define USBIN_CURRENT_LIMIT_CFG_REG			0x370
#define USBIN_CURRENT_LIMIT_MASK			GENMASK(7, 0)

#define USBIN_AICL_OPTIONS_CFG_REG			0x380
#define SUSPEND_ON_COLLAPSE_USBIN_BIT			BIT(7)
#define USBIN_AICL_HDC_EN_BIT				BIT(6)
#define USBIN_AICL_START_AT_MAX_BIT			BIT(5)
#define USBIN_AICL_RERUN_EN_BIT				BIT(4)
#define USBIN_AICL_ADC_EN_BIT				BIT(3)
#define USBIN_AICL_EN_BIT				BIT(2)
#define USBIN_HV_COLLAPSE_RESPONSE_BIT			BIT(1)
#define USBIN_LV_COLLAPSE_RESPONSE_BIT			BIT(0)

#define DC_ENG_SSUPPLY_CFG2_REG				0x4C1
#define ENG_SSUPPLY_IVREF_OTG_SS_MASK			GENMASK(2, 0)
#define OTG_SS_SLOW					0x3

#define DCIN_AICL_REF_SEL_CFG_REG			0x481
#define DCIN_CONT_AICL_THRESHOLD_CFG_MASK		GENMASK(5, 0)

#define WI_PWR_OPTIONS_REG				0x495
#define CHG_OK_BIT					BIT(7)
#define WIPWR_UVLO_IRQ_OPT_BIT				BIT(6)
#define BUCK_HOLDOFF_ENABLE_BIT				BIT(5)
#define CHG_OK_HW_SW_SELECT_BIT				BIT(4)
#define WIPWR_RST_ENABLE_BIT				BIT(3)
#define DCIN_WIPWR_IRQ_SELECT_BIT			BIT(2)
#define AICL_SWITCH_ENABLE_BIT				BIT(1)
#define ZIN_ICL_ENABLE_BIT				BIT(0)


// In the MISC_BASE range, +0x300 from downstream to be relative to charger

#define ICL_STATUS_REG					0x607
#define INPUT_CURRENT_LIMIT_MASK			GENMASK(7, 0)

#define POWER_PATH_STATUS_REG				0x60B
#define P_PATH_INPUT_SS_DONE_BIT			BIT(7)
#define P_PATH_USBIN_SUSPEND_STS_BIT			BIT(6)
#define P_PATH_DCIN_SUSPEND_STS_BIT			BIT(5)
#define P_PATH_USE_USBIN_BIT				BIT(4)
#define P_PATH_USE_DCIN_BIT				BIT(3)
#define P_PATH_POWER_PATH_MASK				GENMASK(2, 1)
#define P_PATH_VALID_INPUT_POWER_SOURCE_STS_BIT		BIT(0)

#define WD_CFG_REG					0x651
#define WATCHDOG_TRIGGER_AFP_EN_BIT			BIT(7)
#define BARK_WDOG_INT_EN_BIT				BIT(6)
#define BITE_WDOG_INT_EN_BIT				BIT(5)
#define SFT_AFTER_WDOG_IRQ_MASK				GENMASK(4, 3)
#define WDOG_IRQ_SFT_BIT				BIT(2)
#define WDOG_TIMER_EN_ON_PLUGIN_BIT			BIT(1)
#define WDOG_TIMER_EN_BIT				BIT(0)

#define AICL_RERUN_TIME_CFG_REG				0x661
#define AICL_RERUN_TIME_MASK				GENMASK(1, 0)

// These are written to the PMIC at offset 0x1000, 0x300 before the smb2 charger block

// Hardcoded values

#define SDP_CURRENT_UA					500000
#define CDP_CURRENT_UA					1500000
#define DCP_CURRENT_UA					1500000
#define HVDCP_CURRENT_UA				3000000
#define TYPEC_DEFAULT_CURRENT_UA			900000
#define TYPEC_MEDIUM_CURRENT_UA				1500000
#define TYPEC_HIGH_CURRENT_UA				3000000

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
	struct qcom_spmi_pmic *pmic;
	struct delayed_work icl_work; // Current limit work
	struct power_supply_battery_info batt_info;

	struct smb_iio iio;

	struct power_supply *chg_psy;
	struct power_supply *otg_psy;

	bool usb_present;
	unsigned char float_cfg;
};

static enum power_supply_property smb2_properties[] = {
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_USB_TYPE,
};

static enum power_supply_usb_type smb2_usb_types[] = {
	POWER_SUPPLY_USB_TYPE_SDP,
	POWER_SUPPLY_USB_TYPE_DCP,
	POWER_SUPPLY_USB_TYPE_CDP,
	POWER_SUPPLY_USB_TYPE_C,
	POWER_SUPPLY_USB_TYPE_PD_DRP
};

/**
 * @brief smb2_read() - Read multiple registers with regmap_bulk_read
 *
 * @param chip Pointer to chip
* @param val Pointer to read values into
 * @param addr Address to read from
 * @return int 0 on success, negative errno on error
 */
static int smb2_read(struct smb2_chip *chip, unsigned char *val, unsigned short addr)
{
	unsigned int temp;
	int rc;
	if ((addr & 0xff00) == 0)
		return -EINVAL;

	rc = regmap_read(chip->regmap, addr, &temp);
	if (rc >= 0)
		*val = (unsigned char)temp;

	//dev_info(chip->dev, "%s: Read val = 0x%x from addr = 0x%x", __func__, temp, addr);
	return rc;
}

/**
 * @brief smb2_write() - Write multiple registers with regmap_bulk_write
 *
 * @param chip Pointer to chip
 * @param val Pointer to write values from
 * @param addr Address to write to
 * @param len Number of registers (bytes) to write
 * @return int 0 on success, negative errno on error
 */
static int smb2_write(struct smb2_chip *chip, unsigned short addr, unsigned char val)
{
	bool sec_access = (addr & 0xff) > 0xd0;
	static unsigned char sec_addr_val = 0xa5;
	int ret;

	if ((addr & 0xff00) == 0)
			return -EINVAL;

	//dev_info(chip->dev, "%s: Writing val = 0x%x to addr = 0x%x", __func__, val, addr);

	if (sec_access) {
		ret = regmap_bulk_write(chip->regmap,
				(addr & 0xff00) | 0xd0,
				&sec_addr_val, 1);
		if (ret)
			return ret;
	}

	return regmap_bulk_write(chip->regmap, addr, &val, 1);
}

/**
 * @brief smb2_write_masked() - Write a register with a mask
 *
 * @param chip Pointer to chip
 * @param val Pointer to write values from
 * @param addr Address to write to
 * @param len Number of registers (bytes) to write
 * @return int 0 on success, negative errno on error
 */
static int smb2_write_masked(struct smb2_chip *chip, unsigned short addr, unsigned char mask, unsigned char val)
{
	bool sec_access = (addr & 0xff) > 0xd0;
	static unsigned char sec_addr_val = 0xa5;
	int ret;

	if ((addr & 0xff00) == 0)
			return -EINVAL;

	//dev_info(chip->dev, "%s: Writing val = 0x%x to addr = 0x%x, mask = 0x%x",
	//	__func__, val, addr, mask);

	if (sec_access) {
		ret = regmap_bulk_write(chip->regmap,
				(addr & 0xff00) | 0xd0,
				&sec_addr_val, 1);
		if (ret)
			return ret;
	}

	return regmap_update_bits(chip->regmap, addr, mask, val);
}


static void smb2_rerun_apsd(struct smb2_chip *chip)
{
	int rc;

	rc = smb2_write_masked(chip, chip->base + CMD_APSD_REG,
				APSD_RERUN_BIT, APSD_RERUN_BIT);
	if (rc < 0)
		dev_err(chip->dev, "Couldn't re-run APSD rc=%d\n", rc);
}

// qcom "automatic power source detection" aka APSD
// tells us what type of charger we're connected to.
static int smb2_apsd_get_charger_type(struct smb2_chip *chip, int* val) {
	int rc;
	unsigned char apsd_stat, stat;

	rc = smb2_read(chip, &apsd_stat, chip->base + APSD_STATUS_REG);
	if (rc < 0) {
		dev_err(chip->dev, "Failed to read apsd status, rc = %d", rc);
		return rc;
	}
	//dev_info(chip->dev, "APSD_STATUS = 0x%02x\n", apsd_stat);
	if (!(apsd_stat & APSD_DTC_STATUS_DONE_BIT)) {
		dev_err(chip->dev, "Apsd not ready");
		return -EAGAIN;
	}

	rc = smb2_read(chip, &stat, chip->base + APSD_RESULT_STATUS_REG);
	if (rc < 0) {
		dev_err(chip->dev, "Failed to read apsd result, rc = %d", rc);
		return rc;
	}

	stat &= APSD_RESULT_STATUS_MASK;

	/*
	 * SDP is a standard PC port, 500mA for usb 2.0, 900mA for usb 3.0
	 * CDP is a standard PC port which supports a high current mode, up to 1.5A
	 * DCP is a wall charger, up to 1.5A
	 */
	if (stat & CDP_CHARGER_BIT) {
		*val = POWER_SUPPLY_USB_TYPE_CDP;
	} else if (stat & (DCP_CHARGER_BIT|OCP_CHARGER_BIT|FLOAT_CHARGER_BIT)) {
		*val = POWER_SUPPLY_USB_TYPE_DCP;
	} else { // SDP_CHARGER_BIT (or others)
		*val = POWER_SUPPLY_USB_TYPE_SDP;
	}

	return 0;
}

int smb2_get_prop_usb_online(struct smb2_chip *chip, int *val) {
	unsigned char stat;
	int rc;

	rc = smb2_read(chip, &stat, chip->base + POWER_PATH_STATUS_REG);
	if (rc < 0){
		dev_err(chip->dev, "Couldn't read POWER_PATH_STATUS! ret=%d\n", rc);
		return rc;
	}

	//dev_info(chip->dev, "USB POWER_PATH_STATUS : 0x%02x\n", stat);
	*val = (stat & P_PATH_USE_USBIN_BIT)
		&& (stat & P_PATH_VALID_INPUT_POWER_SOURCE_STS_BIT);
	return rc;
}

int smb2_get_prop_status(struct smb2_chip *chip, int *val) {
	int usb_online_val;
	unsigned char stat;
	int rc;
	bool usb_online;

	rc = smb2_get_prop_usb_online(chip, &usb_online_val);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't get usb online property rc = %d\n", rc);
		return rc;
	}
	//dev_info(chip->dev, "USB ONLINE val : %d\n", usb_online_val);
	usb_online = (bool)usb_online_val;

	if (!usb_online) {
		*val = POWER_SUPPLY_STATUS_DISCHARGING;
		return rc;
	}

	rc = smb2_read(chip, &stat, chip->base + BATTERY_CHARGER_STATUS_1_REG);
	if (rc < 0){
		dev_err(chip->dev, "Charging status REGMAP read failed! ret=%d\n", rc);
		return rc;
	}

	stat = stat & BATTERY_CHARGER_STATUS_MASK;
	//dev_info(chip->dev, "Charging status : %d!\n", stat);

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

static inline int smb2_get_current_limit(struct smb2_chip *chip, unsigned int *val) {
	int rc = smb2_read(chip, (unsigned char*)val, chip->base + ICL_STATUS_REG);
	// ICL is in 25mA increments
	if (rc >= 0)
		*val *= 25000;
	return rc;
}

static inline int smb2_set_current_limit(struct smb2_chip *chip, unsigned int val) {
	unsigned char val_raw;
	if (val > 4800000) {
		dev_err(chip->dev, "Can't set current limit higher than 4800000uA");
		return -EINVAL;
	}
	val_raw = val / 25000;
	return smb2_write(chip, chip->base + USBIN_CURRENT_LIMIT_CFG_REG, val_raw);
}

// This currently assumes we are UFP
int smb2_get_current_max(struct smb2_chip *chip,
				int *max_current)
{
	int rc = 0;
	unsigned int charger_type, hw_current_limit, current_ua;
	bool non_compliant;
	unsigned char val;
	int usb_online;
	int count;

	smb2_get_prop_usb_online(chip, &usb_online);

	if (usb_online == 0) { // USB is not online so just get the programmed limit
		smb2_get_current_limit(chip, max_current);
		return 0;
	}

	for (count = 0; count < 10; count++) {
		rc = smb2_apsd_get_charger_type(chip, &charger_type);
		if (rc >= 0)
			break;
		msleep(100);
	}

	if (rc < 0) {
		dev_err(chip->dev, "Failed to read APSD, rerun, rc=%d", rc);
		smb2_rerun_apsd(chip);
		return -EAGAIN;
	}
	

	rc = smb2_read(chip, &val, chip->base + TYPE_C_STATUS_5_REG);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't read TYPE_C_STATUS_5 rc=%d\n", rc);
		return rc;
	}
	non_compliant = (val & TYPEC_NONCOMP_LEGACY_CABLE_STATUS_BIT) > 0;
	if (non_compliant)
		dev_info(chip->dev, "Charger is non-compliant"); // TODO: Remove before submitting

	/* get settled ICL */
	rc = smb2_get_current_limit(chip, &hw_current_limit);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't get settled ICL rc=%d\n", rc);
		return rc;
	}
	//dev_info(chip->dev, "apsd charger type = %d, hw current limit = %u", charger_type, hw_current_limit);

	/* QC 2.0/3.0 adapter */
	// if (apsd_result->bit & (QC_3P0_BIT | QC_2P0_BIT)) {
	// 	*max_current = HVDCP_CURRENT_UA;
	// 	return 0;
	// }

	switch (charger_type) {
	case POWER_SUPPLY_USB_TYPE_CDP:
		//dev_info(chip->dev, "%s(): charger_type = %s", __func__,
		//"POWER_SUPPLY_USB_TYPE_CDP");
		current_ua = CDP_CURRENT_UA;
		break;
	case POWER_SUPPLY_USB_TYPE_DCP:
		//dev_info(chip->dev, "%s(): charger_type = %s", __func__,
		//"POWER_SUPPLY_USB_TYPE_DCP");
		current_ua = DCP_CURRENT_UA;
		break;
	case POWER_SUPPLY_USB_TYPE_SDP:
		//dev_info(chip->dev, "%s(): charger_type = %s", __func__,
		//"POWER_SUPPLY_USB_TYPE_SDP");
		current_ua = SDP_CURRENT_UA;
		break;
	default:
		//dev_info(chip->dev, "%s(): charger_type = %s", __func__,
		//"default");
		current_ua = 0;
		break;
	}

	*max_current = max(current_ua, hw_current_limit);
	return 0;
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
	//dev_info(chip->dev, "%s(): iio_read_channel_processed rc = %d, val = %d", __func__, rc, *val);
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
	return rc;
}

static int smb2_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct smb2_chip *chip = power_supply_get_drvdata(psy);
	int error = 0;

	//dev_info(chip->dev, "Getting property: %d", psp);

	switch (psp) {
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "Qualcomm";
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "SMB2 Charger";
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		error = smb2_get_current_max(chip, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		error = smb2_get_current(chip, &val->intval);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		error = smb2_get_voltage(chip, &val->intval);
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		error = smb2_get_prop_usb_online(chip, &val->intval);
		break;
	case POWER_SUPPLY_PROP_STATUS:
		error = smb2_get_prop_status(chip, &val->intval);
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		error = smb2_apsd_get_charger_type(chip, &val->intval);
		break;
	default:
		dev_err(chip->dev, "invalid property: %d\n", psp);
		return -EINVAL;
	}
	return error;
}

static int smb2_set_property(struct power_supply *psy,
	enum power_supply_property psp,
	const union power_supply_propval *val)
{
	struct smb2_chip *chip = power_supply_get_drvdata(psy);
	int error = 0;

	dev_info(chip->dev, "Setting property: %d", psp);

	mutex_lock(&chip->lock);

	switch (psp) {
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		error = smb2_set_current_limit(chip, val->intval);
		break;
	default:
		dev_err(chip->dev, "No setter for property: %d\n", psp);
		error = -EINVAL;
	}

	mutex_unlock(&chip->lock);

	return error;
}

static int smb2_property_is_writable(struct power_supply *psy,
	enum power_supply_property psp)
{
	switch(psp) {
		case POWER_SUPPLY_PROP_CURRENT_MAX:
			return 1;
		default:
			return 0;
	}
}

irqreturn_t smb2_handle_usb_plugin(int irq, void *data){
	struct smb2_chip *chip = data;
	int rc;
	unsigned char intrt_stat; 
	unsigned char usbc_stat;
	unsigned char icl_options;
	union power_supply_propval psval;

	rc = smb2_read(chip, &intrt_stat, chip->base + INT_RT_STS);
	if (rc < 0){
		dev_err(chip->dev, "Couldn't read USB status from reg! ret=%d\n", rc);
		return rc;
	}

	rc = smb2_read(chip, &usbc_stat, chip->base + TYPE_C_CFG_REG);
	if (rc < 0){
		dev_err(chip->dev, "Couldn't read USB status from reg! ret=%d\n", rc);
		return rc;
	}

	chip->usb_present = (bool)(intrt_stat & USBIN_PLUGIN_RT_STS_BIT);
	if (usbc_stat & TYPE_C_OR_U_USB_BIT) {

	}

	//dev_info(chip->dev, "USB IRQ: %s\n", chip->usb_present ? "attached" : "detached");
	power_supply_changed(chip->chg_psy);

	if (chip->usb_present) {
		// Give 50ms for power supply to settle, then set the current limit
		// TODO: is this needed??
		//schedule_delayed_work(&chip->icl_work, msecs_to_jiffies(50));
	}

	// rc = power_supply_get_property(chip->chg_psy, POWER_SUPPLY_PROP_USB_TYPE, &psval);
	// switch(psval.intval) {
	// 	case POWER_SUPPLY_USB_TYPE_SDP:
	// 		icl_options = USB51_MODE_BIT;

	// }

	return IRQ_HANDLED;
}

// Delayed work to set the correct current limit after cable attach
static void smb2_current_limit_work(struct work_struct *work) {
	struct smb2_chip *chip =
		container_of(work, struct smb2_chip, icl_work.work);
	int rc;
	union power_supply_propval val;

	rc = power_supply_get_property(chip->chg_psy,
		POWER_SUPPLY_PROP_CURRENT_MAX, &val);
	if (rc < 0) {
		dev_err(chip->dev, "%s: failed to get max current", __func__);
		return;
	}

	mutex_lock(&chip->lock);
	rc = smb2_set_current_limit(chip, val.intval);
	if (rc < 0)
		dev_err(chip->dev,
			"%s: failed to write max current (%d)",
			__func__, val.intval);
	mutex_unlock(&chip->lock);
	return;
}

static const struct power_supply_desc smb2_psy_desc = {
	.name = "usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.usb_types = smb2_usb_types,
	.num_usb_types = ARRAY_SIZE(smb2_usb_types),
	.properties = smb2_properties,
	.num_properties = ARRAY_SIZE(smb2_properties),
	.get_property = smb2_get_property,
	.set_property = smb2_set_property,
	.property_is_writeable = smb2_property_is_writable,
};

static const struct regulator_ops smb2_chg_otg_ops = {
	.enable = NULL,
	.disable = NULL,
	.is_enabled = NULL,
};

static const struct regulator_desc otg_reg_desc = {
	.name = "otg-vbus",
	.ops = &smb2_chg_otg_ops,
	.owner = THIS_MODULE,
	.type = REGULATOR_VOLTAGE,
	.supply_name = "usb-otg-in",
	.of_match = "otg-vbus",
};

static int smb2_init_hw(struct smb2_chip *chip) {
	int rc;
	int val;
	unsigned char blah;

	/**
	 * 
	 * 
	 * 	TODO:::: copy smb2_post_init()
	 * 
	 */

	mutex_lock(&chip->lock);

	/* set a slower soft start setting for OTG */
	// rc = smb2_write_masked(chip, chip->base + DC_ENG_SSUPPLY_CFG2_REG,
	// 			ENG_SSUPPLY_IVREF_OTG_SS_MASK, OTG_SS_SLOW);
	// if (rc < 0) {
	// 	dev_err(chip->dev, "Couldn't set otg soft start rc=%d\n", rc);
	// 	goto out;
	// }

	/* set a AICL THR for DCIN  */
	// rc = smb2_write(chip, chip->base + DCIN_AICL_REF_SEL_CFG_REG, 0x3);
	// if (rc < 0) {
	// 	pr_err("Couldn't write DCIN_AICL_REF_SEL_CFG_REG\n");
	// 	goto out;
	// }

	/* disable qcom wipower*/
	// rc = smb2_write(chip, chip->base + WI_PWR_OPTIONS_REG, 0);
	// if (rc < 0) {
	// 	pr_err("Couldn't disable qcom wipower\n");
	// 	goto out;
	// }

	/* aicl rerun time */
	rc = smb2_write_masked(chip, chip->base + AICL_RERUN_TIME_CFG_REG,
		AICL_RERUN_TIME_MASK, 0);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't set aicl rerun time rc = %d\n", rc);
		goto out;
	}

	/*
	 * AICL configuration:
	 * start from min and AICL ADC disable (???)
	 */
	rc = smb2_write_masked(chip, chip->base + USBIN_AICL_OPTIONS_CFG_REG,
			USBIN_AICL_START_AT_MAX_BIT
				| USBIN_AICL_ADC_EN_BIT, 0);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't configure AICL rc = %d\n", rc);
		goto out;
	}

	// By default configure us as an upstream facing port
	rc = smb2_write_masked(chip, chip->base + TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
		TYPEC_POWER_ROLE_CMD_MASK,
		UFP_EN_CMD_BIT);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't configure TYPE-C UFP rc = %d\n", rc);
		goto out;
	}

	/*
	 * disable Type-C factory mode and stay in Attached.SRC state when VCONN
	 * over-current happens
	 */
	rc = smb2_write_masked(chip, chip->base + TYPE_C_CFG_REG,
			FACTORY_MODE_DETECTION_EN_BIT | VCONN_OC_CFG_BIT, 0);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't configure Type-C rc = %d\n", rc);
		goto out;
	}

	/* increase VCONN softstart */
	// rc = smb2_write_masked(chip, chip->base + TYPE_C_CFG_2_REG,
	// 		VCONN_SOFTSTART_CFG_MASK, VCONN_SOFTSTART_CFG_MASK);
	// if (rc < 0) {
	// 	dev_err(chip->dev, "Couldn't increase VCONN softstart rc = %d\n",
	// 		rc);
	// 	goto out;
	// }

	/* disable try.SINK mode and legacy cable IRQs */
	// Maybe we do want to enable try sink mode?
	// rc = smb2_write_masked(chip, chip->base + TYPE_C_CFG_3_REG,
	// 	/*EN_TRYSINK_MODE_BIT | */ TYPEC_NONCOMPLIANT_LEGACY_CABLE_INT_EN_BIT |
	// 	TYPEC_LEGACY_CABLE_INT_EN_BIT, 0);
	// if (rc < 0) {
	// 	dev_err(chip->dev, "Couldn't set Type-C config rc = %d\n", rc);
	// 	goto out;
	//}

	/* Set CC threshold to 1.6 V in source mode */
	// rc = smb2_write_masked(chip, chip->base + TYPE_C_CFG_2_REG, DFP_CC_1P4V_OR_1P6V_BIT,
	// 			 DFP_CC_1P4V_OR_1P6V_BIT);
	// if (rc < 0) {
	// 	dev_err(chip->dev,
	// 		"Couldn't configure CC threshold voltage rc = %d\n", rc);
	// 	goto out;
	// }

	/* configure VCONN for software control */
	rc = smb2_write_masked(chip, chip->base + TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				 VCONN_EN_SRC_BIT | VCONN_EN_VALUE_BIT,
				 VCONN_EN_SRC_BIT);
	if (rc < 0) {
		dev_err(chip->dev,
			"Couldn't configure VCONN for SW control rc = %d\n", rc);
		goto out;
	}

	/* configure VBUS for software control */
	rc = smb2_write_masked(chip, chip->base + OTG_CFG_REG, OTG_EN_SRC_CFG_BIT, 0);
	if (rc < 0) {
		dev_err(chip->dev,
			"Couldn't configure VBUS for SW control rc = %d\n", rc);
		goto out;
	}

	/*
	 * allow DRP.DFP time to exceed by tPDdebounce time.
	 */
	// rc = smb2_write_masked(chip, chip->base + TAPER_TIMER_SEL_CFG_REG,
	// 			TYPEC_DRP_DFP_TIME_CFG_BIT,
	// 			TYPEC_DRP_DFP_TIME_CFG_BIT);
	// if (rc < 0) {
	// 	dev_err(chip->dev, "Couldn't configure DRP.DFP time rc = %d\n",
	// 		rc);
	// 	goto out;
	// }

	// This register is modified on USB connect, but on disconnect we want to
	// write back the default.
	// rc = smb2_read(chip, &chip->float_cfg, chip->base + USBIN_OPTIONS_2_CFG_REG);
	// if (rc < 0) {
	// 	dev_err(chip->dev, "Couldn't read float charger options rc = %d\n",
	// 		rc);
	// 	goto out;
	// }

	// need to find out what this is, soc might be state of charge?
	// The flag is called "auto_recharge_soc" and is set by default
	rc = smb2_write_masked(chip, chip->base + FG_UPDATE_CFG_2_SEL_REG,
		SOC_LT_CHG_RECHARGE_THRESH_SEL_BIT |
		VBT_LT_CHG_RECHARGE_THRESH_SEL_BIT,
		VBT_LT_CHG_RECHARGE_THRESH_SEL_BIT); // Or SOC_LT_CHG_RECHARGE_THRESH_SEL_BIT ?
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't configure FG_UPDATE_CFG2_SEL_REG rc = %d\n",
			rc);
		goto out;
	}

	// We can disable HW jeita but, we don't
	// rc = smb2_write_masked(chip, chip->base + JEITA_EN_CFG_REG,
	// 	JEITA_EN_COLD_SL_FCV_BIT
	// 	| JEITA_EN_HOT_SL_FCV_BIT
	// 	| JEITA_EN_HOT_SL_CCC_BIT
	// 	| JEITA_EN_COLD_SL_CCC_BIT, 0);
	// if (rc < 0) {
	// 	dev_err(chip->dev, "Couldn't disable hw jeita rc = %d\n",
	// 		rc);
	// 	goto out;
	// }

	// Now override the max current :>

	/* enforce override */
	rc = smb2_write_masked(chip, chip->base + USBIN_ICL_OPTIONS_REG,
		USBIN_MODE_CHG_BIT, USBIN_MODE_CHG_BIT);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't override the current limit rc = %d\n",
			rc);
		goto out;
	}

	// Write "charge current limit"
	smb2_set_current_limit(chip, 1950 * 1000);
	rc = smb2_write_masked(chip, chip->base + CMD_APSD_REG,
		ICL_OVERRIDE_BIT, ICL_OVERRIDE_BIT);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't set fast charge current limit rc = %d\n",
			rc);
		goto out;
	}

	// Set max vbat
	val = chip->batt_info.voltage_max_design_uv;
	rc = smb2_write_masked(chip, chip->base + FLOAT_VOLTAGE_CFG_REG,
		(unsigned char)(val / 7500), FLOAT_VOLTAGE_SETTING_MASK);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't set vbat max rc = %d\n",
			rc);
		goto out;
	}

	rc = smb2_read(chip, &blah, chip->base + FLOAT_VOLTAGE_CFG_REG);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't read float voltage cfg rc = %d\n",
			rc);
		goto out;
	}
	//dev_info(chip->dev, "FLOAT_VOLTAGE_CFG_REG = 0x%02x\n", blah);

	rc = smb2_write_masked(chip, chip->base + USBIN_AICL_OPTIONS_CFG_REG,
		USBIN_AICL_EN_BIT, 0);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't disable AICL rc = %d\n",
			rc);
		goto out;
	}

	/* Disable HVCDP (i'm guessing support for 9/12v chargers?) 
	  these probably require extra hw on OnePlus devices
	*/
	rc = smb2_write_masked(chip, chip->base + USBIN_OPTIONS_1_CFG_REG,
		HVDCP_EN_BIT, 0);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't disable hvdcp rc = %d\n",
			rc);
		goto out;
	}

	rc = smb2_write_masked(chip, chip->base + CHARGING_ENABLE_CMD_REG,
		CHARGING_ENABLE_CMD_BIT, CHARGING_ENABLE_CMD_BIT);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't enable charging rc = %d\n",
			rc);
		goto out;
	}
	
	rc = smb2_write_masked(chip, chip->base + USBIN_LOAD_CFG_REG,
				ICL_OVERRIDE_AFTER_APSD_BIT,
				ICL_OVERRIDE_AFTER_APSD_BIT);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't override ICL rc = %d\n", rc);
		goto out;
	}
	
	/* Configure charge enable for software control; active high */
	rc = smb2_write_masked(chip, chip->base + CHGR_CFG2_REG,
				 CHG_EN_POLARITY_BIT |
				 CHG_EN_SRC_BIT, 0);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't configure charger rc = %d\n", rc);
		goto out;
	}


	// POST INIT

	rc = smb2_write_masked(chip, chip->base + TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				 UFP_EN_CMD_BIT, UFP_EN_CMD_BIT);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't configure as upstream facing port rc = %d\n", rc);
		goto out;
	}
out:
	mutex_unlock(&chip->lock);
	return rc;
}

static int smb2_probe(struct platform_device *pdev)
{
	struct power_supply_config supply_config = {};
	struct smb2_chip *chip;
	struct spmi_device *sdev;
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
	//dev_info(chip->dev, "chip->base = 0x%x", chip->base);

	supply_config.drv_data = chip;
	supply_config.of_node = pdev->dev.of_node;

	chip->chg_psy = devm_power_supply_register(chip->dev,
			&smb2_psy_desc, &supply_config);
	if (IS_ERR(chip->chg_psy)) {
		dev_err(&pdev->dev, "failed to register power supply\n");
		return PTR_ERR(chip->chg_psy);
	}

	sdev = to_spmi_device(pdev->dev.parent);

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
		dev_err(chip->dev, "Couldn't request irq %d\n", irq);
		return rc;
	}

	INIT_DELAYED_WORK(&chip->icl_work, smb2_current_limit_work);

	rc = power_supply_get_battery_info(chip->chg_psy, &chip->batt_info);
	if (rc) {
		dev_err(&pdev->dev, "Failed to get battery info: %d\n", rc);
		return rc;
	}

	rc = smb2_init_hw(chip);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't init hw %d\n", irq);
		return rc;
	}

	return 0;
}

static int smb2_remove(struct platform_device *pdev)
{
	struct smb2_chip *chip = platform_get_drvdata(pdev);
	cancel_delayed_work(&chip->icl_work);
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
		.name = "qcom-spmi-smb2",
		.of_match_table = fg_match_id_table,
	},
};

module_platform_driver(qcom_fg_driver);

MODULE_AUTHOR("Caleb Connolly <caleb.connolly@linaro.org>");
MODULE_DESCRIPTION("Qualcomm SMB2 Charger Driver");
MODULE_LICENSE("GPL v2");
