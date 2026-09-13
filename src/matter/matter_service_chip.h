/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * matter-service's platform on the Matter SDK: the only file that connects
 * modules/matter-service to the stack.
 */

#ifndef CEDAR_MATTER_SERVICE_CHIP_H_
#define CEDAR_MATTER_SERVICE_CHIP_H_

#ifdef __cplusplus
extern "C" {
#endif

/** Install the platform in matter-service. Call before the stack can start. */
void matter_service_chip_init(void);

#ifdef __cplusplus
}

#include <app/server/AppDelegate.h>
#include <lib/core/CHIPError.h>

/** For ServerInitParams::appDelegate: reports the window opening and closing.
 *  (The SDK declares AppDelegate outside namespace chip.) */
AppDelegate * matter_service_chip_app_delegate();

/** After Server::Init() succeeded: start following the fabric table. */
void matter_service_chip_server_initialized();

/**
 * The stack finished starting, or failed to. Success is reported from the
 * Matter thread, where the first snapshot is read; failure at once.
 */
void matter_service_chip_report_started(chip::ChipError err);

#endif /* __cplusplus */

#endif /* CEDAR_MATTER_SERVICE_CHIP_H_ */
