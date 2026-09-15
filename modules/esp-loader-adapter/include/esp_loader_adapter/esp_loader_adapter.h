/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * esp-loader-adapter: the thin layer between the coprocessor updater and
 * espressif/esp-serial-flasher.
 *
 * Contract: development plan section 3 (module row, "Правила интеграции": one
 * owner of USART3, the library drives EN/BOOT itself), section 8 ("Запись по
 * UART": stop the bridge and RX, hand the UART over, connect with trials,
 * identify, erase/write/verify, normal boot, hand the UART back; never NVS,
 * eFuse or secure download mode) and reports/p6/README.md ("Дизайн"). The
 * shape, and why:
 *
 * - **A session with guaranteed cleanup.** esp_loader_adapter_open() takes the
 *   UART from coprocessor-manager (mode `flashing`), gives it and the straps to
 *   the library and connects. From then on esp_loader_adapter_close() is the
 *   only way out, and it always runs every step - normal boot, library
 *   deinit, EN/BOOT back to inactive outputs, the console's UART configuration
 *   back, the UART back to the console - whatever failed before it, including a
 *   failure in the middle of a write. A step that fails does not skip the
 *   next: a C6 left in reset or in its ROM loader, or a UART left with the
 *   library's interrupt handler, is worse than a reported error.
 *
 * - **The library's port is initialised for as short a time as possible.**
 *   esp_loader_init_serial() installs tty's interrupt handler on USART3 with
 *   receive enabled, and tty's receive path writes a "~" for every byte its
 *   512-byte ring drops - from the ISR, with an unbounded timeout, into a
 *   transmit ring nobody drains while no library call is running. A C6 that
 *   floods its UART (the ROM of an unflashed chip restarts and prints every
 *   0.65 s) therefore hangs the system if the port is left initialised with
 *   no reader: board B did exactly that at boot (reports/p6, hw/logs/02).
 *   So the port is initialised in open() only after coprocessor-manager has
 *   detached the console, immediately before connect, and deinitialised
 *   (RX/TX interrupts off) right after the last library call of every path:
 *   a failure in open, begin, write or finish tears the whole session down
 *   before returning, and a successful session lasts only as long as the
 *   caller's phases, each bounded by its timeout. In the ROM loader the C6
 *   is quiet, so the gaps between the caller's calls do not feed the ring.
 *
 * - **esp_loader_deinit() leaves EN and BOOT floating** (GPIO_DISCONNECTED,
 *   port/zephyr_port.c) and the library does not restore USART3's baud rate
 *   after change_transmission_rate. Those are the two board ops of the seam
 *   (lines_idle, console_restore), called right after deinit.
 *
 * - **Addresses are not an input.** The image is always written from 0x0 with
 *   its verified size (the raw_full_flash format, owner's decision №1). No
 *   stub, no deflate, no erase outside the write, no eFuse, no secure download
 *   mode.
 *
 * - **The library is behind a table of functions** (struct
 *   esp_loader_adapter_lib). The real table wraps esp_loader_* on the
 *   `zephyr,esp-loader` device (lib/esp_loader_adapter_zephyr.c, built with
 *   CONFIG_ESP_LOADER_ADAPTER_ZEPHYR); the sim tier supplies
 *   tests/fakes/fake_esp_loader.c, which records the call sequence so the
 *   cleanup order is a test, not a comment.
 *
 * - **Errors are contract codes.** Every failure fills struct
 *   esp_loader_adapter_error with an ErrorDetail code from the contract's
 *   table, a message for a person, whether a retry may help, and the library's
 *   own code for the log.
 *
 * Threads: one session at a time, on the updater's worker. The functions block
 * for as long as the library does (a write of 1.5 MB at 115200 takes minutes)
 * and must not be called from an ISR or with another module's lock held.
 */

#ifndef ESP_LOADER_ADAPTER_H_
#define ESP_LOADER_ADAPTER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * esp-serial-flasher values the core interprets, mirrored so the core builds
 * without the library; lib/esp_loader_adapter_zephyr.c checks them against
 * esp_loader.h at build time.
 */
/** target_chip_t ESP32C6_CHIP. */
#define ESP_LOADER_ADAPTER_CHIP_ESP32C6 8
/** esp_loader_error_t ESP_LOADER_ERROR_TIMEOUT. */
#define ESP_LOADER_ADAPTER_LIB_TIMEOUT 2
/** esp_loader_error_t ESP_LOADER_ERROR_INVALID_MD5. */
#define ESP_LOADER_ADAPTER_LIB_INVALID_MD5 4

/**
 * The library calls the adapter makes, and the two board steps the library
 * does not cover. Functions returning int return the library's
 * esp_loader_error_t (0 is ESP_LOADER_SUCCESS) unless noted.
 */
struct esp_loader_adapter_lib {
	/** esp_loader_init_serial(): the library's interrupt handler on the UART, EN/BOOT outputs. */
	int (*port_init)(void *ctx);
	/** esp_loader_deinit(): UART interrupts off, EN and BOOT disconnected. */
	void (*port_deinit)(void *ctx);
	/** esp_loader_connect() with the device's trials and sync timeout (enters the ROM loader). */
	int (*connect)(void *ctx);
	/** esp_loader_get_target(), as target_chip_t. */
	int (*get_target)(void *ctx);
	/** esp_loader_flash_start(): the ROM erases the region under the write. */
	int (*flash_start)(void *ctx, uint32_t offset, uint32_t size, uint32_t block_size);
	/** esp_loader_flash_write() of one block. */
	int (*flash_write)(void *ctx, const uint8_t *data, uint32_t len);
	/** esp_loader_flash_finish() with MD5 verification. */
	int (*flash_finish)(void *ctx);
	/** esp_loader_reset_target(): EN pulse with BOOT released - the C6 runs its firmware. */
	void (*reset_target)(void *ctx);
	/** esp_loader_change_transmission_rate(). NULL: the rate is never changed. */
	int (*change_rate)(void *ctx, uint32_t baud);
	/** Board: EN and BOOT back to inactive outputs. 0 or a negative errno. */
	int (*lines_idle)(void *ctx);
	/** Board: USART3 back to the console's configuration. 0 or a negative errno. */
	int (*console_restore)(void *ctx);
	void *ctx;
};

/** Why an adapter call failed. The strings are static. */
struct esp_loader_adapter_error {
	/** ErrorDetail code: "busy", "service_not_ready", "unsupported_target", "internal_error". */
	const char *code;
	const char *message;
	bool retryable;
	/** The library's esp_loader_error_t, or a negative errno for a non-library step, or 0. */
	int cause;
};

/** Block size passed to flash_start and used for every write but the last. */
#define ESP_LOADER_ADAPTER_BLOCK_SIZE CONFIG_ESP_LOADER_ADAPTER_BLOCK_SIZE

/**
 * @brief Use @p lib from now on. Must not be called while a session is open.
 *
 * @retval 0       ready
 * @retval -EINVAL a required function is missing (change_rate may be NULL)
 * @retval -EBUSY  a session is open
 */
int esp_loader_adapter_init(const struct esp_loader_adapter_lib *lib);

/**
 * @brief Take the UART and connect to the C6's ROM loader.
 *
 * coprocessor-manager's mode goes to `flashing`, the library's port is
 * initialised, connect runs with the device's trials, and the target must be
 * an ESP32-C6. On any failure the session is closed again before returning
 * (the same steps as esp_loader_adapter_close()), so the caller only closes
 * after a success.
 *
 * @retval 0          connected; the session is open
 * @retval -EBUSY     the manager refused the UART (USB bridge, a network apply
 *                    or scan holds a claim) or a session is already open -
 *                    code "busy"
 * @retval -EIO       port init or connect failed - "service_not_ready" when the
 *                    ROM loader did not answer
 * @retval -ENOTSUP   the target is not an ESP32-C6 - "unsupported_target"
 * @retval -EAGAIN    esp_loader_adapter_init() has not run
 */
int esp_loader_adapter_open(struct esp_loader_adapter_error *err);

/**
 * @brief Start writing @p size bytes at 0x0. The ROM erases the region.
 *
 * @p size is rounded up to a multiple of 4 as the protocol requires; the
 * padding is written as 0xFF by esp_loader_adapter_write().
 *
 * @retval -EPERM  no session, or a write is already in progress
 * @retval -EINVAL @p size is 0
 * @retval -EIO    the library refused; the session is already closed
 */
int esp_loader_adapter_begin(uint32_t size, struct esp_loader_adapter_error *err);

/**
 * @brief Write the next @p len bytes of the image.
 *
 * Bytes are buffered to whole blocks; the final partial block goes out in
 * esp_loader_adapter_finish(). More bytes than begin() announced is -EINVAL.
 *
 * @retval -EPERM  begin() has not succeeded
 * @retval -EINVAL past the announced size
 * @retval -EIO    a block was refused; the session is already closed
 */
int esp_loader_adapter_write(const uint8_t *data, size_t len, struct esp_loader_adapter_error *err);

/**
 * @brief Send the last block, padded to the rounded size, and verify the MD5.
 *
 * @retval -EPERM  begin() has not succeeded
 * @retval -EINVAL fewer bytes were written than begin() announced
 * @retval -EIO    the last block or the MD5 check failed; the session is
 *                 already closed
 */
int esp_loader_adapter_finish(struct esp_loader_adapter_error *err);

/**
 * @brief End the session: normal boot, library deinit, EN/BOOT idle, console
 *        configuration back, UART back to the console. Every step runs.
 *
 * Does nothing when no session is open.
 *
 * @return 0, or the first step's negative errno that failed (the manager's
 *         set_mode, lines_idle or console_restore)
 */
int esp_loader_adapter_close(void);

/** @brief A session is open. */
bool esp_loader_adapter_is_open(void);

/** @brief Bytes accepted by esp_loader_adapter_write() since begin(). */
uint32_t esp_loader_adapter_written(void);

/**
 * @brief Fill the library members of @p lib with the esp_loader_* calls on the
 *        chosen `zephyr,esp-loader` device (CONFIG_ESP_LOADER_ADAPTER_ZEPHYR).
 *
 * lines_idle, console_restore and ctx are left for the board. The node must be
 * `zephyr,deferred-init` and its device is never initialised by the kernel or
 * by device_init(): the port is initialised only inside a session (see the
 * header comment on tty). port_init refuses when the device data does not
 * have the layout lib/esp_loader_adapter_zephyr.c expects.
 */
void esp_loader_adapter_zephyr_fill(struct esp_loader_adapter_lib *lib);

/**
 * @brief Debug: the port's read and write calls as timed by the Zephyr table.
 *
 * Counts, total and longest time, and for the last failing call its code, the
 * timeout the library gave it and how long it took. Kept across sessions until
 * esp_loader_adapter_zephyr_io_reset().
 */
struct esp_loader_adapter_zephyr_io {
	uint32_t reads;
	int64_t read_ticks;
	uint32_t read_max_ms;
	int read_rc;
	uint32_t read_fail_timeout_ms;
	uint32_t read_fail_ms;
	/** Bytes read since the last write; at the last failing read. */
	uint32_t bytes_since_write;
	uint32_t read_fail_bytes;
	uint32_t writes;
	int64_t write_ticks;
	uint32_t write_max_ms;
	int write_rc;
	uint32_t write_fail_timeout_ms;
	uint32_t write_fail_ms;
	/** Longest time a read ran past the timeout it was given, and the uptime then. */
	uint32_t overrun_max_ms;
	int64_t overrun_at_ms;
	/** Last bytes read, circular; 0x100 marks a write between them. */
	uint16_t rx_tail[128];
	uint16_t rx_tail_next;
	uint16_t rx_tail_count;
};

void esp_loader_adapter_zephyr_io_get(struct esp_loader_adapter_zephyr_io *out);
void esp_loader_adapter_zephyr_io_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_LOADER_ADAPTER_H_ */
