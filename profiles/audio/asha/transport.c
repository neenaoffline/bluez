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

#include "gdbus/gdbus.h"

#include "src/adapter.h"
#include "src/device.h"
#include "src/log.h"
#include "profiles/audio/media.h"
#include "profiles/audio/transport.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

struct asha_transport {
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
static guint resume_asha(struct media_transport *transport,
			 struct media_owner *owner)
{
	struct asha_transport *asha = transport->data;
	struct media_endpoint *endpoint = transport->endpoint;
	struct asha_central *asha_central =
		media_endpoint_get_asha_central(endpoint);

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
}

/*
 * Cancel searches all setups' callbacks for the callback with this id and then
 * cancels it (abort & free)
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
	struct asha_transport *asha;

	DBG("ASHA Transport Init");
	service = btd_device_get_service(transport->device, ASHA_SINK_UUID);
	if (service == NULL)
		return -EINVAL;

	asha = g_new0(struct asha_transport, 1);

	transport->resume = resume_asha;
	transport->suspend = suspend_asha;
	transport->cancel = cancel_asha;
	transport->data = asha;
	transport->destroy = destroy_asha;
}
