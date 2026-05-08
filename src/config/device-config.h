//
// Created by Kirill Shypachov on 04.05.2026.
//

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <events/events.h>

typedef struct {
    uint8_t        endpoint;
    enum io_events device_event;
    bool           state;
} accessory_t;

#ifdef __cplusplus
extern "C" {
#endif

extern accessory_t  accessories_list[];
extern const size_t accessories_count;

accessory_t *accessory_get_by_endpoint(uint8_t endpoint);
accessory_t *accessory_get_by_event(enum io_events event);

#ifdef __cplusplus
}
#endif