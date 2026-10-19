// SPDX-License-Identifier: GPL-2.0-only

#include <linux/completion.h>
#include <linux/capability.h>
#include <linux/compat.h>
#include <linux/kfifo.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/idr.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/platform_data/vrx619.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include "vrx619_fw.h"
#include "include/uapi/linux/vrx619.h"

#define VRX619_FIRMWARE		"vrx619/xcpe_fw.bin"
#define VRX619_PAGE_SIZE		SZ_1M
#define VRX619_ADDR_LIMIT	SZ_16M
#define VRX619_WINDOW_BOOT	3
#define VRX619_WINDOW_MBOX	4
#define VRX619_WINDOW_PAGE	5
#define VRX619_APR(n)		(0x10000 + ((n) + 2) * 4)
#define VRX619_CLOCK		0x00000
#define VRX619_PLL_RESET		0x00100
#define VRX619_RESET		0x08010
#define VRX619_RESET_AUX		0x08014
#define VRX619_D2H_STATUS		0x10004
#define VRX619_CPU_CONTROL	0x10040
#define VRX619_H2D_STATUS		0x10050
#define VRX619_D2H_CLEAR		0x10074
#define VRX619_CHIP_ID		0x18004
#define VRX619_READY		0x25000
#define VRX619_LINE_CONFIG	0x78100
#define VRX619_SCRATCH		0xf10204
#define VRX619_MSG_HEADER	8
#define VRX619_MSG_PAYLOAD	496
#define VRX619_MSG_SIZE		(VRX619_MSG_HEADER + VRX619_MSG_PAYLOAD)
#define VRX619_EVENT_READY	0x02ff
#define VRX619_EVENT_PAGE		0x7f01
#define VRX619_POLL_MS		1
#define VRX619_TIMEOUT_MS	5000

static DEFINE_IDA(vrx619_control_ids);

struct vrx619_pmi {
	struct device *dev;
	struct platform_device *dp;
	void __iomem *base;
	const struct firmware *firmware;
	struct vrx619_fw image;
	struct mutex io_lock; /* MMIO windows and mailbox state */
	struct mutex command_lock; /* One outstanding request across callers */
	struct delayed_work poll;
	struct completion boot_done;
	struct completion command_done;
	struct miscdevice misc;
	struct kref ref;
	struct mutex control_lock; /* Userspace operations, restart and removal */
	wait_queue_head_t event_wait;
	DECLARE_KFIFO(events, struct vrx619_event, 32);
	const struct vrx619_platform_data *pdata;
	int index;
	u32 dropped;
	bool opened;
	bool removed;
	unsigned int transport;
	u32 tx_addr;
	u32 rx_addr;
	u32 page_addr;
	u32 tx_size;
	u32 rx_size;
	u32 pages;
	u16 command;
	u8 sequence;
	u8 reply[VRX619_MSG_SIZE];
	size_t reply_size;
	int error;
	bool ready;
	bool pending;
};

/* All access to APR windows 3 through 5 is serialized by io_lock. */
static void __iomem *vrx619_map(struct vrx619_pmi *pmi, unsigned int window,
				u32 addr)
{
	if (addr < 3 * VRX619_PAGE_SIZE)
		return pmi->base + addr;

	writel(addr >> 20, pmi->base + VRX619_APR(window));
	readl(pmi->base + VRX619_APR(window));
	return pmi->base + window * VRX619_PAGE_SIZE +
		(addr & (VRX619_PAGE_SIZE - 1));
}

static int vrx619_mem_write(struct vrx619_pmi *pmi, unsigned int window,
			    u32 addr, const u8 *data, size_t size)
{
	void __iomem *mem;
	size_t chunk, i;

	if (((addr | size) & 3) || addr >= VRX619_ADDR_LIMIT ||
	    size > VRX619_ADDR_LIMIT - addr)
		return -EINVAL;
	while (size) {
		chunk = min_t(size_t, size, VRX619_PAGE_SIZE -
			      (addr & (VRX619_PAGE_SIZE - 1)));
		mem = vrx619_map(pmi, window, addr);
		for (i = 0; i < chunk; i += 4)
			writel(get_unaligned_le32(data + i), mem + i);
		/* Drain writes before changing this window's translation. */
		readl(mem + chunk - 4);
		addr += chunk;
		data += chunk;
		size -= chunk;
		cond_resched();
	}
	return 0;
}

