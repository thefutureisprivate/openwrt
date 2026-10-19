// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atmdev.h>
#include <linux/bitmap.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <uapi/linux/vrx619.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/firmware.h>
#include <linux/hrtimer.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/platform_data/vrx619.h>
#include <linux/rtnetlink.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include "vrx619_aca.h"
#include "vrx619_atm.h"

#define VRX619_ACA_FIRMWARE	"vrx619/aca_fw.bin"
#define VRX619_TX_COUNT		128
#define VRX619_RX_COUNT		256
#define VRX619_BUFFER_SIZE	SZ_4K
#define VRX619_FRAME_SIZE		2048
#define VRX619_POLL_NS		500000
#define VRX619_DESC_OWN		BIT(31)
#define VRX619_DESC_SOP		BIT(29)
#define VRX619_DESC_EOP		BIT(28)
#define VRX619_DESC_LENGTH	GENMASK(13, 0)
#define VRX619_RXIN_ADD		0x500c8
#define VRX619_TXIN_ADD		0x500cc
#define VRX619_RXOUT_SUB		0x500f8
#define VRX619_TXOUT_SUB		0x500fc
#define VRX619_RXIN_COUNT		0x50118
#define VRX619_TXIN_COUNT		0x5011c
#define VRX619_RXOUT_COUNT	0x50120
#define VRX619_TXOUT_COUNT	0x50124

struct vrx619_ring {
	__le32 *desc;
	dma_addr_t dma;
	u32 count;
	u32 words;
	u32 head;
};

struct vrx619_vcc {
	struct atm_vcc __rcu *vcc;
	struct vrx619_atm_pvc pvc;
};

struct vrx619_dp {
	struct net_device *netdev;
	struct device *pmi;
	const struct vrx619_platform_data *pdata;
	const struct firmware *firmware;
	struct vrx619_aca_fw image;
	struct vrx619_ring ring[VRX619_RING_COUNT];
	struct napi_struct napi;
	struct hrtimer timer;
	struct delayed_work link_work;
	spinlock_t tx_lock; /* TXIN producer and TXOUT completion */
	DECLARE_BITMAP(tx_busy, VRX619_TX_COUNT);
	u16 tx_length[VRX619_TX_COUNT];
	void *tx_buffer;
	void *rx_buffer;
	dma_addr_t tx_dma;
	dma_addr_t rx_dma;
	struct atm_dev *atm;
	struct vrx619_vcc vcc[VRX619_ATM_VCCS];
	struct mutex state_lock; /* Shared ATM/PTM engine lifetime. */
	spinlock_t atm_lock; /* VCC configuration and removal versus send/setup. */
	struct kref ref;
	unsigned int atm_users;
	u32 atm_generation;
	u32 atm_applied;
	unsigned int transport;
	bool ptm_up;
	bool removed;
	bool registered;
	bool running;
	bool traffic_enabled;
	bool fault;
	bool quarantine;
};

static void vrx619_atm_receive(struct vrx619_dp *dp, u32 subif,
			       const u8 *data, size_t len);
static const struct atmdev_ops vrx619_atm_ops;

static u32 vrx619_dp_read(struct vrx619_dp *dp, u32 reg)
{
	return readl(dp->pdata->membase + reg);
}

static void vrx619_dp_write(struct vrx619_dp *dp, u32 reg, u32 value)
{
	writel(value, dp->pdata->membase + reg);
}

static __le32 *vrx619_desc(struct vrx619_ring *ring)
{
	return ring->desc + ring->head * ring->words;
}

static void vrx619_ring_next(struct vrx619_ring *ring)
{
	ring->head = (ring->head + 1) % ring->count;
}

static void vrx619_buffers_free(struct vrx619_dp *dp)
{
	struct device *dev = &dp->pdata->pdev->dev;
	struct vrx619_ring *ring;
	unsigned int i;

	/* Do not reuse memory while a failed endpoint still has pending DMA. */
	if (dp->quarantine)
		return;
	for (i = 0; i < VRX619_RING_COUNT; i++) {
		ring = &dp->ring[i];
		if (ring->desc)
			dma_free_coherent(dev, ring->count * ring->words * 4,
					  ring->desc, ring->dma);
		ring->desc = NULL;
	}
	if (dp->rx_buffer)
		dma_free_coherent(dev, VRX619_RX_COUNT * VRX619_BUFFER_SIZE,
				  dp->rx_buffer, dp->rx_dma);
	if (dp->tx_buffer)
		dma_free_coherent(dev, VRX619_TX_COUNT * VRX619_BUFFER_SIZE,
				  dp->tx_buffer, dp->tx_dma);
	dp->rx_buffer = NULL;
	dp->tx_buffer = NULL;
}

