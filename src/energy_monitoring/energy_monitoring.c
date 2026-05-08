//
// Created by Kirill Shypachov on 26.10.2025.
//

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/net_buf.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/settings/settings.h>
#include <string.h>
#include "../zbus_topics.h"
#include "../settings_topics.h"
#include "energy_monitoring.h"
#include "HLW8032.h"


LOG_MODULE_REGISTER(energy_monitoring);

#define ENERGY_MON_TASK_STACK_SIZE 1024
#define ENERGY_MON_TASK_PRIORITY   2
#define HLW8032_PACKET_SIZE 24

K_THREAD_STACK_DEFINE(energy_mon_task_stack, ENERGY_MON_TASK_STACK_SIZE);
static struct k_thread energy_mon_task_thread_data;

static uint8_t hlw_packet[HLW8032_PACKET_SIZE];

const struct device *uart_dev = 0;//DEVICE_DT_GET(DT_NODELABEL(usart2));

void save_energy_data(void) {

}

int load_energy_data(void) {

    return 0;
}

static void uart_irq_cb(const struct device *dev, void *user_data) {
    static int counter;
    counter++;
    if (counter == 2000) {
        counter = 0;
        LOG_INF("UART IRQ");
    }
}

static void uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
    int err = 0;

    switch (evt->type) {
        case UART_RX_STOPPED:

            if (evt->data.rx_stop.reason == UART_BREAK) {
                LOG_DBG("UART BREAK detected — RX line held LOW too long!\n");
                if (evt->data.rx.len == HLW8032_PACKET_SIZE) {
                    err = RawStringHLW8032(hlw_packet, evt->data.rx.len, NULL);
                    if (err == 0) {
                        LOG_DBG("UART RX parsed data successfully after BREAK");
                    }
                }
            } else {
                LOG_DBG("RX stopped, reason: %d\n", evt->data.rx_stop.reason);
            }

            memset(hlw_packet, 0, sizeof(hlw_packet));
            uart_rx_enable(uart_dev, hlw_packet, sizeof(hlw_packet), 50);
            break;
        case UART_RX_RDY:
            printk("Data received: %d", evt->data.rx.len);
            if (evt->data.rx.len != HLW8032_PACKET_SIZE) {
                // Received part of data less then
                LOG_DBG("Recieved less than HLW8032_PACKET_SIZE");
            }else {
                err = RawStringHLW8032(hlw_packet, evt->data.rx.len, NULL);
                if (err == 0) {
                    LOG_DBG("UART RX parsed data successfully");
                }
            }

            memset(hlw_packet, 0, sizeof(hlw_packet));
            uart_rx_enable(uart_dev, hlw_packet, sizeof(hlw_packet), 50);
            // нормальные данные
            break;
        case UART_RX_BUF_REQUEST:
            LOG_DBG("UART RX buf requested");
            break;
        case UART_RX_BUF_RELEASED:
            LOG_DBG("UART RX buf released");
            break;
        case UART_RX_DISABLED:
            LOG_DBG("UART RX disabled");
            uart_rx_enable(uart_dev, hlw_packet, sizeof(hlw_packet), 50);
            LOG_DBG("UART RX start again");
            break;
        default:
            LOG_ERR("UART unknown event");
            break;
    }
}

static void energy_mon_task(void *a, void *b, void *c) {
    LOG_INF("Energy monitoring task start");

    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    LOG_INF("Checking UART device...");
    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART device not ready!");
        return;
    }
    LOG_INF("UART device ready");


    //int ret = uart_irq_callback_set(uart_dev, uart_irq_cb);
    int ret = uart_callback_set(uart_dev, uart_cb, NULL);
    if (ret < 0) {
        LOG_ERR("Failed to set UART callback: %d", ret);
        return;
    }
    LOG_INF("UART callback set successfully");

    //uart_irq_rx_enable(uart_dev);
    LOG_INF("UART RX interrupts enabled");

    ret = uart_rx_enable(uart_dev, hlw_packet, sizeof(hlw_packet), 50);
    if (ret < 0) {
        LOG_ERR("Failed to enable UART RX: %d (error code)", ret);
        return;
    }
    LOG_INF("UART RX enabled successfully");

    LOG_INF("Energy monitoring started, waiting for data...");

    while (true) {

        k_msleep(1000);
    }

}

void energy_monitoring_init(void) {
    LOG_INF("Energy monitoring init");

    k_tid_t tid = k_thread_create(&energy_mon_task_thread_data,
                          energy_mon_task_stack,
                          K_THREAD_STACK_SIZEOF(energy_mon_task_stack),
                          energy_mon_task,
                          NULL, NULL, NULL,
                          ENERGY_MON_TASK_PRIORITY,
                          0,
                          K_NO_WAIT);

    k_thread_name_set(tid, "energy_mon");
}