static int vrx619_mem_read(struct vrx619_pmi *pmi, unsigned int window,
			   u32 addr, u8 *data, size_t size)
{
	void __iomem *mem;
	size_t chunk, part, done;
	u8 word[4];

	if (addr >= VRX619_ADDR_LIMIT || size > VRX619_ADDR_LIMIT - addr)
		return -EINVAL;
	while (size) {
		chunk = min_t(size_t, size, VRX619_PAGE_SIZE -
			      (addr & (VRX619_PAGE_SIZE - 1)));
		mem = vrx619_map(pmi, window, addr & ~3U);
		for (done = 0; done < chunk; done += part) {
			put_unaligned_le32(readl(mem), word);
			part = min_t(size_t, chunk - done, 4 - ((addr + done) & 3));
			memcpy(data + done, word + ((addr + done) & 3), part);
			mem += 4;
		}
		addr += chunk;
		data += chunk;
		size -= chunk;
	}
	return 0;
}

static int vrx619_boot_write(void *ctx, u32 addr, const u8 *data, size_t size)
{
	return vrx619_mem_write(ctx, VRX619_WINDOW_BOOT, addr, data, size);
}

static int vrx619_page_write(void *ctx, u32 addr, const u8 *data, size_t size)
{
	return vrx619_mem_write(ctx, VRX619_WINDOW_PAGE, addr, data, size);
}

static void vrx619_write(struct vrx619_pmi *pmi, u32 reg, u32 value)
{
	writel(value, pmi->base + reg);
	readl(pmi->base + reg);
}

static int vrx619_detect(struct vrx619_pmi *pmi)
{
	u32 value;
	int ret;

	ret = readl_poll_timeout(pmi->base + VRX619_READY, value,
				 value & BIT(31), 10000, 20000);
	if (ret)
		return ret;
	value = readl(pmi->base + VRX619_CHIP_ID);
	if (((value >> 12) & 0xf) != 5)
		return -ENODEV;
	dev_dbg(pmi->dev, "chip ID %#010x\n", value);
	return 0;
}

static void vrx619_stop(struct vrx619_pmi *pmi)
{
	vrx619_write(pmi, VRX619_CPU_CONTROL, GENMASK(31, 30));
	msleep(100);
}

static int vrx619_reset(struct vrx619_pmi *pmi)
{
	u32 value;
	int ret;

	vrx619_stop(pmi);
	vrx619_write(pmi, VRX619_RESET, 8);
	fsleep(10000);
	vrx619_write(pmi, VRX619_RESET, 0);
	fsleep(10000);
	vrx619_write(pmi, VRX619_CLOCK, 0x0a050fb8);
	/* Single, unbonded VRX619: keep the inter-chip link in reset. */
	vrx619_write(pmi, VRX619_RESET, 0xd18001bf);
	fsleep(10000);
	vrx619_write(pmi, VRX619_RESET_AUX, 0x20);
	msleep(100);
	vrx619_write(pmi, VRX619_RESET, 0);
	vrx619_write(pmi, VRX619_RESET_AUX, 0);
	msleep(100);
	ret = vrx619_detect(pmi);
	if (ret)
		return ret;
	vrx619_write(pmi, VRX619_CLOCK, 0x0a0500b8);
	vrx619_write(pmi, VRX619_PLL_RESET, 0x4000);
	value = readl(pmi->base + VRX619_LINE_CONFIG);
	vrx619_write(pmi, VRX619_LINE_CONFIG, value & ~0x6000);
	return 0;
}

static bool vrx619_mailbox_range(u32 pss, u32 size)
{
	u32 addr = vrx619_pss_to_dev(pss);

	return !(pss & 3) && size >= 12 && size <= SZ_1K &&
	       addr < VRX619_ADDR_LIMIT && size <= VRX619_ADDR_LIMIT - addr &&
	       pss <= U32_MAX - (size - 1) &&
	       vrx619_pss_to_dev(pss + size - 1) == addr + size - 1;
}