static int vrx619_buffers_alloc(struct vrx619_dp *dp)
{
	struct device *dev = &dp->pdata->pdev->dev;
	struct vrx619_ring *ring;
	unsigned int i;

	for (i = 0; i < VRX619_RING_COUNT; i++) {
		ring = &dp->ring[i];
		ring->count = i <= VRX619_TXOUT ? VRX619_TX_COUNT : VRX619_RX_COUNT;
		ring->words = i == VRX619_TXIN || i == VRX619_RXOUT ? 4 : 2;
		ring->head = 0;
		ring->desc = dma_alloc_coherent(dev, ring->count * ring->words * 4,
						&ring->dma, GFP_KERNEL);
		if (!ring->desc)
			goto failed;
	}
	/* Fixed 4 KiB slots avoid the endpoint's packet boundary erratum. */
	dp->tx_buffer = dma_alloc_coherent(dev, VRX619_TX_COUNT * VRX619_BUFFER_SIZE,
					   &dp->tx_dma, GFP_KERNEL);
	if (!dp->tx_buffer)
		goto failed;
	dp->rx_buffer = dma_alloc_coherent(dev, VRX619_RX_COUNT * VRX619_BUFFER_SIZE,
					   &dp->rx_dma, GFP_KERNEL);
	if (!dp->rx_buffer)
		goto failed;
	if ((dp->tx_dma | dp->rx_dma) & (VRX619_BUFFER_SIZE - 1)) {
		vrx619_buffers_free(dp);
		return -EIO;
	}
	bitmap_zero(dp->tx_busy, VRX619_TX_COUNT);
	for (i = 0; i < VRX619_RX_COUNT; i++) {
		dp->ring[VRX619_RXIN].desc[i * 2] =
			cpu_to_le32(dp->rx_dma + i * VRX619_BUFFER_SIZE);
		dp->ring[VRX619_RXOUT].desc[i * 4 + 3] = cpu_to_le32(VRX619_DESC_OWN);
	}
	return 0;

failed:
	vrx619_buffers_free(dp);
	return -ENOMEM;
}

static void vrx619_dp_fault(struct vrx619_dp *dp)
{
	if (!READ_ONCE(dp->fault))
		netdev_err(dp->netdev, "endpoint fault; packet interface stopped\n");
	WRITE_ONCE(dp->fault, true);
	netif_carrier_off(dp->netdev);
	netif_stop_queue(dp->netdev);
}

static bool vrx619_tx_room(struct vrx619_dp *dp)
{
	return !bitmap_full(dp->tx_busy, VRX619_TX_COUNT) &&
	       vrx619_dp_read(dp, VRX619_TXIN_COUNT) < VRX619_TX_COUNT;
}

static void vrx619_tx_complete(struct vrx619_dp *dp)
{
	struct vrx619_ring *ring = &dp->ring[VRX619_TXOUT];
	struct net_device *ndev = dp->netdev;
	u32 count, address, slot, done = 0;
	__le32 *desc;

	spin_lock(&dp->tx_lock);
	count = vrx619_dp_read(dp, VRX619_TXOUT_COUNT);
	if (count > ring->count) {
		vrx619_dp_fault(dp);
		goto unlock;
	}
	while (done < count) {
		desc = vrx619_desc(ring);
		if (le32_to_cpu(READ_ONCE(desc[1])) != VRX619_TXOUT_MAGIC)
			break;
		dma_rmb();
		address = le32_to_cpu(desc[0]) & ~7U;
		if (address < dp->tx_dma || address - dp->tx_dma >=
		    VRX619_TX_COUNT * VRX619_BUFFER_SIZE ||
		    (address & (VRX619_BUFFER_SIZE - 1))) {
			vrx619_dp_fault(dp);
			break;
		}
		slot = (address - dp->tx_dma) / VRX619_BUFFER_SIZE;
		if (!test_and_clear_bit(slot, dp->tx_busy)) {
			vrx619_dp_fault(dp);
			break;
		}
		if (dp->tx_length[slot]) {
			ndev->stats.tx_packets++;
			ndev->stats.tx_bytes += dp->tx_length[slot];
		}
		WRITE_ONCE(desc[1], 0);
		vrx619_ring_next(ring);
		done++;
	}
	if (done) {
		dma_wmb();
		vrx619_dp_write(dp, VRX619_TXOUT_SUB, done);
	}
	if (READ_ONCE(dp->running) && READ_ONCE(dp->ptm_up) &&
	    !READ_ONCE(dp->fault) && netif_queue_stopped(ndev) && vrx619_tx_room(dp))
		netif_wake_queue(ndev);
unlock:
	spin_unlock(&dp->tx_lock);
}

