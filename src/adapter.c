/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2007  Nokia Corporation
 *  Copyright (C) 2004-2009  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <gdbus.h>

#include "logging.h"
#include "textfile.h"

#include "hcid.h"
#include "sdpd.h"
#include "sdp-xml.h"
#include "manager.h"
#include "adapter.h"
#include "device.h"
#include "dbus-common.h"
#include "dbus-hci.h"
#include "error.h"
#include "glib-helper.h"
#include "agent.h"
#include "storage.h"

#define NUM_ELEMENTS(table) (sizeof(table)/sizeof(const char *))

#define IO_CAPABILITY_DISPLAYONLY	0x00
#define IO_CAPABILITY_DISPLAYYESNO	0x01
#define IO_CAPABILITY_KEYBOARDONLY	0x02
#define IO_CAPABILITY_NOINPUTOUTPUT	0x03
#define IO_CAPABILITY_INVALID		0xFF

#define check_address(address) bachk(address)

static DBusConnection *connection = NULL;
static GSList *adapter_drivers = NULL;

struct session_req {
	struct btd_adapter	*adapter;
	DBusConnection		*conn;		/* Connection reference */
	DBusMessage		*msg;		/* Unreplied message ref */
	char			*owner;		/* Bus name of the owner */
	guint			id;		/* Listener id */
	uint8_t			mode;		/* Requested mode */
	int			refcount;	/* Session refcount */
};

struct service_auth {
	service_auth_cb cb;
	void *user_data;
};

struct btd_adapter {
	uint16_t dev_id;
	int up;
	char *path;			/* adapter object path */
	bdaddr_t bdaddr;		/* adapter Bluetooth Address */
	guint discov_timeout_id;	/* discoverable timeout id */
	uint32_t discov_timeout;	/* discoverable time(sec) */
	guint pairable_timeout_id;	/* pairable timeout id */
	uint32_t pairable_timeout;	/* pairable time(sec) */
	uint8_t scan_mode;		/* scan mode: SCAN_DISABLED, SCAN_PAGE,
					 * SCAN_INQUIRY */
	uint8_t mode;			/* off, connectable, discoverable,
					 * limited */
	uint8_t global_mode;		/* last valid global mode */
	int state;			/* standard inq, periodic inq, name
					 * resloving */
	GSList *found_devices;
	GSList *oor_devices;		/* out of range device list */
	DBusMessage *discovery_cancel;	/* discovery cancel message request */
	GSList *passkey_agents;
	struct agent *agent;		/* For the new API */
	GSList *connections;		/* Connected devices */
	GSList *devices;		/* Devices structure pointers */
	GSList *mode_sessions;		/* Request Mode sessions */
	GSList *disc_sessions;		/* Discovery sessions */
	guint scheduler_id;		/* Scheduler handle */

	struct hci_dev dev;		/* hci info */
	gboolean pairable;		/* pairable state */

	gboolean initialized;
	gboolean already_up;		/* adapter was already up on init */

	gboolean off_requested;		/* DEVDOWN ioctl was called */

	uint8_t svc_cache;		/* For bluetoothd startup */
};

static void adapter_set_pairable_timeout(struct btd_adapter *adapter,
					guint interval);

static inline DBusMessage *invalid_args(DBusMessage *msg)
{
	return g_dbus_create_error(msg, ERROR_INTERFACE ".InvalidArguments",
			"Invalid arguments in method call");
}

static inline DBusMessage *not_available(DBusMessage *msg)
{
	return g_dbus_create_error(msg, ERROR_INTERFACE ".NotAvailable",
			"Not Available");
}

static inline DBusMessage *adapter_not_ready(DBusMessage *msg)
{
	return g_dbus_create_error(msg, ERROR_INTERFACE ".NotReady",
			"Adapter is not ready");
}

static inline DBusMessage *no_such_adapter(DBusMessage *msg)
{
	return g_dbus_create_error(msg, ERROR_INTERFACE ".NoSuchAdapter",
							"No such adapter");
}

static inline DBusMessage *failed_strerror(DBusMessage *msg, int err)
{
	return g_dbus_create_error(msg, ERROR_INTERFACE ".Failed",
							strerror(err));
}

static inline DBusMessage *in_progress(DBusMessage *msg, const char *str)
{
	return g_dbus_create_error(msg, ERROR_INTERFACE ".InProgress", str);
}

static inline DBusMessage *not_in_progress(DBusMessage *msg, const char *str)
{
	return g_dbus_create_error(msg, ERROR_INTERFACE ".NotInProgress", str);
}

static inline DBusMessage *not_authorized(DBusMessage *msg)
{
	return g_dbus_create_error(msg, ERROR_INTERFACE ".NotAuthorized",
			"Not authorized");
}

static inline DBusMessage *unsupported_major_class(DBusMessage *msg)
{
	return g_dbus_create_error(msg,
			ERROR_INTERFACE ".UnsupportedMajorClass",
			"Unsupported Major Class");
}

static int found_device_cmp(const struct remote_dev_info *d1,
			const struct remote_dev_info *d2)
{
	int ret;

	if (bacmp(&d2->bdaddr, BDADDR_ANY)) {
		ret = bacmp(&d1->bdaddr, &d2->bdaddr);
		if (ret)
			return ret;
	}

	if (d2->name_status != NAME_ANY) {
		ret = (d1->name_status - d2->name_status);
		if (ret)
			return ret;
	}

	return 0;
}

static void dev_info_free(struct remote_dev_info *dev)
{
	g_free(dev->alias);
	g_free(dev);
}

int pending_remote_name_cancel(struct btd_adapter *adapter)
{
	struct remote_dev_info *dev, match;
	GSList *l;
	int dd, err = 0;

	/* find the pending remote name request */
	memset(&match, 0, sizeof(struct remote_dev_info));
	bacpy(&match.bdaddr, BDADDR_ANY);
	match.name_status = NAME_REQUESTED;

	l = g_slist_find_custom(adapter->found_devices, &match,
			(GCompareFunc) found_device_cmp);
	if (!l) /* no pending request */
		return 0;

	dd = hci_open_dev(adapter->dev_id);
	if (dd < 0)
		return -ENODEV;

	dev = l->data;

	if (hci_read_remote_name_cancel(dd, &dev->bdaddr,
					HCI_REQ_TIMEOUT) < 0) {
		error("Remote name cancel failed: %s(%d)", strerror(errno),
									errno);
		err = -errno;
	}

	/* free discovered devices list */
	g_slist_foreach(adapter->found_devices, (GFunc) dev_info_free, NULL);
	g_slist_free(adapter->found_devices);
	adapter->found_devices = NULL;

	hci_close_dev(dd);

	return err;
}

static int set_limited_discoverable(int dd, const uint8_t *cls,
							gboolean limited)
{
	uint32_t dev_class;
	int num = (limited ? 2 : 1);
	uint8_t lap[] = { 0x33, 0x8b, 0x9e, 0x00, 0x8b, 0x9e };
	/*
	 * 1: giac
	 * 2: giac + liac
	 */
	if (hci_write_current_iac_lap(dd, num, lap, HCI_REQ_TIMEOUT) < 0) {
		int err = -errno;
		error("Can't write current IAC LAP: %s(%d)",
				strerror(err), err);
		return err;
	}

	if (limited) {
		if (cls[1] & 0x20)
			return 0; /* Already limited */

		dev_class = (cls[2] << 16) | ((cls[1] | 0x20) << 8) | cls[0];
	} else {
		if (!(cls[1] & 0x20))
			return 0; /* Already clear */

		dev_class = (cls[2] << 16) | ((cls[1] & 0xdf) << 8) | cls[0];
	}

	if (hci_write_class_of_dev(dd, dev_class, HCI_REQ_TIMEOUT) < 0) {
		int err = -errno;
		error("Can't write class of device: %s (%d)",
							strerror(err), err);
		return err;
	}

	return 0;
}

static const char *mode2str(uint8_t mode)
{
	switch(mode) {
	case MODE_OFF:
		return "off";
	case MODE_CONNECTABLE:
		return "connectable";
	case MODE_DISCOVERABLE:
	case MODE_LIMITED:
		return "discoverable";
	default:
		return "unknown";
	}
}

static uint8_t get_mode(const bdaddr_t *bdaddr, const char *mode)
{
	if (strcasecmp("off", mode) == 0)
		return MODE_OFF;
	else if (strcasecmp("connectable", mode) == 0)
		return MODE_CONNECTABLE;
	else if (strcasecmp("discoverable", mode) == 0)
		return MODE_DISCOVERABLE;
	else if (strcasecmp("limited", mode) == 0)
		return MODE_LIMITED;
	else if (strcasecmp("on", mode) == 0) {
		char onmode[14], srcaddr[18];

		ba2str(bdaddr, srcaddr);
		if (read_on_mode(srcaddr, onmode, sizeof(onmode)) < 0)
			return MODE_CONNECTABLE;

		return get_mode(bdaddr, onmode);
	} else
		return MODE_UNKNOWN;
}

static void adapter_remove_discov_timeout(struct btd_adapter *adapter)
{
	if (!adapter)
		return;

	if(adapter->discov_timeout_id == 0)
		return;

	g_source_remove(adapter->discov_timeout_id);
	adapter->discov_timeout_id = 0;
}

static gboolean discov_timeout_handler(gpointer user_data)
{
	struct btd_adapter *adapter = user_data;
	int dd;
	uint8_t scan_enable;
	uint16_t dev_id = adapter->dev_id;

	adapter->discov_timeout_id = 0;

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		error("HCI device open failed: hci%d", dev_id);
		return FALSE;
	}

	scan_enable = adapter->scan_mode & ~SCAN_INQUIRY;

	hci_send_cmd(dd, OGF_HOST_CTL, OCF_WRITE_SCAN_ENABLE,
					1, &scan_enable);
	hci_close_dev(dd);

	return FALSE;
}

static void adapter_set_discov_timeout(struct btd_adapter *adapter,
					guint interval)
{
	if (adapter->discov_timeout_id) {
		g_source_remove(adapter->discov_timeout_id);
		adapter->discov_timeout_id = 0;
	}

	if (interval == 0)
		return;

	adapter->discov_timeout_id = g_timeout_add_seconds(interval,
							discov_timeout_handler,
							adapter);
}

