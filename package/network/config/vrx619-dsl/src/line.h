/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VRX619_LINE_H
#define VRX619_LINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <linux/vrx619.h>

enum vrx619_mode {
	VRX619_VDSL,
	VRX619_ADSL,
	VRX619_AUTO,
};

enum vrx619_phase {
	VRX619_DOWN,
	VRX619_RESTART,
	VRX619_HW_CONFIG,
	VRX619_IDLE,
	VRX619_TRAINING,
	VRX619_SHOWTIME,
	VRX619_RETRY,
	VRX619_COOLING,
	VRX619_ERROR,
};

struct vrx619_config {
	enum vrx619_mode mode;
	uint16_t profiles;
	unsigned int training_timeout;
	bool annex_b;
	bool vectoring;
	bool input_swap;
	bool highpass_17mhz;
};

struct vrx619_ops {
	/* xfer returns the response length or a negative errno. */
	int (*xfer)(void *ctx, uint16_t command, const void *tx, size_t tx_size,
		    void *rx, size_t rx_size);
	int (*restart)(void *ctx);
	int (*transport)(void *ctx, unsigned int transport);
};

struct vrx619_line {
	struct vrx619_config config;
	const struct vrx619_ops *ops;
	void *ctx;
	enum vrx619_phase phase;
	uint64_t deadline;
	uint64_t communication_deadline;
	uint64_t showtime_since;
	uint32_t state;
	uint32_t vtse;
	uint32_t attempts;
	uint32_t restarts;
	uint32_t retrains;
	uint16_t substate;
	uint16_t profile;
	uint16_t failed_command;
	uint16_t exception;
	unsigned int failures;
	unsigned int boot_failures;
	int error;
	bool enabled;
	unsigned int transport;
};

int vrx619_line_init(struct vrx619_line *line, const struct vrx619_config *config,
		     const struct vrx619_ops *ops, void *ctx);
void vrx619_line_start(struct vrx619_line *line, uint64_t now);
int vrx619_line_stop(struct vrx619_line *line);
void vrx619_line_tick(struct vrx619_line *line, uint64_t now);
void vrx619_line_event(struct vrx619_line *line, uint16_t command,
		       const uint8_t *data, size_t size, uint64_t now);
void vrx619_line_lost_events(struct vrx619_line *line, uint64_t now);
const char *vrx619_phase_name(enum vrx619_phase phase);

#endif
