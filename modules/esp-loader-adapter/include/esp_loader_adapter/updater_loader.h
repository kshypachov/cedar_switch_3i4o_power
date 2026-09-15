/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * esp-loader-adapter's session as the loader coprocessor-updater takes.
 * Separate from esp_loader_adapter.h so the adapter does not depend on the
 * updater unless both are built (CONFIG_ESP_LOADER_ADAPTER_UPDATER_OPS).
 */

#ifndef ESP_LOADER_ADAPTER_UPDATER_LOADER_H_
#define ESP_LOADER_ADAPTER_UPDATER_LOADER_H_

#include <coprocessor_updater/coprocessor_updater.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief open/begin/write/finish/close over esp_loader_adapter_*(); ctx unused. */
const struct coprocessor_updater_loader *esp_loader_adapter_updater_loader(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_LOADER_ADAPTER_UPDATER_LOADER_H_ */
