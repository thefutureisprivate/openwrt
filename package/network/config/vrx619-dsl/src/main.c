// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <linux/vrx619.h>
#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubus.h>

#include "line.h"

struct controller {
	struct vrx619_line line;
	struct uloop_timeout timer;
	struct ubus_auto_conn bus;
	struct blob_buf reply;
	uint32_t dropped;
	uint16_t firmware_error;
	int fd;
};

static struct controller controller;

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int modem_xfer(void *ctx, uint16_t command, const void *tx, size_t tx_size,
		      void *rx, size_t rx_size)
{
	struct controller *c = ctx;
	struct vrx619_xfer xfer = {
		.command = command,
		.tx_size = tx_size,
		.rx_size = VRX619_MAX_PAYLOAD,
	};
	int ret;

	if (tx_size > sizeof(xfer.data))
		return -EMSGSIZE;
	if (tx_size)
		memcpy(xfer.data, tx, tx_size);
	ret = ioctl(c->fd, VRX619_IOC_XFER, &xfer);
	if (ret < 0) {
		ret = -errno;
		if (ret == -EREMOTEIO && xfer.response_size >= 2)
			c->firmware_error = xfer.data[0] | xfer.data[1] << 8;
		return ret;
	}
	if (!rx)
		return 0;
	if (xfer.response_size > rx_size)
		return -EMSGSIZE;
	memcpy(rx, xfer.data, xfer.response_size);
	return xfer.response_size;
}

static int modem_restart(void *ctx)
{
	struct controller *c = ctx;

	c->firmware_error = 0;
	return ioctl(c->fd, VRX619_IOC_RESTART, 0) < 0 ? -errno : 0;
}

static int modem_transport(void *ctx, unsigned int transport)
{
	struct controller *c = ctx;
	uint32_t value = transport;

	return ioctl(c->fd, VRX619_IOC_SET_TRANSPORT, &value) < 0 ? -errno : 0;
}

static const struct vrx619_ops modem_ops = {
	.xfer = modem_xfer,
	.restart = modem_restart,
	.transport = modem_transport,
};

static void status_blob(struct controller *c)
{
	struct vrx619_line *line = &c->line;
	struct blob_buf *b = &c->reply;
	uint64_t uptime = line->showtime_since ? now_ms() - line->showtime_since : 0;
	static const char *const modes[] = { "vdsl", "adsl", "auto" };

	blob_buf_init(b, 0);
	blobmsg_add_string(b, "state", vrx619_phase_name(line->phase));
	blobmsg_add_u8(b, "enabled", line->enabled);
	blobmsg_add_u8(b, "ptm_enabled", line->transport == VRX619_TRANSPORT_PTM);
	blobmsg_add_u8(b, "atm_enabled", line->transport == VRX619_TRANSPORT_ATM);
	blobmsg_add_string(b, "mode", modes[line->config.mode]);
	blobmsg_add_string(b, "annex", line->config.annex_b ? "b" : "a");
	blobmsg_add_u32(b, "firmware_state", line->state);
	blobmsg_add_u32(b, "firmware_substate", line->substate);
	blobmsg_add_u32(b, "vtse", line->vtse);
	blobmsg_add_u32(b, "profile_mask", line->profile);
	blobmsg_add_u64(b, "uptime", uptime / 1000);
	blobmsg_add_u32(b, "activations", line->attempts);
	blobmsg_add_u32(b, "firmware_restarts", line->restarts);
	blobmsg_add_u32(b, "retrains", line->retrains);
	blobmsg_add_u32(b, "dropped_events", c->dropped);
	blobmsg_add_u32(b, "error", line->error < 0 ? -line->error : 0);
	blobmsg_add_u32(b, "failed_command", line->failed_command);
	blobmsg_add_u32(b, "firmware_error", c->firmware_error);
	blobmsg_add_u32(b, "exception", line->exception);
	if (line->error)
		blobmsg_add_string(b, "error_text", strerror(-line->error));
}

static void state_changed(struct controller *c, enum vrx619_phase previous)
{
	if (previous == c->line.phase)
		return;
	syslog(c->line.phase == VRX619_ERROR ? LOG_ERR : LOG_INFO,
	       "line %s: state %#x, error %d, command %#x",
	       vrx619_phase_name(c->line.phase), c->line.state,
	       c->line.error, c->line.failed_command);
	if (c->bus.ctx.sock.registered) {
		status_blob(c);
		ubus_send_event(&c->bus.ctx, "dsl", c->reply.head);
	}
}

