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
#include "src/dbus-common.h"
#include "src/shared/att.h"
#include "src/shared/gatt-client.h"
#include "profiles/audio/media.h"
#include "profiles/audio/transport.h"
#include "profiles/audio/asha/transport.h"
#include "profiles/audio/asha.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

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
static void set_bluetooth_mtu(int s, int mtu)
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
static int l2cap_connect(struct asha_transport *t)
{
	int s = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	int status = -1;

	if (s == -1) {
		DBG("Could not create an L2CAP socket %u. %s (%d)\n",
		    t->asha->psm, strerror(errno), errno);
		return -1;
	}

	DBG("Created L2CAP socket %u\n", t->asha->psm);

	struct sockaddr_l2 addr = { 0 };
	addr.l2_family = AF_BLUETOOTH;
	addr.l2_bdaddr_type = BDADDR_LE_PUBLIC;

	// Currently need to bind before connect to workaround an issue where the
	// addr type is incorrectly set otherwise

	status = bind(s, (struct sockaddr *)&addr, sizeof(addr));

	if (status < 0) {
		DBG("Could not bind L2CAP socket: %s\n", strerror(errno));
		return -1;
	}

	DBG("L2CAP socket bound\n");

	addr.l2_psm = htobs(*t->asha->psm);

	memcpy(&addr.l2_bdaddr, t->addr, sizeof(bdaddr_t));
	set_bluetooth_mtu(s, t->imtu);

	status = connect(s, (struct sockaddr *)&addr, sizeof(addr));

	if (status == 0) {
		DBG("L2CAP socket connected to PSM: %u\n", t->asha->psm);
	} else {
		DBG("Could not connect L2CAP socket to PSM: %u. Error %d. %d (%s)\n",
		    t->asha->psm, status, errno, strerror(errno));
		return -1;
	}

	return s;
}

static void send_audio_control_point_start_cb(bool success,
					      const uint8_t att_ecode,
					      void *user_data)
{
	struct asha_transport *t = user_data;

	if (!success) {
		DBG("Writing control point failed with ATT error: %u",
		    att_ecode);
		return;
	}

	DBG("Control point written: %u", att_ecode);
	transport_set_state(t->transport, TRANSPORT_STATE_REQUESTING);
}

static void send_audio_control_point_stop_cb(bool success,
					     const uint8_t att_ecode,
					     void *user_data)
{
	struct asha_transport *t = user_data;

	if (!success) {
		DBG("Writing control point failed with ATT error: %u",
		    att_ecode);
		return;
	}

	DBG("Control point written: %u", att_ecode);
}

static gboolean connect_and_set_fd(gpointer data)
{
	gboolean ret;
	struct asha_transport *t = data;

	int fd = l2cap_connect(t);
	media_transport_set_fd(t->transport, fd, t->imtu, t->omtu);

	struct media_request *req = t->owner->pending;

	// Need to do this for media_owner_remove
	req->id = 0;

	ret = g_dbus_send_reply(btd_get_dbus_connection(), req->msg,
				DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_UINT16,
				&t->imtu, DBUS_TYPE_UINT16, &t->omtu,
				DBUS_TYPE_INVALID);

	if (ret == FALSE) {
		media_transport_remove_owner(t->transport);
		return FALSE;
	}

	media_owner_remove(t->owner);

	send_audio_control_point_start(t, send_audio_control_point_start_cb);

	return FALSE;
}

static guint resume_asha(struct media_transport *transport,
			 struct media_owner *owner)
{
	struct asha_transport *t = transport->data;
	struct media_endpoint *endpoint = transport->endpoint;
	uint16_t psm;

	const bdaddr_t *addr = device_get_address(transport->device);
	gboolean ret;
	uint16_t imtu = MTU, omtu = MTU;

	if (!asha_get_psm(transport->device, &psm)) {
		DBG("Cannot read PSM");
		return 0;
	}

	if (addr == NULL) {
		DBG("Cannot read bd addr");
		return 0;
	}

	t->addr = (bdaddr_t *)addr;
	t->imtu = MTU;
	t->omtu = MTU;
	// Duplicated between the asha struct and here
	t->psm = psm;
	t->owner = owner;
	t->transport = transport;

	g_idle_add(connect_and_set_fd, t);
	DBG("ASHA Transport Resume");

	if (transport->state == TRANSPORT_STATE_IDLE)
		transport_set_state(transport, TRANSPORT_STATE_REQUESTING);

	return cb_id++;

fail:
	media_transport_remove_owner(transport);

	return 0;
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
	struct asha_transport *t = transport->data;

	if (transport->state != TRANSPORT_STATE_ACTIVE) {
		return 0;
	}

	close(transport->fd);

	send_audio_control_point_stop(t, send_audio_control_point_stop_cb);

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
	// TODO
	//
	// Once the transport actions are moved to callbacks that are stored
	// centrally (maybe in an asha_setup structure) we should interrupt(?) them &
	// remove them here.
}

/*
 *
 */
static void destroy_asha(void *data)
{
	g_free((struct asha_transport *)data);
}

/*
 * Sets up the media_transport lifecycle functions with asha functions
 */
int asha_transport_init(struct media_transport *transport)
{
	struct btd_service *service;
	struct asha_transport *asha_transport;

	DBG("ASHA Transport Init");
	service = btd_device_get_service(transport->device, ASHA_SINK_UUID);

	// We shouldn't have been called if the device doesn't have an ASHA service
	// associated
	if (service == NULL)
		return -EINVAL;

	// Create a structure to hold information that the transport functionality
	// requires
	asha_transport = g_new0(struct asha_transport, 1);
	asha_transport->asha = btd_service_get_user_data(service);

	/*
   * NOTE
   *
   * We have a handle on the asha struct through asha_transport. The asha
   * struct goes away when the device/service goes away. This means that we
   * should terminate any transport code that accesses the asha struct when the device/service goes away.
   *
   * TODO Remove this note once we know this is happening.
   *
   * Is there a better way to structure this so the interdependency is more obvious?
   */

	transport->resume = resume_asha;
	transport->suspend = suspend_asha;
	transport->cancel = cancel_asha;
	transport->data = asha_transport;
	transport->destroy = destroy_asha;
}
