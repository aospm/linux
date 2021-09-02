// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021, Caleb Connolly <caleb.connolly@linaro.org>
 */

// The registers are grouped up like this, these addresses are relative to
// the pmic. The registers as noted in qcom_smb2.c are all relative and include
// the offsets from here.
#define CHGR_BASE				0x1000
#define OTG_BASE				0x1100
#define BATIF_BASE				0x1200
#define USBIN_BASE				0x1300
#define DCIN_BASE				0x1400
#define MISC_BASE				0x1600
#define REG_BASE				0x4000
#define REG_BATT				0x4100
#define REG_MEM					0x4400

enum wa_flags {
	PMI8998_V1_REV_WA,
	PMI8998_V2_REV_WA,
};

enum pmi8998_rev_offsets {
	DIG_MINOR = 0x0,
	DIG_MAJOR = 0x1,
	ANA_MINOR = 0x2,
	ANA_MAJOR = 0x3,
};
enum pmi8998_rev {
	DIG_REV_1 = 0x1,
	DIG_REV_2 = 0x2,
	DIG_REV_3 = 0x3,
};