static int vrx619_rx_poll(struct vrx619_dp *dp, int budget)
{
	struct vrx619_ring *ring = &dp->ring[VRX619_RXOUT];
	struct vrx619_ring *rxin = &dp->ring[VRX619_RXIN];
	struct net_device *ndev = dp->netdev;
	u32 count, flags, address, slot, length;
	struct sk_buff *skb;
	__le32 *desc, *in;
	int done = 0;

	count = vrx619_dp_read(dp, VRX619_RXOUT_COUNT);
	if (count > ring->count) {
		vrx619_dp_fault(dp);
		return 0;
	}
	while (done < count && done < budget) {
		desc = vrx619_desc(ring);
		flags = le32_to_cpu(READ_ONCE(desc[3]));
		if (flags & VRX619_DESC_OWN)
			break;
		dma_rmb();
		address = le32_to_cpu(desc[2]) & ~7U;
		if (address < dp->rx_dma || address - dp->rx_dma >=
		    VRX619_RX_COUNT * VRX619_BUFFER_SIZE ||
		    (address & (VRX619_BUFFER_SIZE - 1))) {
			vrx619_dp_fault(dp);
			break;
		}
		slot = (address - dp->rx_dma) / VRX619_BUFFER_SIZE;
		length = flags & VRX619_DESC_LENGTH;
		if (vrx619_dp_read(dp, VRX619_RXIN_COUNT) >= rxin->count) {
			vrx619_dp_fault(dp);
			break;
		}
		if (length < ETH_HLEN ||
		    length > VRX619_FRAME_SIZE ||
		    (flags & (VRX619_DESC_SOP | VRX619_DESC_EOP)) !=
		    (VRX619_DESC_SOP | VRX619_DESC_EOP)) {
			ndev->stats.rx_errors++;
		} else if (READ_ONCE(dp->transport) == VRX619_TRANSPORT_ATM &&
			   vrx619_pmi_transport(dp->pmi) == VRX619_TRANSPORT_ATM) {
			vrx619_atm_receive(dp, le32_to_cpu(desc[0]), dp->rx_buffer +
					   slot * VRX619_BUFFER_SIZE, length);
		} else if (READ_ONCE(dp->transport) == VRX619_TRANSPORT_PTM &&
			   vrx619_pmi_transport(dp->pmi) == VRX619_TRANSPORT_PTM &&
			   READ_ONCE(dp->ptm_up) && length >= ETH_HLEN + ETH_FCS_LEN) {
			length -= ETH_FCS_LEN;
			skb = napi_alloc_skb(&dp->napi, length);
			if (skb) {
				skb_put_data(skb, dp->rx_buffer +
					     slot * VRX619_BUFFER_SIZE, length);
				skb->protocol = eth_type_trans(skb, ndev);
				ndev->stats.rx_packets++;
				ndev->stats.rx_bytes += length;
				napi_gro_receive(&dp->napi, skb);
			} else {
				ndev->stats.rx_dropped++;
			}
		}
		WRITE_ONCE(desc[3], cpu_to_le32(VRX619_DESC_OWN));
		vrx619_ring_next(ring);
		in = vrx619_desc(rxin);
		in[0] = cpu_to_le32(dp->rx_dma + slot * VRX619_BUFFER_SIZE);
		in[1] = 0;
		vrx619_ring_next(rxin);
		dma_wmb();
		vrx619_dp_write(dp, VRX619_RXOUT_SUB, 1);
		vrx619_dp_write(dp, VRX619_RXIN_ADD, 1);
		done++;
	}
	return done;
}

static int vrx619_napi_poll(struct napi_struct *napi, int budget)
{
	struct vrx619_dp *dp = container_of(napi, struct vrx619_dp, napi);
	int done = 0;

	if (!READ_ONCE(dp->fault)) {
		vrx619_tx_complete(dp);
		if (budget && !READ_ONCE(dp->fault))
			done = vrx619_rx_poll(dp, budget);
	}
	if (budget && done < budget)
		napi_complete_done(napi, done);
	return done;
}

static enum hrtimer_restart vrx619_dp_timer(struct hrtimer *timer)
{
	struct vrx619_dp *dp = container_of(timer, struct vrx619_dp, timer);

	if (!READ_ONCE(dp->running) || READ_ONCE(dp->fault))
		return HRTIMER_NORESTART;
	napi_schedule(&dp->napi);
	hrtimer_forward_now(timer, ns_to_ktime(VRX619_POLL_NS));
	return HRTIMER_RESTART;
}

static int vrx619_traffic(struct vrx619_dp *dp, bool enable)
{
	__le16 port = 0;
	__le16 traffic[2] = { cpu_to_le16(enable), 0 };
	int ret;

	if (enable) {
		ret = vrx619_pmi_xfer(dp->pmi, 0x8107, 0, &port, sizeof(port), NULL, 0);
		if (ret)
			return ret;
	}
	return vrx619_pmi_xfer(dp->pmi, 0x8108, 0, traffic, sizeof(traffic), NULL, 0);
}

