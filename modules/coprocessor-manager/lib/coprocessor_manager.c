/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * coprocessor-manager: the UART ownership automaton. See
 * include/coprocessor_manager/coprocessor_manager.h for the contract; this
 * file documents only how the switch and the claims are carried out.
 *
 * Three pieces of state, three kinds of protection:
 *
 * - The working state (mode, generation, counters) is changed only under the
 *   mutex, by set_mode(), reset() and note_banner(). A switch sleeps while it
 *   waits for the receive interrupt to go quiet, so the mutex can be held for
 *   up to CONFIG_COPROCESSOR_MANAGER_RX_STOP_TIMEOUT_MS.
 *
 * - What readers see is a copy of it, published under a spinlock at every
 *   transition boundary. get_status() takes only the spinlock, so an HTTP
 *   handler never waits for a switch that is sleeping.
 *
 * - The exclusion between this module and network-manager is two atomics: a
 *   bit per exclusive activity here (a USB bridge, a flasher, a reset in
 *   progress) and a counter per claim from outside. Each side sets its own
 *   first and then reads the other's. With sequentially consistent atomics,
 *   of two conflicting operations at least one sees the other and backs off,
 *   whatever the interleaving - and neither ever holds a lock across the other
 *   module, so network-manager can claim with its own mutex held.
 *
 * Markers are placed so that the log's order is right without the platform
 * having to synchronise anything: the pause is written once the console has
 * provably stopped (every byte received before the handover lands in front of
 * it), and the reset that starts a generation is written before the console's
 * handler is attached again (no byte of the new generation can land in front
 * of it).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <coprocessor_manager/coprocessor_manager.h>

/* Exclusive activities, as bits of `exclusive`. */
#define EX_BRIDGE   BIT(0)
#define EX_FLASHING BIT(1)
#define EX_RESET    BIT(2)

/* The receive counter has to stand still this many intervals in a row. */
#define QUIET_SAMPLES 2

static const char *const paused_text[COPROCESSOR_UART_MODE_COUNT] = {
	[COPROCESSOR_UART_USB_BRIDGE] = "UART handed to the USB bridge",
	[COPROCESSOR_UART_FLASHING] = "UART handed to the flasher",
};

static K_MUTEX_DEFINE(lock);

/* Under `lock`. */
static const struct coprocessor_platform *plat;
static struct coprocessor_status st = {.uart_mode = COPROCESSOR_UART_UNAVAILABLE};
static int64_t reset_at_ms;

/* Under `snap_lock`: what get_status() returns. */
static struct k_spinlock snap_lock;
static struct coprocessor_status snap = {.uart_mode = COPROCESSOR_UART_UNAVAILABLE};
static const struct coprocessor_platform *snap_plat;

static atomic_t exclusive;
static atomic_t claims[COPROCESSOR_CLAIM_COUNT];

/* --- wire names --------------------------------------------------------- */

const char *coprocessor_uart_mode_str(enum coprocessor_uart_mode mode)
{
	static const char *const names[COPROCESSOR_UART_MODE_COUNT] = {
		[COPROCESSOR_UART_CONSOLE] = "console",
		[COPROCESSOR_UART_USB_BRIDGE] = "usb_bridge",
		[COPROCESSOR_UART_FLASHING] = "flashing",
		[COPROCESSOR_UART_UNAVAILABLE] = "unavailable",
	};

	return ((unsigned int)mode < ARRAY_SIZE(names)) ? names[mode] : NULL;
}

const char *coprocessor_logs_unavailable_reason(enum coprocessor_uart_mode mode)
{
	switch (mode) {
	case COPROCESSOR_UART_CONSOLE:
		return NULL;
	case COPROCESSOR_UART_USB_BRIDGE:
		return "uart_usb_bridge";
	case COPROCESSOR_UART_FLASHING:
		return "uart_flashing";
	default:
		return "uart_unavailable";
	}
}

/* --- helpers ------------------------------------------------------------ */

static void publish_locked(void)
{
	k_spinlock_key_t key = k_spin_lock(&snap_lock);

	snap = st;
	snap_plat = plat;
	k_spin_unlock(&snap_lock, key);
}

static atomic_val_t mode_bit(enum coprocessor_uart_mode mode)
{
	switch (mode) {
	case COPROCESSOR_UART_USB_BRIDGE:
		return EX_BRIDGE;
	case COPROCESSOR_UART_FLASHING:
		return EX_FLASHING;
	default:
		return 0;
	}
}

/* Does an exclusive activity in @p bits collide with a claim held now? */
static bool claimed_against(atomic_val_t bits)
{
	if ((bits & (EX_BRIDGE | EX_FLASHING | EX_RESET)) != 0 &&
	    atomic_get(&claims[COPROCESSOR_CLAIM_NETWORK_APPLY]) > 0) {
		return true;
	}
	if ((bits & EX_FLASHING) != 0 && atomic_get(&claims[COPROCESSOR_CLAIM_WIFI_SCAN]) > 0) {
		return true;
	}
	return false;
}

