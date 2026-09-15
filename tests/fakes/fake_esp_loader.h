/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * esp-serial-flasher, and the board's two steps around it, as a table that can
 * be told to fail.
 *
 * esp-loader-adapter's promises are about order and completeness: the UART is
 * taken before the library initialises its port, the chip is booted normally
 * before the port is deinitialised, EN/BOOT and the console's configuration
 * come back before the console does, and all of it happens whichever step
 * failed. This fake records every call in order, the UART owner
 * coprocessor-manager reported at that moment, and every byte written, so a
 * test can assert each of those directly.
 */

#ifndef FAKE_ESP_LOADER_H_
#define FAKE_ESP_LOADER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <coprocessor_manager/coprocessor_manager.h>
#include <esp_loader_adapter/esp_loader_adapter.h>

enum fake_esp_call {
	FEL_PORT_INIT = 0,
	FEL_PORT_DEINIT,
	FEL_CONNECT,
	FEL_GET_TARGET,
	FEL_FLASH_START,
	FEL_FLASH_WRITE,
	FEL_FLASH_FINISH,
	FEL_RESET_TARGET,
	FEL_CHANGE_RATE,
	FEL_LINES_IDLE,
	FEL_CONSOLE_RESTORE,

	FEL_CALL_COUNT
};

#define FAKE_ESP_LOADER_MAX_CALLS 64
#define FAKE_ESP_LOADER_MAX_BYTES 16384

struct fake_esp_loader {
	/** Calls in order; consecutive flash_write calls are recorded once. */
	enum fake_esp_call calls[FAKE_ESP_LOADER_MAX_CALLS];
	size_t call_count;
	/** How many times each was called. */
	unsigned int count[FEL_CALL_COUNT];
	/** coprocessor-manager's UART mode at the last call of each kind. */
	enum coprocessor_uart_mode mode_at[FEL_CALL_COUNT];

	/** Return value of each int call (the library's code or an errno). Sticky. */
	int result[FEL_CALL_COUNT];
	/** Fail only the Nth flash_write (1-based) with result[FEL_FLASH_WRITE]; 0: every one. */
	unsigned int write_fail_at;
	/** What get_target reports. */
	int target;

	uint32_t start_offset;
	uint32_t start_size;
	uint32_t start_block;
	uint32_t rate;
	/** Every byte flash_write received, up to FAKE_ESP_LOADER_MAX_BYTES. */
	uint8_t bytes[FAKE_ESP_LOADER_MAX_BYTES];
	uint32_t written;
	uint32_t largest_write;
	/** The port is initialised and not deinitialised. */
	bool port_live;
};

extern struct fake_esp_loader fake_esp_loader;
extern const struct esp_loader_adapter_lib fake_esp_loader_lib;

/** Every call succeeds, the target is an ESP32-C6, nothing recorded. */
void fake_esp_loader_init(void);

/** Position of the first call of @p call in calls[], or -1. */
int fake_esp_loader_first(enum fake_esp_call call);

/** Position of the last call of @p call in calls[], or -1. */
int fake_esp_loader_last(enum fake_esp_call call);

#endif /* FAKE_ESP_LOADER_H_ */