static uint8_t mode2scan(uint8_t mode)
{
	switch (mode) {
	case MODE_OFF:
		return SCAN_DISABLED;
	case MODE_CONNECTABLE:
		return SCAN_PAGE;
	case MODE_DISCOVERABLE:
	case MODE_LIMITED:
		return (SCAN_PAGE | SCAN_INQUIRY);
	default:
		error("Invalid mode given to mode2scan: %u", mode);
		return SCAN_PAGE;
	}
}

static int set_mode(struct btd_adapter *adapter, uint8_t new_mode)
{
	uint8_t scan_enable;
	uint8_t current_scan = adapter->scan_mode;
	int err, dd;
	const char *modestr;

	scan_enable = mode2scan(new_mode);

	dd = hci_open_dev(adapter->dev_id);
	if (dd < 0)
		return -EIO;

	if (!adapter->up && scan_enable != SCAN_DISABLED) {
		/* Start HCI device */
		if (ioctl(dd, HCIDEVUP, adapter->dev_id) == 0)
			goto done; /* on success */

		if (errno != EALREADY) {
			err = -errno;
			error("Can't init device hci%d: %s (%d)\n",
				adapter->dev_id, strerror(errno), errno);

			hci_close_dev(dd);
			return err;
		}
	}

	if (adapter->up && scan_enable == SCAN_DISABLED) {
		struct hci_request rq = {
			.ogf = OGF_HOST_CTL,
			.ocf = OCF_WRITE_SCAN_ENABLE,
			.cparam = &scan_enable,
			.clen = sizeof(scan_enable),
		};

		hci_send_req(dd, &rq, HCI_REQ_TIMEOUT);

		if (ioctl(dd, HCIDEVDOWN, adapter->dev_id) < 0) {
			err = -errno;
			hci_close_dev(dd);
			return err;
		}

		adapter->off_requested = TRUE;

		goto done;
	}

	if (current_scan != scan_enable) {
		err = hci_send_cmd(dd, OGF_HOST_CTL, OCF_WRITE_SCAN_ENABLE,
					1, &scan_enable);
		if (err < 0) {
			hci_close_dev(dd);
			return err;
		}
	} else if ((scan_enable & SCAN_INQUIRY) &&
						(new_mode != adapter->mode)) {
		adapter_remove_discov_timeout(adapter);
		if (adapter->discov_timeout)
			adapter_set_discov_timeout(adapter,
						adapter->discov_timeout);
		if (new_mode == MODE_LIMITED)
			set_limited_discoverable(dd, adapter->dev.class,
									TRUE);
		else if (adapter->mode == MODE_LIMITED)
			set_limited_discoverable(dd, adapter->dev.class,
									FALSE);
	}
done:
	modestr = mode2str(new_mode);

	write_device_mode(&adapter->bdaddr, modestr);

	hci_close_dev(dd);

	adapter->mode = new_mode;

	return 0;
}

