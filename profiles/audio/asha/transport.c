/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2022 Asymptotic Inc
 *
 */

#include <stdbool.h>
#include <stdint.h>

#include <glib.h>

#include "lib/sdp.h"
#include "lib/uuid.h"
#include "lib/bluetooth.h"

#include "gdbus/gdbus.h"

#include "src/adapter.h"
#include "src/device.h"
#include "src/service.h"
#include "src/log.h"
#include "src/shared/gatt-client.h"
#include "profiles/audio/media.h"
#include "profiles/audio/transport.h"
#include "profiles/audio/asha.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

struct asha_transport {
	struct asha *asha;
};

// matching what a2dp.c is doing with cb_id for now Which is, just have a new
// incrementing int for each cb For a2dp, this is incremented each time resume
// is called and a new cb is set up
static unsigned int cb_id = 1;

/*
 * Return an id that is unique (per callback registered, according to a2dp.c)
 * Or return 0 in case of failure
 *
 * The media_owner is set on the transport when acquire is called after the
 * resume fn is called. It has a reference to the transport itself. It also has
 * a reference to the disconnect watcher for NameOwnerChanges on dbus of this
 * owner.
 *
 * Looks like all behaviour is queued up for executing when the mainloop is idle
 * using g_idle_add. We shall do this as well.
 */
static void set_mtu(int s, int mtu)
{
	struct l2cap_options opts = { 0 };
	int err = 0;
	unsigned int size = sizeof(opts.imtu);

	opts.omtu = opts.imtu = mtu;
	DBG("Setting sockopts\n");

	err = setsockopt(s, SOL_BLUETOOTH, BT_RCVMTU, &opts.imtu, size);
	if (err) {
		DBG("Unable to set recv mtu. %s (%d)\n", strerror(errno),
		    errno);
	} else {
		DBG("Set recv mtu\n");
	}

	err = setsockopt(s, SOL_BLUETOOTH, BT_SNDMTU, &opts.imtu, size);
	if (err) {
		DBG("Unable to set send mtu. %s (%d)\n", strerror(errno),
		    errno);
	} else {
		DBG("Set send mtu\n");
	}
}

#define MTU 167
static int xyz_connect(bdaddr_t *bd_addr, uint16_t psm)
{
	char addrstr[100];
	ba2str(bd_addr, addrstr);
	DBG("XYZ Addr: %s", addrstr);
	int s = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	int status = -1;

	if (s == -1) {
		DBG("L2CAP: Could not create a socket %u. %s (%d)\n", psm,
		    strerror(errno), errno);
		return -1;
	}

	DBG("L2CAP: Created socket %u\n", psm);

	struct sockaddr_l2 addr = { 0 };
	addr.l2_family = AF_BLUETOOTH;
	addr.l2_bdaddr_type = BDADDR_LE_PUBLIC;

	// Currently need to bind before connect to workaround an issue where the
	// addr type is incorrectly set otherwise

	status = bind(s, (struct sockaddr *)&addr, sizeof(addr));

	if (status < 0) {
		DBG("L2CAP: Could not bind %s\n", strerror(errno));
		return -1;
	}

	DBG("L2CAP: socket bound\n");

	addr.l2_psm = htobs(psm);
	memcpy(&addr.l2_bdaddr, bd_addr, sizeof(bdaddr_t));
	set_mtu(s, MTU);

	status = connect(s, (struct sockaddr *)&addr, sizeof(addr));

	if (status == 0) {
		DBG("L2CAP: Connected to %u\n", psm);
	} else {
		DBG("L2CAP: Could not connect to %u. Error %d. %d (%s)\n", psm,
		    status, errno, strerror(errno));
		return -1;
	}

	return s;
}

static void send_audio_control_point_cb(bool success, uint8_t att_ecode,
					const uint8_t *value, uint16_t length,
					void *user_data)
{
	struct asha *asha = user_data;

	if (!success) {
		DBG("Writing control point failed with ATT error: %u",
		    att_ecode);
		return;
	}

	DBG("Control point written: %u", att_ecode);
}

static guint resume_asha(struct media_transport *transport,
			 struct media_owner *owner)
{
	struct asha_transport *asha_transport = transport->data;
	struct asha *asha = asha_transport->asha;
	struct media_endpoint *endpoint = transport->endpoint;
	struct asha_central *asha_central =
		media_endpoint_get_asha_central(endpoint);
	uint16_t psm;

	char addrstr[100];
	const bdaddr_t *addr = device_get_address(transport->device);
	ba2str(addr, addrstr);

	if (!asha_get_psm(transport->device, &psm)) {
		DBG("Cannot read PSM");
		return 0;
	}

	if (addr == NULL) {
		DBG("Cannot read bd addr");
		return 0;
	}

	DBG("XYZ Addr: %s", addrstr);
	transport_set_state(transport, TRANSPORT_STATE_REQUESTING);

	// TODO: Do the following in a g_idle_add call
	int fd = xyz_connect((bdaddr_t *)addr, psm);
	media_transport_set_fd(transport, fd, MTU, MTU);
	send_audio_control_point_start(asha, send_audio_control_point_cb);

	transport_set_state(transport, TRANSPORT_STATE_ACTIVE);
	DBG("ASHA Transport Resume");

	return cb_id++;
}

/*
 * In suspend_a2dp...
 *
 * This registers a suspend cb with the list of callbacks in a2dp_setup for
 * a2dp. These callbacks are later called when finalize_suspend is called when
 * the mainloop is next idle (via g_idle_add) in case the underlying avdtp
 * stream is OPEN. In case the stream is in STREAMING state, avdtp_suspend is
 * called to suspend the stream.
 *
 * finalize_suspend could also be called by .suspend funcs on the avdtp_sep_ind
 * and avdtp_sep_cfm structs, which are in turn called by some signalling code
 * in profiles/audio/avdtp.c
 *
 * For suspend_asha...
 *
 * This is called when Release is called. When release is called,
 *
 * If we are OPEN (open & streaming don't need to be different states for asha)
 *
 * 1. We should send a stop on audio control point if we are not already
 * stopped.
 * 2. Close the socket
 *
 * In any other state, do nothing.
 *
 */
static guint suspend_asha(struct media_transport *transport,
			  struct media_owner *owner)
{
	struct asha_transport *asha_transport = transport->data;

	if (transport->state != TRANSPORT_STATE_ACTIVE) {
		return 0;
	}

	close(transport->fd);
	send_audio_control_point_stop(asha_transport->asha,
				      send_audio_control_point_cb);
	return 0;
}

/*
 * Cancel searches all setups' callbacks for the callback with this id and then
 * cancels it (abort & free)
 *
 * Since we don't have any callbacks, maybe we don't need to do anything here?
 * (We just immediately open a socket synchronously)
 */
static void cancel_asha(struct media_transport *transport, guint id)
{
}

/*
 *
 */
static void destroy_asha(void *data)
{
	g_free((struct asha_transport *)data);
}

int asha_transport_init(struct media_transport *transport)
{
	struct btd_service *service;
	struct asha_transport *asha_transport;

	DBG("ASHA Transport Init");
	service = btd_device_get_service(transport->device, ASHA_SINK_UUID);
	if (service == NULL)
		return -EINVAL;

	asha_transport = g_new0(struct asha_transport, 1);
	asha_transport->asha = btd_service_get_user_data(service);

	transport->resume = resume_asha;
	transport->suspend = suspend_asha;
	transport->cancel = cancel_asha;
	transport->data = asha_transport;
	transport->destroy = destroy_asha;
}
