/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2022 Asymptotic Inc
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <dbus/dbus.h>
#include <glib.h>

#include "lib/bluetooth.h"
#include "lib/sdp.h"
#include "lib/sdp_lib.h"
#include "lib/uuid.h"

#include "gdbus/gdbus.h"

#include "src/adapter.h"
#include "src/device.h"
#include "src/log.h"
#include "src/plugin.h"
#include "src/profile.h"
#include "src/service.h"
#include "src/shared/att.h"
#include "src/shared/gatt-client.h"
#include "src/shared/gatt-db.h"
#include "src/shared/util.h"

#include "asha.h"
#include "media.h"

#define ASHA_UUID16 0xfdf0

#define ASHA_CHARACTERISTIC_PSM "2d410339-82b6-42aa-b34e-e2e01df8cc1a"
#define ASHA_CHARACTERISTIC_VOLUME "00e4ca9e-ab14-41e4-8823-f9e70c7e91df"
#define ASHA_CHARACTERISTIC_AUDIO_CONTROL_POINT                                \
	"f0d4de7e-4a88-476c-9d9f-1937b0996cc0"
#define ASHA_CHARACTERISTIC_AUDIO_STATUS "38663f1a-e711-4cac-b641-326b56404837"
#define ASHA_CHARACTERISTIC_READ_ONLY_PROPERTIES                               \
	"6333651e-c481-4a3e-9169-7c902aad37bb"

static GSList *centrals = NULL;

static struct asha_central *find_central(GSList *list, struct btd_adapter *a)
{
	for (; list; list = list->next) {
		struct asha_central *central = list->data;

		if (central->adapter == a)
			return central;
	}

	return NULL;
}

struct asha_central *asha_add_central(struct btd_adapter *adapter,
				      struct asha_endpoint *endpoint,
				      GDestroyNotify destroy, void *user_data,
				      int *err)
{
	struct asha_central *central = NULL;

	central = g_new0(struct asha_central, 1);
	central->endpoint = endpoint;
	central->adapter = adapter;
	central->user_data = user_data;

	centrals = g_slist_append(centrals, central);

	return central;
}

static int asha_probe(struct btd_service *service)
{
	// get the device from the service
	struct btd_device *device = btd_service_get_device(service);

	struct asha *asha = g_new0(struct asha, 1);

	asha->device = device;

	btd_service_set_user_data(service, asha);

	// We may not actually need to do anything here just yet
	return 0;
}

static void asha_remove(struct btd_service *service)
{
	return;
}

static void debug_log_ro_properties(struct asha_ro_properties *ro_properties)
{
	uint8_t *ro_properties_bytes = (uint8_t *)ro_properties;
	DBG("Data (uint8): ");
	for (int i = 0; i < 17; i++)
		DBG("%u", ro_properties_bytes[i]);

	DBG("Data (hex): ");
	for (int i = 0; i < 17; i++)
		DBG("%X", ro_properties_bytes[i]);

	DBG("Version: %u\n", ro_properties->version);
	DBG("Device Capabilities(side): %u\n",
	    ro_properties->device_capabilities.side);
	DBG("Device Capabilities(type): %u\n",
	    ro_properties->device_capabilities.type);
	DBG("HiSync ID: %lu\n", ro_properties->hi_sync_id);
	DBG("Feature map: %u\n",
	    ro_properties->feature_map.coc_streaming_supported);
	DBG("Render delay: %u\n", ro_properties->render_delay);
	DBG("Reserved: %u\n", ro_properties->reserved);
	DBG("Supported Codecs: %u\n", ro_properties->supported_codecs.g722);
}

static void read_ro_properties_cb(bool success, uint8_t att_ecode,
				  const uint8_t *value, uint16_t length,
				  void *user_data)
{
	struct asha *asha = user_data;

	if (!success) {
		DBG("Reading ASHA read only properties failed with ATT error: %u",
		    att_ecode);
		return;
	}

	if (length != 17) {
		DBG("ASHA read only properties have incorrect length");
		return;
	}

	asha->ro_properties = util_memdup(value, length);

	// TODO: Remove this
	debug_log_ro_properties(asha->ro_properties);
}

static void set_configuration_via_endpoint(struct asha *asha)
{
	struct asha_central *central =
		find_central(centrals, device_get_adapter(asha->device));

	if (central == NULL)
		return;

	central->endpoint->set_configuration(asha->device, central);
}

static void read_psm_cb(bool success, uint8_t att_ecode, const uint8_t *value,
			uint16_t length, void *user_data)
{
	struct asha *asha = user_data;

	if (!success) {
		DBG("Reading ASHA PSM failed with ATT errror: %u", att_ecode);
		return;
	}

	if (length != 2) {
		DBG("ASHA PSM read with incorrect length");
		return;
	}

	asha->psm = util_memdup(value, length);
	DBG("ASHA PSM read %u", *asha->psm);

	// NOTE: No guarantee that RO properties have been read by this point
	// TODO: Call setConfiguration on the endpoint now
	set_configuration_via_endpoint(asha);
}

static void log_read_value_err(char *value, unsigned int ret)
{
	if (!ret) {
		DBG("Failed to send request to read %s", value);
	} else {
		DBG("Initiated read of %s", value);
	}
}

