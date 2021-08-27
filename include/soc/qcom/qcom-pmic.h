/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2021 Linaro. All rights reserved.
 * Copyright (c) 2021 Caleb Connolly <caleb.connolly@linaro.org>
 */

#define PM8941_SUBTYPE		0x01
#define PM8841_SUBTYPE		0x02
#define PM8019_SUBTYPE		0x03
#define PM8226_SUBTYPE		0x04
#define PM8110_SUBTYPE		0x05
#define PMA8084_SUBTYPE		0x06
#define PMI8962_SUBTYPE		0x07
#define PMD9635_SUBTYPE		0x08
#define PM8994_SUBTYPE		0x09
#define PMI8994_SUBTYPE		0x0a
#define PM8916_SUBTYPE		0x0b
#define PM8004_SUBTYPE		0x0c
#define PM8909_SUBTYPE		0x0d
#define PM8950_SUBTYPE		0x10
#define PMI8950_SUBTYPE		0x11
#define PM8998_SUBTYPE		0x14
#define PMI8998_SUBTYPE		0x15
#define PM8005_SUBTYPE		0x18
#define PM660L_SUBTYPE		0x1A
#define PM660_SUBTYPE		0x1B


struct qcom_spmi_pmic {
	unsigned int type;
	unsigned int subtype;
	unsigned int major;
	unsigned int minor;
	unsigned int rev2;
	char *name;
};

static inline void print_pmic_info(struct device *dev, struct qcom_spmi_pmic *pmic)
{
	dev_info(dev, "%x: %s v%d.%d\n",
		pmic->subtype, pmic->name, pmic->major, pmic->minor);
}
