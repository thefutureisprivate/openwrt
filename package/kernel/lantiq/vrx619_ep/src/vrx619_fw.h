/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VRX619_FW_H
#define _VRX619_FW_H

#include <linux/types.h>

#define VRX619_FW_INFO_SIZE	40
#define VRX619_FW_TABLE_OFFSET	72
#define VRX619_FW_SECTION_SIZE	20

struct vrx619_fw {
	const u8 *data;
	size_t size;
	u32 sections;
	u32 mode;
	u32 table_addr;
	u32 info_addr;
	u32 scratch;
};

typedef int (*vrx619_fw_write_t)(void *ctx, u32 addr, const u8 *data,
			       size_t size);

u32 vrx619_pss_to_dev(u32 addr);
int vrx619_fw_parse(struct vrx619_fw *fw, const u8 *data, size_t size);
int vrx619_fw_upload(const struct vrx619_fw *fw, vrx619_fw_write_t write,
		     void *ctx);
int vrx619_fw_page(const struct vrx619_fw *fw, u32 offset, u32 addr, u32 size,
		   vrx619_fw_write_t write, void *ctx);

#endif