static int vrx619_atm_configure(struct vrx619_dp *dp)
{
	struct vrx619_atm_pvc pvc[VRX619_ATM_VCCS];
	struct vrx619_atm_tables tables;
	unsigned int i, count;
	u32 generation;
	int ret;

	spin_lock_bh(&dp->atm_lock);
	for (i = 0; i < VRX619_ATM_VCCS; i++)
		pvc[i] = dp->vcc[i].pvc;
	generation = dp->atm_generation;
	spin_unlock_bh(&dp->atm_lock);
	count = vrx619_atm_tables(pvc, &tables);
	if (!count)
		return 0;
	ret = vrx619_pmi_xfer(dp->pmi, 0x9117, 0, tables.vccs,
			      sizeof(tables.vccs), NULL, 0);
	if (!ret)
		ret = vrx619_pmi_xfer(dp->pmi, 0x9116, 0, tables.queues,
				      sizeof(tables.queues), NULL, 0);
	if (!ret)
		ret = vrx619_pmi_xfer(dp->pmi, 0x9118, 0, tables.vlans,
				      sizeof(tables.vlans), NULL, 0);
	if (ret)
		return ret;
	spin_lock_bh(&dp->atm_lock);
	if (dp->atm_generation == generation)
		dp->atm_applied = generation;
	else
		ret = -EAGAIN;
	spin_unlock_bh(&dp->atm_lock);
	return ret ? ret : count;
}

static void vrx619_link_work(struct work_struct *work)
{
	struct vrx619_dp *dp = container_of(to_delayed_work(work), struct vrx619_dp,
					   link_work);
	__le16 query = cpu_to_le16(6);
	unsigned int transport;
	u8 status[6];
	bool showtime = false, empty;
	int ret;

	if (!READ_ONCE(dp->running))
		return;
	if (READ_ONCE(dp->fault)) {
		atm_dev_signal_change(dp->atm, ATM_PHY_SIG_LOST);
		return;
	}
	transport = vrx619_pmi_transport(dp->pmi);
	if (transport == VRX619_TRANSPORT_PTM && !READ_ONCE(dp->ptm_up))
		transport = VRX619_TRANSPORT_NONE;
	if (transport != READ_ONCE(dp->transport) ||
	    (transport == VRX619_TRANSPORT_NONE && READ_ONCE(dp->traffic_enabled)) ||
	    (transport == VRX619_TRANSPORT_ATM &&
	     READ_ONCE(dp->atm_generation) != READ_ONCE(dp->atm_applied))) {
		spin_lock_bh(&dp->atm_lock);
		spin_lock(&dp->tx_lock);
		WRITE_ONCE(dp->transport, VRX619_TRANSPORT_NONE);
		spin_unlock(&dp->tx_lock);
		spin_unlock_bh(&dp->atm_lock);
		netif_carrier_off(dp->netdev);
		atm_dev_signal_change(dp->atm, ATM_PHY_SIG_LOST);
		/* Drain TX before disabling firmware traffic; reject new submissions. */
		spin_lock_bh(&dp->tx_lock);
		empty = bitmap_empty(dp->tx_busy, VRX619_TX_COUNT);
		spin_unlock_bh(&dp->tx_lock);
		if (!empty)
			goto again;
		ret = vrx619_traffic(dp, false);
		if (ret)
			goto error;
		WRITE_ONCE(dp->traffic_enabled, false);
		/* Discard old RX packets before reusing a firmware VCC/queue index. */
		if (vrx619_dp_read(dp, VRX619_RXOUT_COUNT))
			goto again;
		if (transport == VRX619_TRANSPORT_ATM) {
			ret = vrx619_atm_configure(dp);
			if (ret < 0)
				goto error;
			if (!ret)
				goto again;
		}
		WRITE_ONCE(dp->transport, transport);
	}
	if (transport != VRX619_TRANSPORT_NONE) {
		ret = vrx619_pmi_xfer(dp->pmi, 0x0200, 0, &query, sizeof(query),
				      status, sizeof(status));
		if (ret < 0)
			goto error;
		if (ret != sizeof(status)) {
			ret = -EPROTO;
			goto error;
		}
		showtime = get_unaligned_le32(status) == 0x80000 ||
			   get_unaligned_le32(status) == 0x20000;
	}
	if (showtime != READ_ONCE(dp->traffic_enabled)) {
		if (!showtime) {
			netif_carrier_off(dp->netdev);
			atm_dev_signal_change(dp->atm, ATM_PHY_SIG_LOST);
		}
		ret = vrx619_traffic(dp, showtime);
		if (ret)
			goto error;
		WRITE_ONCE(dp->traffic_enabled, showtime);
		if (showtime && transport == VRX619_TRANSPORT_PTM)
			netif_carrier_on(dp->netdev);
		if (showtime && transport == VRX619_TRANSPORT_ATM)
			atm_dev_signal_change(dp->atm, ATM_PHY_SIG_FOUND);
	}
again:
	if (READ_ONCE(dp->running))
		schedule_delayed_work(&dp->link_work, msecs_to_jiffies(250));
	return;
error:
	if (ret == -EBUSY || ret == -EAGAIN)
		goto again;
	atm_dev_signal_change(dp->atm, ATM_PHY_SIG_LOST);
	vrx619_dp_fault(dp);
}

