//
// Created by Kirill Shypachov on 26.04.2026.
//

#include "net_init.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/ethernet_mgmt.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "zephyr/net/net_config.h"
#include "../matter/matter_init.h"

LOG_MODULE_REGISTER(net_init, LOG_LEVEL_INF);

#define EEPROM_I2C_BUS_NAME  "i2c@40005400"
#define EEPROM_I2C_ADDR      0x50
#define EEPROM_MAC_OFFSET    0xFA
#define MAC_ADDR_LEN         6

static struct net_mgmt_event_callback cb;
static struct net_mgmt_event_callback cb6;

static bool mac_is_all_value(const uint8_t *mac, uint8_t value)
{
    for (size_t i = 0; i < MAC_ADDR_LEN; i++) {
        if (mac[i] != value) {
            return false;
        }
    }

    return true;
}

static bool mac_is_valid(const uint8_t *mac)
{
    /* Multicast bit must be 0 for station MAC address. */
    if ((mac[0] & 0x01U) != 0U) {
        return false;
    }

    if (mac_is_all_value(mac, 0x00U) || mac_is_all_value(mac, 0xFFU)) {
        return false;
    }

    return true;
}

static int read_mac_from_eeprom(uint8_t mac[MAC_ADDR_LEN])
{
    const struct device *i2c_dev = device_get_binding(EEPROM_I2C_BUS_NAME);

    if (i2c_dev == NULL) {
        LOG_ERR("I2C bus not found: %s", EEPROM_I2C_BUS_NAME);
        return -ENODEV;
    }

    uint8_t offset = EEPROM_MAC_OFFSET;
    int ret = i2c_write_read(i2c_dev, EEPROM_I2C_ADDR, &offset, sizeof(offset), mac, MAC_ADDR_LEN);
    if (ret < 0) {
        LOG_ERR("EEPROM MAC read failed: %d", ret);
        return ret;
    }

    if (!mac_is_valid(mac)) {
        LOG_ERR("EEPROM MAC invalid: %02x:%02x:%02x:%02x:%02x:%02x",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return -EINVAL;
    }

    LOG_INF("EEPROM MAC: %02x:%02x:%02x:%02x:%02x:%02x",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}


int ethernet_interfaces_init(void)
{
    int ret;
    uint8_t mac[MAC_ADDR_LEN];
    const uint8_t fallback_mac[MAC_ADDR_LEN] = { 0x02, 0x00, 0x00, 0x12, 0x34, 0x56 };

    /*
     * С Wi-Fi (esp_hosted_mcu) сетевых интерфейсов два, и их порядок не
     * гарантирован. Берём W5500 по устройству и оставляем его интерфейсом по
     * умолчанию, как было до появления Wi-Fi.
     */
    struct net_if *iface = net_if_lookup_by_dev(DEVICE_DT_GET_ONE(wiznet_w5500));
    if (!iface) {
        LOG_ERR("W5500 network interface not found");
        return -ENODEV;
    }
    net_if_set_default(iface);

    ret = read_mac_from_eeprom(mac);
    if (ret < 0) {
        memcpy(mac, fallback_mac, sizeof(mac));
        LOG_WRN("Using fallback MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    struct ethernet_req_params mac_params = { 0 };
    memcpy(mac_params.mac_address.addr, mac, sizeof(mac));

    net_if_down(iface);
    net_dhcpv4_stop(iface);

    ret = net_mgmt(NET_REQUEST_ETHERNET_SET_MAC_ADDRESS,
                   iface,
                   &mac_params,
                   sizeof(struct ethernet_req_params));
    if (ret < 0) {
        LOG_ERR("Failed to set MAC address: %d", ret);
        return ret;
    }

    LOG_INF("Network interface up with MAC %02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);


    net_mgmt_init_event_callback(&cb6, start_matter, NET_EVENT_IPV6_ADDR_ADD);
    net_mgmt_add_event_callback(&cb6);

    net_if_up(iface); /* DHCP starts automatically if CONFIG_NET_DHCPV4=y */
    (void)net_dhcpv4_start(iface); /* Explicit DHCP start */

    return 0;
}
