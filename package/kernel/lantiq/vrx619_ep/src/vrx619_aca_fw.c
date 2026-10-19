// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "vrx619_aca.h"

#define VRX619_ACA_MAGIC		0x41434132
#define VRX619_ACA_TYPE		0x001f0106
#define VRX619_ACA_HEADER		20
#define VRX619_ACA_SRAM_START	0x100000
#define VRX619_ACA_SRAM_END	0x17fc00

static bool vrx619_aca_sram(u32 base, u32 size)
{
	return !(base & 7) && base >= VRX619_ACA_SRAM_START &&
	       base < VRX619_ACA_SRAM_END &&
	       size <= VRX619_ACA_SRAM_END - base;
}

static int vrx619_aca_mem_parse(struct vrx619_aca_fw *fw, u32 type,
				const u8 *data)
{
	static const u8 ring_id[] = {
		VRX619_TXIN, VRX619_TXIN, VRX619_TXOUT, VRX619_TXOUT,
		VRX619_RXOUT, VRX619_RXOUT, VRX619_RXIN, VRX619_RXIN,
	};
	struct vrx619_aca_mem *mem;
	u32 count = get_unaligned_be32(data);
	u32 base = get_unaligned_be32(data + 4);

	if (!count || count > 1024 || !vrx619_aca_sram(base, count * 8))
		return -EINVAL;
	mem = type & 1 ? fw->pd : fw->host;
	mem += ring_id[type - 4];
	mem->count = count;
	mem->base = base;
	return 0;
}

static int vrx619_aca_code_parse(struct vrx619_aca_fw *fw, const u8 *data,
				 u32 count, const u8 *payload, u32 size)
{
	u32 id, offset, len, base, lower, upper, seen = 0;
	unsigned int i;

	if (count != VRX619_ACA_CODE_BLOCKS)
		return -EINVAL;
	for (i = 0; i < count; i++, data += 16) {
		id = get_unaligned_be32(data);
		offset = get_unaligned_be32(data + 4);
		len = get_unaligned_be32(data + 8);
		base = get_unaligned_be32(data + 12);
		if (!id || id > VRX619_ACA_CODE_BLOCKS || seen & BIT(id))
			return -EINVAL;
		seen |= BIT(id);
		lower = id == 5 ? 0x58000 : 0x50800 + (id - 1) * 0x100;
		upper = id == 5 ? 0x5c000 : lower + 0x100;
		if (!len || ((offset | len | base) & 3) ||
		    offset > size || len > size - offset ||
		    base < lower || base >= upper || len > upper - base)
			return -EINVAL;
		fw->code[id - 1].data = payload + offset;
		fw->code[id - 1].size = len;
		fw->code[id - 1].base = base;
	}
	return 0;
}

static bool vrx619_aca_layout_valid(const struct vrx619_aca_fw *fw)
{
	struct vrx619_aca_mem regions[VRX619_RING_COUNT * 2 + VRX619_ACA_CHANNELS + 1];
	unsigned int i, j, n = 0;

	for (i = 0; i < VRX619_RING_COUNT; i++) {
		regions[n++] = fw->host[i];
		regions[n++] = fw->pd[i];
	}
	for (i = 0; i < VRX619_ACA_CHANNELS; i++)
		regions[n++] = (struct vrx619_aca_mem) { 1, fw->dma[i] };
	regions[n++] = (struct vrx619_aca_mem) {
		VRX619_ACA_HIF_WORDS / 2, fw->init_addr,
	};
	/* Every descriptor occupies at least two words. */
	for (i = 0; i < n; i++)
		for (j = 0; j < i; j++)
			if (regions[i].base < regions[j].base + regions[j].count * 8 &&
			    regions[j].base < regions[i].base + regions[i].count * 8)
				return false;
	return true;
}