/* Make the exclusive bits say what the mode now is. The current bit goes up
 * before the others come down, so a claim never finds a gap. */
static void settle_bits_locked(void)
{
	const atomic_val_t current = mode_bit(st.uart_mode);

	if (current != 0) {
		(void)atomic_or(&exclusive, current);
	}
	(void)atomic_and(&exclusive, ~((EX_BRIDGE | EX_FLASHING) & ~current));
}

static void marker_locked(enum log_store_kind kind, const char *text)
{
	plat->marker(plat->ctx, kind, st.generation, text);
}

/*
 * Give the UART back to the log. The generation starts, and says so, before
 * the handler is attached; if the handler cannot be attached the log is told
 * it paused again.
 */
static int attach_console_locked(void)
{
	st.generation++;
	marker_locked(LOG_STORE_KIND_RESET, "UART returned to the log");
	if (plat->uart_attach(plat->ctx, COPROCESSOR_UART_CONSOLE) != 0) {
		marker_locked(LOG_STORE_KIND_PAUSED, "UART could not be given back to the log");
		return -EIO;
	}
	return 0;
}

static int attach_locked(enum coprocessor_uart_mode owner)
{
	switch (owner) {
	case COPROCESSOR_UART_CONSOLE:
		return attach_console_locked();
	case COPROCESSOR_UART_UNAVAILABLE:
		return -EIO;
	default:
		return plat->uart_attach(plat->ctx, owner);
	}
}

/*
 * Wait until the receive counter stands still for QUIET_SAMPLES intervals.
 * Bounded twice: by the clock, and by the number of looks the timeout allows,
 * so a clock that does not move cannot turn a noisy UART into a hang.
 */
static int wait_quiet_locked(void)
{
	const uint32_t step = CONFIG_COPROCESSOR_MANAGER_RX_QUIET_STEP_MS;
	const uint32_t timeout = CONFIG_COPROCESSOR_MANAGER_RX_STOP_TIMEOUT_MS;
	const uint32_t max_looks = timeout / step + QUIET_SAMPLES;
	const int64_t start = plat->now_ms(plat->ctx);
	uint32_t last = plat->uart_rx_activity(plat->ctx);
	uint32_t stable = 0;

	for (uint32_t look = 0; look < max_looks; look++) {
		plat->sleep_ms(plat->ctx, step);

		const uint32_t now = plat->uart_rx_activity(plat->ctx);

		if (now == last) {
			if (++stable >= QUIET_SAMPLES) {
				return 0;
			}
		} else {
			stable = 0;
			last = now;
		}
		if (plat->now_ms(plat->ctx) - start >= (int64_t)timeout) {
			break;
		}
	}

	return -ETIMEDOUT;
}

static int switch_locked(enum coprocessor_uart_mode target)
{
	const enum coprocessor_uart_mode old = st.uart_mode;

	/*
	 * A detach that reports an error cannot promise the handler is gone,
	 * which is the same as an interrupt that does not go quiet: the UART is
	 * not handed to anyone, and the old owner gets its handler back. Nothing
	 * was handed over, so the log is not told anything.
	 */
	if (plat->uart_detach(plat->ctx) != 0 || wait_quiet_locked() != 0) {
		st.rx_stop_failures++;
		if (old == COPROCESSOR_UART_UNAVAILABLE ||
		    plat->uart_attach(plat->ctx, old) != 0) {
			if (old == COPROCESSOR_UART_CONSOLE) {
				marker_locked(LOG_STORE_KIND_PAUSED,
					      "UART could not be given back to the log");
			}
			st.uart_mode = COPROCESSOR_UART_UNAVAILABLE;
		}
		return -ETIMEDOUT;
	}

	if (old == COPROCESSOR_UART_CONSOLE) {
		marker_locked(LOG_STORE_KIND_PAUSED, paused_text[target]);
	}

	if (attach_locked(target) != 0) {
		/* Put the old owner back, or nobody. */
		if (attach_locked(old) != 0) {
			st.uart_mode = COPROCESSOR_UART_UNAVAILABLE;
		}
		return -EIO;
	}

	st.uart_mode = target;
	st.switches++;

	return 0;
}

/* --- API ---------------------------------------------------------------- */

