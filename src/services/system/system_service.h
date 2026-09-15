/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The board's system service: what the STM32 firmware update needs from this
 * board (reports/stm32-update/README.md).
 *
 * - **Reset cause.** Read at PRE_KERNEL_1, before Matter's diagnostics clear the
 *   RCC flags, so an IWDG reset can still be told apart later.
 * - **Independent watchdog** (owner's decision 2026-09-15: on now). Started by its
 *   own feeding thread, which is cooperative at the highest priority: busy
 *   preemptible work never starves it, only an IRQ lock, a hang or a cooperative
 *   thread that does not yield (the accepted W5500 risk) does - and those are
 *   what should reset the board. Paused while a debugger halts the core.
 * - **The two update modules' platforms**: system-image-store over MCUboot's
 *   slot 2 (flash area slot1_partition on the SPI NOR) and system-updater over
 *   bootutil (swap request, confirmation), the running image in slot 1, a
 *   journal in /lfs/firmware and the restart.
 * - **Confirmation runs on this service's thread.** It writes image_ok into slot
 *   1 through the XIP flash driver, whose guarded write must find its source
 *   buffer and its own stack frame in internal SRAM; this library's thread
 *   stacks are in SRAM, the v1 API worker's is in PSRAM. The platform's confirm
 *   refuses to run on a stack outside SRAM.
 * - **`sysupd` shell commands** - the debug path of the update, kept alongside
 *   the API: slot state, copy of the running image into slot 2, request of a
 *   swap, confirmation, a deliberate hang.
 */

#ifndef SYSTEM_SERVICE_H_
#define SYSTEM_SERVICE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Log the reset cause and the running image, open system-image-store
 *        and system-updater. Called from main() once /lfs is mounted.
 *
 * Also the reason the linker keeps this library's object: the watchdog thread,
 * the PRE_KERNEL_1 hook and the shell commands are only reached through
 * iterable sections, which do not pull an object out of a static archive.
 */
void system_service_start(void);

/**
 * "major.minor.revision+build" of the image MCUboot started, read from slot 1's
 * header in system_service_start(); NULL before that or when unreadable. What the
 * device reports as its firmware version: a version string compiled into the code
 * can disagree with the header (an incremental build that missed VERSION's change
 * did, reports/stm32-update), the header is what MCUboot swapped and compares.
 */
const char *system_service_running_version(void);

/** Both update modules opened: the STM32 update bindings may be enabled. */
bool system_service_update_ready(void);

/** Largest STM32 upload: slot 2 less MCUboot's trailer sector. */
uint32_t system_service_upload_max_bytes(void);

/** hwinfo RESET_* flags of this boot, as read before anyone cleared them; 0 if unknown. */
uint32_t system_service_reset_cause(void);

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_SERVICE_H_ */
