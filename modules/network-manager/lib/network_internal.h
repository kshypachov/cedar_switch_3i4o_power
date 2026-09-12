/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared between the transaction state machine and the validator. Not
 * installed: nothing outside modules/network-manager/lib includes this.
 */

#ifndef NETWORK_INTERNAL_H_
#define NETWORK_INTERNAL_H_

#include <network_manager/network_manager.h>

/**
 * @brief Check a proposal against the contract, the committed configuration
 *        and what the interfaces are doing right now.
 *
 * @param status  Current runtime state, needed for the recovery-path rule.
 * @param err     Receives field-level detail. Already initialised by the caller.
 * @return true when the proposal may be staged.
 */
bool network_validate_config(const struct network_config_input *input,
			     const struct device_config *committed,
			     const struct network_status *status, struct api_error *err);

/**
 * @brief Whether every enabled interface of @p cfg is actually working.
 *
 * Link, address, and a route when the configuration asked for a gateway.
 */
bool network_config_is_healthy(const struct device_config *cfg,
			       const struct network_status *status);

#endif /* NETWORK_INTERNAL_H_ */