static int vrx619_mailbox_setup(struct vrx619_pmi *pmi)
{
	u32 addr = vrx619_pss_to_dev(pmi->image.info_addr);
	u32 tx, rx, tx_size, rx_size, prefix;
	u8 data[16];
	u16 type, size;
	u32 offset = 0;
	int ret;

	while (offset <= SZ_1K - 4) {
		ret = vrx619_mem_read(pmi, VRX619_WINDOW_BOOT, addr + offset,
				      data, 4);
		if (ret)
			return ret;
		type = get_unaligned_le16(data);
		size = get_unaligned_le16(data + 2);
		if (!type)
			break;
		if (size > SZ_1K - offset - 4)
			return -EBADMSG;
		if (type == 1 || type == 4) {
			if (size != sizeof(data))
				return -EBADMSG;
			ret = vrx619_mem_read(pmi, VRX619_WINDOW_BOOT,
					      addr + offset + 4, data, sizeof(data));
			if (ret)
				return ret;
			tx = get_unaligned_le32(data);
			tx_size = get_unaligned_le32(data + 4);
			rx = get_unaligned_le32(data + 8);
			rx_size = get_unaligned_le32(data + 12);
			prefix = type == 4 ? 4 : 0;
			if (!vrx619_mailbox_range(tx, tx_size) ||
			    !vrx619_mailbox_range(rx, rx_size) ||
			    min(tx_size, rx_size) < prefix + 12)
				return -EBADMSG;
			pmi->page_addr = prefix ? vrx619_pss_to_dev(rx) : 0;
			pmi->tx_addr = vrx619_pss_to_dev(tx) + prefix;
			pmi->rx_addr = vrx619_pss_to_dev(rx) + prefix;
			pmi->tx_size = tx_size - prefix - 4;
			pmi->rx_size = rx_size - prefix - 4;
			return 0;
		}
		offset += size + 4;
	}
	return -ENODATA;
}

static int vrx619_receive(struct vrx619_pmi *pmi, u8 *message)
{
	void __iomem *control;
	u32 header;
	size_t size;
	int ret;

	control = vrx619_map(pmi, VRX619_WINDOW_MBOX, pmi->rx_addr);
	if (!(readl(control) & 1))
		return 0;
	ret = vrx619_mem_read(pmi, VRX619_WINDOW_MBOX, pmi->rx_addr + 4,
			      message, VRX619_MSG_HEADER);
	if (ret)
		return ret;
	size = get_unaligned_le16(message + 6) & 0x3fff;
	if (ALIGN(size, 4) > VRX619_MSG_PAYLOAD ||
	    ALIGN(size, 4) + VRX619_MSG_HEADER > pmi->rx_size)
		return -EMSGSIZE;
	ret = vrx619_mem_read(pmi, VRX619_WINDOW_MBOX,
			      pmi->rx_addr + 4 + VRX619_MSG_HEADER,
			      message + VRX619_MSG_HEADER, size);
	if (ret)
		return ret;

	header = get_unaligned_le32(message);
	header = (header & ~GENMASK(19, 16)) | (0xa << 16);
	writel(header, vrx619_map(pmi, VRX619_WINDOW_MBOX, pmi->rx_addr + 4));
	control = vrx619_map(pmi, VRX619_WINDOW_MBOX, pmi->rx_addr);
	writel(0, control);
	readl(control);
	return VRX619_MSG_HEADER + size;
}

