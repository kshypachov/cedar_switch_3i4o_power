/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The board's coprocessor service: coprocessor-manager's platform over USART3,
 * the USB CDC passthrough and the C6's EN/BOOT lines, and the ESP32 log source
 * running on its worker. Design in README.md.
 */

#ifndef COPROCESSOR_SERVICE_H_
#define COPROCESSOR_SERVICE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Take USART3 for the console, install the CDC handler and start the
 *        worker.
 *
 * After the ESP-Hosted driver's init (it resets the C6) and before the network
 * service, whose apply and scan claim against the UART's owner.
 */
int coprocessor_service_start(void);

/** @brief The firmware version ESP-Hosted reported, as "v1.2.3"; false if unknown. */
bool coprocessor_service_firmware_version(char *buf, size_t cap);

/** @brief The C6 has sent a byte on its UART since boot. */
bool coprocessor_service_rx_seen(void);

/**
 * @brief firmware-store opened /lfs/firmware at start (and, with it, the
 *        updater read its journal). False: the upload bindings stay closed (503).
 */
bool coprocessor_service_firmware_ready(void);

/**
 * @brief The request's local address (@p family 4 or 6, @p addr in network order)
 *        belongs to the Ethernet interface: the install's "over Ethernet" rule.
 */
bool coprocessor_service_request_over_ethernet(uint8_t family, const uint8_t *addr);

#endif /* COPROCESSOR_SERVICE_H_ */