int coprocessor_manager_init(const struct coprocessor_platform *platform)
{
	if (platform == NULL || platform->uart_attach == NULL || platform->uart_detach == NULL ||
	    platform->uart_rx_activity == NULL || platform->c6_reset == NULL ||
	    platform->marker == NULL || platform->now_ms == NULL || platform->sleep_ms == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	plat = platform;
	memset(&st, 0, sizeof(st));
	/* The ESP-Hosted driver's init has just reset the C6. */
	st.generation = 1;
	reset_at_ms = plat->now_ms(plat->ctx);
	atomic_clear(&exclusive);
	for (size_t i = 0; i < ARRAY_SIZE(claims); i++) {
		atomic_clear(&claims[i]);
	}

	const int rc = plat->uart_attach(plat->ctx, COPROCESSOR_UART_CONSOLE);

	st.uart_mode = (rc == 0) ? COPROCESSOR_UART_CONSOLE : COPROCESSOR_UART_UNAVAILABLE;
	st.last_switch_error = (rc == 0) ? 0 : -EIO;
	publish_locked();

	k_mutex_unlock(&lock);

	return (rc == 0) ? 0 : -EIO;
}

int coprocessor_manager_set_mode(enum coprocessor_uart_mode mode)
{
	if ((unsigned int)mode >= COPROCESSOR_UART_UNAVAILABLE) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	int rc = 0;
	const enum coprocessor_uart_mode old = st.uart_mode;

	if (plat == NULL) {
		rc = -EAGAIN;
		goto out;
	}
	if (old == mode) {
		goto out;
	}
	if ((old == COPROCESSOR_UART_USB_BRIDGE && mode == COPROCESSOR_UART_FLASHING) ||
	    (old == COPROCESSOR_UART_FLASHING && mode == COPROCESSOR_UART_USB_BRIDGE)) {
		/* One programmer must not take the chip from another. */
		rc = -EBUSY;
		goto out;
	}

	/* Claim-then-check: mark the intent, then look at the claims. */
	const atomic_val_t intent = mode_bit(mode);

	if (intent != 0) {
		(void)atomic_or(&exclusive, intent);
		if (claimed_against(intent)) {
			(void)atomic_and(&exclusive, ~intent);
			rc = -EBUSY;
			goto out;
		}
	}

	rc = switch_locked(mode);
	st.last_switch_error = rc;
	settle_bits_locked();
	publish_locked();

out:
	k_mutex_unlock(&lock);

	return rc;
}

int coprocessor_manager_reset(bool download)
{
	k_mutex_lock(&lock, K_FOREVER);

	int rc;

	if (plat == NULL) {
		rc = -EAGAIN;
		goto out;
	}
	if (st.uart_mode == COPROCESSOR_UART_FLASHING) {
		/* The flasher drives EN and BOOT itself. */
		rc = -EBUSY;
		goto out;
	}

	(void)atomic_or(&exclusive, EX_RESET);
	if (claimed_against(EX_RESET)) {
		(void)atomic_and(&exclusive, ~EX_RESET);
		rc = -EBUSY;
		goto out;
	}

	rc = plat->c6_reset(plat->ctx, download);
	if (rc == 0) {
		st.generation++;
		st.resets++;
		reset_at_ms = plat->now_ms(plat->ctx);
		marker_locked(LOG_STORE_KIND_RESET,
			      download ? "ESP32 reset into its ROM loader through EN"
				       : "ESP32 reset through EN");
		publish_locked();
	}
	(void)atomic_and(&exclusive, ~EX_RESET);

out:
	k_mutex_unlock(&lock);

	return rc;
}

void coprocessor_manager_note_banner(void)
{
	/*
	 * Not waiting: whoever holds the mutex is resetting the chip or taking
	 * the UART away from the console, and a banner seen meanwhile is either
	 * that reset's own or the last bytes before the handover. Waiting would
	 * also stall the worker that feeds the log for as long as a switch takes.
	 */
	if (k_mutex_lock(&lock, K_NO_WAIT) != 0) {
		return;
	}

	if (plat != NULL &&
	    plat->now_ms(plat->ctx) - reset_at_ms >= CONFIG_COPROCESSOR_MANAGER_RESET_WINDOW_MS) {
		st.generation++;
		st.unexpected_resets++;
		marker_locked(LOG_STORE_KIND_RESET, "ESP32 restarted by itself");
		publish_locked();
	}

	k_mutex_unlock(&lock);
}

int coprocessor_manager_claim(enum coprocessor_claim what)
{
	if ((unsigned int)what >= COPROCESSOR_CLAIM_COUNT) {
		return -EINVAL;
	}

	(void)atomic_inc(&claims[what]);

	const atomic_val_t bits = atomic_get(&exclusive);
	const atomic_val_t excluded = (what == COPROCESSOR_CLAIM_NETWORK_APPLY)
					      ? (EX_BRIDGE | EX_FLASHING | EX_RESET)
					      : EX_FLASHING;

	if ((bits & excluded) != 0) {
		(void)atomic_dec(&claims[what]);
		return -EBUSY;
	}

	return 0;
}

void coprocessor_manager_release(enum coprocessor_claim what)
{
	if ((unsigned int)what >= COPROCESSOR_CLAIM_COUNT) {
		return;
	}

	/* A release without a claim must not leave a negative count behind,
	 * which would let the next claim look like no claim at all. */
	for (;;) {
		const atomic_val_t held = atomic_get(&claims[what]);

		if (held <= 0 || atomic_cas(&claims[what], held, held - 1)) {
			return;
		}
	}
}

void coprocessor_manager_get_status(struct coprocessor_status *out)
{
	const struct coprocessor_platform *p;
	k_spinlock_key_t key = k_spin_lock(&snap_lock);

	*out = snap;
	p = snap_plat;
	k_spin_unlock(&snap_lock, key);

	out->transport_ready =
		(p != NULL && p->transport_ready != NULL) ? p->transport_ready(p->ctx) : false;
}