static int vrx619_event(struct vrx619_pmi *pmi, const u8 *message, size_t size)
{
	void __iomem *status;
	int ret;

	switch (get_unaligned_le16(message)) {
	case VRX619_EVENT_READY:
		WRITE_ONCE(pmi->transport, VRX619_TRANSPORT_NONE);
		/* The initial READY is consumed by the synchronous boot operation. */
		if (!pmi->ready) {
			pmi->ready = true;
			complete_all(&pmi->boot_done);
			return 0;
		}
		pmi->ready = true;
		complete_all(&pmi->boot_done);
		break;
	case VRX619_EVENT_PAGE:
		if (!pmi->page_addr || size < VRX619_MSG_HEADER + 12)
			return -EBADMSG;
		status = vrx619_map(pmi, VRX619_WINDOW_MBOX, pmi->page_addr);
		if ((readl(status) & 0xffff) != 1)
			return -EPROTO;
		ret = vrx619_fw_page(&pmi->image,
				     get_unaligned_le32(message + 8),
				     get_unaligned_le32(message + 12),
				     get_unaligned_le32(message + 16),
				     vrx619_page_write, pmi);
		if (ret)
			return ret;
		/* Page data must reach the endpoint before its completion flag. */
		status = vrx619_map(pmi, VRX619_WINDOW_MBOX, pmi->page_addr);
		writel(2, status);
		readl(status);
		pmi->pages++;
		return 0;
	default:
		dev_dbg(pmi->dev, "event %#06x, %zu bytes\n",
			get_unaligned_le16(message), size);
		break;
	}
	if (pmi->opened) {
		struct vrx619_event event = {
			.command = get_unaligned_le16(message),
			.field = get_unaligned_le16(message + 4),
			.size = size - VRX619_MSG_HEADER,
		};

		memcpy(event.data, message + VRX619_MSG_HEADER, event.size);
		if (!kfifo_put(&pmi->events, event))
			pmi->dropped++;
		wake_up_interruptible(&pmi->event_wait);
	}
	return 0;
}

static void vrx619_poll(struct work_struct *work)
{
	struct vrx619_pmi *pmi = container_of(to_delayed_work(work),
					     struct vrx619_pmi, poll);
	u8 message[VRX619_MSG_SIZE];
	u32 status;
	u16 command, tag;
	int size, ret = 0;

	mutex_lock(&pmi->io_lock);
	ret = pmi->error;
	if (ret)
		goto failed;
	status = readl(pmi->base + VRX619_D2H_STATUS) & 3;
	if (status) {
		writel(status, pmi->base + VRX619_D2H_CLEAR);
		readl(pmi->base + VRX619_D2H_STATUS);
	}
	/* Also check ownership to catch a mailbox published across a poll. */
	size = vrx619_receive(pmi, message);
	if (size < 0) {
		ret = size;
	} else if (size) {
		command = get_unaligned_le16(message);
		tag = get_unaligned_le16(message + 2);
		if ((tag & 0xf0) == 0xf0) {
			ret = vrx619_event(pmi, message, size);
		} else if (pmi->pending && command == pmi->command &&
			   (tag >> 8) == pmi->sequence) {
			memcpy(pmi->reply, message, size);
			pmi->reply_size = size;
			pmi->pending = false;
			complete(&pmi->command_done);
		}
	}
failed:
	if (ret) {
		pmi->error = ret;
		pmi->ready = false;
		vrx619_stop(pmi);
		complete_all(&pmi->boot_done);
		complete_all(&pmi->command_done);
		dev_err(pmi->dev, "mailbox service failed: %d\n", ret);
		wake_up_interruptible(&pmi->event_wait);
	} else {
		schedule_delayed_work(&pmi->poll, msecs_to_jiffies(VRX619_POLL_MS));
	}
	mutex_unlock(&pmi->io_lock);
}