static DBusMessage *set_powered(DBusConnection *conn, DBusMessage *msg,
				gboolean powered, void *data)
{
	struct btd_adapter *adapter = data;
	uint8_t mode;
	int err;

	mode = powered ? get_mode(&adapter->bdaddr, "on") : MODE_OFF;

	if (mode == adapter->mode)
		return dbus_message_new_method_return(msg);

	err = set_mode(adapter, mode);
	if (err < 0)
		return failed_strerror(msg, -err);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *set_discoverable(DBusConnection *conn, DBusMessage *msg,
				gboolean discoverable, void *data)
{
	struct btd_adapter *adapter = data;
	uint8_t mode;
	int err;

	mode = discoverable ? MODE_DISCOVERABLE : MODE_CONNECTABLE;

	if (mode == MODE_DISCOVERABLE && adapter->pairable)
		mode = MODE_LIMITED;

	if (mode == adapter->mode)
		return dbus_message_new_method_return(msg);

	err = set_mode(adapter, mode);
	if (err < 0)
		return failed_strerror(msg, -err);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *set_pairable(DBusConnection *conn, DBusMessage *msg,
				gboolean pairable, void *data)
{
	struct btd_adapter *adapter = data;
	uint8_t mode;
	int err;

	if (adapter->scan_mode == SCAN_DISABLED)
		return adapter_not_ready(msg);

	if (pairable == adapter->pairable)
		goto done;

	adapter->pairable = pairable;

	write_device_pairable(&adapter->bdaddr, pairable);

	emit_property_changed(connection, adapter->path,
				ADAPTER_INTERFACE, "Pairable",
				DBUS_TYPE_BOOLEAN, &pairable);

	if (pairable && adapter->pairable_timeout)
		adapter_set_pairable_timeout(adapter,
						adapter->pairable_timeout);

	if (!(adapter->scan_mode & SCAN_INQUIRY))
		goto done;

	mode = pairable ? MODE_LIMITED : MODE_DISCOVERABLE;

	err = set_mode(adapter, mode);
	if (err < 0 && msg)
		return failed_strerror(msg, -err);

done:
	return msg ? dbus_message_new_method_return(msg) : NULL;
}

static gboolean pairable_timeout_handler(void *data)
{
	set_pairable(NULL, NULL, FALSE, data);

	return FALSE;
}

static void adapter_set_pairable_timeout(struct btd_adapter *adapter,
					guint interval)
{
	if (adapter->pairable_timeout_id) {
		g_source_remove(adapter->pairable_timeout_id);
		adapter->pairable_timeout_id = 0;
	}

	if (interval == 0)
		return;

	adapter->pairable_timeout_id = g_timeout_add_seconds(interval,
						pairable_timeout_handler,
						adapter);
}

static struct session_req *find_session(GSList *list, const char *sender)
{
	GSList *l;

	for (l = list; l; l = l->next) {
		struct session_req *req = l->data;

		if (g_str_equal(req->owner, sender))
			return req;
	}

	return NULL;
}

static uint8_t get_needed_mode(struct btd_adapter *adapter, uint8_t mode)
{
	GSList *l;

	if (adapter->global_mode > mode)
		mode = adapter->global_mode;

	for (l = adapter->mode_sessions; l; l = l->next) {
		struct session_req *req = l->data;

		if (req->mode > mode)
			mode = req->mode;
	}

	return mode;
}

static void session_remove(struct session_req *req)
{
	struct btd_adapter *adapter = req->adapter;

	if (req->mode) {
		uint8_t mode;

		adapter->mode_sessions = g_slist_remove(adapter->mode_sessions,
							req);

		mode = get_needed_mode(adapter, adapter->global_mode);

		if (mode == adapter->mode)
			return;

		debug("Switching to '%s' mode", mode2str(mode));

		set_mode(adapter, mode);
	} else {
		adapter->disc_sessions = g_slist_remove(adapter->disc_sessions,
							req);

		if (adapter->disc_sessions)
			return;

		debug("Stopping discovery");

		g_slist_foreach(adapter->found_devices, (GFunc) dev_info_free,
				NULL);
		g_slist_free(adapter->found_devices);
		adapter->found_devices = NULL;

		if (adapter->state & STD_INQUIRY)
			cancel_discovery(adapter);
		else if (adapter->scheduler_id)
			g_source_remove(adapter->scheduler_id);
		else
			cancel_periodic_discovery(adapter);
	}
}

static void session_free(struct session_req *req)
{
	debug("%s session %p with %s deactivated",
		req->mode ? "Mode" : "Discovery", req, req->owner);

	if (req->id)
		g_dbus_remove_watch(req->conn, req->id);

	session_remove(req);

	if (req->msg)
		dbus_message_unref(req->msg);
	if (req->conn)
		dbus_connection_unref(req->conn);
	g_free(req->owner);
	g_free(req);
}

static void session_owner_exit(DBusConnection *conn, void *user_data)
{
	struct session_req *req = user_data;

	req->id = 0;

	session_free(req);
}

static struct session_req *session_ref(struct session_req *req)
{
	req->refcount++;

	debug("session_ref(%p): ref=%d", req, req->refcount);

	return req;
}

static void session_unref(struct session_req *req)
{
	req->refcount--;

	debug("session_unref(%p): ref=%d", req, req->refcount);

	if (req->refcount)
		return;

	session_free(req);
}

static struct session_req *create_session(struct btd_adapter *adapter,
					DBusConnection *conn, DBusMessage *msg,
					uint8_t mode, GDBusWatchFunction cb)
{
	struct session_req *req;
	const char *sender = dbus_message_get_sender(msg);

	req = g_new0(struct session_req, 1);
	req->adapter = adapter;
	req->conn = dbus_connection_ref(conn);
	req->msg = dbus_message_ref(msg);
	req->owner = g_strdup(dbus_message_get_sender(msg));
	req->mode = mode;

	if (cb)
		req->id = g_dbus_add_disconnect_watch(conn, sender, cb, req,
							NULL);

	info("%s session %p with %s activated",
		req->mode ? "Mode" : "Discovery", req, sender);

	return session_ref(req);
}

static void confirm_mode_cb(struct agent *agent, DBusError *derr, void *data)
{
	struct session_req *req = data;
	int err;
	DBusMessage *reply;

	if (derr && dbus_error_is_set(derr)) {
		reply = dbus_message_new_error(req->msg, derr->name,
						derr->message);
		g_dbus_send_message(req->conn, reply);
		session_unref(req);
		return;
	}

	err = set_mode(req->adapter, req->mode);
	if (err < 0)
		reply = failed_strerror(req->msg, -err);
	else
		reply = dbus_message_new_method_return(req->msg);

	g_dbus_send_message(req->conn, reply);

	dbus_message_unref(req->msg);
	req->msg = NULL;

	if (!find_session(req->adapter->mode_sessions, req->owner))
		session_unref(req);
}

static DBusMessage *set_discoverable_timeout(DBusConnection *conn,
							DBusMessage *msg,
							uint32_t timeout,
							void *data)
{
	struct btd_adapter *adapter = data;
	const char *path;

	if (adapter->discov_timeout == timeout && timeout == 0)
		return dbus_message_new_method_return(msg);

	if (adapter->scan_mode & SCAN_INQUIRY)
		adapter_set_discov_timeout(adapter, timeout);

	adapter->discov_timeout = timeout;

	write_discoverable_timeout(&adapter->bdaddr, timeout);

	path = dbus_message_get_path(msg);

	emit_property_changed(conn, path,
				ADAPTER_INTERFACE, "DiscoverableTimeout",
				DBUS_TYPE_UINT32, &timeout);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *set_pairable_timeout(DBusConnection *conn,
						DBusMessage *msg,
						uint32_t timeout,
						void *data)
{
	struct btd_adapter *adapter = data;
	const char *path;

	if (adapter->pairable_timeout == timeout && timeout == 0)
		return dbus_message_new_method_return(msg);

	if (adapter->pairable)
		adapter_set_pairable_timeout(adapter, timeout);

	adapter->pairable_timeout = timeout;

	write_pairable_timeout(&adapter->bdaddr, timeout);

	path = dbus_message_get_path(msg);

	emit_property_changed(conn, path,
				ADAPTER_INTERFACE, "PairableTimeout",
				DBUS_TYPE_UINT32, &timeout);

	return dbus_message_new_method_return(msg);
}

static void update_ext_inquiry_response(int dd, struct hci_dev *dev)
{
	uint8_t fec = 0, data[240];

	if (!(dev->features[6] & LMP_EXT_INQ))
		return;

	memset(data, 0, sizeof(data));

	if (dev->ssp_mode > 0)
		create_ext_inquiry_response((char *) dev->name, data);

	if (hci_write_ext_inquiry_response(dd, fec, data,
						HCI_REQ_TIMEOUT) < 0)
		error("Can't write extended inquiry response: %s (%d)",
						strerror(errno), errno);
}

void adapter_name_changed(struct btd_adapter *adapter, const char *name)
{
	struct hci_dev *dev = &adapter->dev;
	int dd;

	if (strncmp(name, (char *) dev->name, 248) == 0)
		return;

	write_local_name(&adapter->bdaddr, (char *) name);

	strncpy((char *) dev->name, name, 248);

	dd = hci_open_dev(adapter->dev_id);
	if (dd >= 0) {
		update_ext_inquiry_response(dd, dev);
		hci_close_dev(dd);
	}

	emit_property_changed(connection, adapter->path, ADAPTER_INTERFACE,
				"Name", DBUS_TYPE_STRING, &name);
}

static int adapter_set_name(struct btd_adapter *adapter, const char *name)
{
	struct hci_dev *dev = &adapter->dev;
	int dd, err;

	write_local_name(&adapter->bdaddr, (char *) name);

	strncpy((char *) dev->name, name, 248);

	if (!adapter->up)
		return 0;

	dd = hci_open_dev(adapter->dev_id);
	if (dd < 0) {
		err = -errno;
		error("Can't open device hci%d: %s (%d)",
					adapter->dev_id, strerror(err), err);
		return err;
	}

	if (hci_write_local_name(dd, name, HCI_REQ_TIMEOUT) < 0) {
		err = -errno;
		error("Can't write name for hci%d: %s (%d)",
					adapter->dev_id, strerror(err), err);
		hci_close_dev(dd);
		return err;
	}

	update_ext_inquiry_response(dd, dev);

	hci_close_dev(dd);

	return 0;
}

static DBusMessage *set_name(DBusConnection *conn, DBusMessage *msg,
					const char *name, void *data)
{
	struct btd_adapter *adapter = data;
	int ecode;

	if (!g_utf8_validate(name, -1, NULL)) {
		error("Name change failed: supplied name isn't valid UTF-8");
		return invalid_args(msg);
	}

	ecode = adapter_set_name(adapter, name);
	if (ecode < 0)
		return failed_strerror(msg, -ecode);

	emit_property_changed(conn, adapter->path, ADAPTER_INTERFACE,
				"Name", DBUS_TYPE_STRING, &name);

	return dbus_message_new_method_return(msg);
}

struct btd_device *adapter_find_device(struct btd_adapter *adapter,
							const char *dest)
{
	struct btd_device *device;
	GSList *l;

	if (!adapter)
		return NULL;

	l = g_slist_find_custom(adapter->devices, dest,
					(GCompareFunc) device_address_cmp);
	if (!l)
		return NULL;

	device = l->data;

	return device;
}

struct btd_device *adapter_find_connection(struct btd_adapter *adapter,
						uint16_t handle)
{
	GSList *l;

	for (l = adapter->connections; l; l = l->next) {
		struct btd_device *device = l->data;

		if (device_has_connection(device, handle))
			return device;
	}

	return NULL;
}

static void adapter_update_devices(struct btd_adapter *adapter)
{
	char **devices;
	int i;
	GSList *l;

	/* Devices */
	devices = g_new0(char *, g_slist_length(adapter->devices) + 1);
	for (i = 0, l = adapter->devices; l; l = l->next, i++) {
		struct btd_device *dev = l->data;
		devices[i] = (char *) device_get_path(dev);
	}

	emit_array_property_changed(connection, adapter->path,
					ADAPTER_INTERFACE, "Devices",
					DBUS_TYPE_OBJECT_PATH, &devices);
	g_free(devices);
}

struct btd_device *adapter_create_device(DBusConnection *conn,
						struct btd_adapter *adapter,
						const char *address)
{
	struct btd_device *device;
	const char *path;

	debug("adapter_create_device(%s)", address);

	device = device_create(conn, adapter, address);
	if (!device)
		return NULL;

	device_set_temporary(device, TRUE);

	adapter->devices = g_slist_append(adapter->devices, device);

	path = device_get_path(device);
	g_dbus_emit_signal(conn, adapter->path,
			ADAPTER_INTERFACE, "DeviceCreated",
			DBUS_TYPE_OBJECT_PATH, &path,
			DBUS_TYPE_INVALID);

	adapter_update_devices(adapter);

	return device;
}

void adapter_remove_device(DBusConnection *conn, struct btd_adapter *adapter,
				struct btd_device *device)
{
	const gchar *dev_path = device_get_path(device);
	struct agent *agent;

	adapter->devices = g_slist_remove(adapter->devices, device);
	adapter->connections = g_slist_remove(adapter->connections, device);

	adapter_update_devices(adapter);

	g_dbus_emit_signal(conn, adapter->path,
			ADAPTER_INTERFACE, "DeviceRemoved",
			DBUS_TYPE_OBJECT_PATH, &dev_path,
			DBUS_TYPE_INVALID);

	agent = device_get_agent(device);

	if (agent) {
		agent_destroy(agent, FALSE);
		device_set_agent(device, NULL);
	}

	device_remove(device, conn, TRUE);
}

struct btd_device *adapter_get_device(DBusConnection *conn,
						struct btd_adapter *adapter,
						const gchar *address)
{
	struct btd_device *device;

	debug("adapter_get_device(%s)", address);

	if (!adapter)
		return NULL;

	device = adapter_find_device(adapter, address);
	if (device)
		return device;

	return adapter_create_device(conn, adapter, address);
}

static int start_inquiry(struct btd_adapter *adapter)
{
	inquiry_cp cp;
	evt_cmd_status rp;
	struct hci_request rq;
	uint8_t lap[3] = { 0x33, 0x8b, 0x9e };
	int dd, err;

	pending_remote_name_cancel(adapter);

	dd = hci_open_dev(adapter->dev_id);
	if (dd < 0)
		return dd;

	memset(&cp, 0, sizeof(cp));
	memcpy(&cp.lap, lap, 3);
	cp.length = 0x08;
	cp.num_rsp = 0x00;

	memset(&rq, 0, sizeof(rq));
	rq.ogf = OGF_LINK_CTL;
	rq.ocf = OCF_INQUIRY;
	rq.cparam = &cp;
	rq.clen = INQUIRY_CP_SIZE;
	rq.rparam = &rp;
	rq.rlen = EVT_CMD_STATUS_SIZE;
	rq.event = EVT_CMD_STATUS;

	if (hci_send_req(dd, &rq, HCI_REQ_TIMEOUT) < 0) {
		err = -errno;
		error("Unable to start inquiry: %s (%d)",
			strerror(err), err);
		hci_close_dev(dd);
		return err;
	}

	if (rp.status) {
		error("HCI_Inquiry command failed with status 0x%02x",
			rp.status);
		hci_close_dev(dd);
		return -bt_error(rp.status);
	}

	hci_close_dev(dd);

	adapter->state |= RESOLVE_NAME;

	return 0;
}

static int start_periodic_inquiry(struct btd_adapter *adapter)
{
	periodic_inquiry_cp cp;
	struct hci_request rq;
	uint8_t lap[3] = { 0x33, 0x8b, 0x9e };
	uint8_t status;
	int dd, err;

	dd = hci_open_dev(adapter->dev_id);
	if (dd < 0)
		return dd;

	memset(&cp, 0, sizeof(cp));
	memcpy(&cp.lap, lap, 3);
	cp.max_period = htobs(24);
	cp.min_period = htobs(16);
	cp.length  = 0x08;
	cp.num_rsp = 0x00;

	memset(&rq, 0, sizeof(rq));
	rq.ogf    = OGF_LINK_CTL;
	rq.ocf    = OCF_PERIODIC_INQUIRY;
	rq.cparam = &cp;
	rq.clen   = PERIODIC_INQUIRY_CP_SIZE;
	rq.rparam = &status;
	rq.rlen   = sizeof(status);
	rq.event  = EVT_CMD_COMPLETE;

	if (hci_send_req(dd, &rq, HCI_REQ_TIMEOUT) < 0) {
		err = -errno;
		error("Unable to start periodic inquiry: %s (%d)",
				strerror(err), err);
		hci_close_dev(dd);
		return err;
	}

	if (status) {
		error("HCI_Periodic_Inquiry_Mode failed with status 0x%02x",
				status);
		hci_close_dev(dd);
		return -bt_error(status);
	}

	hci_close_dev(dd);

	adapter->state |= RESOLVE_NAME;

	return 0;
}

static DBusMessage *adapter_start_discovery(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct session_req *req;
	struct btd_adapter *adapter = data;
	const char *sender = dbus_message_get_sender(msg);
	int err;

	if (!adapter->up)
		return adapter_not_ready(msg);

	req = find_session(adapter->disc_sessions, sender);
	if (req) {
		session_ref(req);
		return dbus_message_new_method_return(msg);
	}

	if (adapter->disc_sessions)
		goto done;

	if (main_opts.inqmode)
		err = start_inquiry(adapter);
	else
		err = start_periodic_inquiry(adapter);

	if (err < 0)
		return failed_strerror(msg, -err);

done:
	req = create_session(adapter, conn, msg, 0,
				session_owner_exit);

	adapter->disc_sessions = g_slist_append(adapter->disc_sessions, req);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *adapter_stop_discovery(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct btd_adapter *adapter = data;
	struct session_req *req;
	const char *sender = dbus_message_get_sender(msg);

	if (!adapter->up)
		return adapter_not_ready(msg);

	req = find_session(adapter->disc_sessions, sender);
	if (!req)
		return g_dbus_create_error(msg, ERROR_INTERFACE ".Failed",
				"Invalid discovery session");

	session_unref(req);

	return dbus_message_new_method_return(msg);
}

struct remote_device_list_t {
	GSList *list;
	time_t time;
};

static DBusMessage *get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct btd_adapter *adapter = data;
	const char *property;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	char str[249], srcaddr[18];
	uint32_t class;
	gboolean value;
	char **devices;
	int i;
	GSList *l;

	ba2str(&adapter->bdaddr, srcaddr);

	if (check_address(srcaddr) < 0)
		return adapter_not_ready(msg);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	/* Address */
	property = srcaddr;
	dict_append_entry(&dict, "Address", DBUS_TYPE_STRING, &property);

	/* Name */
	memset(str, 0, sizeof(str));
	strncpy(str, (char *) adapter->dev.name, 248);
	property = str;

	dict_append_entry(&dict, "Name", DBUS_TYPE_STRING, &property);

	/* Class */
	class = adapter->dev.class[0] |
			adapter->dev.class[1] << 8 |
			adapter->dev.class[2] << 16;
	dict_append_entry(&dict, "Class", DBUS_TYPE_UINT32, &class);

	/* Powered */
	value = adapter->up ? TRUE : FALSE;
	dict_append_entry(&dict, "Powered", DBUS_TYPE_BOOLEAN, &value);

	/* Discoverable */
	value = adapter->scan_mode & SCAN_INQUIRY ? TRUE : FALSE;
	dict_append_entry(&dict, "Discoverable", DBUS_TYPE_BOOLEAN, &value);

	/* Pairable */
	dict_append_entry(&dict, "Pairable", DBUS_TYPE_BOOLEAN,
				&adapter->pairable);

	/* DiscoverableTimeout */
	dict_append_entry(&dict, "DiscoverableTimeout",
				DBUS_TYPE_UINT32, &adapter->discov_timeout);

	/* PairableTimeout */
	dict_append_entry(&dict, "PairableTimeout",
				DBUS_TYPE_UINT32, &adapter->pairable_timeout);


	if (adapter->state & PERIODIC_INQUIRY || adapter->state & STD_INQUIRY)
		value = TRUE;
	else
		value = FALSE;

	/* Discovering */
	dict_append_entry(&dict, "Discovering", DBUS_TYPE_BOOLEAN, &value);

	/* Devices */
	devices = g_new0(char *, g_slist_length(adapter->devices) + 1);
	for (i = 0, l = adapter->devices; l; l = l->next, i++) {
		struct btd_device *dev = l->data;
		devices[i] = (char *) device_get_path(dev);
	}
	dict_append_array(&dict, "Devices", DBUS_TYPE_OBJECT_PATH,
								&devices, i);
	g_free(devices);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *set_property(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct btd_adapter *adapter = data;
	DBusMessageIter iter;
	DBusMessageIter sub;
	const char *property;
	char srcaddr[18];

	ba2str(&adapter->bdaddr, srcaddr);

	if (!dbus_message_iter_init(msg, &iter))
		return invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return invalid_args(msg);
	dbus_message_iter_recurse(&iter, &sub);

	if (g_str_equal("Name", property)) {
		const char *name;

		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_STRING)
			return invalid_args(msg);
		dbus_message_iter_get_basic(&sub, &name);

		return set_name(conn, msg, name, data);
	} else if (g_str_equal("Powered", property)) {
		gboolean powered;

		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_BOOLEAN)
			return invalid_args(msg);

		dbus_message_iter_get_basic(&sub, &powered);

		return set_powered(conn, msg, powered, data);
	} else if (g_str_equal("Discoverable", property)) {
		gboolean discoverable;

		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_BOOLEAN)
			return invalid_args(msg);

		dbus_message_iter_get_basic(&sub, &discoverable);

		return set_discoverable(conn, msg, discoverable, data);
	} else if (g_str_equal("DiscoverableTimeout", property)) {
		uint32_t timeout;

		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_UINT32)
			return invalid_args(msg);

		dbus_message_iter_get_basic(&sub, &timeout);

		return set_discoverable_timeout(conn, msg, timeout, data);
	} else if (g_str_equal("Pairable", property)) {
		gboolean pairable;

		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_BOOLEAN)
			return invalid_args(msg);

		dbus_message_iter_get_basic(&sub, &pairable);

		return set_pairable(conn, msg, pairable, data);
	} else if (g_str_equal("PairableTimeout", property)) {
		uint32_t timeout;

		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_UINT32)
			return invalid_args(msg);

		dbus_message_iter_get_basic(&sub, &timeout);

		return set_pairable_timeout(conn, msg, timeout, data);
	}

	return invalid_args(msg);
}

