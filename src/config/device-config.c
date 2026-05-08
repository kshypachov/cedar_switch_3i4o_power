//
// Created by Kirill Shypachov on 04.05.2026.
//

#include "device-config.h"

#include "zephyr/sys/util.h"


accessory_t accessories_list[] = {
    { 1,OUTPUT_1_CONTROL,false },
    { 2,OUTPUT_2_CONTROL,false },
    { 3,OUTPUT_3_CONTROL,false },
    { 4,OUTPUT_4_CONTROL,false },
    { 5,INPUT_1_STATUS,false },
    { 6,INPUT_2_STATUS,false },
    { 7,INPUT_3_STATUS,false },
};

const size_t accessories_count = ARRAY_SIZE(accessories_list);

accessory_t *accessory_get_by_endpoint(uint8_t endpoint)
{
    for (size_t i = 0; i < accessories_count; i++) {
        if (accessories_list[i].endpoint == endpoint) {
            return &accessories_list[i];
        }
    }
    return NULL;
}

accessory_t *accessory_get_by_event(enum io_events event)
{
    for (size_t i = 0; i < accessories_count; i++) {
        if (accessories_list[i].device_event == event) {
            return &accessories_list[i];
        }
    }
    return NULL;
}
