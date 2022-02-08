/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2022 Asymptotic Inc
 *
 */

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
