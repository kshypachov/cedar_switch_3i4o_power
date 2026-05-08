//
// Created by Kirill Shypachov on 14.09.2025.
//

#include "io.h"
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <string.h>
#include <zephyr/input/input.h>
#include "../zbus_topics.h"
#include "../settings_topics.h"


LOG_MODULE_REGISTER(io);

/* Стек для нового потока */
#define IO_TASK_STACK_SIZE 1024
#define IO_TASK_PRIORITY   2

#define RELAY1_NODE DT_ALIAS(relay1)
#define RELAY2_NODE DT_ALIAS(relay2)
#define RELAY3_NODE DT_ALIAS(relay3)
#define RELAY4_NODE DT_ALIAS(relay4)

static void input_cb(struct input_event *evt, void *user_data);
INPUT_CALLBACK_DEFINE(NULL, input_cb, NULL);

K_THREAD_STACK_DEFINE(io_task_stack, IO_TASK_STACK_SIZE);
static struct k_thread io_task_thread_data;

relays_def_state relays_default_state;

static const struct device *gpio_inputs_dev = DEVICE_DT_GET(DT_NODELABEL(gpio_inputs));

/* ====== DT relays (gpio-leds children) ====== */
static const struct gpio_dt_spec relay1 = GPIO_DT_SPEC_GET(RELAY1_NODE, gpios);
static const struct gpio_dt_spec relay2 = GPIO_DT_SPEC_GET(RELAY2_NODE, gpios);
static const struct gpio_dt_spec relay3 = GPIO_DT_SPEC_GET(RELAY3_NODE, gpios);
static const struct gpio_dt_spec relay4 = GPIO_DT_SPEC_GET(RELAY4_NODE, gpios);

static const struct gpio_dt_spec inputs[] = {
    GPIO_DT_SPEC_GET(DT_NODELABEL(input_ac1), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(input_ac2), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(input_ac3), gpios),
};

static const uint16_t input_codes[] = {
    INPUT_KEY_1, INPUT_KEY_2, INPUT_KEY_3,
};

static void input_cb(struct input_event *evt, void *user_data)
{
    if (evt->type == INPUT_EV_KEY) {
        io_event_data_t io_event_data;
        io_event_data.event_direction = FROM_INTERFACE;
        io_event_data.bool_data = evt->value;

        switch (evt->code) {
            case INPUT_KEY_1:
                printk("Input AC1: %s\n", evt->value ? "pressed" : "released");
                io_event_data.io_event = INPUT_1_STATUS;
                break;
            case INPUT_KEY_2:
                printk("Input AC2: %s\n", evt->value ? "pressed" : "released");
                io_event_data.io_event = INPUT_2_STATUS;
                break;
            case INPUT_KEY_3:
                printk("Input AC3: %s\n", evt->value ? "pressed" : "released");
                io_event_data.io_event = INPUT_3_STATUS;
                break;
            default:
                return;
        }

        int ret = zbus_chan_pub(&io_events, &io_event_data, K_NO_WAIT);
        if (ret != 0) {
            LOG_ERR("Failed to publish zbus event: %d", ret);
        } else {
            LOG_INF("Published io_event: %d, bool: %d", io_event_data.io_event, io_event_data.bool_data);
        }
    }
}

static void write_relays_once(uint8_t state) {
    gpio_pin_set_dt(&relay1, (state & 1)? 1 : 0);
    gpio_pin_set_dt(&relay2, (state & 2)? 1 : 0);
    gpio_pin_set_dt(&relay3, (state & 4)? 1 : 0);
    gpio_pin_set_dt(&relay4, (state & 8)? 1 : 0);
}

void force_get_inputs_state(void)
{
    // Immidiate
    for (int i = 0; i < ARRAY_SIZE(inputs); i++) {
        int val = gpio_pin_get_dt(&inputs[i]);
        input_report_key(gpio_inputs_dev, input_codes[i], val, true, K_NO_WAIT);
    }
}