static int vrx619_command(struct vrx619_pmi *pmi, u8 *message, size_t size,
			  u8 *reply, size_t *reply_size)
{
	void __iomem *control;
	unsigned long waited;
	u16 field;
	int ret;

	ret = mutex_lock_interruptible(&pmi->command_lock);
	if (ret)
		return ret;
	mutex_lock(&pmi->io_lock);
	ret = pmi->error;
	if (ret)
		goto unlock;
	if (!pmi->ready) {
		ret = -EAGAIN;
		goto unlock;
	}
	if (ALIGN(size, 4) > pmi->tx_size) {
		ret = -EMSGSIZE;
		goto unlock;
	}
	control = vrx619_map(pmi, VRX619_WINDOW_MBOX, pmi->tx_addr);
	if ((readl(control) & 1) ||
	    (readl(pmi->base + VRX619_H2D_STATUS) & 3)) {
		ret = -EBUSY;
		goto unlock;
	}
	pmi->sequence = pmi->sequence < 0xfe ? pmi->sequence + 1 : 0;
	put_unaligned_le16(pmi->sequence << 8, message + 2);
	field = get_unaligned_le16(message + 4);
	if ((field & 0xc000) == 0x4000)
		put_unaligned_le16(field & ~0x4000, message + 4);
	ret = vrx619_mem_write(pmi, VRX619_WINDOW_MBOX, pmi->tx_addr + 4,
			       message, ALIGN(size, 4));
	if (ret)
		goto unlock;
	pmi->command = get_unaligned_le16(message);
	pmi->reply_size = 0;
	pmi->pending = true;
	reinit_completion(&pmi->command_done);
	control = vrx619_map(pmi, VRX619_WINDOW_MBOX, pmi->tx_addr);
	writel(1, control);
	readl(control);
	mutex_unlock(&pmi->io_lock);

	waited = wait_for_completion_timeout(&pmi->command_done,
					     msecs_to_jiffies(VRX619_TIMEOUT_MS));
	mutex_lock(&pmi->io_lock);
	if (pmi->error) {
		ret = pmi->error;
	} else if (!waited || pmi->pending) {
		ret = -ETIMEDOUT;
		/* A late reply must not be mistaken for a future transaction. */
		pmi->error = ret;
		pmi->ready = false;
	} else {
		memcpy(reply, pmi->reply, pmi->reply_size);
		*reply_size = pmi->reply_size;
	}
	pmi->pending = false;
unlock:
	mutex_unlock(&pmi->io_lock);
	mutex_unlock(&pmi->command_lock);
	return ret;
}

int vrx619_pmi_xfer(struct device *dev, u16 command, u16 field,
		    const void *tx, size_t tx_size, void *rx, size_t rx_size)
{
	struct vrx619_pmi *pmi = dev_get_drvdata(dev);
	u8 message[VRX619_MSG_SIZE] = {};
	u8 reply[VRX619_MSG_SIZE];
	size_t size = 0;
	int ret;

	if (tx_size > VRX619_MSG_PAYLOAD || (tx_size && !tx))
		return -EINVAL;
	put_unaligned_le16(command, message);
	put_unaligned_le16(field, message + 4);
	put_unaligned_le16(tx_size, message + 6);
	if (tx_size)
		memcpy(message + VRX619_MSG_HEADER, tx, tx_size);
	ret = vrx619_command(pmi, message, tx_size + VRX619_MSG_HEADER,
			     reply, &size);
	if (ret)
		return ret;
	if (get_unaligned_le16(reply + 4) & BIT(15))
		return -EREMOTEIO;
	size -= VRX619_MSG_HEADER;
	if (!rx)
		return 0;
	if (size > rx_size)
		return -EMSGSIZE;
	memcpy(rx, reply + VRX619_MSG_HEADER, size);
	return size;
}
EXPORT_SYMBOL_GPL(vrx619_pmi_xfer);

unsigned int vrx619_pmi_transport(struct device *dev)
{
	struct vrx619_pmi *pmi = dev_get_drvdata(dev);

	return READ_ONCE(pmi->error) ? VRX619_TRANSPORT_NONE : READ_ONCE(pmi->transport);
}
EXPORT_SYMBOL_GPL(vrx619_pmi_transport);

static int vrx619_restart(struct vrx619_pmi *pmi);

static void vrx619_free(struct kref *ref)
{
	struct vrx619_pmi *pmi = container_of(ref, struct vrx619_pmi, ref);

	if (pmi->index >= 0)
		ida_free(&vrx619_control_ids, pmi->index);
	kfree(pmi->misc.name);
	kfree(pmi);
}

static int vrx619_control_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct vrx619_pmi *pmi = container_of(misc, struct vrx619_pmi, misc);
	int ret = 0;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	mutex_lock(&pmi->io_lock);
	if (pmi->removed) {
		ret = -ENODEV;
	} else if (pmi->opened) {
		ret = -EBUSY;
	} else {
		kref_get(&pmi->ref);
		pmi->opened = true;
		pmi->dropped = 0;
		kfifo_reset(&pmi->events);
		file->private_data = pmi;
	}
	mutex_unlock(&pmi->io_lock);
	return ret ?: nonseekable_open(inode, file);
}

