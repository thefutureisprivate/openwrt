// SPDX-License-Identifier: GPL-2.0-only

#include <linux/crc32.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "vrx619_fw.h"

#define VRX619_FW_MAGIC		0x676d4940
#define VRX619_FW_HEADER_SIZE	32
#define VRX619_FW_COPY		0
#define VRX619_FW_FILL		2
#define VRX619_FW_INCREMENT	4
#define VRX619_FW_TYPE_MASK	6

u32 vrx619_pss_to_dev(u32 addr)
{
	u32 page;

	if ((addr & 0xf0000000) == 0xf0000000)
		page = (addr >> 4) & 0xf00000;
	else
		page = (addr >> 8) & 0xf00000;

	return page | (addr & 0xfffff);
}

static bool vrx619_fw_range(size_t total, u32 offset, u32 size)
{
	return offset <= total && size <= total - offset;
}

static bool vrx619_fw_destination(u32 addr, u32 size)
{
	u32 last;

	if ((addr | size) & 3)
		return false;
	if (!size)
		return true;
	if (size > 0x1000000 - vrx619_pss_to_dev(addr))
		return false;
	if (check_add_overflow(addr, size - 1, &last))
		return false;

	/* The PSS address space has aliases between its one MiB windows. */
	return vrx619_pss_to_dev(last) == vrx619_pss_to_dev(addr) + size - 1;
}

static bool vrx619_fw_crc(const u8 *data, size_t size, u32 expected)
{
	return !expected || (crc32_le(~0U, data, size) ^ ~0U) == expected;
}

int vrx619_fw_parse(struct vrx619_fw *fw, const u8 *data, size_t size)
{
	struct vrx619_fw image = {};
	u32 count, table_size, selected = 0;
	u32 flags, source, target, length, crc;
	const u8 *entry;
	u32 i;

	if (size < VRX619_FW_TABLE_OFFSET || size > U32_MAX || (size & 3))
		return -EINVAL;
	if (get_unaligned_le32(data) != VRX619_FW_MAGIC ||
	    get_unaligned_le32(data + 4) != VRX619_FW_HEADER_SIZE ||
	    get_unaligned_le32(data + 8) != size ||
	    get_unaligned_le32(data + 32) != VRX619_FW_INFO_SIZE ||
	    get_unaligned_le32(data + 36) != VRX619_FW_SECTION_SIZE)
		return -EINVAL;

	image.mode = get_unaligned_le32(data + 20);
	if (image.mode > 1)
		return -EOPNOTSUPP;
	count = get_unaligned_le32(data + 40);
	if (!count || count > (size - VRX619_FW_TABLE_OFFSET) /
			      VRX619_FW_SECTION_SIZE)
		return -EINVAL;
	table_size = count * VRX619_FW_SECTION_SIZE;
	if (!vrx619_fw_crc(data + VRX619_FW_TABLE_OFFSET, table_size,
			   get_unaligned_le32(data + 64)))
		return -EBADMSG;

	image.table_addr = get_unaligned_le32(data + 44);
	image.info_addr = get_unaligned_le32(data + 48);
	image.scratch = get_unaligned_le32(data + 52);
	if (!vrx619_fw_destination(image.table_addr,
				   VRX619_FW_INFO_SIZE + table_size) ||
	    !vrx619_fw_destination(image.info_addr, 1024))
		return -EINVAL;

	for (i = 0; i < count; i++) {
		entry = data + VRX619_FW_TABLE_OFFSET +
			i * VRX619_FW_SECTION_SIZE;
		flags = get_unaligned_le32(entry);
		source = get_unaligned_le32(entry + 4);
		target = get_unaligned_le32(entry + 8);
		length = get_unaligned_le32(entry + 12);
		crc = get_unaligned_le32(entry + 16);
		if (!vrx619_fw_destination(target, length))
			return -EINVAL;

		switch (flags & VRX619_FW_TYPE_MASK) {
		case VRX619_FW_COPY:
			if (!vrx619_fw_range(size, source, length))
				return -EINVAL;
			if (!vrx619_fw_crc(data + source, length, crc))
				return -EBADMSG;
			break;
		case VRX619_FW_FILL:
		case VRX619_FW_INCREMENT:
			break;
		default:
			return -EINVAL;
		}
		if (flags & (image.mode ? BIT(3) : BIT(0)))
			selected++;
	}
	if (!selected)
		return -EINVAL;

	image.data = data;
	image.size = size;
	image.sections = count;
	*fw = image;
	return 0;
}

int vrx619_fw_upload(const struct vrx619_fw *fw, vrx619_fw_write_t write,
		     void *ctx)
{
	u32 i, j, flags, source, target, length, done, chunk;
	u8 buffer[256];
	const u8 *entry;
	int ret;

	for (i = 0; i < fw->sections; i++) {
		entry = fw->data + VRX619_FW_TABLE_OFFSET +
			i * VRX619_FW_SECTION_SIZE;
		flags = get_unaligned_le32(entry);
		if (!(flags & (fw->mode ? BIT(3) : BIT(0))))
			continue;
		source = get_unaligned_le32(entry + 4);
		target = vrx619_pss_to_dev(get_unaligned_le32(entry + 8));
		length = get_unaligned_le32(entry + 12);
		if ((flags & VRX619_FW_TYPE_MASK) == VRX619_FW_COPY) {
			ret = write(ctx, target, fw->data + source, length);
			if (ret)
				return ret;
			continue;
		}
		for (done = 0; done < length; done += chunk) {
			chunk = min_t(u32, sizeof(buffer), length - done);
			for (j = 0; j < chunk; j++) {
				buffer[j] = source;
				if ((flags & VRX619_FW_TYPE_MASK) ==
				    VRX619_FW_INCREMENT)
					buffer[j] += done + j;
			}
			ret = write(ctx, target + done, buffer, chunk);
			if (ret)
				return ret;
		}
	}

	return write(ctx, vrx619_pss_to_dev(fw->table_addr),
		     fw->data + VRX619_FW_HEADER_SIZE,
		     VRX619_FW_INFO_SIZE + fw->sections * VRX619_FW_SECTION_SIZE);
}

int vrx619_fw_page(const struct vrx619_fw *fw, u32 offset, u32 addr, u32 size,
		   vrx619_fw_write_t write, void *ctx)
{
	if (!vrx619_fw_range(fw->size, offset, size) ||
	    !vrx619_fw_destination(addr, size))
		return -EINVAL;

	return write(ctx, vrx619_pss_to_dev(addr), fw->data + offset, size);
}
