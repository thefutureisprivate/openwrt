// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <string.h>

#include "line.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define WRITE(line, cmd, ...) \
	write_words(line, cmd, (const uint16_t[]){ __VA_ARGS__ }, \
		    ARRAY_SIZE(((const uint16_t[]){ __VA_ARGS__ })))

#define FSM_HW_CONFIG 0x000002
#define FSM_IDLE      0x000004
#define FSM_FAIL      0x000010
#define FSM_SHOWTIME  0x080000
#define FSM_SHOWTIME_ALT 0x020000
#define VTSE_VDSL     0x00040000
#define VTSE_ADSL     0x2000fbff
#define IDLE_TIMEOUT  10000
#define COOLING_TIME  120000

static uint16_t get_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)get_le16(p) | (uint32_t)get_le16(p + 2) << 16;
}

static int write_words(struct vrx619_line *line, uint16_t command,
		       const uint16_t *words, size_t count)
{
	uint8_t data[496];
	size_t i;
	int ret;

	if (count > sizeof(data) / 2)
		return -EMSGSIZE;
	for (i = 0; i < count; i++) {
		data[2 * i] = words[i];
		data[2 * i + 1] = words[i] >> 8;
	}
	ret = line->ops->xfer(line->ctx, command, data, count * 2, NULL, 0);
	if (ret < 0)
		line->failed_command = command;
	return ret < 0 ? ret : 0;
}

static int read_status(struct vrx619_line *line, uint16_t command,
		       uint8_t *data, size_t size)
{
	uint8_t query[2] = { size, size >> 8 };
	int ret = line->ops->xfer(line->ctx, command, query, sizeof(query), data, size);

	if (ret != (int)size) {
		line->failed_command = command;
		return ret < 0 ? ret : -EPROTO;
	}
	return 0;
}

static int request_idle(struct vrx619_line *line)
{
	int ret;

	line->transport = VRX619_TRANSPORT_NONE;
	ret = line->ops->transport(line->ctx, VRX619_TRANSPORT_NONE);
	if (ret < 0)
		return ret;
	return WRITE(line, 0x8100, FSM_IDLE, 0);
}

static int configure_hardware(struct vrx619_line *line)
{
	uint16_t subtype = 0x10;
	int ret;

	if (line->config.input_swap)
		subtype |= 0x200;
	if (line->config.highpass_17mhz)
		subtype |= 1;
	/* Dual hybrid, LE87286 line driver, relay mode 1, single CPE line. */
	ret = WRITE(line, 0x8106, 0);
	if (ret < 0)
		return ret;
	ret = WRITE(line, 0x8101, 0, subtype, 3, 0x10, 1, 0, 120, 120, 1);
	if (ret < 0)
		return ret;
	ret = WRITE(line, 0x8102, 0, 0, 1, 0, 1, 0, line->config.annex_b);
	if (ret < 0)
		return ret;
	return request_idle(line);
}

static int configure_common(struct vrx619_line *line)
{
	int ret;

	/* Firmware temperature supervision and FSM/OLR/failure notifications. */
	ret = WRITE(line, 0x8837, 1, 250);
	if (ret < 0)
		return ret;
	ret = WRITE(line, 0x8900, 0x06ff, 0x1eff, 6);
	if (ret < 0)
		return ret;
	ret = WRITE(line, 0x890f, 0x1f6);
	if (ret < 0)
		return ret;
	ret = WRITE(line, 0x8910, 0x1f6);
	if (ret < 0)
		return ret;
	ret = WRITE(line, 0x8a00, 0x7fff, 0x4b);
	if (ret < 0)
		return ret;
	ret = WRITE(line, 0x8901, 0x301b);
	if (ret < 0)
		return ret;
	ret = WRITE(line, 0x8902, 0x201f);
	if (ret < 0)
		return ret;
	ret = WRITE(line, 0x9120, 0, 0, 0x603);
	if (ret < 0)
		return ret;
	return WRITE(line, 0x9121, 0, 0, 0x603);
}

