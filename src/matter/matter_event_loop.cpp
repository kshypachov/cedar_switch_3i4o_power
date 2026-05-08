//
// Created by Kirill Shypachov on 18.04.2026.
//

#include "matter_event_loop.h"

#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include <app/ConcreteAttributePath.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app-common/zap-generated/ids/Attributes.h>

//#include "zbus_topics.h"

LOG_MODULE_REGISTER(matter_callbacks, LOG_LEVEL_INF);

using namespace chip;
using namespace chip::app::Clusters;

void MatterPostAttributeChangeCallback(
    const chip::app::ConcreteAttributePath & path,
    uint8_t type,
    uint16_t size,
    uint8_t * value)
{
    LOG_INF("Call cluster ID: %d, Attribute ID: %d", path.mClusterId, path.mAttributeId);
    if (path.mClusterId == OnOff::Id && path.mAttributeId == OnOff::Attributes::OnOff::Id)
    {
        const bool on = (*value != 0);

        LOG_INF("Matter OnOff changed: endpoint=%u state=%u",
                path.mEndpointId, on);

        // io_event_data_t event = {
        //     .event_direction = FROM_INTERFACE,
        //     .io_event = OUTPUT_1_CONTROL,
        //     .bool_data = on,
        // };
        //
        // zbus_chan_pub(&io_events, &event, K_NO_WAIT);
    }
}