static void tick(struct uloop_timeout *timer)
{
	struct controller *c = container_of(timer, struct controller, timer);
	enum vrx619_phase previous = c->line.phase;
	struct vrx619_event event;
	unsigned int count;
	ssize_t size;

	/* Bound each drain so event traffic cannot starve line supervision. */
	for (count = 0; count < 128; count++) {
		size = read(c->fd, &event, sizeof(event));
		if (size < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (size != sizeof(event) || event.size > sizeof(event.data))
			break;
		if (c->dropped != event.dropped) {
			c->dropped = event.dropped;
			vrx619_line_lost_events(&c->line, now_ms());
		}
		vrx619_line_event(&c->line, event.command, event.data, event.size, now_ms());
	}
	vrx619_line_tick(&c->line, now_ms());
	state_changed(c, previous);
	uloop_timeout_set(timer, 1000);
}

static int handle_status(struct ubus_context *ctx, struct ubus_object *obj,
			 struct ubus_request_data *req, const char *method,
			 struct blob_attr *msg)
{
	(void)obj;
	(void)method;
	(void)msg;
	status_blob(&controller);
	return ubus_send_reply(ctx, req, controller.reply.head);
}

static int handle_control(struct ubus_context *ctx, struct ubus_object *obj,
			  struct ubus_request_data *req, const char *method,
			  struct blob_attr *msg)
{
	struct controller *c = &controller;
	enum vrx619_phase previous = c->line.phase;
	int ret = 0;

	if (!strcmp(method, "down"))
		ret = vrx619_line_stop(&c->line);
	else if (!strcmp(method, "retrain") || !c->line.enabled)
		vrx619_line_start(&c->line, now_ms());
	state_changed(c, previous);
	uloop_timeout_set(&c->timer, 1);
	if (ret)
		return UBUS_STATUS_UNKNOWN_ERROR;
	return handle_status(ctx, obj, req, method, msg);
}

static const struct ubus_method methods[] = {
	UBUS_METHOD_NOARG("status", handle_status),
	UBUS_METHOD_NOARG("up", handle_control),
	UBUS_METHOD_NOARG("down", handle_control),
	UBUS_METHOD_NOARG("retrain", handle_control),
};

static struct ubus_object_type object_type = UBUS_OBJECT_TYPE("dsl", methods);
static struct ubus_object object = {
	.name = "dsl",
	.type = &object_type,
	.methods = methods,
	.n_methods = ARRAY_SIZE(methods),
};

static void bus_connected(struct ubus_context *ctx)
{
	int ret = ubus_add_object(ctx, &object);

	if (ret)
		syslog(LOG_ERR, "cannot register DSL control: %s", ubus_strerror(ret));
}

static int number(const char *value, unsigned long limit, unsigned long *result)
{
	char *end;

	if (!*value || *value == '-')
		return -EINVAL;
	errno = 0;
	*result = strtoul(value, &end, 0);
	return errno || *end || *result > limit ? -EINVAL : 0;
}

int main(int argc, char **argv)
{
	struct controller *c = &controller;
	struct vrx619_config config = {
		.mode = VRX619_VDSL,
		.profiles = 0x1ff,
		.training_timeout = 180,
		.annex_b = true,
		.vectoring = true,
	};
	const char *device = "/dev/vrx619-0";
	unsigned long value;
	int ch, ret;

	while ((ch = getopt(argc, argv, "d:a:m:p:t:vsf")) != -1) {
		switch (ch) {
		case 'd':
			device = optarg;
			break;
		case 'a':
			if (strcmp(optarg, "a") && strcmp(optarg, "b"))
				goto usage;
			config.annex_b = !strcmp(optarg, "b");
			break;
		case 'm':
			if (!strcmp(optarg, "vdsl"))
				config.mode = VRX619_VDSL;
			else if (!strcmp(optarg, "adsl"))
				config.mode = VRX619_ADSL;
			else if (!strcmp(optarg, "auto"))
				config.mode = VRX619_AUTO;
			else
				goto usage;
			break;
		case 'p':
			if (number(optarg, 0x1ff, &value))
				goto usage;
			config.profiles = value;
			break;
		case 't':
			if (number(optarg, 600, &value))
				goto usage;
			config.training_timeout = value;
			break;
		case 'v':
			config.vectoring = false;
			break;
		case 's':
			config.input_swap = true;
			break;
		case 'f':
			config.highpass_17mhz = true;
			break;
		default:
			goto usage;
		}
	}
	if (optind != argc || vrx619_line_init(&c->line, &config, &modem_ops, c))
		goto usage;
	openlog("vrx619-dsl", LOG_PID | LOG_PERROR, LOG_DAEMON);
	c->fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (c->fd < 0) {
		syslog(LOG_ERR, "cannot open %s: %s", device, strerror(errno));
		return 1;
	}
	if (uloop_init()) {
		close(c->fd);
		return 1;
	}
	c->bus.cb = bus_connected;
	ubus_auto_connect(&c->bus);
	c->timer.cb = tick;
	vrx619_line_start(&c->line, now_ms());
	uloop_timeout_set(&c->timer, 1);
	uloop_run();
	ret = vrx619_line_stop(&c->line);
	if (ret)
		syslog(LOG_ERR, "cannot stop line: %s", strerror(-ret));
	ubus_auto_shutdown(&c->bus);
	uloop_done();
	blob_buf_free(&c->reply);
	close(c->fd);
	closelog();
	return ret ? 1 : 0;

usage:
	fprintf(stderr, "Usage: %s [-d device] [-a a|b] [-m vdsl|adsl|auto]\n"
		"  [-p profile-mask] [-t training-seconds] [-v] [-s] [-f]\n"
		"  -v disable vectoring; -s swap input; -f enable 17 MHz high-pass\n",
		argv[0]);
	return 2;
}