static DBusMessage *request_session(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct btd_adapter *adapter = data;
	struct session_req *req;
	const char *sender = dbus_message_get_sender(msg);
	uint8_t new_mode;
	int ret;

	if (!adapter->agent)
		return g_dbus_create_error(msg, ERROR_INTERFACE ".Failed",
						"No agent registered");

	if (!adapter->mode_sessions)
		adapter->global_mode = adapter->mode;

	new_mode = get_mode(&adapter->bdaddr, "on");

	req = find_session(adapter->mode_sessions, sender);
	if (req) {
		session_ref(req);
		return dbus_message_new_method_return(msg);
	} else {
		req = create_session(adapter, conn, msg, new_mode,
					session_owner_exit);
		adapter->mode_sessions = g_slist_append(adapter->mode_sessions,
							req);
	}

	/* No need to change mode */
	if (adapter->mode >= new_mode)
		return dbus_message_new_method_return(msg);

	ret = agent_confirm_mode_change(adapter->agent, mode2str(new_mode),
					confirm_mode_cb, req);
	if (ret < 0) {
		session_unref(req);
		return failed_strerror(msg, -ret);
	}

	return NULL;
}

static DBusMessage *release_session(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct btd_adapter *adapter = data;
	struct session_req *req;
	const char *sender = dbus_message_get_sender(msg);

	req = find_session(adapter->mode_sessions, sender);
	if (!req)
		return g_dbus_create_error(msg, ERROR_INTERFACE ".Failed",
				"No Mode to release");

	session_unref(req);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *list_devices(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct btd_adapter *adapter = data;
	DBusMessage *reply;
	GSList *l;
	DBusMessageIter iter;
	DBusMessageIter array_iter;
	const gchar *dev_path;

	if (!dbus_message_has_signature(msg, DBUS_TYPE_INVALID_AS_STRING))
		return invalid_args(msg);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
				DBUS_TYPE_OBJECT_PATH_AS_STRING, &array_iter);

	for (l = adapter->devices; l; l = l->next) {
		struct btd_device *device = l->data;

		dev_path = device_get_path(device);

		dbus_message_iter_append_basic(&array_iter,
				DBUS_TYPE_OBJECT_PATH, &dev_path);
	}

	dbus_message_iter_close_container(&iter, &array_iter);

	return reply;
}

static DBusMessage *cancel_device_creation(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct btd_adapter *adapter = data;
	const gchar *address, *sender = dbus_message_get_sender(msg);
	struct btd_device *device;

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &address,
						DBUS_TYPE_INVALID) == FALSE)
		return invalid_args(msg);

	if (check_address(address) < 0)
		return invalid_args(msg);

	device = adapter_find_device(adapter, address);
	if (!device)
		return g_dbus_create_error(msg,
				ERROR_INTERFACE ".NotInProgress",
				"Device creation not in progress");

	if (!device_is_temporary(device) || !device_is_bonding(device, sender))
		return not_authorized(msg);

	adapter_remove_device(conn, adapter, device);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *create_device(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct btd_adapter *adapter = data;
	struct btd_device *device;
	const gchar *address;

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &address,
						DBUS_TYPE_INVALID) == FALSE)
		return invalid_args(msg);

	if (check_address(address) < 0)
		return invalid_args(msg);

	if (adapter_find_device(adapter, address))
		return g_dbus_create_error(msg,
				ERROR_INTERFACE ".AlreadyExists",
				"Device already exists");

	debug("create_device(%s)", address);

	device = adapter_create_device(conn, adapter, address);
	if (!device)
		return NULL;

	device_set_temporary(device, FALSE);

	device_browse(device, conn, msg, NULL, FALSE);

	return NULL;
}

static uint8_t parse_io_capability(const char *capability)
{
	if (g_str_equal(capability, ""))
		return IO_CAPABILITY_DISPLAYYESNO;
	if (g_str_equal(capability, "DisplayOnly"))
		return IO_CAPABILITY_DISPLAYONLY;
	if (g_str_equal(capability, "DisplayYesNo"))
		return IO_CAPABILITY_DISPLAYYESNO;
	if (g_str_equal(capability, "KeyboardOnly"))
		return IO_CAPABILITY_KEYBOARDONLY;
	if (g_str_equal(capability, "NoInputOutput"))
		return IO_CAPABILITY_NOINPUTOUTPUT;
	return IO_CAPABILITY_INVALID;
}