static netdev_tx_t vrx619_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct vrx619_dp *dp = netdev_priv(ndev);
	struct vrx619_ring *ring = &dp->ring[VRX619_TXIN];
	u32 slot, length = max_t(u32, skb->len, ETH_ZLEN);
	__le32 *desc;
	void *buffer;

	if (unlikely(READ_ONCE(dp->fault) || length > VRX619_FRAME_SIZE ||
		     READ_ONCE(dp->transport) != VRX619_TRANSPORT_PTM ||
		     vrx619_pmi_transport(dp->pmi) != VRX619_TRANSPORT_PTM))
		goto drop;
	spin_lock_bh(&dp->tx_lock);
	if (READ_ONCE(dp->transport) != VRX619_TRANSPORT_PTM) {
		spin_unlock_bh(&dp->tx_lock);
		goto drop;
	}
	if (!vrx619_tx_room(dp)) {
		netif_stop_queue(ndev);
		spin_unlock_bh(&dp->tx_lock);
		return NETDEV_TX_BUSY;
	}
	slot = find_first_zero_bit(dp->tx_busy, VRX619_TX_COUNT);
	buffer = dp->tx_buffer + slot * VRX619_BUFFER_SIZE;
	if (skb_copy_bits(skb, 0, buffer, skb->len)) {
		spin_unlock_bh(&dp->tx_lock);
		goto drop;
	}
	if (length > skb->len)
		memset(buffer + skb->len, 0, length - skb->len);
	set_bit(slot, dp->tx_busy);
	dp->tx_length[slot] = length;
	desc = vrx619_desc(ring);
	desc[0] = 0;
	desc[1] = 0;
	desc[2] = cpu_to_le32(dp->tx_dma + slot * VRX619_BUFFER_SIZE);
	dma_wmb();
	WRITE_ONCE(desc[3], cpu_to_le32(length | VRX619_DESC_OWN |
					 VRX619_DESC_SOP | VRX619_DESC_EOP));
	vrx619_ring_next(ring);
	dma_wmb();
	vrx619_dp_write(dp, VRX619_TXIN_ADD, 1);
	if (!vrx619_tx_room(dp))
		netif_stop_queue(ndev);
	spin_unlock_bh(&dp->tx_lock);
	dev_consume_skb_any(skb);
	return NETDEV_TX_OK;
drop:
	ndev->stats.tx_dropped++;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static void vrx619_dma_stop(struct vrx619_dp *dp)
{
	struct pci_dev *pdev = dp->pdata->pdev;
	int ret;

	ret = vrx619_aca_stop(dp->pdata);
	pci_clear_master(pdev);
	if (ret) {
		dp->quarantine = true;
		dev_err(&pdev->dev, "ACA stop failed: %d; retaining buffers until reboot\n", ret);
	}
	if (!pci_wait_for_pending_transaction(pdev)) {
		dp->quarantine = true;
		dev_err(&pdev->dev, "DMA did not drain; retaining buffers until reboot\n");
	}
	if (dp->quarantine)
		WRITE_ONCE(dp->fault, true);
}

static int vrx619_start(struct vrx619_dp *dp)
{
	struct vrx619_aca_mem rings[VRX619_RING_COUNT];
	__le32 hif[VRX619_ACA_HIF_WORDS];
	unsigned int i;
	int ret;

	if (READ_ONCE(dp->fault))
		return -EIO;
	ret = vrx619_buffers_alloc(dp);
	if (ret)
		return ret;
	for (i = 0; i < VRX619_RING_COUNT; i++) {
		rings[i].base = dp->ring[i].dma;
		rings[i].count = dp->ring[i].count;
	}
	vrx619_aca_hif(&dp->image, pci_bus_address(dp->pdata->pdev, 0), rings, hif);
	pci_set_master(dp->pdata->pdev);
	ret = vrx619_aca_init(dp->pdata, &dp->image, hif);
	if (ret)
		goto stop;
	dma_wmb();
	vrx619_dp_write(dp, VRX619_RXIN_ADD, VRX619_RX_COUNT);
	ret = vrx619_aca_start(dp->pdata, &dp->image);
	if (ret)
		goto stop;
	napi_enable(&dp->napi);
	WRITE_ONCE(dp->running, true);
	hrtimer_start(&dp->timer, ns_to_ktime(VRX619_POLL_NS), HRTIMER_MODE_REL);
	schedule_delayed_work(&dp->link_work, 0);
	return 0;
stop:
	vrx619_dma_stop(dp);
	vrx619_buffers_free(dp);
	return ret;
}

static void vrx619_stop(struct vrx619_dp *dp)
{
	struct net_device *ndev = dp->netdev;

	WRITE_ONCE(dp->running, false);
	netif_tx_disable(ndev);
	cancel_delayed_work_sync(&dp->link_work);
	hrtimer_cancel(&dp->timer);
	napi_disable(&dp->napi);
	vrx619_traffic(dp, false);
	dp->traffic_enabled = false;
	WRITE_ONCE(dp->transport, VRX619_TRANSPORT_NONE);
	netif_carrier_off(ndev);
	atm_dev_signal_change(dp->atm, ATM_PHY_SIG_LOST);
	vrx619_dma_stop(dp);
	vrx619_buffers_free(dp);
}

