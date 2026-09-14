/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * coprocessor-manager: who owns the ESP32-C6's UART and its EN/BOOT lines.
 *
 * Contract: section 3 of the development plan ("UART RX callback один",
 * "coprocessor-manager обязан полностью освободить USART3 ... до первого вызова
 * библиотеки", the mutual exclusion of update, network apply, manual C6 reset
 * and USB programming), section 7 (paused and new-generation markers) and
 * section 8 ("Запись по UART"). CoprocessorStatus in openapi.json. The shape,
 * and why:
 *
 * - **Exactly one owner of USART3 at a time**, named by the UART mode:
 *   `console` (the ESP32 log source), `usb_bridge` (bytes passed through to the
 *   board's USB CDC), `flashing` (the ROM loader library, P6; a stand-in in P5)
 *   and `unavailable` (the UART could not be given to anyone). The board's
 *   Zephyr `uart-bridge0` node is disabled in the application's overlay, so no
 *   driver claims the UART's interrupt at init behind this module's back.
 *
 * - **A switch is stop, wait, hand over.** The current owner's receive
 *   interrupt is disabled and its callback removed; then the manager waits
 *   until the interrupt has demonstrably gone quiet
 *   (CONFIG_COPROCESSOR_MANAGER_RX_STOP_TIMEOUT_MS). A UART that keeps
 *   receiving after that is a failed stop: the switch returns -ETIMEDOUT and
 *   the mode does not change - handing a UART that is still being read to a
 *   second reader is exactly the double ownership section 8 says would show up
 *   as corrupted traffic rather than an error. If the new owner cannot attach,
 *   the old one is put back; if that fails too, the mode is `unavailable`.
 *
 * - **The log sees every handover.** Leaving `console` flushes the partial line
 *   and writes a `paused` marker once the console has stopped; coming back
 *   increments the generation and writes a `reset` marker before the console's
 *   handler is attached, so no byte lands on the wrong side of either. Resetting the C6 through EN does the same. A ROM
 *   banner the manager did not cause (a watchdog, a brown-out) also starts a
 *   generation, unless it arrives within
 *   CONFIG_COPROCESSOR_MANAGER_RESET_WINDOW_MS of a reset the manager made.
 *
 * - **Mutual exclusion is claim-then-check on both sides.** A network apply or
 *   scan claims first and then checks the UART; a switch to `usb_bridge` or
 *   `flashing`, or a C6 reset, marks itself first and then checks the claims.
 *   Whichever interleaving the threads take, at most one of two conflicting
 *   operations proceeds - no lock is held across the other module. The rules
 *   (plan section 3): apply excludes `usb_bridge`, `flashing` and a reset; a
 *   scan excludes `flashing` only.
 *
 * - **The pure core decides, the platform does.** Everything that touches a
 *   device - attaching an interrupt handler, the EN and BOOT pins, reading the
 *   ESP-Hosted transport, writing a marker - is a function in
 *   struct coprocessor_platform. The sim tier supplies a fake UART that can
 *   refuse to go quiet; the board's service in src/services/coprocessor
 *   supplies the real one. Reads of the status never wait for a switch in
 *   progress.
 */

#ifndef COPROCESSOR_MANAGER_H_
#define COPROCESSOR_MANAGER_H_

#include <stdbool.h>
#include <stdint.h>

#include <log_store/log_store.h>

#ifdef __cplusplus
extern "C" {
#endif

/** CoprocessorStatus.uart_mode. */
enum coprocessor_uart_mode {
	COPROCESSOR_UART_CONSOLE = 0,
	COPROCESSOR_UART_USB_BRIDGE,
	COPROCESSOR_UART_FLASHING,
	COPROCESSOR_UART_UNAVAILABLE,

	COPROCESSOR_UART_MODE_COUNT
};

/** Operations outside this module that conflict with the UART's owner. */
enum coprocessor_claim {
	/** A network transaction from apply until it is committed, rolled back or failed. */
	COPROCESSOR_CLAIM_NETWORK_APPLY = 0,
	/** A Wi-Fi scan from its start until its results are in. */
	COPROCESSOR_CLAIM_WIFI_SCAN,

	COPROCESSOR_CLAIM_COUNT
};

/**
 * What the manager needs done. Every function returns 0 or a negative errno.
 *
 * All but transport_ready are called with the manager's mutex held, on the
 * thread that asked for the switch or the reset: they must not call back into
 * the manager. transport_ready is optional (NULL: never ready).
 */
struct coprocessor_platform {
	/**
	 * Give the UART's receive interrupt to @p owner and enable it. CONSOLE
	 * attaches the log source's handler, USB_BRIDGE the passthrough's,
	 * FLASHING nothing (the flasher opens the UART itself).
	 */
	int (*uart_attach)(void *ctx, enum coprocessor_uart_mode owner);
	/** Disable the receive interrupt and remove whichever handler is installed. */
	int (*uart_detach)(void *ctx);
	/**
	 * A counter the receive handler increments on every entry, so the
	 * manager can see it stop. Read without a lock.
	 */
	uint32_t (*uart_rx_activity)(void *ctx);
	/**
	 * Pulse EN. With @p download the BOOT strap is held while EN is
	 * released, so the C6 starts its ROM loader instead of its firmware.
	 */
	int (*c6_reset)(void *ctx, bool download);
	/**
	 * ESP-Hosted has a working transport (the Wi-Fi device is ready). Called
	 * by get_status() on the reader's thread without any lock; must not block.
	 */
	bool (*transport_ready)(void *ctx);
	/**
	 * Emit a partial console line now and write @p kind with @p text to the
	 * ESP32 ring at @p generation.
	 */
	void (*marker)(void *ctx, enum log_store_kind kind, uint32_t generation, const char *text);
	int64_t (*now_ms)(void *ctx);
	void (*sleep_ms)(void *ctx, uint32_t ms);
	void *ctx;
};

struct coprocessor_status {
	enum coprocessor_uart_mode uart_mode;
	uint32_t generation;
	bool transport_ready;
	/** The last switch that failed, as a negative errno, or 0. */
	int last_switch_error;
	uint32_t switches;
	uint32_t rx_stop_failures;
	uint32_t resets;
	/** Resets noticed from a ROM banner rather than made by the manager. */
	uint32_t unexpected_resets;
};

/**
 * @brief Start in `console` (attach the log source), or `unavailable` if
 *        that fails. Generation starts at 1: the C6 was reset by the
 *        ESP-Hosted driver's init, so a banner within the reset window of
 *        this call is expected. Every claim is cleared.
 *
 * @retval 0       the console owns the UART
 * @retval -EINVAL a required platform function is missing; nothing changed
 * @retval -EIO    the console could not attach; the manager runs, `unavailable`
 */
int coprocessor_manager_init(const struct coprocessor_platform *platform);

/**
 * @brief Hand the UART to @p mode.
 *
 * @retval 0          switched, or already there
 * @retval -EINVAL    @p mode is `unavailable`, which is not requested
 * @retval -EBUSY     a conflicting claim is held, or the UART is `flashing`
 *                    and @p mode is `usb_bridge` (or the other way round):
 *                    one programmer must not take the chip from another
 * @retval -ETIMEDOUT the receive interrupt did not go quiet; mode unchanged
 * @retval -EIO       the new owner could not attach; mode is the old one, or
 *                    `unavailable` if that could not be restored either
 * @retval -EAGAIN    coprocessor_manager_init() has not run
 */
int coprocessor_manager_set_mode(enum coprocessor_uart_mode mode);

/**
 * @brief Reset the C6 through EN, into its firmware or (@p download) its ROM
 *        loader. Nothing is written to the chip.
 *
 * @retval -EBUSY  a network apply holds its claim, or the UART is `flashing`
 *                 (the flasher drives EN itself)
 * @retval -EAGAIN coprocessor_manager_init() has not run
 * @return otherwise what the platform's c6_reset() returned; no generation
 *         starts when the pulse failed
 */
int coprocessor_manager_reset(bool download);

/**
 * @brief The console saw a ROM banner. Starts a generation unless the manager
 *        reset the chip within the reset window.
 *
 * Does not wait: while a switch or a reset holds the manager the banner is
 * that operation's own, and is ignored.
 */
void coprocessor_manager_note_banner(void);

/**
 * @brief Claim @p what before starting it.
 *
 * Never blocks and takes no lock, so it may be called with another module's
 * mutex held (network-manager does). Claims of one kind are counted: two
 * claims need two releases, and a release without a claim does nothing.
 *
 * @retval 0       claimed; call coprocessor_manager_release() when it ends
 * @retval -EBUSY  the UART's owner excludes it right now
 * @retval -EINVAL not a claim
 */
int coprocessor_manager_claim(enum coprocessor_claim what);
void coprocessor_manager_release(enum coprocessor_claim what);

/** @brief Never waits for a switch in progress. */
void coprocessor_manager_get_status(struct coprocessor_status *out);

/** @brief Wire names: "console", "usb_bridge", "flashing", "unavailable". */
const char *coprocessor_uart_mode_str(enum coprocessor_uart_mode mode);

/**
 * @brief The reason `esp32_logs` is unavailable in @p mode, or NULL when the
 *        console owns the UART.
 */
const char *coprocessor_logs_unavailable_reason(enum coprocessor_uart_mode mode);

#ifdef __cplusplus
}
#endif

#endif /* COPROCESSOR_MANAGER_H_ */