static DBusMessage *create_paired_device(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct btd_adapter *adapter = data;
	struct btd_device *device;
	const gchar *address, *agent_path, *capability, *sender;
	uint8_t cap;

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &address,
					DBUS_TYPE_OBJECT_PATH, &agent_path,
					DBUS_TYPE_STRING, &capability,
						DBUS_TYPE_INVALID) == FALSE)
		return invalid_args(msg);

	if (check_address(address) < 0)
		return invalid_args(msg);

	sender = dbus_message_get_sender(msg);
	if (adapter->agent &&
			agent_matches(adapter->agent, sender, agent_path)) {
		error("Refusing adapter agent usage as device specific one");
		return invalid_args(msg);
	}

	cap = parse_io_capability(capability);
	if (cap == IO_CAPABILITY_INVALID)
		return invalid_args(msg);

	device = adapter_get_device(conn, adapter, address);

	return device_create_bonding(device, conn, msg, agent_path, cap);
}

static gint device_path_cmp(struct btd_device *device, const gchar *path)
{
	const gchar *dev_path = device_get_path(device);

	return strcasecmp(dev_path, path);
}

static DBusMessage *remove_device(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct btd_adapter *adapter = data;
	struct btd_device *device;
	const char *path;
	GSList *l;

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
						DBUS_TYPE_INVALID) == FALSE)
		return invalid_args(msg);

	l = g_slist_find_custom(adapter->devices,
			path, (GCompareFunc) device_path_cmp);
	if (!l)
		return g_dbus_create_error(msg,
				ERROR_INTERFACE ".DoesNotExist",
				"Device does not exist");
	device = l->data;

	if (device_is_temporary(device) || device_is_busy(device))
		return g_dbus_create_error(msg,
				ERROR_INTERFACE ".DoesNotExist",
				"Device creation in progress");

	adapter_remove_device(conn, adapter, device);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *find_device(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct btd_adapter *adapter = data;
	struct btd_device *device;
	DBusMessage *reply;
	const gchar *address;
	GSList *l;
	const gchar *dev_path;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &address,
						DBUS_TYPE_INVALID))
		return invalid_args(msg);

	l = g_slist_find_custom(adapter->devices,
			address, (GCompareFunc) device_address_cmp);
	if (!l)
		return g_dbus_create_error(msg,
				ERROR_INTERFACE ".DoesNotExist",
				"Device does not exist");

	device = l->data;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dev_path = device_get_path(device);

	dbus_message_append_args(reply,
				DBUS_TYPE_OBJECT_PATH, &dev_path,
				DBUS_TYPE_INVALID);

	return reply;
}

static void agent_removed(struct agent *agent, struct btd_adapter *adapter)
{
	adapter->agent = NULL;
}

static DBusMessage *register_agent(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	const char *path, *name, *capability;
	struct agent *agent;
	struct btd_adapter *adapter = data;
	uint8_t cap;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
			DBUS_TYPE_STRING, &capability, DBUS_TYPE_INVALID))
		return NULL;

	if (adapter->agent)
		return g_dbus_create_error(msg,
				ERROR_INTERFACE ".AlreadyExists",
				"Agent already exists");

	cap = parse_io_capability(capability);
	if (cap == IO_CAPABILITY_INVALID)
		return invalid_args(msg);

	name = dbus_message_get_sender(msg);

	agent = agent_create(adapter, name, path, cap,
				(agent_remove_cb) agent_removed, adapter);
	if (!agent)
		return g_dbus_create_error(msg,
				ERROR_INTERFACE ".Failed",
				"Failed to create a new agent");

	adapter->agent = agent;

	debug("Agent registered for hci%d at %s:%s", adapter->dev_id, name,
			path);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *unregister_agent(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	const char *path, *name;
	struct btd_adapter *adapter = data;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
						DBUS_TYPE_INVALID))
		return NULL;

	name = dbus_message_get_sender(msg);

	if (!adapter->agent || !agent_matches(adapter->agent, name, path))
		return g_dbus_create_error(msg,
				ERROR_INTERFACE ".DoesNotExist",
				"No such agent");

	agent_destroy(adapter->agent, FALSE);
	adapter->agent = NULL;

	return dbus_message_new_method_return(msg);
}

static GDBusMethodTable adapter_methods[] = {
	{ "GetProperties",	"",	"a{sv}",get_properties		},
	{ "SetProperty",	"sv",	"",	set_property,
						G_DBUS_METHOD_FLAG_ASYNC},
	{ "RequestSession",	"",	"",	request_session,
						G_DBUS_METHOD_FLAG_ASYNC},
	{ "ReleaseSession",	"",	"",	release_session		},
	{ "StartDiscovery",	"",	"",	adapter_start_discovery },
	{ "StopDiscovery",	"",	"",	adapter_stop_discovery,
						G_DBUS_METHOD_FLAG_ASYNC},
	{ "ListDevices",	"",	"ao",	list_devices,
						G_DBUS_METHOD_FLAG_DEPRECATED},
	{ "CreateDevice",	"s",	"o",	create_device,
						G_DBUS_METHOD_FLAG_ASYNC},
	{ "CreatePairedDevice",	"sos",	"o",	create_paired_device,
						G_DBUS_METHOD_FLAG_ASYNC},
	{ "CancelDeviceCreation","s",	"",	cancel_device_creation	},
	{ "RemoveDevice",	"o",	"",	remove_device		},
	{ "FindDevice",		"s",	"o",	find_device		},
	{ "RegisterAgent",	"os",	"",	register_agent		},
	{ "UnregisterAgent",	"o",	"",	unregister_agent	},
	{ }
};

static GDBusSignalTable adapter_signals[] = {
	{ "PropertyChanged",		"sv"		},
	{ "DeviceCreated",		"o"		},
	{ "DeviceRemoved",		"o"		},
	{ "DeviceFound",		"sa{sv}"	},
	{ "DeviceDisappeared",		"s"		},
	{ }
};

static inline uint8_t get_inquiry_mode(struct hci_dev *dev)
{
	if (dev->features[6] & LMP_EXT_INQ)
		return 2;

	if (dev->features[3] & LMP_RSSI_INQ)
		return 1;

	if (dev->manufacturer == 11 &&
			dev->hci_rev == 0x00 && dev->lmp_subver == 0x0757)
		return 1;

	if (dev->manufacturer == 15) {
		if (dev->hci_rev == 0x03 && dev->lmp_subver == 0x6963)
			return 1;
		if (dev->hci_rev == 0x09 && dev->lmp_subver == 0x6963)
			return 1;
		if (dev->hci_rev == 0x00 && dev->lmp_subver == 0x6965)
			return 1;
	}

	if (dev->manufacturer == 31 &&
			dev->hci_rev == 0x2005 && dev->lmp_subver == 0x1805)
		return 1;

	return 0;
}

static int adapter_read_bdaddr(uint16_t dev_id, bdaddr_t *bdaddr)
{
	int dd, err;

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		err = -errno;
		error("Can't open device hci%d: %s (%d)",
					dev_id, strerror(err), err);
		return err;
	}

	if (hci_read_bd_addr(dd, bdaddr, HCI_REQ_TIMEOUT) < 0) {
		err = -errno;
		error("Can't read address for hci%d: %s (%d)",
					dev_id, strerror(err), err);
		hci_close_dev(dd);
		return err;
	}

	hci_close_dev(dd);

	return 0;
}

static int adapter_setup(struct btd_adapter *adapter, int dd)
{
	struct hci_dev *dev = &adapter->dev;
	uint8_t events[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0x1f, 0x00, 0x00 };
	uint8_t inqmode;
	int err;
	char name[249];

	if (dev->hci_rev > 1) {
		if (dev->features[5] & LMP_SNIFF_SUBR)
			events[5] |= 0x20;

		if (dev->features[5] & LMP_PAUSE_ENC)
			events[5] |= 0x80;

		if (dev->features[6] & LMP_EXT_INQ)
			events[5] |= 0x40;

		if (dev->features[6] & LMP_NFLUSH_PKTS)
			events[7] |= 0x01;

		if (dev->features[7] & LMP_LSTO)
			events[6] |= 0x80;

		if (dev->features[6] & LMP_SIMPLE_PAIR) {
			events[6] |= 0x01;	/* IO Capability Request */
			events[6] |= 0x02;	/* IO Capability Response */
			events[6] |= 0x04;	/* User Confirmation Request */
			events[6] |= 0x08;	/* User Passkey Request */
			events[6] |= 0x10;	/* Remote OOB Data Request */
			events[6] |= 0x20;	/* Simple Pairing Complete */
			events[7] |= 0x04;	/* User Passkey Notification */
			events[7] |= 0x08;	/* Keypress Notification */
			events[7] |= 0x10;	/* Remote Host Supported
						 * Features Notification */
		}

		hci_send_cmd(dd, OGF_HOST_CTL, OCF_SET_EVENT_MASK,
						sizeof(events), events);
	}

	if (read_local_name(&adapter->bdaddr, name) == 0) {
		memcpy(dev->name, name, 248);
		hci_write_local_name(dd, name, HCI_REQ_TIMEOUT);
	}

	update_ext_inquiry_response(dd, dev);

	inqmode = get_inquiry_mode(dev);
	if (inqmode < 1)
		return 0;

	if (hci_write_inquiry_mode(dd, inqmode, HCI_REQ_TIMEOUT) < 0) {
		err = -errno;
		error("Can't write inquiry mode for %s: %s (%d)",
					adapter->path, strerror(err), err);
		hci_close_dev(dd);
		return err;
	}

	return 0;
}

static void create_stored_device_from_profiles(char *key, char *value,
						void *user_data)
{
	struct btd_adapter *adapter = user_data;
	GSList *uuids = bt_string2list(value);
	struct btd_device *device;
	bdaddr_t dst;
	char srcaddr[18], dstaddr[18];

	ba2str(&adapter->bdaddr, srcaddr);

	if (g_slist_find_custom(adapter->devices,
				key, (GCompareFunc) device_address_cmp))
		return;

	device = device_create(connection, adapter, key);
	if (!device)
		return;

	device_set_temporary(device, FALSE);
	adapter->devices = g_slist_append(adapter->devices, device);

	device_get_address(device, &dst);
	ba2str(&dst, dstaddr);

	device_probe_drivers(device, uuids);

	g_slist_foreach(uuids, (GFunc) g_free, NULL);
	g_slist_free(uuids);
}