static int vrx619_open(struct net_device *ndev)
{
	struct vrx619_dp *dp = netdev_priv(ndev);
	int ret = 0;

	mutex_lock(&dp->state_lock);
	if (dp->removed || !READ_ONCE(dp->registered))
		ret = -ENODEV;
	else if (!dp->running)
		ret = vrx619_start(dp);
	if (!ret) {
		WRITE_ONCE(dp->ptm_up, true);
		netif_start_queue(ndev);
	}
	mutex_unlock(&dp->state_lock);
	return ret;
}

static int vrx619_close(struct net_device *ndev)
{
	struct vrx619_dp *dp = netdev_priv(ndev);

	mutex_lock(&dp->state_lock);
	WRITE_ONCE(dp->ptm_up, false);
	netif_tx_disable(ndev);
	netif_carrier_off(ndev);
	if (!dp->atm_users && dp->running)
		vrx619_stop(dp);
	mutex_unlock(&dp->state_lock);
	return 0;
}

static void vrx619_dp_release(struct kref *ref)
{
	struct vrx619_dp *dp = container_of(ref, struct vrx619_dp, ref);

	free_netdev(dp->netdev);
}

static void vrx619_atm_dev_close(struct atm_dev *atm)
{
	struct vrx619_dp *dp = atm->dev_data;

	kref_put(&dp->ref, vrx619_dp_release);
}

static int vrx619_atm_open(struct atm_vcc *vcc)
{
	struct vrx619_dp *dp = vcc->dev->dev_data;
	unsigned int i, slot = VRX619_ATM_VCCS;
	int ret = 0;

	if (!dp)
		return -EAGAIN;
	if (vcc->qos.aal != ATM_AAL5 ||
	    (vcc->qos.txtp.traffic_class != ATM_UBR &&
	     vcc->qos.txtp.traffic_class != ATM_NONE) ||
	    (vcc->qos.rxtp.traffic_class != ATM_UBR &&
	     vcc->qos.rxtp.traffic_class != ATM_NONE))
		return -EOPNOTSUPP;
	if (vcc->vpi < 0 || vcc->vpi > 255 || vcc->vci < 32 || vcc->vci > 65535)
		return -EINVAL;
	mutex_lock(&dp->state_lock);
	if (dp->removed || dp->fault || !READ_ONCE(dp->registered)) {
		ret = -ENODEV;
		goto out;
	}
	for (i = 0; i < VRX619_ATM_VCCS; i++) {
		struct vrx619_vcc *vc = &dp->vcc[i];

		if (!rcu_access_pointer(vc->vcc)) {
			if (slot == VRX619_ATM_VCCS)
				slot = i;
		} else if (vc->pvc.vpi == vcc->vpi && vc->pvc.vci == vcc->vci) {
			ret = -EADDRINUSE;
			goto out;
		}
	}
	if (slot == VRX619_ATM_VCCS) {
		ret = -ENOSPC;
		goto out;
	}
	if (!dp->running) {
		ret = vrx619_start(dp);
		if (ret)
			goto out;
	}
	vcc->qos.txtp.max_sdu = min(vcc->qos.txtp.max_sdu, VRX619_FRAME_SIZE);
	vcc->qos.rxtp.max_sdu = min(vcc->qos.rxtp.max_sdu, VRX619_FRAME_SIZE);
	spin_lock_bh(&dp->atm_lock);
	memset(&dp->vcc[slot].pvc, 0, sizeof(dp->vcc[slot].pvc));
	dp->vcc[slot].pvc.vpi = vcc->vpi;
	dp->vcc[slot].pvc.vci = vcc->vci;
	vcc->dev_data = &dp->vcc[slot];
	rcu_assign_pointer(dp->vcc[slot].vcc, vcc);
	dp->atm_users++;
	dp->atm_generation++;
	spin_unlock_bh(&dp->atm_lock);
	set_bit(ATM_VF_ADDR, &vcc->flags);
	set_bit(ATM_VF_READY, &vcc->flags);
out:
	mutex_unlock(&dp->state_lock);
	return ret;
}

static void vrx619_atm_close(struct atm_vcc *vcc)
{
	struct vrx619_dp *dp = vcc->dev->dev_data;
	struct vrx619_vcc *vc = vcc->dev_data;

	clear_bit(ATM_VF_READY, &vcc->flags);
	clear_bit(ATM_VF_ADDR, &vcc->flags);
	if (!vc)
		return;
	mutex_lock(&dp->state_lock);
	spin_lock_bh(&dp->atm_lock);
	RCU_INIT_POINTER(vc->vcc, NULL);
	vc->pvc.configured = false;
	dp->atm_users--;
	dp->atm_generation++;
	spin_unlock_bh(&dp->atm_lock);
	synchronize_net();
	vcc->dev_data = NULL;
	if (!dp->removed && dp->running) {
		if (!dp->atm_users && !dp->ptm_up)
			vrx619_stop(dp);
		else
			mod_delayed_work(system_wq, &dp->link_work, 0);
	}
	mutex_unlock(&dp->state_lock);
}

