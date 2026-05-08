//
// Created by Kirill Shypachov on 11.04.2026.
//

#ifndef CEDAR_SWITCH_3IN4OUT_POWER_MATTER_H
#define CEDAR_SWITCH_3IN4OUT_POWER_MATTER_H

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>

#ifdef __cplusplus
extern "C" {
#endif
    void matter_init();
    void matter_start();
    void init_ip_v6_address(struct net_mgmt_event_callback *cb, uint64_t mgmt_event, struct net_if *iface);
#ifdef __cplusplus
}
#endif

#endif //CEDAR_SWITCH_3IN4OUT_POWER_MATTER_H