static void create_stored_device_from_linkkeys(char *key, char *value,
						void *user_data)
{
	struct btd_adapter *adapter = user_data;
	struct btd_device *device;

	if (g_slist_find_custom(adapter->devices,
				key, (GCompareFunc) device_address_cmp))
		return;

	device = device_create(connection, adapter, key);
	if (device) {
		device_set_temporary(device, FALSE);
		adapter->devices = g_slist_append(adapter->devices, device);
	}
}

static void load_devices(struct btd_adapter *adapter)
{
	char filename[PATH_MAX + 1];
	char srcaddr[18];

	ba2str(&adapter->bdaddr, srcaddr);

	create_name(filename, PATH_MAX, STORAGEDIR, srcaddr, "profiles");
	textfile_foreach(filename, create_stored_device_from_profiles,
								adapter);

	create_name(filename, PATH_MAX, STORAGEDIR, srcaddr, "linkkeys");
	textfile_foreach(filename, create_stored_device_from_linkkeys,
								adapter);
}

static void probe_driver(gpointer data, gpointer user_data)
{
	struct btd_adapter *adapter = data;
	struct btd_adapter_driver *driver = user_data;
	int err;

	if (!adapter->up)
		return;

	err = driver->probe(adapter);
	if (err < 0)
		error("%s: %s (%d)", driver->name, strerror(-err), -err);
}

static void load_drivers(struct btd_adapter *adapter)
{
	GSList *l;

	for (l = adapter_drivers; l; l = l->next) {
		struct btd_adapter_driver *driver = l->data;

		if (driver->probe == NULL)
			continue;

		probe_driver(adapter, driver);
	}
}

static void load_connections(struct btd_adapter *adapter, int dd)
{
	struct hci_conn_list_req *cl = NULL;
	struct hci_conn_info *ci;
	int i;

	cl = g_malloc0(10 * sizeof(*ci) + sizeof(*cl));

	cl->dev_id = adapter->dev_id;
	cl->conn_num = 10;
	ci = cl->conn_info;

	if (ioctl(dd, HCIGETCONNLIST, cl) != 0) {
		g_free(cl);
		return;
	}

	for (i = 0; i < cl->conn_num; i++, ci++) {
		struct btd_device *device;
		char address[18];

		ba2str(&ci->bdaddr, address);
		device = adapter_get_device(connection, adapter, address);
		if (device)
			adapter_add_connection(adapter, device, ci->handle);
	}

	g_free(cl);
}

static int get_discoverable_timeout(const char *src)
{
	int timeout;

	if (read_discoverable_timeout(src, &timeout) == 0)
		return timeout;

	return main_opts.discovto;
}

static int get_pairable_timeout(const char *src)
{
	int timeout;

	if (read_pairable_timeout(src, &timeout) == 0)
		return timeout;

	return main_opts.pairto;
}

static int adapter_up(struct btd_adapter *adapter, int dd)
{
	char mode[14], srcaddr[18];
	uint8_t scan_mode;
	gboolean powered, dev_down = FALSE;

	ba2str(&adapter->bdaddr, srcaddr);

	adapter->off_requested = FALSE;
	adapter->up = 1;
	adapter->discov_timeout = get_discoverable_timeout(srcaddr);
	adapter->pairable_timeout = get_pairable_timeout(srcaddr);
	adapter->state = DISCOVER_TYPE_NONE;
	adapter->mode = MODE_CONNECTABLE;
	scan_mode = SCAN_PAGE;
	powered = TRUE;

	/* Set pairable mode */
	if (read_device_pairable(&adapter->bdaddr, &adapter->pairable) < 0)
		adapter->pairable = TRUE;

	if (!adapter->initialized && !main_opts.remember_powered) {
		if (main_opts.mode == MODE_OFF)
			strcpy(mode, "off");
		else
			strcpy(mode, "connectable");
	} else if (read_device_mode(srcaddr, mode, sizeof(mode)) < 0) {
		if (!adapter->initialized && main_opts.mode == MODE_OFF)
			strcpy(mode, "off");
		else
			goto proceed;
	}

	if (g_str_equal(mode, "off")) {
		powered = FALSE;

		if (!adapter->initialized) {
			dev_down = TRUE;
			goto proceed;
		}

		if (read_on_mode(srcaddr, mode, sizeof(mode)) < 0)
			write_device_mode(&adapter->bdaddr, "connectable");
		else
			write_device_mode(&adapter->bdaddr, mode);

		return adapter_up(adapter, dd);
	} else if (!g_str_equal(mode, "connectable") &&
			adapter->discov_timeout == 0) {
		/* Set discoverable only if timeout is 0 */
		adapter->mode = adapter->pairable ?
					MODE_LIMITED : MODE_DISCOVERABLE;
		scan_mode = SCAN_PAGE | SCAN_INQUIRY;
	}

proceed:
	if (dev_down == FALSE) {
		hci_send_cmd(dd, OGF_HOST_CTL, OCF_WRITE_SCAN_ENABLE,
				1, &scan_mode);

		emit_property_changed(connection, adapter->path,
					ADAPTER_INTERFACE, "Powered",
					DBUS_TYPE_BOOLEAN, &powered);
	}

	if (adapter->initialized == FALSE) {
		load_drivers(adapter);
		load_devices(adapter);

		/* retrieve the active connections: address the scenario where
		 * the are active connections before the daemon've started */
		load_connections(adapter, dd);

		adapter->initialized = TRUE;

		adapter_update(adapter, 0, FALSE);
	}

	if (dev_down) {
		ioctl(dd, HCIDEVDOWN, adapter->dev_id);
		return 1;
	}

	return 0;
}

int adapter_start(struct btd_adapter *adapter)
{
	struct hci_dev *dev = &adapter->dev;
	struct hci_dev_info di;
	struct hci_version ver;
	uint8_t features[8];
	int dd, err;
	char name[249];

	if (hci_devinfo(adapter->dev_id, &di) < 0)
		return -errno;

	if (hci_test_bit(HCI_RAW, &di.flags)) {
		dev->ignore = 1;
		return -1;
	}

	if (!bacmp(&di.bdaddr, BDADDR_ANY)) {
		int err;

		debug("Adapter %s without an address", adapter->path);

		err = adapter_read_bdaddr(adapter->dev_id, &di.bdaddr);
		if (err < 0)
			return err;
	}

	bacpy(&adapter->bdaddr, &di.bdaddr);
	memcpy(dev->features, di.features, 8);

	dd = hci_open_dev(adapter->dev_id);
	if (dd < 0) {
		err = -errno;
		error("Can't open adapter %s: %s (%d)",
					adapter->path, strerror(err), err);
		return err;
	}

	if (hci_read_local_version(dd, &ver, HCI_REQ_TIMEOUT) < 0) {
		err = -errno;
		error("Can't read version info for %s: %s (%d)",
					adapter->path, strerror(err), err);
		hci_close_dev(dd);
		return err;
	}

	dev->hci_rev = ver.hci_rev;
	dev->lmp_ver = ver.lmp_ver;
	dev->lmp_subver = ver.lmp_subver;
	dev->manufacturer = ver.manufacturer;

	if (hci_read_local_features(dd, features, HCI_REQ_TIMEOUT) < 0) {
		err = -errno;
		error("Can't read features for %s: %s (%d)",
					adapter->path, strerror(err), err);
		hci_close_dev(dd);
		return err;
	}

	memcpy(dev->features, features, 8);

	if (hci_read_class_of_dev(dd, dev->class, HCI_REQ_TIMEOUT) < 0) {
		err = -errno;
		error("Can't read class of adapter on %s: %s (%d)",
					adapter->path, strerror(err), err);
		hci_close_dev(dd);
		return err;
	}

	if (hci_read_local_name(dd, sizeof(name), name,
						HCI_REQ_TIMEOUT) < 0) {
		err = -errno;
		error("Can't read local name on %s: %s (%d)",
					adapter->path, strerror(err), err);
		hci_close_dev(dd);
		return err;
	}

	memcpy(dev->name, name, 248);

	if (!(features[6] & LMP_SIMPLE_PAIR))
		goto setup;

	if (ioctl(dd, HCIGETAUTHINFO, NULL) < 0 && errno != EINVAL)
		hci_write_simple_pairing_mode(dd, 0x01, HCI_REQ_TIMEOUT);

	if (hci_read_simple_pairing_mode(dd, &dev->ssp_mode,
						HCI_REQ_TIMEOUT) < 0) {
		err = -errno;
		error("Can't read simple pairing mode on %s: %s (%d)",
					adapter->path, strerror(err), err);
		/* Fall through since some chips have broken
		 * read_simple_pairing_mode behavior */
	}

setup:
	hci_send_cmd(dd, OGF_LINK_POLICY, OCF_READ_DEFAULT_LINK_POLICY,
								0, NULL);

	if (hci_test_bit(HCI_INQUIRY, &di.flags)) {
		debug("inquiry_cancel at adapter startup");
		inquiry_cancel(dd, HCI_REQ_TIMEOUT);
	} else if (!adapter->initialized && adapter->already_up) {
		debug("periodic_inquiry_exit at adapter startup");
		periodic_inquiry_exit(dd, HCI_REQ_TIMEOUT);
	}

	adapter->state &= ~STD_INQUIRY;

	adapter_setup(adapter, dd);
	err = adapter_up(adapter, dd);

	hci_close_dev(dd);

	info("Adapter %s has been enabled", adapter->path);

	return err;
}

static void reply_pending_requests(struct btd_adapter *adapter)
{
	GSList *l;

	if (!adapter)
		return;

	/* pending bonding */
	for (l = adapter->devices; l; l = l->next) {
		struct btd_device *device = l->data;

		if (device_is_bonding(device, NULL))
			device_cancel_bonding(device,
						HCI_OE_USER_ENDED_CONNECTION);
	}

	if (adapter->state & STD_INQUIRY) {
		/* Cancel inquiry initiated by D-Bus client */
		if (adapter->disc_sessions)
			cancel_discovery(adapter);
	}

	if (adapter->state & PERIODIC_INQUIRY) {
		/* Stop periodic inquiry initiated by D-Bus client */
		if (adapter->disc_sessions)
			cancel_periodic_discovery(adapter);
	}
}

