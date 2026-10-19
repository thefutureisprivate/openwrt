/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VRX619_ATM_H
#define VRX619_ATM_H

#include <linux/types.h>

#define VRX619_ATM_VCCS 15

struct vrx619_atm_pvc {
	u16 vpi;
	u16 vci;
	u8 type;
	bool llc;
	bool configured;
};

struct vrx619_atm_tables {
	__le16 queues[66];
	__le16 vccs[130];
	__le16 vlans[34];
};

unsigned int vrx619_atm_tables(const struct vrx619_atm_pvc *pvc,
			     struct vrx619_atm_tables *tables);
int vrx619_atm_encapsulate(const struct vrx619_atm_pvc *pvc, u8 *out,
			 size_t size, const u8 *data, size_t len);

#endif
