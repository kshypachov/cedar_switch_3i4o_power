//
// Created by Kirill Shypachov on 18.04.2026.
//

#include "matter_event_loop.h"

#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include <app/ConcreteAttributePath.h>
#include <app/EventLogging.h>
#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <platform/CHIPDeviceLayer.h>

#include <zbus_topics.h>
#include <config/device-config.h>

#include "app/util/af-types.h"

LOG_MODULE_REGISTER(matter_event_loop);

#define IO_TO_MATTER_TASK_STACK_SIZE 4096
#define IO_TO_MATTER_TASK_PRIORITY   10
K_THREAD_STACK_DEFINE(io_to_matter_task_stack, IO_TO_MATTER_TASK_STACK_SIZE);
static struct k_thread io_to_matter_thread_data;


using namespace chip;
using namespace chip::app::Clusters;

static void update_onoff_attribute(EndpointId endpoint, bool value)
{
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    OnOff::Attributes::OnOff::Set(endpoint, value);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    LOG_INF("OnOff updated from hw: endpoint=%u, value=%u", endpoint, value);
}

static bool update_switch_attribute(EndpointId endpoint, bool value)
{

    uint8_t current_position = value ? 1 : 0;
    uint8_t previous_position = value ? 0 : 1;
    chip::EventNumber event_number = 0;
    CHIP_ERROR evt_status = CHIP_NO_ERROR;

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    const auto attr_status = Switch::Attributes::CurrentPosition::Set(endpoint, current_position);
    if (attr_status == Protocols::InteractionModel::Status::Success)
    {
        if (value)
        {
            evt_status = chip::app::LogEvent(Switch::Events::InitialPress::Type{ current_position }, endpoint, event_number);
        }
        else
        {
            evt_status = chip::app::LogEvent(Switch::Events::ShortRelease::Type{ previous_position }, endpoint, event_number);
        }
    }
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    if (attr_status != Protocols::InteractionModel::Status::Success)
    {
        LOG_ERR("Failed to update switch: endpoint=%u, value=%u, status=%d",
                endpoint, value, static_cast<int>(attr_status));
        return false;
    }

    if (evt_status != CHIP_NO_ERROR)
    {
        LOG_ERR("Failed to log switch event: endpoint=%u, value=%u, err=%" CHIP_ERROR_FORMAT,
                endpoint, value, evt_status.Format());
        return false;
    }

    LOG_INF("switch updated: endpoint=%u, value=%u", endpoint, value);
    return true;
}

// Callback for sending data from Matter to relays
void MatterPostAttributeChangeCallback(
    const chip::app::ConcreteAttributePath & path,
    uint8_t type,
    uint16_t size,
    uint8_t * value)
{
    LOG_INF("Call cluster ID: %d, Attribute ID: %d", path.mClusterId, path.mAttributeId);
    accessory_t *accessory = accessory_get_by_endpoint(path.mEndpointId);
    if (accessory == nullptr)
    {
        LOG_ERR("Unknown endpoint ID: %d", path.mEndpointId);
        return;
    }

    io_event_data_t event = {
        .event_direction = TO_INTERFACE,
    };

    if (path.mClusterId == OnOff::Id && path.mAttributeId == OnOff::Attributes::OnOff::Id)
    {
        const bool on = (*value != 0);
        LOG_INF("Matter OnOff changed: endpoint=%u state=%u", path.mEndpointId, on);
        accessory->state = on;

        event.bool_data = on;
        event.io_event = accessory->device_event;
        zbus_chan_pub(&io_events, &event, K_NO_WAIT);
        return;
    }
    if (path.mClusterId == OnOff::Id && path.mAttributeId == OnOff::Attributes::StartUpOnOff::Id) {
        LOG_INF("StartUpOnOff changed: endpoint=%u", path.mEndpointId);
        return;  // значение уже сохранено через persist
    }

    LOG_ERR("Unknown endpoint ID: %d", path.mEndpointId);
}



void io_to_matter_stream(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    LOG_INF("Start io_to_matter_stream");

    const struct zbus_channel *chan;
    io_event_data_t evt;

    while (1)
    {
        zbus_sub_wait_msg(&matter_sub, &chan, &evt, K_FOREVER);
        if (evt.event_direction != FROM_INTERFACE)
        {
            continue;
        }

        accessory_t *accessory = accessory_get_by_event(evt.io_event);
        if (accessory == NULL)
        {
            LOG_ERR("Unknown event: %d", evt.io_event);
            continue;
        }

        LOG_INF("Got event from interface: %d", evt.io_event);

        switch (evt.io_event)
        {
        case INPUT_1_STATUS:
        case INPUT_2_STATUS:
        case INPUT_3_STATUS:
            update_switch_attribute(accessory->endpoint, evt.bool_data);
            break;

        case OUTPUT_1_CONTROL:
        case OUTPUT_2_CONTROL:
        case OUTPUT_3_CONTROL:
        case OUTPUT_4_CONTROL:
            update_onoff_attribute(accessory->endpoint, evt.bool_data);
            break;
        default:
            LOG_ERR("Unknown event: %d", evt.io_event);
            break;
        }
    }
}

void start_io_to_matter_stream(void)
{
    LOG_INF("Init io_to_matter_stream");


    k_tid_t tid = k_thread_create(&io_to_matter_thread_data,
                          io_to_matter_task_stack,
                          K_THREAD_STACK_SIZEOF(io_to_matter_task_stack),
                          io_to_matter_stream,
                          NULL, NULL, NULL,
                          IO_TO_MATTER_TASK_PRIORITY,
                          0,
                          K_NO_WAIT);

    k_thread_name_set(tid, "io_to_matter_stream");

}