static void unload_drivers(struct btd_adapter *adapter)
{
	GSList *l;

	for (l = adapter_drivers; l; l = l->next) {
		struct btd_adapter_driver *driver = l->data;

		if (driver->remove)
			driver->remove(adapter);
	}
}

int adapter_stop(struct btd_adapter *adapter)
{
	gboolean powered, discoverable, pairable;

	/* cancel pending timeout */
	if (adapter->discov_timeout_id) {
		g_source_remove(adapter->discov_timeout_id);
		adapter->discov_timeout_id = 0;
	}

	/* check pending requests */
	reply_pending_requests(adapter);

	if (adapter->disc_sessions) {
		g_slist_foreach(adapter->disc_sessions, (GFunc) session_free,
				NULL);
		g_slist_free(adapter->disc_sessions);
		adapter->disc_sessions = NULL;
	}

	if (adapter->found_devices) {
		g_slist_foreach(adapter->found_devices,
				(GFunc) dev_info_free, NULL);
		g_slist_free(adapter->found_devices);
		adapter->found_devices = NULL;
	}

	if (adapter->oor_devices) {
		g_slist_foreach(adapter->oor_devices, (GFunc) free, NULL);
		g_slist_free(adapter->oor_devices);
		adapter->oor_devices = NULL;
	}

	if (adapter->connections) {
		g_slist_free(adapter->connections);
		adapter->connections = NULL;
	}

	if (adapter->scan_mode == (SCAN_PAGE | SCAN_INQUIRY)) {
		discoverable = FALSE;
		emit_property_changed(connection, adapter->path,
					ADAPTER_INTERFACE, "Discoverable",
					DBUS_TYPE_BOOLEAN, &discoverable);
	}

	if ((adapter->scan_mode & SCAN_PAGE) && adapter->pairable == TRUE) {
		pairable = FALSE;
		emit_property_changed(connection, adapter->path,
					ADAPTER_INTERFACE, "Pairable",
					DBUS_TYPE_BOOLEAN, &pairable);
	}

	powered = FALSE;
	emit_property_changed(connection, adapter->path, ADAPTER_INTERFACE,
				"Powered", DBUS_TYPE_BOOLEAN, &powered);

	adapter->up = 0;
	adapter->scan_mode = SCAN_DISABLED;
	adapter->mode = MODE_OFF;
	adapter->state = DISCOVER_TYPE_NONE;

	info("Adapter %s has been disabled", adapter->path);

	return 0;
}

int adapter_update(struct btd_adapter *adapter, uint8_t new_svc,
							gboolean starting)
{
	struct hci_dev *dev = &adapter->dev;
	int dd;
	uint8_t svclass;

	if (dev->ignore)
		return 0;

	if (starting || !adapter->initialized) {
		adapter->svc_cache = new_svc;
		return 0;
	}

	if (new_svc)
		svclass = new_svc;
	else
		svclass = adapter->svc_cache;

	dd = hci_open_dev(adapter->dev_id);
	if (dd < 0) {
		int err = -errno;
		error("Can't open adapter %s: %s (%d)",
					adapter->path, strerror(err), err);
		return err;
	}

	if (svclass)
		set_service_classes(dd, adapter->dev.class, svclass);

	update_ext_inquiry_response(dd, dev);

	hci_close_dev(dd);

	return 0;
}

int adapter_get_class(struct btd_adapter *adapter, uint8_t *cls)
{
	struct hci_dev *dev = &adapter->dev;

	memcpy(cls, dev->class, 3);

	return 0;
}

int adapter_set_class(struct btd_adapter *adapter, uint8_t *cls)
{
	struct hci_dev *dev = &adapter->dev;
	uint32_t class;

	if (adapter->svc_cache)
		adapter->svc_cache = 0;

	if (memcmp(dev->class, cls, 3) == 0)
		return 0;

	memcpy(dev->class, cls, 3);

	write_local_class(&adapter->bdaddr, cls);

	class = cls[0] | (cls[1] << 8) | (cls[2] << 16);

	emit_property_changed(connection, adapter->path, ADAPTER_INTERFACE,
				"Class", DBUS_TYPE_UINT32, &class);

	return 0;
}

int adapter_update_ssp_mode(struct btd_adapter *adapter, int dd, uint8_t mode)
{
	struct hci_dev *dev = &adapter->dev;

	dev->ssp_mode = mode;

	update_ext_inquiry_response(dd, dev);

	hci_close_dev(dd);

	return 0;
}

static void adapter_free(gpointer user_data)
{
	struct btd_adapter *adapter = user_data;

	agent_destroy(adapter->agent, FALSE);
	adapter->agent = NULL;

	g_free(adapter->path);
	g_free(adapter);

	return;
}

struct btd_adapter *adapter_create(DBusConnection *conn, int id,
				gboolean devup)
{
	char path[MAX_PATH_LENGTH];
	struct btd_adapter *adapter;
	const char *base_path = manager_get_base_path();

	if (!connection)
		connection = conn;

	snprintf(path, sizeof(path), "%s/hci%d", base_path, id);

	adapter = g_try_new0(struct btd_adapter, 1);
	if (!adapter) {
		error("adapter_create: failed to alloc memory for %s", path);
		return NULL;
	}

	adapter->dev_id = id;
	adapter->state |= RESOLVE_NAME;
	adapter->path = g_strdup(path);
	adapter->already_up = devup;

	if (!g_dbus_register_interface(conn, path, ADAPTER_INTERFACE,
			adapter_methods, adapter_signals, NULL,
			adapter, adapter_free)) {
		error("Adapter interface init failed on path %s", path);
		adapter_free(adapter);
		return NULL;
	}

	return adapter;
}

void adapter_remove(struct btd_adapter *adapter)
{
	GSList *l;
	char *path = g_strdup(adapter->path);

	debug("Removing adapter %s", path);

	unload_drivers(adapter);

	for (l = adapter->devices; l; l = l->next)
		device_remove(l->data, connection, FALSE);
	g_slist_free(adapter->devices);

	/* Return adapter to down state if it was not up on init */
	if (adapter->up && !adapter->already_up) {
		int dd = hci_open_dev(adapter->dev_id);
		if (dd < 0)
			goto done;

		ioctl(dd, HCIDEVDOWN, adapter->dev_id);
		hci_close_dev(dd);
	}

done:
	g_dbus_unregister_interface(connection, path, ADAPTER_INTERFACE);
	g_free(path);
}

uint16_t adapter_get_dev_id(struct btd_adapter *adapter)
{
	return adapter->dev_id;
}

const gchar *adapter_get_path(struct btd_adapter *adapter)
{
	if (!adapter)
		return NULL;

	return adapter->path;
}

void adapter_get_address(struct btd_adapter *adapter, bdaddr_t *bdaddr)
{
	bacpy(bdaddr, &adapter->bdaddr);
}

void adapter_set_state(struct btd_adapter *adapter, int state)
{
	gboolean discov_active = FALSE;
	const char *path = adapter->path;

	if (adapter->state == state)
		return;

	if (state & PERIODIC_INQUIRY || state & STD_INQUIRY)
		discov_active = TRUE;
	else if (adapter->disc_sessions && main_opts.inqmode)
		adapter->scheduler_id = g_timeout_add_seconds(main_opts.inqmode,
						(GSourceFunc) start_inquiry,
						adapter);

	/* Send out of range */
	if (!discov_active)
		adapter_update_oor_devices(adapter);

	emit_property_changed(connection, path,
				ADAPTER_INTERFACE, "Discovering",
				DBUS_TYPE_BOOLEAN, &discov_active);

	adapter->state = state;
}

int adapter_get_state(struct btd_adapter *adapter)
{
	return adapter->state;
}

struct remote_dev_info *adapter_search_found_devices(struct btd_adapter *adapter,
						struct remote_dev_info *match)
{
	GSList *l;

	l = g_slist_find_custom(adapter->found_devices, match,
					(GCompareFunc) found_device_cmp);
	if (l)
		return l->data;

	return NULL;
}

static int dev_rssi_cmp(struct remote_dev_info *d1, struct remote_dev_info *d2)
{
	int rssi1, rssi2;

	rssi1 = d1->rssi < 0 ? -d1->rssi : d1->rssi;
	rssi2 = d2->rssi < 0 ? -d2->rssi : d2->rssi;

	return rssi1 - rssi2;
}

int adapter_add_found_device(struct btd_adapter *adapter, bdaddr_t *bdaddr,
				int8_t rssi, uint32_t class, const char *alias,
				name_status_t name_status)
{
	struct remote_dev_info *dev, match;

	memset(&match, 0, sizeof(struct remote_dev_info));
	bacpy(&match.bdaddr, bdaddr);
	match.name_status = NAME_ANY;

	/* ignore repeated entries */
	dev = adapter_search_found_devices(adapter, &match);
	if (dev) {
		/* device found, update the attributes */
		if (rssi != 0)
			dev->rssi = rssi;

		dev->class = class;

		if (alias) {
			g_free(dev->alias);
			dev->alias = g_strdup(alias);
		}

		/* Get remote name can be received while inquiring.  Keep in
		 * mind that multiple inquiry result events can be received
		 * from the same remote device.
		 */
		if (name_status != NAME_NOT_REQUIRED)
			dev->name_status = name_status;

		adapter->found_devices = g_slist_sort(adapter->found_devices,
						(GCompareFunc) dev_rssi_cmp);

		return -EALREADY;
	}

	dev = g_new0(struct remote_dev_info, 1);

	bacpy(&dev->bdaddr, bdaddr);
	dev->rssi = rssi;
	dev->class = class;
	if (alias)
		dev->alias = g_strdup(alias);
	dev->name_status = name_status;

	adapter->found_devices = g_slist_insert_sorted(adapter->found_devices,
						dev, (GCompareFunc) dev_rssi_cmp);

	return 0;
}

int adapter_remove_found_device(struct btd_adapter *adapter, bdaddr_t *bdaddr)
{
	struct remote_dev_info *dev, match;

	memset(&match, 0, sizeof(struct remote_dev_info));
	bacpy(&match.bdaddr, bdaddr);

	dev = adapter_search_found_devices(adapter, &match);
	if (!dev)
		return -1;

	dev->name_status = NAME_NOT_REQUIRED;

	return 0;
}

