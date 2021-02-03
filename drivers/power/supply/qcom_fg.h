// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2020, The Linux Foundation. All rights reserved. */

/* SOC */
#define BATT_MONOTONIC_SOC		0x009

/* BATT */
#define PARAM_ADDR_BATT_TEMP		0x150
#define BATT_INFO_JEITA_COLD		0x162
#define BATT_INFO_JEITA_COOL		0x163
#define BATT_INFO_JEITA_WARM		0x164
#define BATT_INFO_JEITA_HOT		0x165
#define PARAM_ADDR_BATT_VOLTAGE		0x1a0
#define PARAM_ADDR_BATT_CURRENT		0x1a2

/* MEMIF */
#define MEM_INTF_STS			0x410
#define MEM_INTF_CFG			0x450
#define MEM_INTF_CTL			0x451
#define MEM_INTF_IMA_CFG		0x452
#define MEM_INTF_IMA_EXP_STS		0x455
#define MEM_INTF_IMA_HW_STS		0x456
#define MEM_INTF_IMA_ERR_STS		0x45f
#define MEM_INTF_IMA_BYTE_EN		0x460
#define MEM_INTF_ADDR_LSB		0x461
#define MEM_INTF_RD_DATA0		0x467
#define MEM_INTF_WR_DATA0		0x463
#define MEM_IF_DMA_STS			0x470
#define MEM_IF_DMA_CTL			0x471

/* SRAM addresses */
#define TEMP_THRESHOLD			0x454
#define BATT_TEMP			0x550
#define BATT_VOLTAGE_CURRENT		0x5cc

#define BATT_TEMP_LSB_MASK		GENMASK(7, 0)
#define BATT_TEMP_MSB_MASK		GENMASK(2, 0)

#define BATT_TEMP_JEITA_COLD		100
#define BATT_TEMP_JEITA_COOL		50
#define BATT_TEMP_JEITA_WARM		400
#define BATT_TEMP_JEITA_HOT		450

#define MEM_INTF_AVAIL			BIT(0)
#define MEM_INTF_CTL_BURST		BIT(7)
#define MEM_INTF_CTL_WR_EN		BIT(6)
#define RIF_MEM_ACCESS_REQ		BIT(7)

#define MEM_IF_TIMEOUT_MS		5000
#define SRAM_ACCESS_RELEASE_DELAY_MS	500

struct qcom_fg_chip;

struct qcom_fg_ops {
	int (*get_capacity)(struct qcom_fg_chip *chip, int *);
	int (*get_temperature)(struct qcom_fg_chip *chip, int *);
	int (*get_current)(struct qcom_fg_chip *chip, int *);
	int (*get_voltage)(struct qcom_fg_chip *chip, int *);
	int (*get_temp_threshold)(struct qcom_fg_chip *chip,
			enum power_supply_property psp, int *);
	int (*set_temp_threshold)(struct qcom_fg_chip *chip,
			enum power_supply_property psp, int);
};

struct qcom_fg_chip {
	struct device *dev;
	unsigned int base;
	struct regmap *regmap;
	const struct qcom_fg_ops *ops;

	struct power_supply *batt_psy;
	struct power_supply_battery_info batt_info;

	struct completion sram_access_granted;
	struct completion sram_access_revoked;
	struct workqueue_struct *sram_wq;
	struct delayed_work sram_release_access_work;
	spinlock_t sram_request_lock;
	spinlock_t sram_rw_lock;
	int sram_requests;
};