static int vrx619_control_release(struct inode *inode, struct file *file)
{
	struct vrx619_pmi *pmi = file->private_data;

	mutex_lock(&pmi->io_lock);
	pmi->opened = false;
	WRITE_ONCE(pmi->transport, VRX619_TRANSPORT_NONE);
	mutex_unlock(&pmi->io_lock);
	kref_put(&pmi->ref, vrx619_free);
	return 0;
}

static ssize_t vrx619_control_read(struct file *file, char __user *buf,
				   size_t size, loff_t *pos)
{
	struct vrx619_pmi *pmi = file->private_data;
	struct vrx619_event event;
	int ret;

	if (size < sizeof(event))
		return -EMSGSIZE;
	for (;;) {
		mutex_lock(&pmi->io_lock);
		ret = pmi->error;
		if (pmi->removed)
			ret = -ENODEV;
		if (!ret && kfifo_get(&pmi->events, &event)) {
			event.dropped = pmi->dropped;
			mutex_unlock(&pmi->io_lock);
			return copy_to_user(buf, &event, sizeof(event)) ?
				-EFAULT : sizeof(event);
		}
		mutex_unlock(&pmi->io_lock);
		if (ret)
			return ret;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(pmi->event_wait,
					       !kfifo_is_empty(&pmi->events) ||
					       READ_ONCE(pmi->error));
		if (ret)
			return ret;
	}
}

static __poll_t vrx619_control_poll(struct file *file, poll_table *wait)
{
	struct vrx619_pmi *pmi = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &pmi->event_wait, wait);
	mutex_lock(&pmi->io_lock);
	if (!kfifo_is_empty(&pmi->events))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (pmi->error)
		mask |= EPOLLERR;
	if (pmi->removed)
		mask |= EPOLLHUP;
	mutex_unlock(&pmi->io_lock);
	return mask;
}

static long vrx619_control_xfer(struct vrx619_pmi *pmi, void __user *arg)
{
	struct vrx619_xfer xfer;
	u8 message[VRX619_MSG_SIZE] = {};
	u8 reply[VRX619_MSG_SIZE];
	size_t size = 0;
	int ret;

	if (copy_from_user(&xfer, arg, sizeof(xfer)))
		return -EFAULT;
	if (xfer.reserved || xfer.response_field || xfer.response_size ||
	    xfer.tx_size > VRX619_MAX_PAYLOAD || xfer.rx_size > VRX619_MAX_PAYLOAD)
		return -EINVAL;
	put_unaligned_le16(xfer.command, message);
	put_unaligned_le16(xfer.field, message + 4);
	put_unaligned_le16(xfer.tx_size, message + 6);
	memcpy(message + VRX619_MSG_HEADER, xfer.data, xfer.tx_size);
	ret = vrx619_command(pmi, message, xfer.tx_size + VRX619_MSG_HEADER,
			     reply, &size);
	if (ret)
		return ret;
	xfer.response_field = get_unaligned_le16(reply + 4);
	xfer.response_size = size - VRX619_MSG_HEADER;
	memset(xfer.data, 0, sizeof(xfer.data));
	memcpy(xfer.data, reply + VRX619_MSG_HEADER,
	       min(xfer.response_size, xfer.rx_size));
	if (copy_to_user(arg, &xfer, sizeof(xfer)))
		return -EFAULT;
	if (xfer.response_field & BIT(15))
		return -EREMOTEIO;
	return xfer.response_size > xfer.rx_size ? -EMSGSIZE : 0;
}

