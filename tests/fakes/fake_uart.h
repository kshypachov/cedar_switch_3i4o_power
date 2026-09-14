/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * A coprocessor UART, EN line and clock that can be told to misbehave.
 *
 * coprocessor-manager's rules are about the moments hardware makes hard to
 * reproduce: a receive interrupt that keeps firing after its handler was
 * removed, a handler that will not attach, a reset that fails, a claim that
 * arrives while a switch is sleeping. This fake produces each on demand and
 * records what the manager did - which owner has the UART, whether a handler
 * is installed, the markers in order and whether the console was still
 * attached when each was written.
 *
 * It also stands in for the P6 flasher: fake_uart_flasher_open() succeeds only
 * when no receive handler is installed, so a test can show that the flasher
 * never shares the UART with the log.
 */

#ifndef FAKE_UART_H_
#define FAKE_UART_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <coprocessor_manager/coprocessor_manager.h>

#define FAKE_UART_MAX_MARKERS 32

/** noisy_reads value for a receive interrupt that never goes quiet. */
#define FAKE_UART_NEVER_QUIET UINT32_MAX

struct fake_uart_marker {
	enum log_store_kind kind;
	uint32_t generation;
	char text[64];
	/** A receive handler was installed when the marker was written. */
	bool handler_attached;
};

struct fake_uart {
	/* The UART. */
	/** uart_attach() succeeded and no uart_detach() since (FLASHING too). */
	bool attached;
	/** The owner of the last successful attach. */
	enum coprocessor_uart_mode owner;
	/** A receive handler (console or bridge) is installed. */
	bool handler;
	unsigned int attach_calls;
	unsigned int detach_calls;
	/** errno for attaching that owner, or 0. Sticky until the test clears it. */
	int attach_errno[COPROCESSOR_UART_MODE_COUNT];
	/** errno for the next detach, or 0. Sticky. */
	int detach_errno;
	/** The receive interrupt's entry counter. */
	uint32_t activity;
	/**
	 * Reads of the counter, while no handler is installed, that still see it
	 * move: an interrupt that fires on after its handler is gone.
	 */
	uint32_t noisy_reads;

	/* Where received bytes went. */
	size_t console_bytes;
	size_t bridge_bytes;
	size_t flasher_bytes;
	size_t lost_bytes;

	/* The stand-in flasher. */
	bool flasher_open;
	/** Times anything tried to read the UART while another reader had it. */
	unsigned int double_ownership;

	/* The C6. */
	unsigned int reset_calls;
	bool last_reset_download;
	/** errno for c6_reset(), or 0. Sticky. */
	int reset_errno;
	bool transport_ready;

	/* The log. */
	struct fake_uart_marker markers[FAKE_UART_MAX_MARKERS];
	size_t marker_count;

	/* Time. */
	int64_t now_ms;
	/** Sleep for real and read k_uptime_get(): for tests with two threads. */
	bool real_time;
	unsigned int sleeps;

	/** Run once inside the next sleep or reset: something arriving mid-operation. */
	void (*during_call)(void *arg);
	void *during_call_arg;
};

extern struct fake_uart fake_uart;
extern const struct coprocessor_platform fake_uart_platform;

/** Forget everything: nothing attached, clock at 0, no injection. */
void fake_uart_init(void);

/** @p n bytes arrive: they reach whoever reads the UART, or are lost. */
void fake_uart_receive(size_t n);

/** The flasher opens the UART. -EBUSY if a receive handler is still installed. */
int fake_uart_flasher_open(void);
void fake_uart_flasher_close(void);

#endif /* FAKE_UART_H_ */