static void vrx619_atm_setup(struct atm_vcc *vcc, enum atm_mpoa_type type, bool llc)
{
	struct vrx619_dp *dp = vcc->dev->dev_data;
	struct vrx619_vcc *vc = vcc->dev_data;

	spin_lock_bh(&dp->atm_lock);
	if (!dp->removed && vc) {
		vc->pvc.type = type;
		vc->pvc.llc = llc;
		vc->pvc.configured = true;
		dp->atm_generation++;
		mod_delayed_work(system_wq, &dp->link_work, 0);
	}
	spin_unlock_bh(&dp->atm_lock);
}

static int vrx619_atm_send(struct atm_vcc *vcc, struct sk_buff *skb)
{
	struct vrx619_dp *dp = vcc->dev->dev_data;
	struct vrx619_vcc *vc = vcc->dev_data;
	struct vrx619_ring *ring = &dp->ring[VRX619_TXIN];
	u32 slot, length, subif;
	__le32 *desc;
	void *buffer;
	int ret = -ENOLINK;

	spin_lock_bh(&dp->atm_lock);
	if (dp->removed || !vc || !vc->pvc.configured ||
	    !READ_ONCE(dp->running) || READ_ONCE(dp->fault) ||
	    READ_ONCE(dp->transport) != VRX619_TRANSPORT_ATM ||
	    vrx619_pmi_transport(dp->pmi) != VRX619_TRANSPORT_ATM ||
	    !READ_ONCE(dp->traffic_enabled) || dp->atm_generation != dp->atm_applied)
		goto unlock;
	ret = -EMSGSIZE;
	if (!skb->len || skb->len > VRX619_FRAME_SIZE)
		goto unlock;
	/* The raw-AAL5 descriptor bit preserves the stack's encapsulation. */
	length = max_t(u32, skb->len, vc->pvc.type == ATM_MPOA_EOA ? 68 : 60);
	slot = vc - dp->vcc;
	subif = slot << 8 | slot << 3 | BIT(2) | vc->pvc.type;
	spin_lock(&dp->tx_lock);
	ret = -ENOBUFS;
	if (!vrx619_tx_room(dp))
		goto tx_unlock;
	slot = find_first_zero_bit(dp->tx_busy, VRX619_TX_COUNT);
	buffer = dp->tx_buffer + slot * VRX619_BUFFER_SIZE;
	ret = skb_copy_bits(skb, 0, buffer, skb->len);
	if (ret)
		goto tx_unlock;
	memset(buffer + skb->len, 0, length - skb->len);
	set_bit(slot, dp->tx_busy);
	dp->tx_length[slot] = 0;
	desc = vrx619_desc(ring);
	desc[0] = cpu_to_le32(subif);
	desc[1] = 0;
	desc[2] = cpu_to_le32(dp->tx_dma + slot * VRX619_BUFFER_SIZE);
	dma_wmb();
	WRITE_ONCE(desc[3], cpu_to_le32(length | VRX619_DESC_OWN |
					 VRX619_DESC_SOP | VRX619_DESC_EOP));
	vrx619_ring_next(ring);
	dma_wmb();
	vrx619_dp_write(dp, VRX619_TXIN_ADD, 1);
tx_unlock:
	spin_unlock(&dp->tx_lock);
unlock:
	spin_unlock_bh(&dp->atm_lock);
	atomic_inc(ret ? &vcc->stats->tx_err : &vcc->stats->tx);
	if (vcc->pop)
		vcc->pop(vcc, skb);
	else
		dev_kfree_skb_any(skb);
	return ret;
}

static void vrx619_atm_receive(struct vrx619_dp *dp, u32 subif,
			       const u8 *data, size_t len)
{
	unsigned int slot = (subif >> 8) & 0xf;
	struct vrx619_atm_pvc pvc;
	struct atm_vcc *vcc;
	struct sk_buff *skb;
	int size;

	if (slot >= VRX619_ATM_VCCS)
		return;
	rcu_read_lock();
	spin_lock(&dp->atm_lock);
	vcc = rcu_dereference(dp->vcc[slot].vcc);
	pvc = dp->vcc[slot].pvc;
	if (dp->atm_generation != dp->atm_applied || !pvc.configured)
		vcc = NULL;
	spin_unlock(&dp->atm_lock);
	if (!vcc || !test_bit(ATM_VF_READY, &vcc->flags))
		goto out;
	skb = napi_alloc_skb(&dp->napi, VRX619_FRAME_SIZE);
	if (!skb)
		goto drop;
	size = vrx619_atm_encapsulate(&pvc, skb->data, VRX619_FRAME_SIZE, data, len);
	if (size < 0 || size > vcc->qos.rxtp.max_sdu ||
	    !atm_charge(vcc, skb->truesize)) {
		dev_kfree_skb_any(skb);
		goto drop;
	}
	skb_put(skb, size);
	ATM_SKB(skb)->vcc = vcc;
	atomic_inc(&vcc->stats->rx);
	vcc->push(vcc, skb);
	goto out;
drop:
	atomic_inc(&vcc->stats->rx_drop);
out:
	rcu_read_unlock();
}