/* Validate the complete image before exposing any register or memory writes. */
int vrx619_aca_parse(struct vrx619_aca_fw *fw, const u8 *data, size_t size)
{
	u32 header, payload, sections, pos, type, count, stride, id, base;
	u32 seen = 0, dma_seen = 0, bit;
	const u8 *entry;
	unsigned int i, j;
	int ret;

	memset(fw, 0, sizeof(*fw));
	if (size < VRX619_ACA_HEADER ||
	    get_unaligned_be32(data) != VRX619_ACA_MAGIC ||
	    get_unaligned_be32(data + 4) != VRX619_ACA_TYPE)
		return -EINVAL;
	header = get_unaligned_be32(data + 8);
	payload = get_unaligned_be32(data + 12);
	sections = get_unaligned_be32(data + 16);
	if (header < VRX619_ACA_HEADER || header > size ||
	    payload != size - header || ((header | payload) & 3) ||
	    sections != 13)
		return -EINVAL;
	pos = VRX619_ACA_HEADER;
	for (i = 0; i < sections; i++) {
		if (header - pos < 8)
			return -EINVAL;
		type = get_unaligned_be32(data + pos);
		count = get_unaligned_be32(data + pos + 4);
		pos += 8;
		if (type == 1 || (type >= 4 && type <= 0xd))
			bit = BIT(type);
		else if (type == 0x44)
			bit = BIT(14);
		else if (type == 0x88)
			bit = BIT(15);
		else
			return -EOPNOTSUPP;
		if (seen & bit)
			return -EINVAL;
		seen |= bit;
		stride = type == 0x88 ? 16 : 8;
		if (!count || count > (header - pos) / stride)
			return -EINVAL;
		entry = data + pos;
		if (type >= 4 && type <= 0xb) {
			if (count != 1)
				return -EINVAL;
			ret = vrx619_aca_mem_parse(fw, type, entry);
			if (ret)
				return ret;
		} else if (type == 1) {
			for (j = 0; j < count; j++) {
				id = get_unaligned_be32(entry + j * 8);
				if (id < 0x14 || id > 0x20 || (id & 3))
					return -EINVAL;
			}
			fw->regs = entry;
			fw->reg_count = count;
		} else if (type == 0xc) {
			if (count != VRX619_ACA_CHANNELS)
				return -EINVAL;
			for (j = 0; j < count; j++) {
				id = get_unaligned_be32(entry + j * 8);
				base = get_unaligned_be32(entry + j * 8 + 4);
				if (id >= VRX619_ACA_CHANNELS ||
				    dma_seen & BIT(id) || !vrx619_aca_sram(base, 8))
					return -EINVAL;
				dma_seen |= BIT(id);
				fw->dma[id] = base;
			}
		} else if (type == 0xd) {
			if (count != 1 || get_unaligned_be32(entry) != VRX619_ACA_HIF_WORDS)
				return -EINVAL;
			fw->init_addr = get_unaligned_be32(entry + 4);
			if (!vrx619_aca_sram(fw->init_addr, VRX619_ACA_HIF_WORDS * 4))
				return -EINVAL;
		} else if (type == 0x44) {
			if (count != 8)
				return -EINVAL;
			for (j = 0; j < count; j++) {
				id = get_unaligned_be32(entry + j * 8);
				if (id < 0x48 || id > 0x70 || (id & 3))
					return -EINVAL;
			}
			fw->sifs = entry;
		} else {
			ret = vrx619_aca_code_parse(fw, entry, count,
						    data + header, payload);
			if (ret)
				return ret;
		}
		pos += count * stride;
	}
	return pos == header && seen == 0xfff2 && vrx619_aca_layout_valid(fw) ? 0 : -EINVAL;
}

void vrx619_aca_hif(const struct vrx619_aca_fw *fw, u32 bus_addr,
		    const struct vrx619_aca_mem rings[VRX619_RING_COUNT],
		    __le32 hif[VRX619_ACA_HIF_WORDS])
{
	unsigned int i, pos;

	memset(hif, 0, VRX619_ACA_HIF_WORDS * sizeof(*hif));
	hif[0] = cpu_to_le32(0xf);
	for (i = 0; i < VRX619_RING_COUNT; i++) {
		pos = 1 + i * 7;
		hif[pos] = cpu_to_le32(rings[i].base);
		hif[pos + 1] = cpu_to_le32(rings[i].count);
		hif[pos + 2] = cpu_to_le32(bus_addr + fw->host[i].base);
		hif[pos + 3] = cpu_to_le32(fw->host[i].count);
		hif[pos + 4] = cpu_to_le32(bus_addr + fw->pd[i].base);
		hif[pos + 5] = cpu_to_le32(fw->pd[i].count);
		if (i == VRX619_TXOUT || i == VRX619_RXOUT)
			hif[pos + 6] = cpu_to_le32(fw->host[i].count);
	}
	hif[42] = cpu_to_le32(VRX619_TXOUT_MAGIC);
}
