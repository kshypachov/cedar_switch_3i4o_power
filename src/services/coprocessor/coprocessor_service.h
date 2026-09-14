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

#endif /* COPROCESSOR_SERVICE_H_ */
