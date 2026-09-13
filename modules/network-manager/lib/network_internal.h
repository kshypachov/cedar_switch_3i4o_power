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
 * @param status  Current runtime state, needed for the radio and recovery-path rules.
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

/**
 * @brief What an interface is doing, from what the adapter observed.
 *
 * @param enabled  Whether the configuration in force enables it.
 */
enum network_iface_state network_iface_state_of(enum device_config_interface iface,
						const struct network_iface_status *st,
						bool enabled);

/**
 * @brief The interface that should carry traffic with no route of its own.
 *
 * The preferred interface while it has a link and an address, else the other
 * one if that does; false when neither does.
 */
bool network_select_default(const struct device_config *cfg, const struct network_status *status,
			    enum device_config_interface *out);

#endif /* NETWORK_INTERNAL_H_ */
