/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2021 Linaro. All rights reserved.
 * Copyright (c) Caleb Connolly <caleb.connolly@linaro.org>
 */

struct qcom_spmi_pmic {
        unsigned int type;
        unsigned int subtype;
        unsigned int major;
        unsigned int minor;
        unsigned int rev2;
        char* name;
};

static inline void print_pmic_info(struct device *dev, struct qcom_spmi_pmic *pmic) {
        dev_info(dev, "%x: %s v%d.%d\n",
                pmic->subtype, pmic->name, pmic->major, pmic->minor);
}