void adapter_update_oor_devices(struct btd_adapter *adapter)
{
	GSList *l;
	struct remote_dev_info *dev;
	bdaddr_t tmp;

	for (l = adapter->oor_devices; l; l = l->next) {
		char *address = l->data;
		struct remote_dev_info match;

		memset(&match, 0, sizeof(struct remote_dev_info));
		str2ba(address, &match.bdaddr);

		g_dbus_emit_signal(connection, adapter->path,
				ADAPTER_INTERFACE, "DeviceDisappeared",
				DBUS_TYPE_STRING, &address,
				DBUS_TYPE_INVALID);

		dev = adapter_search_found_devices(adapter, &match);
		if (!dev)
			continue;

		adapter->found_devices = g_slist_remove(adapter->found_devices, dev);
		dev_info_free(dev);
	}

	g_slist_foreach(adapter->oor_devices, (GFunc) free, NULL);
	g_slist_free(adapter->oor_devices);
	adapter->oor_devices = NULL;

	for (l = adapter->found_devices; l; l = l->next) {
		dev = l->data;
		baswap(&tmp, &dev->bdaddr);
		adapter->oor_devices = g_slist_append(adapter->oor_devices,
							batostr(&tmp));
	}
}

void adapter_remove_oor_device(struct btd_adapter *adapter, char *peer_addr)
{
	GSList *l;

	l = g_slist_find_custom(adapter->oor_devices, peer_addr,
				(GCompareFunc) strcmp);
	if (l) {
		char *dev = l->data;
		adapter->oor_devices = g_slist_remove(adapter->oor_devices,
								dev);
		g_free(dev);
	}
}

void adapter_mode_changed(struct btd_adapter *adapter, uint8_t scan_mode)
{
	const gchar *path = adapter_get_path(adapter);
	gboolean powered, discoverable, pairable;
	uint8_t real_class[3];
	int dd;

	if (adapter->scan_mode == scan_mode)
		return;

	adapter_remove_discov_timeout(adapter);

	switch (scan_mode) {
	case SCAN_DISABLED:
		adapter->mode = MODE_OFF;
		powered = FALSE;
		discoverable = FALSE;
		pairable = FALSE;
		break;
	case SCAN_PAGE:
		adapter->mode = MODE_CONNECTABLE;
		powered = TRUE;
		discoverable = FALSE;
		pairable = adapter->pairable;
		break;
	case (SCAN_PAGE | SCAN_INQUIRY):
		adapter->mode = MODE_DISCOVERABLE;
		powered = TRUE;
		discoverable = TRUE;
		pairable = adapter->pairable;
		if (adapter->discov_timeout != 0)
			adapter_set_discov_timeout(adapter,
						adapter->discov_timeout);
		break;
	case SCAN_INQUIRY:
		/* Address the scenario where a low-level application like
		 * hciconfig changed the scan mode */
		if (adapter->discov_timeout != 0)
			adapter_set_discov_timeout(adapter,
						adapter->discov_timeout);

		/* ignore, this event should not be sent */
	default:
		/* ignore, reserved */
		return;
	}

	if (powered == FALSE)
		emit_property_changed(connection, path,
					ADAPTER_INTERFACE, "Powered",
					DBUS_TYPE_BOOLEAN, &powered);

	/* If page scanning gets toggled emit the Pairable property */
	if ((adapter->scan_mode & SCAN_PAGE) != (scan_mode & SCAN_PAGE))
		emit_property_changed(connection, adapter->path,
					ADAPTER_INTERFACE, "Pairable",
					DBUS_TYPE_BOOLEAN, &pairable);

	dd = hci_open_dev(adapter->dev_id);
	if (dd < 0) {
		error("HCI device open failed: hci%d", adapter->dev_id);
		goto done;
	}

	memcpy(real_class, adapter->dev.class, 3);
	if (adapter->svc_cache)
		real_class[2] = adapter->svc_cache;

	if (discoverable && adapter->pairable)
		set_limited_discoverable(dd, real_class, TRUE);
	else if (!discoverable)
		set_limited_discoverable(dd, real_class, FALSE);

	hci_close_dev(dd);
done:
	emit_property_changed(connection, path,
				ADAPTER_INTERFACE, "Discoverable",
				DBUS_TYPE_BOOLEAN, &discoverable);

	adapter->scan_mode = scan_mode;
}

struct agent *adapter_get_agent(struct btd_adapter *adapter)
{
	if (!adapter || !adapter->agent)
		return NULL;

	return adapter->agent;
}

void adapter_add_connection(struct btd_adapter *adapter,
				struct btd_device *device, uint16_t handle)
{
	if (g_slist_find(adapter->connections, device)) {
		error("Unable to add connection %d", handle);
		return;
	}

	device_add_connection(device, connection, handle);

	adapter->connections = g_slist_append(adapter->connections, device);
}

void adapter_remove_connection(struct btd_adapter *adapter,
				struct btd_device *device, uint16_t handle)
{
	bdaddr_t bdaddr;

	if (!g_slist_find(adapter->connections, device)) {
		error("No matching connection for handle %u", handle);
		return;
	}

	device_remove_connection(device, connection, handle);

	adapter->connections = g_slist_remove(adapter->connections, device);

	/* clean pending HCI cmds */
	device_get_address(device, &bdaddr);
	hci_req_queue_remove(adapter->dev_id, &bdaddr);

	if (device_is_authenticating(device))
		device_cancel_authentication(device, TRUE);

	if (device_is_temporary(device)) {
		const char *path = device_get_path(device);

		debug("Removing temporary device %s", path);
		adapter_remove_device(connection, adapter, device);
	}
}

gboolean adapter_has_discov_sessions(struct btd_adapter *adapter)
{
	if (!adapter || !adapter->disc_sessions)
		return FALSE;

	return TRUE;
}

int btd_register_adapter_driver(struct btd_adapter_driver *driver)
{
	GSList *adapters;

	adapter_drivers = g_slist_append(adapter_drivers, driver);

	if (driver->probe == NULL)
		return 0;

	adapters = manager_get_adapters();
	g_slist_foreach(adapters, probe_driver, driver);

	return 0;
}

void btd_unregister_adapter_driver(struct btd_adapter_driver *driver)
{
	adapter_drivers = g_slist_remove(adapter_drivers, driver);
}

static void agent_auth_cb(struct agent *agent, DBusError *derr,
							void *user_data)
{
	struct service_auth *auth = user_data;

	auth->cb(derr, auth->user_data);

	g_free(auth);
}

static int btd_adapter_authorize(struct btd_adapter *adapter,
					const bdaddr_t *dst,
					const char *uuid,
					service_auth_cb cb, void *user_data)
{
	struct service_auth *auth;
	struct btd_device *device;
	struct agent *agent;
	char address[18];
	gboolean trusted;
	const gchar *dev_path;

	ba2str(dst, address);
	device = adapter_find_device(adapter, address);
	if (!device)
		return -EPERM;

	/* Device connected? */
	if (!g_slist_find(adapter->connections, device))
		return -ENOTCONN;

	trusted = read_trust(&adapter->bdaddr, address, GLOBAL_TRUST);

	if (trusted) {
		cb(NULL, user_data);
		return 0;
	}

	device = adapter_find_device(adapter, address);
	if (!device)
		return -EPERM;

	agent = device_get_agent(device);

	if (!agent)
		agent =  adapter->agent;

	if (!agent)
		return -EPERM;

	auth = g_try_new0(struct service_auth, 1);
	if (!auth)
		return -ENOMEM;

	auth->cb = cb;
	auth->user_data = user_data;

	dev_path = device_get_path(device);

	return agent_authorize(agent, dev_path, uuid, agent_auth_cb, auth);
}

int btd_request_authorization(const bdaddr_t *src, const bdaddr_t *dst,
		const char *uuid, service_auth_cb cb, void *user_data)
{
	struct btd_adapter *adapter;
	GSList *adapters;

	if (src == NULL || dst == NULL)
		return -EINVAL;

	if (bacmp(src, BDADDR_ANY) != 0)
		goto proceed;

	/* Handle request authorization for ANY adapter */
	adapters = manager_get_adapters();

	for (; adapters; adapters = adapters->next) {
		int err;
		adapter = adapters->data;

		err = btd_adapter_authorize(adapter, dst, uuid, cb, user_data);
		if (err == 0)
			return 0;
	}

	return -EPERM;

proceed:
	adapter = manager_find_adapter(src);
	if (!adapter)
		return -EPERM;

	return btd_adapter_authorize(adapter, dst, uuid, cb, user_data);
}

int btd_cancel_authorization(const bdaddr_t *src, const bdaddr_t *dst)
{
	struct btd_adapter *adapter = manager_find_adapter(src);
	struct btd_device *device;
	struct agent *agent;
	char address[18];

	if (!adapter)
		return -EPERM;

	ba2str(dst, address);
	device = adapter_find_device(adapter, address);
	if (!device)
		return -EPERM;

	/*
	 * FIXME: Cancel fails if authorization is requested to adapter's
	 * agent and in the meanwhile CreatePairedDevice is called.
	 */

	agent = device_get_agent(device);

	if (!agent)
		agent =  adapter->agent;

	if (!agent)
		return -EPERM;

	return agent_cancel(agent);
}

static gchar *adapter_any_path = NULL;
static int adapter_any_refcount = 0;

const char *adapter_any_get_path(void)
{
	return adapter_any_path;
}

const char *btd_adapter_any_request_path(void)
{
	if (adapter_any_refcount > 0)
		return adapter_any_path;

	adapter_any_path = g_strdup_printf("%s/any", manager_get_base_path());
	adapter_any_refcount++;

	return adapter_any_path;
}

void btd_adapter_any_release_path(void)
{
	adapter_any_refcount--;

	if (adapter_any_refcount > 0)
		return;

	g_free(adapter_any_path);
	adapter_any_path = NULL;
}

gboolean adapter_is_pairable(struct btd_adapter *adapter)
{
	return adapter->pairable;
}

gboolean adapter_powering_down(struct btd_adapter *adapter)
{
	return adapter->off_requested;
}