static long vrx619_control_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	struct vrx619_pmi *pmi = file->private_data;
	u32 transport;
	int ret;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	ret = mutex_lock_interruptible(&pmi->control_lock);
	if (ret)
		return ret;
	if (pmi->removed) {
		ret = -ENODEV;
	} else if (cmd == VRX619_IOC_XFER) {
		ret = vrx619_control_xfer(pmi, (void __user *)arg);
	} else if (cmd == VRX619_IOC_RESTART && !arg) {
		ret = vrx619_restart(pmi);
	} else if (cmd == VRX619_IOC_SET_TRANSPORT) {
		ret = -EFAULT;
		if (!get_user(transport, (u32 __user *)arg)) {
			ret = transport > VRX619_TRANSPORT_ATM ? -EINVAL : 0;
			if (!ret)
				WRITE_ONCE(pmi->transport, transport);
		}
	} else {
		ret = -ENOTTY;
	}
	mutex_unlock(&pmi->control_lock);
	return ret;
}

static const struct file_operations vrx619_control_fops = {
	.owner = THIS_MODULE,
	.open = vrx619_control_open,
	.release = vrx619_control_release,
	.read = vrx619_control_read,
	.poll = vrx619_control_poll,
	.unlocked_ioctl = vrx619_control_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static int vrx619_start(struct vrx619_pmi *pmi)
{
	void __iomem *scratch;
	unsigned long waited;
	int ret;

	reinit_completion(&pmi->boot_done);
	ret = vrx619_fw_parse(&pmi->image, pmi->firmware->data,
			      pmi->firmware->size);
	if (ret)
		goto release;
	/* The recovered 5690 Pro firmware uses the single-stage boot mode. */
	if (pmi->image.mode) {
		ret = -EOPNOTSUPP;
		goto release;
	}
	ret = vrx619_detect(pmi);
	if (ret)
		goto release;
	ret = vrx619_reset(pmi);
	if (ret)
		goto stop;
	ret = vrx619_fw_upload(&pmi->image, vrx619_boot_write, pmi);
	if (ret)
		goto stop;
	scratch = vrx619_map(pmi, VRX619_WINDOW_BOOT, VRX619_SCRATCH);
	writel(pmi->image.scratch, scratch);
	readl(scratch);
	ret = vrx619_mailbox_setup(pmi);
	if (ret)
		goto stop;
	vrx619_write(pmi, VRX619_CPU_CONTROL, 1);
	schedule_delayed_work(&pmi->poll, 0);
	waited = wait_for_completion_timeout(&pmi->boot_done,
					     msecs_to_jiffies(VRX619_TIMEOUT_MS));
	mutex_lock(&pmi->io_lock);
	ret = pmi->error;
	if (!ret && (!waited || !pmi->ready))
		ret = -ETIMEDOUT;
	mutex_unlock(&pmi->io_lock);
	if (ret) {
		cancel_delayed_work_sync(&pmi->poll);
		goto stop;
	}
	pmi->dp = platform_device_register_data(pmi->dev, "vrx619-dp",
						PLATFORM_DEVID_AUTO,
						pmi->pdata, sizeof(*pmi->pdata));
	if (IS_ERR(pmi->dp)) {
		ret = PTR_ERR(pmi->dp);
		pmi->dp = NULL;
		cancel_delayed_work_sync(&pmi->poll);
		goto stop;
	}
	dev_info(pmi->dev, "modem firmware ready\n");
	return 0;

stop:
	vrx619_stop(pmi);
release:
	mutex_lock(&pmi->io_lock);
	pmi->error = ret;
	pmi->ready = false;
	mutex_unlock(&pmi->io_lock);
	wake_up_interruptible(&pmi->event_wait);
	return ret;
}

/* control_lock excludes userspace commands and another restart/removal. */
static void vrx619_halt(struct vrx619_pmi *pmi)
{
	WRITE_ONCE(pmi->transport, VRX619_TRANSPORT_NONE);
	if (pmi->dp) {
		platform_device_unregister(pmi->dp);
		pmi->dp = NULL;
	}
	cancel_delayed_work_sync(&pmi->poll);
	mutex_lock(&pmi->command_lock);
	mutex_lock(&pmi->io_lock);
	pmi->ready = false;
	vrx619_stop(pmi);
	mutex_unlock(&pmi->io_lock);
	mutex_unlock(&pmi->command_lock);
}

static int vrx619_restart(struct vrx619_pmi *pmi)
{
	vrx619_halt(pmi);
	mutex_lock(&pmi->io_lock);
	pmi->error = 0;
	pmi->pending = false;
	pmi->pages = 0;
	kfifo_reset(&pmi->events);
	mutex_unlock(&pmi->io_lock);
	return vrx619_start(pmi);
}

static int vrx619_pmi_probe(struct platform_device *pdev)
{
	const struct vrx619_platform_data *pdata = dev_get_platdata(&pdev->dev);
	struct vrx619_pmi *pmi;
	int ret;

	if (!pdata || pdata->memsize < 6 * VRX619_PAGE_SIZE)
		return -EINVAL;
	pmi = kzalloc(sizeof(*pmi), GFP_KERNEL);
	if (!pmi)
		return -ENOMEM;
	kref_init(&pmi->ref);
	pmi->index = ida_alloc(&vrx619_control_ids, GFP_KERNEL);
	if (pmi->index < 0) {
		ret = pmi->index;
		goto put;
	}
	pmi->dev = &pdev->dev;
	pmi->base = pdata->membase;
	pmi->pdata = pdata;
	mutex_init(&pmi->io_lock);
	mutex_init(&pmi->command_lock);
	mutex_init(&pmi->control_lock);
	init_completion(&pmi->boot_done);
	init_completion(&pmi->command_done);
	init_waitqueue_head(&pmi->event_wait);
	INIT_KFIFO(pmi->events);
	INIT_DELAYED_WORK(&pmi->poll, vrx619_poll);
	platform_set_drvdata(pdev, pmi);
	pmi->misc.minor = MISC_DYNAMIC_MINOR;
	pmi->misc.name = kasprintf(GFP_KERNEL, "vrx619-%d", pmi->index);
	pmi->misc.fops = &vrx619_control_fops;
	pmi->misc.parent = &pdev->dev;
	pmi->misc.mode = 0600;
	if (!pmi->misc.name) {
		ret = -ENOMEM;
		goto put;
	}
	ret = request_firmware(&pmi->firmware, VRX619_FIRMWARE, &pdev->dev);
	if (ret)
		goto put;
	ret = vrx619_start(pmi);
	if (ret)
		goto release;
	ret = misc_register(&pmi->misc);
	if (!ret)
		return 0;
	vrx619_halt(pmi);
release:
	release_firmware(pmi->firmware);
put:
	kref_put(&pmi->ref, vrx619_free);
	return dev_err_probe(&pdev->dev, ret, "firmware bring-up failed\n");
}

static void vrx619_disconnect(struct vrx619_pmi *pmi)
{
	mutex_lock(&pmi->io_lock);
	pmi->removed = true;
	pmi->error = -ENODEV;
	pmi->ready = false;
	complete_all(&pmi->command_done);
	mutex_unlock(&pmi->io_lock);
	wake_up_interruptible(&pmi->event_wait);
	vrx619_halt(pmi);
}

static void vrx619_pmi_remove(struct platform_device *pdev)
{
	struct vrx619_pmi *pmi = platform_get_drvdata(pdev);

	misc_deregister(&pmi->misc);
	mutex_lock(&pmi->control_lock);
	vrx619_disconnect(pmi);
	release_firmware(pmi->firmware);
	mutex_unlock(&pmi->control_lock);
	kref_put(&pmi->ref, vrx619_free);
}

static void vrx619_pmi_shutdown(struct platform_device *pdev)
{
	struct vrx619_pmi *pmi = platform_get_drvdata(pdev);

	mutex_lock(&pmi->control_lock);
	vrx619_disconnect(pmi);
	mutex_unlock(&pmi->control_lock);
}

static struct platform_driver vrx619_pmi_driver = {
	.probe = vrx619_pmi_probe,
	.remove = vrx619_pmi_remove,
	.shutdown = vrx619_pmi_shutdown,
	.driver = {
		.name = "vrx619-pmi",
	},
};
module_platform_driver(vrx619_pmi_driver);

MODULE_ALIAS("platform:vrx619-pmi");
MODULE_FIRMWARE(VRX619_FIRMWARE);
MODULE_DESCRIPTION("VRX619 firmware loading and mailbox service");
MODULE_LICENSE("GPL");