static void handle_characteristic(struct gatt_db_attribute *characteristic,
				  void *user_data)
{
	struct asha *asha = user_data;
	uint16_t value_handle;
	bt_uuid_t uuid, psm_uuid, volume_uuid, audio_control_point_uuid,
		audio_status_uuid, read_only_properties_uuid;

	if (!gatt_db_attribute_get_char_data(
		    characteristic, NULL, &value_handle, NULL, NULL, &uuid)) {
		error("Failed to obtain characteristic data");
		return;
	}

	bt_string_to_uuid(&psm_uuid, ASHA_CHARACTERISTIC_PSM);
	bt_string_to_uuid(&volume_uuid, ASHA_CHARACTERISTIC_VOLUME);
	bt_string_to_uuid(&audio_control_point_uuid,
			  ASHA_CHARACTERISTIC_AUDIO_CONTROL_POINT);
	bt_string_to_uuid(&audio_status_uuid, ASHA_CHARACTERISTIC_AUDIO_STATUS);
	bt_string_to_uuid(&read_only_properties_uuid,
			  ASHA_CHARACTERISTIC_READ_ONLY_PROPERTIES);

	if (bt_uuid_cmp(&uuid, &read_only_properties_uuid) == 0) {
		asha->ro_properties_handle = value_handle;

		log_read_value_err("RO Properties",
				   bt_gatt_client_read_value(
					   asha->client,
					   asha->ro_properties_handle,
					   read_ro_properties_cb, asha, NULL));
	} else if (bt_uuid_cmp(&uuid, &audio_control_point_uuid) == 0) {
		asha->audio_control_point_handle = value_handle;
	} else if (bt_uuid_cmp(&uuid, &audio_status_uuid) == 0) {
		asha->audio_status_handle = value_handle;
		// TODO: Look into something about a CCC descriptor in the android code.
		// https://cs.android.com/android/platform/superproject/+/master:packages/modules/Bluetooth/system/bta/hearing_aid/hearing_aid.cc;l=650
	} else if (bt_uuid_cmp(&uuid, &volume_uuid) == 0) {
		asha->volume_handle = value_handle;
	} else if (bt_uuid_cmp(&uuid, &psm_uuid) == 0) {
		asha->psm_handle = value_handle;
	} else {
		char uuid_str[MAX_LEN_UUID_STR];
		DBG("Unsupported ASHA characteristic: %s",
		    bt_uuid_to_string(&uuid, uuid_str, sizeof(uuid_str)));
	}
}

static void handle_asha_service(struct asha *asha)
{
	gatt_db_service_foreach_char(asha->svc_attr, handle_characteristic,
				     asha);
	log_read_value_err("ASHA PSM", bt_gatt_client_read_value(
					       asha->client, asha->psm_handle,
					       read_psm_cb, asha, NULL));
}

static void foreach_asha_service(struct gatt_db_attribute *attr,
				 void *user_data)
{
	struct asha *asha = user_data;

	if (asha->svc_attr != NULL) {
		error("More than one BATT service exists for this device");
		return;
	}

	asha->svc_attr = attr;
	handle_asha_service(asha);
}

gboolean asha_get_psm(struct btd_device *asha_device, uint16_t *psm)
{
	struct btd_service *service =
		btd_device_get_service(asha_device, ASHA_SINK_UUID);
	if (service == NULL)
		return FALSE;

	struct asha *asha = btd_service_get_user_data(service);
	if ((asha == NULL) || (asha->psm == NULL))
		return FALSE;

	memcpy(psm, asha->psm, sizeof(uint16_t));

	return TRUE;
}

static int asha_accept(struct btd_service *service)
{
	struct asha *asha = btd_service_get_user_data(service);
	struct gatt_db *db = btd_device_get_gatt_db(asha->device);
	struct bt_gatt_client *client =
		btd_device_get_gatt_client(asha->device);

	asha->db = gatt_db_ref(db);
	asha->client = bt_gatt_client_clone(client);

	bt_uuid_t asha_uuid;
	bt_uuid16_create(&asha_uuid, ASHA_UUID16);

	gatt_db_foreach_service(asha->db, &asha_uuid, foreach_asha_service,
				asha);
	return 0;
}

static int asha_disconnect(struct btd_service *service)
{
	return 0;
}

static struct btd_profile asha_sink_profile = {
	.name = "asha-sink",
	.priority = BTD_PROFILE_PRIORITY_MEDIUM,

	.remote_uuid = ASHA_SINK_UUID,
	.device_probe =
		asha_probe, // set up profile specific structure in user_data(service)
	.device_remove =
		asha_remove, // Free mem allocated for the struct set up in probe
	.accept = asha_accept, // Use the struct and handle a connected device
	.disconnect = asha_disconnect, // Cleanup the struct without freeing

	.auto_connect = true,
};

static int asha_init(void)
{
	btd_profile_register(&asha_sink_profile);

	return 0;
}

static void asha_exit(void)
{
	btd_profile_unregister(&asha_sink_profile);
}

BLUETOOTH_PLUGIN_DEFINE(asha, VERSION, BLUETOOTH_PLUGIN_PRIORITY_DEFAULT,
			asha_init, asha_exit)