static int configure_line(struct vrx619_line *line)
{
	const struct vrx619_config *cfg = &line->config;
	const bool vdsl = cfg->mode != VRX619_ADSL;
	const bool adsl = cfg->mode != VRX619_VDSL;
	static const uint16_t bands[][2] = {
		{ 1, 380 }, { 2, 380 }, { 7, 329 }, { 8, 329 },
		{ 3, 380 }, { 4, 380 }, { 5, 380 }, { 6, 380 },
		{ 0, 300 }, { 9, 650 },
	};
	uint16_t priority[22], rank = 1;
	uint16_t inventory[16] = {};
	size_t i;
	int ret;

	ret = WRITE(line, 0x8904, 1);
	if (ret < 0)
		return ret;
	ret = WRITE(line, 0x8905, 0x161b, 7);
	if (ret < 0)
		return ret;
	ret = WRITE(line, 0x8906, 0, 0);
	if (ret < 0)
		return ret;
	memset(priority, 0xff, sizeof(priority));
	if (vdsl) {
		if (cfg->vectoring)
			priority[1] = rank++;
		priority[3] = rank++;
	}
	if (adsl) {
		priority[cfg->annex_b ? 10 : 4] = rank++;
		priority[cfg->annex_b ? 16 : 17] = rank++;
		priority[cfg->annex_b ? 6 : 5] = rank++;
	}
	ret = write_words(line, 0x8501, priority, ARRAY_SIZE(priority));
	if (ret < 0)
		return ret;
	/* Keep RX FCS for the host; check it in firmware and add TX CRC there. */
	ret = WRITE(line, 0x9103, 0, 0, 0, 2048, 2048, 0x8240);
	if (ret < 0)
		return ret;
	ret = WRITE(line, 0x882a, cfg->annex_b ? 4 : 1, 0);
	if (ret < 0)
		return ret;
	if (vdsl) {
		ret = WRITE(line, 0x8f0b, 2, 0, 9, 3, 8, 0, 0x851e, 1, 8, 0, 0x30d4);
		if (ret < 0)
			return ret;
	}
	if (adsl) {
		ret = WRITE(line, 0x8f0b, 0, 0, 1, 0, 8, 0, 0x400, 0, 8, 0, 0x30d4);
		if (ret < 0)
			return ret;
	}
	if (vdsl) {
		ret = WRITE(line, 0x8f0c, 2, 0, 9, 3, 8, 0, 0x851e, 1, 8, 0, 63, 0);
		if (ret < 0)
			return ret;
	}
	if (adsl) {
		ret = WRITE(line, 0x8f0c, 0, 0, 9, 0, 8, 0, 0x1ff8, 0, 8, 0, 63, 0);
		if (ret < 0)
			return ret;
	}
	/* No PAF bonding. Operation options retain the reference CPE defaults. */
	ret = WRITE(line, 0x9161, 0, 0);
	if (ret < 0)
		return ret;
	ret = WRITE(line, 0x8851, 0x0c09, vdsl ? 0x02b2 : 0x0290, 0, adsl);
	if (ret < 0)
		return ret;
	ret = WRITE(line, 0x8502, 0, 0, 0, 0, 0, 0, 0, 8);
	if (ret < 0)
		return ret;
	ret = write_words(line, 0x8338, inventory, 4);
	if (ret < 0)
		return ret;
	ret = write_words(line, 0x833a, inventory, 8);
	if (ret < 0)
		return ret;
	ret = write_words(line, 0x8339, inventory, 16);
	if (ret < 0)
		return ret;
	for (i = 0; i < ARRAY_SIZE(bands); i++) {
		ret = WRITE(line, 0x8823, bands[i][0], 0x8000, 0,
			    bands[i][1], bands[i][1], 0);
		if (ret < 0)
			return ret;
	}
	ret = WRITE(line, 0x8f06, 0, 0, 10, 60, 0, 0, 0, 90);
	if (ret < 0)
		return ret;
	ret = WRITE(line, 0x850f, 0, 0, 0x140a);
	if (ret < 0)
		return ret;
	ret = WRITE(line, 0x850f, 1, 0, 0x140a);
	if (ret < 0)
		return ret;
	if (vdsl) {
		ret = WRITE(line, 0x882c, cfg->profiles, 0, 0xffff, 0x3f,
			    0, 0, 0x1f, 1, 0x3033, 0);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int negotiated_mode(struct vrx619_line *line)
{
	uint8_t status[12];
	int ret;

	ret = read_status(line, 0x0201, status, 4);
	if (ret < 0)
		return ret;
	line->vtse = get_le32(status);
	line->profile = 0;
	if (!line->vtse)
		return -EAGAIN;
	if (line->vtse == VTSE_VDSL) {
		if (line->config.mode == VRX619_ADSL)
			return -EPROTO;
		ret = read_status(line, 0x03cd, status, sizeof(status));
		if (ret < 0)
			return ret;
		line->profile = get_le16(status + 2) & 0x1ff;
		if (!line->profile || (line->profile & (line->profile - 1)) ||
		    !(line->profile & line->config.profiles))
			return -EPROTO;
		ret = line->ops->transport(line->ctx, VRX619_TRANSPORT_PTM);
		if (ret < 0)
			return ret;
		line->transport = VRX619_TRANSPORT_PTM;
		return 0;
	}
	/* G.992.x/T1.413 selections from the firmware VTSE status word. */
	if (line->config.mode == VRX619_VDSL ||
	    line->vtse & ~VTSE_ADSL || (line->vtse & (line->vtse - 1)))
		return -EPROTO;
	ret = line->ops->transport(line->ctx, VRX619_TRANSPORT_ATM);
	if (ret < 0)
		return ret;
	line->transport = VRX619_TRANSPORT_ATM;
	return 0;
}

static void fail(struct vrx619_line *line, int error)
{
	request_idle(line);
	line->enabled = false;
	line->error = error;
	line->phase = VRX619_ERROR;
}

static void retry(struct vrx619_line *line, uint64_t now, int error, bool restart)
{
	unsigned int shift = line->failures < 4 ? line->failures : 4;

	line->ops->transport(line->ctx, VRX619_TRANSPORT_NONE);
	line->transport = VRX619_TRANSPORT_NONE;
	line->vtse = 0;
	line->profile = 0;
	line->showtime_since = 0;
	line->error = error;
	line->failures++;
	if (restart && ++line->boot_failures > 3) {
		fail(line, error);
		return;
	}
	line->phase = restart ? VRX619_RESTART : VRX619_RETRY;
	line->deadline = now + (5000U << shift);
}

int vrx619_line_init(struct vrx619_line *line, const struct vrx619_config *config,
		     const struct vrx619_ops *ops, void *ctx)
{
	if (!line || !config || !ops || !ops->xfer || !ops->restart || !ops->transport ||
	    (unsigned int)config->mode > VRX619_AUTO || !config->profiles ||
	    (config->profiles & ~0x1ff) || config->training_timeout < 30 ||
	    config->training_timeout > 600 ||
	    (config->highpass_17mhz && !config->input_swap))
		return -EINVAL;
	memset(line, 0, sizeof(*line));
	line->config = *config;
	line->ops = ops;
	line->ctx = ctx;
	return 0;
}

void vrx619_line_start(struct vrx619_line *line, uint64_t now)
{
	line->enabled = true;
	if (line->phase == VRX619_COOLING && now < line->deadline)
		return;
	line->ops->transport(line->ctx, VRX619_TRANSPORT_NONE);
	line->transport = VRX619_TRANSPORT_NONE;
	line->error = 0;
	line->failed_command = 0;
	line->exception = 0;
	line->boot_failures = 0;
	line->failures = 0;
	line->showtime_since = 0;
	line->phase = VRX619_RESTART;
	line->deadline = now;
	line->communication_deadline = now + IDLE_TIMEOUT;
}

int vrx619_line_stop(struct vrx619_line *line)
{
	int ret;

	line->enabled = false;
	line->showtime_since = 0;
	ret = request_idle(line);
	/* Preserve the cooling deadline across an administrative stop/start. */
	if (line->phase != VRX619_COOLING)
		line->phase = ret ? VRX619_ERROR : VRX619_DOWN;
	line->error = ret;
	return ret;
}

void vrx619_line_tick(struct vrx619_line *line, uint64_t now)
{
	uint8_t status[6];
	bool showtime;
	int ret;

	if (!line->enabled)
		return;
	if (line->phase == VRX619_COOLING) {
		if (now < line->deadline)
			return;
		line->phase = VRX619_RESTART;
	}
	if (line->phase == VRX619_RESTART) {
		if (now < line->deadline)
			return;
		ret = line->ops->restart(line->ctx);
		if (ret) {
			retry(line, now, ret, true);
			return;
		}
		line->restarts++;
		line->state = 0;
		line->vtse = 0;
		line->profile = 0;
		line->phase = VRX619_HW_CONFIG;
		line->deadline = now + IDLE_TIMEOUT;
	}
	if (line->phase == VRX619_RETRY) {
		if (now < line->deadline)
			return;
		ret = request_idle(line);
		if (ret)
			goto recovery;
		line->phase = VRX619_IDLE;
		line->deadline = now + IDLE_TIMEOUT;
	}
	ret = read_status(line, 0x0200, status, sizeof(status));
	if (ret)
		goto recovery;
	line->state = get_le32(status);
	line->substate = get_le16(status + 4);
	line->communication_deadline = now + IDLE_TIMEOUT;
	showtime = line->state == FSM_SHOWTIME || line->state == FSM_SHOWTIME_ALT;

	if (line->phase == VRX619_HW_CONFIG && line->state == FSM_HW_CONFIG) {
		ret = configure_hardware(line);
		if (ret)
			goto recovery;
		line->phase = VRX619_IDLE;
		line->deadline = now + IDLE_TIMEOUT;
		return;
	}
	if (line->phase == VRX619_IDLE && line->state == FSM_IDLE) {
		ret = configure_common(line);
		if (!ret)
			ret = configure_line(line);
		if (!ret)
			ret = WRITE(line, 0x8100, 8, 0);
		if (ret)
			goto recovery;
		line->attempts++;
		line->phase = VRX619_TRAINING;
		line->deadline = now + (uint64_t)line->config.training_timeout * 1000;
		return;
	}
	if (line->phase == VRX619_TRAINING && showtime) {
		ret = negotiated_mode(line);
		if (ret)
			goto recovery;
		line->showtime_since = now;
		line->phase = VRX619_SHOWTIME;
		line->error = 0;
		line->failures = 0;
		line->boot_failures = 0;
		return;
	}
	if (line->phase == VRX619_SHOWTIME) {
		if (!showtime) {
			line->retrains++;
			retry(line, now, -ENOLINK, line->state == FSM_HW_CONFIG);
		}
		return;
	}
	if (line->phase == VRX619_TRAINING &&
	    (line->state == FSM_FAIL || line->state == FSM_IDLE)) {
		retry(line, now, -ENOLINK, false);
		return;
	}
	if (now < line->deadline)
		return;
	ret = -ETIMEDOUT;
recovery:
	if ((ret == -EBUSY || ret == -EAGAIN) &&
	    now < (line->phase == VRX619_SHOWTIME ?
		   line->communication_deadline : line->deadline))
		return;
	/* Rejected configuration is not fixed by repeated line activations. */
	if (ret == -EREMOTEIO || ret == -EINVAL || ret == -ENODEV) {
		fail(line, ret);
		return;
	}
	if (line->phase == VRX619_TRAINING && ret == -ETIMEDOUT) {
		int idle_ret = request_idle(line);

		retry(line, now, ret, idle_ret != 0);
	} else {
		retry(line, now, ret, true);
	}
}

void vrx619_line_event(struct vrx619_line *line, uint16_t command,
		       const uint8_t *data, size_t size, uint64_t now)
{
	if (command == 0x0205 && size >= 2) {
		line->exception = get_le16(data);
		if (data[1] == 0x37) {
			request_idle(line);
			line->showtime_since = 0;
			line->phase = VRX619_COOLING;
			line->deadline = now + COOLING_TIME;
			line->error = -EOVERFLOW;
		} else if (line->enabled && data[0] == 1 &&
			   line->phase != VRX619_RESTART && line->phase != VRX619_COOLING) {
			fail(line, -EINVAL);
		}
	}
	if (line->enabled && command == 0x02ff && line->phase != VRX619_HW_CONFIG &&
	    line->phase != VRX619_RESTART && line->phase != VRX619_COOLING)
		retry(line, now, -EPIPE, true);
}

void vrx619_line_lost_events(struct vrx619_line *line, uint64_t now)
{
	/* An over-temperature exception may have been lost with the events. */
	request_idle(line);
	line->showtime_since = 0;
	line->phase = VRX619_COOLING;
	line->deadline = now + COOLING_TIME;
	line->error = -ENOBUFS;
}

const char *vrx619_phase_name(enum vrx619_phase phase)
{
	static const char *const names[] = {
		"down", "restart", "hardware", "idle", "training", "showtime",
		"retry", "cooling", "error",
	};

	return (unsigned int)phase < ARRAY_SIZE(names) ? names[phase] : "unknown";
}
