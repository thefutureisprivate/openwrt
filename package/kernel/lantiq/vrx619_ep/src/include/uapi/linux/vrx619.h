/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_VRX619_H
#define _UAPI_LINUX_VRX619_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define VRX619_MAX_PAYLOAD 496

/* Header members use host byte order; firmware payloads use little endian. */
struct vrx619_xfer {
	__u16 command;
	__u16 field;
	__u16 tx_size;
	__u16 rx_size;
	__u16 response_field;
	__u16 response_size;
	__u32 reserved;
	__u8 data[VRX619_MAX_PAYLOAD];
};

/* read() returns one complete record. dropped counts events lost since open. */
struct vrx619_event {
	__u16 command;
	__u16 field;
	__u16 size;
	__u16 reserved;
	__u32 dropped;
	__u8 data[VRX619_MAX_PAYLOAD];
};

/* One exclusive controller per endpoint; CAP_SYS_RAWIO is required. */
#define VRX619_IOC_XFER _IOWR('V', 0, struct vrx619_xfer)
/* Stops and recreates the datapath and reloads the modem firmware. */
#define VRX619_IOC_RESTART _IO('V', 1)
#define VRX619_TRANSPORT_NONE 0
#define VRX619_TRANSPORT_PTM  1
#define VRX619_TRANSPORT_ATM  2
/* Select the transport only after identifying the negotiated DSL mode. */
#define VRX619_IOC_SET_TRANSPORT _IOW('V', 2, __u32)

#endif