static const struct atmdev_ops vrx619_atm_ops = {
	.dev_close = vrx619_atm_dev_close,
	.open = vrx619_atm_open,
	.close = vrx619_atm_close,
	.setup = vrx619_atm_setup,
	.send = vrx619_atm_send,
	.owner = THIS_MODULE,
};

static const struct net_device_ops vrx619_netdev_ops = {
	.ndo_open = vrx619_open,
	.ndo_stop = vrx619_close,
	.ndo_start_xmit = vrx619_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};

static int vrx619_dp_probe(struct platform_device *pdev)
{
	const struct vrx619_platform_data *pdata = dev_get_platdata(&pdev->dev);
	struct net_device *ndev;
	struct vrx619_dp *dp;
	int ret;

	if (!pdata || pdata->memsize < SZ_8M)
		return -EINVAL;
	ndev = alloc_etherdev(sizeof(*dp));
	if (!ndev)
		return -ENOMEM;
	SET_NETDEV_DEV(ndev, &pdev->dev);
	strscpy(ndev->name, "ptm%d", IFNAMSIZ);
	ndev->netdev_ops = &vrx619_netdev_ops;
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = ETH_DATA_LEN;
	eth_hw_addr_random(ndev);
	{
		u8 mac[ETH_ALEN];

		if (!device_get_mac_address(&pdata->pdev->dev, mac))
			eth_hw_addr_set(ndev, mac);
	}
	netif_carrier_off(ndev);
	dp = netdev_priv(ndev);
	dp->netdev = ndev;
	dp->pdata = pdata;
	dp->pmi = pdev->dev.parent;
	spin_lock_init(&dp->tx_lock);
	spin_lock_init(&dp->atm_lock);
	mutex_init(&dp->state_lock);
	kref_init(&dp->ref);
	INIT_DELAYED_WORK(&dp->link_work, vrx619_link_work);
	hrtimer_setup(&dp->timer, vrx619_dp_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	netif_napi_add(ndev, &dp->napi, vrx619_napi_poll);
	ret = request_firmware(&dp->firmware, VRX619_ACA_FIRMWARE, &pdev->dev);
	if (ret)
		goto free;
	ret = vrx619_aca_parse(&dp->image, dp->firmware->data, dp->firmware->size);
	if (ret)
		goto release;
	ret = register_netdev(ndev);
	if (ret)
		goto release;
	dp->atm = atm_dev_register("vrx619", &pdev->dev, &vrx619_atm_ops, -1, NULL);
	if (!dp->atm) {
		unregister_netdev(ndev);
		ret = -ENOMEM;
		goto release;
	}
	kref_get(&dp->ref);
	dp->atm->dev_data = dp;
	dp->atm->ci_range.vpi_bits = 8;
	dp->atm->ci_range.vci_bits = 16;
	dp->atm->link_rate = 0;
	ether_addr_copy(dp->atm->esi, ndev->dev_addr);
	atm_dev_signal_change(dp->atm, ATM_PHY_SIG_LOST);
	WRITE_ONCE(dp->registered, true);
	platform_set_drvdata(pdev, ndev);
	return 0;
release:
	release_firmware(dp->firmware);
free:
	netif_napi_del(&dp->napi);
	free_netdev(ndev);
	return dev_err_probe(&pdev->dev, ret, "packet interface setup failed\n");
}

static void vrx619_dp_remove(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct vrx619_dp *dp = netdev_priv(ndev);

	mutex_lock(&dp->state_lock);
	spin_lock_bh(&dp->atm_lock);
	WRITE_ONCE(dp->removed, true);
	spin_unlock_bh(&dp->atm_lock);
	if (dp->running)
		vrx619_stop(dp);
	mutex_unlock(&dp->state_lock);
	atm_dev_deregister(dp->atm);
	unregister_netdev(ndev);
	netif_napi_del(&dp->napi);
	release_firmware(dp->firmware);
	kref_put(&dp->ref, vrx619_dp_release);
}

static void vrx619_dp_shutdown(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct vrx619_dp *dp = netdev_priv(ndev);

	mutex_lock(&dp->state_lock);
	spin_lock_bh(&dp->atm_lock);
	WRITE_ONCE(dp->removed, true);
	spin_unlock_bh(&dp->atm_lock);
	if (dp->running)
		vrx619_stop(dp);
	mutex_unlock(&dp->state_lock);
}

static struct platform_driver vrx619_dp_driver = {
	.probe = vrx619_dp_probe,
	.remove = vrx619_dp_remove,
	.shutdown = vrx619_dp_shutdown,
	.driver = {
		.name = "vrx619-dp",
	},
};
module_platform_driver(vrx619_dp_driver);

MODULE_ALIAS("platform:vrx619-dp");
MODULE_FIRMWARE(VRX619_ACA_FIRMWARE);
MODULE_DESCRIPTION("VRX619 PTM and ATM packet transport");
MODULE_LICENSE("GPL");
