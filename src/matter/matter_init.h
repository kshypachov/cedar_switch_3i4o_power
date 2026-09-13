//
// Created by Kirill Shypachov on 11.04.2026.
//

#ifndef CEDAR_SWITCH_3IN4OUT_POWER_MATTER_H
#define CEDAR_SWITCH_3IN4OUT_POWER_MATTER_H

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>

#ifdef __cplusplus
extern "C" {
#endif
    //void matter_init();
    //void matter_start();
    /* net_mgmt handler for NET_EVENT_IPV6_ADDR_ADD: starts the stack once. */
    void start_matter(struct net_mgmt_event_callback *cb, uint64_t mgmt_event, struct net_if *iface);
#ifdef __cplusplus
}
#endif

#endif //CEDAR_SWITCH_3IN4OUT_POWER_MATTER_H
