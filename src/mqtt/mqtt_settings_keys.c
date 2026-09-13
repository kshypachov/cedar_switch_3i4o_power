/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * MQTT connection settings in settings-registry, under "reg/mqtt/".
 *
 * These keys lived in the legacy REST handler src/web/REST_endpoints/
 * mqtt_settings.c, which P2 removed together with every unauthenticated legacy
 * route (owner's decision, 2026-09-13). The keys stay: they are the MQTT
 * module's configuration and the stored values survive on existing boards.
 * Nothing reads them at runtime yet - the loader in ha_mqtt.c is commented out
 * - and nothing writes them without the web page, which MQTT does not have in
 * the first version (plan section 1). Settings posted to the old endpoint
 * remain in /lfs/settings under these keys.
 */

#include <stdbool.h>
#include <stdint.h>

#include <settings_registry/settings_registry.h>

#include "../settings_topics.h"

#define MQTT_KEY_ENABLED "mqtt/enabled"
#define MQTT_KEY_HOST    "mqtt/host"
#define MQTT_KEY_PORT    "mqtt/port"
#define MQTT_KEY_USER    "mqtt/user"
#define MQTT_KEY_PASS    "mqtt/pass"

static bool mqtt_enabled_storage;
static uint32_t mqtt_port_storage;
static char mqtt_host_storage[sizeof(((mqtt_settings_t *)0)->host)];
static char mqtt_user_storage[sizeof(((mqtt_settings_t *)0)->user)];
static char mqtt_pass_storage[sizeof(((mqtt_settings_t *)0)->pass)];

SETTING_REGISTRY_DEFINE(mqtt_enabled_setting, .key = MQTT_KEY_ENABLED,
                        .type = SETTING_TYPE_BOOL, .storage = SETTING_STORAGE_OWNED,
                        .persistence = SETTING_PERSISTENT, .mirror = SETTING_MIRROR_RAM,
                        .ram_storage = &mqtt_enabled_storage);
SETTING_REGISTRY_DEFINE(mqtt_host_setting, .key = MQTT_KEY_HOST,
                        .type = SETTING_TYPE_STRING, .storage = SETTING_STORAGE_OWNED,
                        .persistence = SETTING_PERSISTENT, .mirror = SETTING_MIRROR_RAM,
                        .max_len = sizeof(mqtt_host_storage), .default_value = "",
                        .ram_storage = mqtt_host_storage);
/* The broker port is a uint16_t: reject what would silently wrap. */
SETTING_REGISTRY_DEFINE(mqtt_port_setting, .key = MQTT_KEY_PORT,
                        .type = SETTING_TYPE_U32, .storage = SETTING_STORAGE_OWNED,
                        .persistence = SETTING_PERSISTENT, .mirror = SETTING_MIRROR_RAM,
                        .ram_storage = &mqtt_port_storage,
                        .validate = setting_validate_range_u32,
                        .validate_ctx = &(struct setting_range_u32){ .min = 0, .max = UINT16_MAX });
SETTING_REGISTRY_DEFINE(mqtt_user_setting, .key = MQTT_KEY_USER,
                        .type = SETTING_TYPE_STRING, .storage = SETTING_STORAGE_OWNED,
                        .persistence = SETTING_PERSISTENT, .mirror = SETTING_MIRROR_RAM,
                        .max_len = sizeof(mqtt_user_storage), .default_value = "",
                        .ram_storage = mqtt_user_storage);
SETTING_REGISTRY_DEFINE(mqtt_pass_setting, .key = MQTT_KEY_PASS,
                        .type = SETTING_TYPE_STRING, .storage = SETTING_STORAGE_OWNED,
                        .persistence = SETTING_PERSISTENT, .mirror = SETTING_MIRROR_RAM,
                        .max_len = sizeof(mqtt_pass_storage), .default_value = "",
                        .ram_storage = mqtt_pass_storage);
