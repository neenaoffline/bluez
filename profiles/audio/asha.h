/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2022 Asymptotic Inc
 *
 */

struct asha_device_capabilities {
	uint8_t side : 1;
	uint8_t type : 1;
	uint8_t reserved : 6;
};

struct asha_feature_map {
	uint8_t coc_streaming_supported : 1;
	uint8_t reserved : 7;
};

struct asha_supported_codecs {
	uint8_t reserved : 1;
	uint8_t g722 : 1;
	uint16_t also_reserved : 14;
};

struct asha_ro_properties {
	uint8_t version;
	struct asha_device_capabilities device_capabilities;
	uint64_t hi_sync_id;
	struct asha_feature_map feature_map;
	uint16_t render_delay;
	uint16_t reserved;
	struct asha_supported_codecs supported_codecs;
};

struct asha {
	struct btd_device *device;
	struct gatt_db *db;
	struct bt_gatt_client *client;
	struct gatt_db_attribute *svc_attr;

	uint16_t psm_handle;
	uint16_t ro_properties_handle;
	uint16_t audio_control_point_handle;
	uint16_t audio_status_handle;
	uint16_t volume_handle;

	struct asha_ro_properties *ro_properties;
	uint16_t *psm;
};

struct asha_central {
	struct asha_endpoint *endpoint;
	struct btd_adapter *adapter;

	void *user_data; // This is the media_endpoint

	struct media_transport *transport;
};

struct asha_endpoint {
	size_t (*get_capabilities)(struct asha_central *central,
				   uint8_t **capabilities, void *user_data);
	size_t (*set_configuration)(struct btd_device *device,
				    struct asha_central *central);
};

struct asha_central *asha_add_central(struct btd_adapter *adapter,
				      struct asha_endpoint *endpoint,
				      GDestroyNotify destroy, void *user_data,
				      int *err);

gboolean asha_get_psm(struct btd_device *asha_device, uint16_t *psm);
void send_audio_control_point_start(struct asha *asha,
				    bt_gatt_client_callback_t callback);
void send_audio_control_point_stop(struct asha *asha,
				   bt_gatt_client_callback_t callback);
