/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2007  Nokia Corporation
 *  Copyright (C) 2004-2009  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 */

typedef enum {
	TRANSPORT_STATE_IDLE, /* Not acquired and suspended */
	TRANSPORT_STATE_PENDING, /* Playing but not acquired */
	TRANSPORT_STATE_REQUESTING, /* Acquire in progress */
	TRANSPORT_STATE_ACTIVE, /* Acquired and playing */
	TRANSPORT_STATE_SUSPENDING, /* Release in progress */
} transport_state_t;

struct media_request {
	DBusMessage *msg;
	guint id;
};

struct media_owner {
	/*
   * DBus Name Owner of a name
   *
   * Which 'owns' a certain media_transport
   *
   * And has pending media_request
   */
	struct media_transport *transport;
	struct media_request *pending;
	char *name;
	guint watch;
};
struct media_transport {
	char *path; /* Transport object path */
	struct btd_device *device; /* Transport device */
	const char *remote_endpoint; /* Transport remote SEP */
	struct media_endpoint *endpoint; /* Transport endpoint */
	struct media_owner *owner; /* Transport owner */
	uint8_t *configuration; /* Transport configuration */
	int size; /* Transport configuration size */
	int fd; /* Transport file descriptor */
	uint16_t imtu; /* Transport input mtu */
	uint16_t omtu; /* Transport output mtu */
	transport_state_t state;
	guint hs_watch;
	guint source_watch;
	guint sink_watch;
	guint (*resume)(struct media_transport *transport,
			struct media_owner *owner);
	guint (*suspend)(struct media_transport *transport,
			 struct media_owner *owner);
	void (*cancel)(struct media_transport *transport, guint id);
	GDestroyNotify destroy;
	void *data;
};

struct media_transport *media_transport_create(struct btd_device *device,
					       const char *remote_endpoint,
					       uint8_t *configuration,
					       size_t size, void *data);

void media_owner_remove(struct media_owner *owner);
void media_transport_remove_owner(struct media_transport *transport);
void media_transport_destroy(struct media_transport *transport);
const char *media_transport_get_path(struct media_transport *transport);
struct btd_device *media_transport_get_dev(struct media_transport *transport);
int8_t media_transport_get_volume(struct media_transport *transport);
struct media_endpoint *
media_transport_get_endpoint(struct media_transport *transport);
void media_transport_update_delay(struct media_transport *transport,
				  uint16_t delay);
void media_transport_update_volume(struct media_transport *transport,
				   int8_t volume);
gboolean media_transport_set_fd(struct media_transport *transport, int fd,
				uint16_t imtu, uint16_t omtu);
void transport_get_properties(struct media_transport *transport,
			      DBusMessageIter *iter);
void transport_set_state(struct media_transport *transport,
			 transport_state_t state);

int8_t media_transport_get_device_volume(struct btd_device *dev);
void media_transport_update_device_volume(struct btd_device *dev,
					  int8_t volume);
