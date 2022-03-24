/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2022 Asymptotic Inc
 *
 */

struct media_transport;

struct asha_transport {
	struct asha *asha;
	bdaddr_t *addr;
	uint16_t imtu;
	uint16_t omtu;
	uint16_t psm;

	struct media_owner *owner;
	struct media_transport *transport;
};

int asha_transport_init(struct media_transport *transport);
