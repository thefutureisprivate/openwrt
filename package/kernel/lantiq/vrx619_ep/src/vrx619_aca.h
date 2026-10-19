/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VRX619_ACA_H
#define _VRX619_ACA_H

#include <linux/types.h>

#define VRX619_ACA_CHANNELS	8
#define VRX619_ACA_CODE_BLOCKS	5
#define VRX619_ACA_HIF_WORDS	46
#define VRX619_TXOUT_MAGIC	0x886359ac

enum vrx619_ring_id {
	VRX619_TXIN,
	VRX619_TXOUT,
	VRX619_RXIN,
	VRX619_RXOUT,
	VRX619_RING_COUNT,
};

struct vrx619_aca_mem {
	u32 count;
	u32 base;
};

struct vrx619_aca_code {
	const u8 *data;
	u32 size;
	u32 base;
};

struct vrx619_aca_fw {
	struct vrx619_aca_mem host[VRX619_RING_COUNT];
	struct vrx619_aca_mem pd[VRX619_RING_COUNT];
	struct vrx619_aca_code code[VRX619_ACA_CODE_BLOCKS];
	u32 dma[VRX619_ACA_CHANNELS];
	u32 init_addr;
	const u8 *regs;
	u32 reg_count;
	const u8 *sifs;
};

struct vrx619_platform_data;

int vrx619_aca_parse(struct vrx619_aca_fw *fw, const u8 *data, size_t size);
void vrx619_aca_hif(const struct vrx619_aca_fw *fw, u32 bus_addr,
		    const struct vrx619_aca_mem rings[VRX619_RING_COUNT],
		    __le32 hif[VRX619_ACA_HIF_WORDS]);
int vrx619_aca_init(const struct vrx619_platform_data *pdata,
		    const struct vrx619_aca_fw *fw,
		    const __le32 hif[VRX619_ACA_HIF_WORDS]);
int vrx619_aca_start(const struct vrx619_platform_data *pdata,
		     const struct vrx619_aca_fw *fw);
int vrx619_aca_stop(const struct vrx619_platform_data *pdata);

#endif
