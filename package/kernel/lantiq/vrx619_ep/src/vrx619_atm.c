// SPDX-License-Identifier: GPL-2.0-only
#include <linux/atmdev.h>
#include <linux/errno.h>
#include <linux/if_ether.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "vrx619_atm.h"

unsigned int vrx619_atm_tables(const struct vrx619_atm_pvc *pvc,
			     struct vrx619_atm_tables *tables)
{
	unsigned int i, n = 0;
	__le16 *q, *v;

	memset(tables, 0, sizeof(*tables));
	for (i = 0; i < VRX619_ATM_VCCS; i++) {
		if (!pvc[i].configured)
			continue;
		q = &tables->queues[2 + i * 4];
		v = &tables->vccs[2 + i * 8];
		q[0] = cpu_to_le16(i);
		q[1] = cpu_to_le16(i);
		q[2] = cpu_to_le16(i);
		v[0] = cpu_to_le16(pvc[i].vpi);
		v[1] = cpu_to_le16(pvc[i].vci);
		v[2] = cpu_to_le16(pvc[i].llc ? 2 : 1);
		v[3] = cpu_to_le16(pvc[i].type);
		/* EoA: append a host-side RX FCS; LLC also removes the line FCS. */
		if (pvc[i].type < ATM_MPOA_PPP)
			v[5] = cpu_to_le16(pvc[i].llc ? 0x50 : 0x40);
		v[6] = cpu_to_le16(i);
		n++;
	}
	tables->queues[0] = cpu_to_le16(n);
	tables->vccs[0] = cpu_to_le16(n);
	tables->vlans[0] = cpu_to_le16(n);
	return n;
}

int vrx619_atm_encapsulate(const struct vrx619_atm_pvc *pvc, u8 *out,
			 size_t size, const u8 *data, size_t len)
{
	static const u8 eoa_llc[] = { 0xaa, 0xaa, 3, 0, 0x80, 0xc2, 0, 7, 0, 0 };
	static const u8 ppp_llc[] = { 0xfe, 0xfe, 3, 0xcf };
	static const u8 ip_llc[] = { 0xaa, 0xaa, 3, 0, 0, 0 };
	size_t head, skip;
	u16 protocol;

	if (len < ETH_HLEN)
		return -EINVAL;
	switch (pvc->type) {
	case ATM_MPOA_EOA:
		if (len < ETH_HLEN + ETH_FCS_LEN)
			return -EINVAL;
		len -= ETH_FCS_LEN;
		head = pvc->llc ? sizeof(eoa_llc) : 2;
		skip = 0;
		break;
	case ATM_MPOA_PPP:
		head = pvc->llc ? sizeof(ppp_llc) + 2 : 2;
		skip = ETH_HLEN;
		break;
	case ATM_MPOA_IP:
		head = pvc->llc ? sizeof(ip_llc) : 0;
		skip = pvc->llc ? 12 : ETH_HLEN;
		break;
	default:
		return -EOPNOTSUPP;
	}
	if (head > size || len - skip > size - head)
		return -EMSGSIZE;
	memcpy(out + head, data + skip, len - skip);
	if (pvc->type == ATM_MPOA_EOA) {
		if (pvc->llc)
			memcpy(out, eoa_llc, head);
		else
			memset(out, 0, head);
	} else if (pvc->type == ATM_MPOA_PPP) {
		protocol = get_unaligned_be16(data + 12);
		if (protocol == ETH_P_IP)
			protocol = 0x21;
		else if (protocol == ETH_P_IPV6)
			protocol = 0x57;
		if (pvc->llc)
			memcpy(out, ppp_llc, sizeof(ppp_llc));
		put_unaligned_be16(protocol, out + head - 2);
	} else if (pvc->llc) {
		memcpy(out, ip_llc, head);
	}
	return head + len - skip;
}