static void write_relays(bool state, enum io_events event) {
    switch (event) {
        case OUTPUT_1_CONTROL:
            gpio_pin_set_dt(&relay1, state);
            break;
        case OUTPUT_2_CONTROL:
            gpio_pin_set_dt(&relay2, state);
            break;
        case OUTPUT_3_CONTROL:
            gpio_pin_set_dt(&relay3, state);
            break;
        case OUTPUT_4_CONTROL:
            gpio_pin_set_dt(&relay4, state);
            break;
        default:
            LOG_ERR("Unknown event: %d", event);
            break;
    }
}



void io_outputs_task(void *a, void *b, void *c) {

    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    LOG_INF("Start io outputs task");


    // Check if inputs ready
    if (!device_is_ready(relay1.port) || !device_is_ready(relay2.port) || !device_is_ready(relay3.port) || !device_is_ready(relay4.port)) {
        /* Порт не готов – нечего делать */
        LOG_ERR("GPIO outputs device is not ready");
        return;
    }

    uint8_t applied_state = 0x00; /* force first apply */
    write_relays_once(applied_state);

    const struct zbus_channel *chan;
    io_event_data_t evt;

    while (1) {
        if (zbus_sub_wait(&io_sub, &chan, K_FOREVER) == 0) {
            if (chan == &io_events) {
                int ret = zbus_chan_read(chan, &evt, K_FOREVER);
                if (ret != 0) {
                    LOG_ERR("zbus_chan_read failed: %d", ret);
                    continue;
                }

                LOG_INF("io event handled: evt.io_event = %d, evt.bool_data = %d, evt.event_direction = %d",
                        evt.io_event, evt.bool_data, evt.event_direction);
                if (evt.event_direction == TO_INTERFACE) {
                    write_relays(evt.bool_data, evt.io_event);
                }
            }
        }
    }
}

void io_init(void) {
    LOG_INF("Start io init");

    if ( 0 > settings_load_one(def_relays_state_enable_settings_topik, &relays_default_state.enabled, sizeof(relays_default_state.enabled))) {
        LOG_ERR("Failed to load topik %s init default", def_relays_state_enable_settings_topik);
        relays_default_state.enabled = false;
        settings_save_one(def_relays_state_enable_settings_topik, &relays_default_state.enabled, sizeof(relays_default_state.enabled));
    }
    if ( 0 > settings_load_one(def_relays1_state_settings_topik, &relays_default_state.relay1, sizeof(relays_default_state.relay1))) {
        LOG_ERR("Failed to load topik %s init default", def_relays1_state_settings_topik);
        relays_default_state.relay1 = false;
        settings_save_one(def_relays1_state_settings_topik, &relays_default_state.relay1, sizeof(relays_default_state.relay1));
    }
    if ( 0 > settings_load_one(def_relays2_state_settings_topik, &relays_default_state.relay2, sizeof(relays_default_state.relay2))) {
        LOG_ERR("Failed to load topik %s init default", def_relays2_state_settings_topik);
        relays_default_state.relay2 = false;
        settings_save_one(def_relays2_state_settings_topik, &relays_default_state.relay2, sizeof(relays_default_state.relay2));
    }
    if ( 0 > settings_load_one(def_relays3_state_settings_topik, &relays_default_state.relay3, sizeof(relays_default_state.relay3))) {
        LOG_ERR("Failed to load topik %s init default", def_relays3_state_settings_topik);
        relays_default_state.relay3 = false;
        settings_save_one(def_relays3_state_settings_topik, &relays_default_state.relay3, sizeof(relays_default_state.relay3));
    }

    if (0 > settings_load_one(def_relays4_state_settings_topik, &relays_default_state.relay4, sizeof(relays_default_state.relay4))) {
        LOG_ERR("Failed to load topik %s init default", def_relays4_state_settings_topik);
        relays_default_state.relay4 = false;
        settings_save_one(def_relays4_state_settings_topik, &relays_default_state.relay4, sizeof(relays_default_state.relay4));
    }

    k_tid_t tid = k_thread_create(&io_task_thread_data,
                              io_task_stack,
                              K_THREAD_STACK_SIZEOF(io_task_stack),
                              io_outputs_task,
                              NULL, NULL, NULL,
                              IO_TASK_PRIORITY,
                              0,
                              K_NO_WAIT);

    k_thread_name_set(tid, "io_outputs_task");
}
