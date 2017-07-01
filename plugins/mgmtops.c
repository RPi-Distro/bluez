/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2010  Nokia Corporation
 *  Copyright (C) 2010  Marcel Holtmann <marcel@holtmann.org>
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

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include <glib.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/mgmt.h>

#include "plugin.h"
#include "log.h"
#include "adapter.h"
#include "manager.h"
#include "device.h"
#include "event.h"

#define MGMT_BUF_SIZE 1024

static int max_index = -1;
static struct controller_info {
	gboolean valid;
	gboolean notified;
	uint8_t type;
	bdaddr_t bdaddr;
	uint8_t features[8];
	uint8_t dev_class[3];
	uint16_t manufacturer;
	uint8_t hci_ver;
	uint16_t hci_rev;
	gboolean enabled;
	gboolean discoverable;
	gboolean pairable;
	uint8_t sec_mode;
} *controllers = NULL;

static int mgmt_sock = -1;
static guint mgmt_watch = 0;

static uint8_t mgmt_version = 0;
static uint16_t mgmt_revision = 0;

static void read_version_complete(int sk, void *buf, size_t len)
{
	struct mgmt_hdr hdr;
	struct mgmt_rp_read_version *rp = buf;

	if (len < sizeof(*rp)) {
		error("Too small read version complete event");
		return;
	}

	mgmt_revision = btohs(bt_get_unaligned(&rp->revision));
	mgmt_version = rp->version;

	DBG("version %u revision %u", mgmt_version, mgmt_revision);

	memset(&hdr, 0, sizeof(hdr));
	hdr.opcode = MGMT_OP_READ_INDEX_LIST;
	if (write(sk, &hdr, sizeof(hdr)) < 0)
		error("Unable to read controller index list: %s (%d)",
						strerror(errno), errno);
}

static void add_controller(uint16_t index)
{
	if (index > max_index) {
		size_t size = sizeof(struct controller_info) * (index + 1);
		max_index = index;
		controllers = g_realloc(controllers, size);
	}

	memset(&controllers[index], 0, sizeof(struct controller_info));

	controllers[index].valid = TRUE;

	DBG("Added controller %u", index);
}

static void read_info(int sk, uint16_t index)
{
	char buf[MGMT_HDR_SIZE + sizeof(struct mgmt_cp_read_info)];
	struct mgmt_hdr *hdr = (void *) buf;
	struct mgmt_cp_read_info *cp = (void *) &buf[sizeof(*hdr)];

	memset(buf, 0, sizeof(buf));
	hdr->opcode = MGMT_OP_READ_INFO;
	hdr->len = htobs(sizeof(*cp));

	cp->index = htobs(index);

	if (write(sk, buf, sizeof(buf)) < 0)
		error("Unable to send read_info command: %s (%d)",
						strerror(errno), errno);
}

static void mgmt_index_added(int sk, void *buf, size_t len)
{
	struct mgmt_ev_index_added *ev = buf;
	uint16_t index;

	if (len < sizeof(*ev)) {
		error("Too small index added event");
		return;
	}

	index = btohs(bt_get_unaligned(&ev->index));

	add_controller(index);
	read_info(sk, index);
}

static void remove_controller(uint16_t index)
{
	if (index > max_index)
		return;

	if (!controllers[index].valid)
		return;

	btd_manager_unregister_adapter(index);

	memset(&controllers[index], 0, sizeof(struct controller_info));

	DBG("Removed controller %u", index);
}

static void mgmt_index_removed(int sk, void *buf, size_t len)
{
	struct mgmt_ev_index_removed *ev = buf;
	uint16_t index;

	if (len < sizeof(*ev)) {
		error("Too small index removed event");
		return;
	}

	index = btohs(bt_get_unaligned(&ev->index));

	remove_controller(index);
}

static void mgmt_powered(int sk, void *buf, size_t len)
{
	struct btd_adapter *adapter;
	struct mgmt_ev_powered *ev = buf;
	uint16_t index;

	if (len < sizeof(*ev)) {
		error("Too small powered event");
		return;
	}

	index = btohs(bt_get_unaligned(&ev->index));

	if (index > max_index) {
		DBG("Ignoring powered event for unknown controller %u", index);
		return;
	}

	controllers[index].enabled = ev->powered;

	DBG("Controller %u powered %s", index, ev->powered ? "on" : "off");

	adapter = manager_find_adapter(&controllers[index].bdaddr);
	if (adapter == NULL) {
		DBG("Adapter not found");
		return;
	}

	if (ev->powered)
		btd_adapter_start(adapter);
	else
		btd_adapter_stop(adapter);
}

static void read_index_list_complete(int sk, void *buf, size_t len)
{
	struct mgmt_rp_read_index_list *rp = buf;
	uint16_t num;
	int i;

	if (len < sizeof(*rp)) {
		error("Too small read index list complete event");
		return;
	}

	num = btohs(bt_get_unaligned(&rp->num_controllers));

	if (num * sizeof(uint16_t) + sizeof(*rp) != len) {
		error("Incorrect packet size for index list event");
		return;
	}

	for (i = 0; i < num; i++) {
		uint16_t index;

		index = btohs(bt_get_unaligned(&rp->index[i]));

		add_controller(index);
		read_info(sk, index);
	}
}

static int mgmt_set_discoverable(int index, gboolean discoverable)
{
	DBG("index %d discoverable %d", index, discoverable);
	return -ENOSYS;
}

static int mgmt_set_pairable(int index, gboolean pairable)
{
	DBG("index %d pairable %d", index, pairable);
	return -ENOSYS;
}

static int mgmt_set_powered(int index, gboolean powered)
{
	char buf[MGMT_HDR_SIZE + sizeof(struct mgmt_cp_set_powered)];
	struct mgmt_hdr *hdr = (void *) buf;
	struct mgmt_cp_set_powered *cp = (void *) &buf[sizeof(*hdr)];

	DBG("index %d powered %d", index, powered);

	memset(buf, 0, sizeof(buf));
	hdr->opcode = MGMT_OP_SET_POWERED;
	hdr->len = htobs(sizeof(*cp));

	cp->index = htobs(index);
	cp->powered = powered;

	if (write(mgmt_sock, buf, sizeof(buf)) < 0)
		return -errno;

	return 0;
}

static void read_info_complete(int sk, void *buf, size_t len)
{
	struct mgmt_rp_read_info *rp = buf;
	struct controller_info *info;
	struct btd_adapter *adapter;
	uint8_t mode;
	gboolean pairable, discoverable;
	uint16_t index;
	char addr[18];

	if (len < sizeof(*rp)) {
		error("Too small read info complete event");
		return;
	}

	index = btohs(bt_get_unaligned(&rp->index));
	if (index > max_index) {
		error("Unexpected index %u in read info complete", index);
		return;
	}

	info = &controllers[index];
	info->type = rp->type;
	info->enabled = rp->powered;
	info->discoverable = rp->discoverable;
	info->pairable = rp->pairable;
	info->sec_mode = rp->sec_mode;
	bacpy(&info->bdaddr, &rp->bdaddr);
	memcpy(info->dev_class, rp->dev_class, 3);
	memcpy(info->features, rp->features, 8);
	info->manufacturer = btohs(bt_get_unaligned(&rp->manufacturer));
	info->hci_ver = rp->hci_ver;
	info->hci_rev = btohs(bt_get_unaligned(&rp->hci_rev));

	ba2str(&info->bdaddr, addr);
	DBG("hci%u type %u addr %s", index, info->type, addr);
	DBG("hci%u class 0x%02x%02x%02x", index,
		info->dev_class[2], info->dev_class[1], info->dev_class[0]);
	DBG("hci%u manufacturer %d HCI ver %d:%d", index, info->manufacturer,
						info->hci_ver, info->hci_rev);
	DBG("hci%u enabled %u discoverable %u pairable %u sec_mode %u", index,
					info->enabled, info->discoverable,
					info->pairable, info->sec_mode);

	adapter = btd_manager_register_adapter(index);
	if (adapter == NULL) {
		error("mgmtops: unable to register adapter");
		return;
	}

	btd_adapter_get_mode(adapter, &mode, NULL, &pairable);
	if (mode == MODE_OFF) {
		mgmt_set_powered(index, FALSE);
		return;
	}

	discoverable = (mode == MODE_DISCOVERABLE);

	if (info->discoverable != discoverable)
		mgmt_set_discoverable(index, discoverable);

	if (info->pairable != pairable)
		mgmt_set_pairable(index, pairable);

	if (info->enabled)
		btd_adapter_start(adapter);

	btd_adapter_unref(adapter);
}

static void mgmt_cmd_complete(int sk, void *buf, size_t len)
{
	struct mgmt_ev_cmd_complete *ev = buf;
	uint16_t opcode;

	DBG("");

	if (len < sizeof(*ev)) {
		error("Too small management command complete event packet");
		return;
	}

	opcode = btohs(bt_get_unaligned(&ev->opcode));

	switch (opcode) {
	case MGMT_OP_READ_VERSION:
		read_version_complete(sk, ev->data, len - sizeof(*ev));
		break;
	case MGMT_OP_READ_INDEX_LIST:
		read_index_list_complete(sk, ev->data, len - sizeof(*ev));
		break;
	case MGMT_OP_READ_INFO:
		read_info_complete(sk, ev->data, len - sizeof(*ev));
		break;
	default:
		error("Unknown command complete for opcode %u", opcode);
		break;
	}
}

static void mgmt_cmd_status(int sk, void *buf, size_t len)
{
	struct mgmt_ev_cmd_status *ev = buf;
	uint16_t opcode;

	if (len < sizeof(*ev)) {
		error("Too small management command status event packet");
		return;
	}

	opcode = btohs(bt_get_unaligned(&ev->opcode));

	DBG("status %u opcode %u", ev->status, opcode);
}

static void mgmt_controller_error(int sk, void *buf, size_t len)
{
	struct mgmt_ev_controller_error *ev = buf;
	uint16_t index;

	if (len < sizeof(*ev)) {
		error("Too small management controller error event packet");
		return;
	}

	index = btohs(bt_get_unaligned(&ev->index));

	DBG("index %u error_code %u", index, ev->error_code);
}

static gboolean mgmt_event(GIOChannel *io, GIOCondition cond, gpointer user_data)
{
	char buf[MGMT_BUF_SIZE];
	struct mgmt_hdr *hdr = (void *) buf;
	int sk;
	ssize_t ret;
	uint16_t len, opcode;

	DBG("cond %d", cond);

	if (cond & G_IO_NVAL)
		return FALSE;

	sk = g_io_channel_unix_get_fd(io);

	if (cond & (G_IO_ERR | G_IO_HUP)) {
		error("Error on management socket");
		return FALSE;
	}

	ret = read(sk, buf, sizeof(buf));
	if (ret < 0) {
		error("Unable to read from management socket: %s (%d)",
						strerror(errno), errno);
		return TRUE;
	}

	DBG("Received %zd bytes from management socket", ret);

	if (ret < MGMT_HDR_SIZE) {
		error("Too small Management packet");
		return TRUE;
	}

	opcode = btohs(bt_get_unaligned(&hdr->opcode));
	len = btohs(bt_get_unaligned(&hdr->len));

	if (ret != MGMT_HDR_SIZE + len) {
		error("Packet length mismatch. ret %zd len %u", ret, len);
		return TRUE;
	}

	switch (opcode) {
	case MGMT_EV_CMD_COMPLETE:
		mgmt_cmd_complete(sk, buf + MGMT_HDR_SIZE, len);
		break;
	case MGMT_EV_CMD_STATUS:
		mgmt_cmd_status(sk, buf + MGMT_HDR_SIZE, len);
		break;
	case MGMT_EV_CONTROLLER_ERROR:
		mgmt_controller_error(sk, buf + MGMT_HDR_SIZE, len);
		break;
	case MGMT_EV_INDEX_ADDED:
		mgmt_index_added(sk, buf + MGMT_HDR_SIZE, len);
		break;
	case MGMT_EV_INDEX_REMOVED:
		mgmt_index_removed(sk, buf + MGMT_HDR_SIZE, len);
		break;
	case MGMT_EV_POWERED:
		mgmt_powered(sk, buf + MGMT_HDR_SIZE, len);
		break;
	default:
		error("Unknown Management opcode %u", opcode);
		break;
	}

	return TRUE;
}

static int mgmt_setup(void)
{
	struct mgmt_hdr hdr;
	struct sockaddr_hci addr;
	GIOChannel *io;
	GIOCondition condition;
	int dd, err;

	dd = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
	if (dd < 0)
		return -errno;

	memset(&addr, 0, sizeof(addr));
	addr.hci_family = AF_BLUETOOTH;
	addr.hci_dev = HCI_DEV_NONE;
	addr.hci_channel = HCI_CHANNEL_CONTROL;

	if (bind(dd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		err = -errno;
		goto fail;
	}

	memset(&hdr, 0, sizeof(hdr));
	hdr.opcode = MGMT_OP_READ_VERSION;
	if (write(dd, &hdr, sizeof(hdr)) < 0) {
		err = -errno;
		goto fail;
	}

	io = g_io_channel_unix_new(dd);
	condition = G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL;
	mgmt_watch = g_io_add_watch(io, condition, mgmt_event, NULL);
	g_io_channel_unref(io);

	mgmt_sock = dd;

	info("Bluetooth Management interface initialized");

	return 0;

fail:
	close(dd);
	return err;
}

static void mgmt_cleanup(void)
{
	g_free(controllers);
	controllers = NULL;
	max_index = -1;

	if (mgmt_sock >= 0) {
		close(mgmt_sock);
		mgmt_sock = -1;
	}

	if (mgmt_watch > 0) {
		g_source_remove(mgmt_watch);
		mgmt_watch = 0;
	}
}

static int mgmt_set_connectable(int index, gboolean connectable)
{
	DBG("index %d connectable %d", index, connectable);
	return -ENOSYS;
}

static int mgmt_set_dev_class(int index, uint8_t major, uint8_t minor)
{
	DBG("index %d major %u minor %u", index, major, minor);
	return -ENOSYS;
}

static int mgmt_set_limited_discoverable(int index, gboolean limited)
{
	DBG("index %d limited %d", index, limited);
	return -ENOSYS;
}

static int mgmt_start_inquiry(int index, uint8_t length, gboolean periodic)
{
	DBG("index %d length %u periodic %d", index, length, periodic);
	return -ENOSYS;
}

static int mgmt_stop_inquiry(int index)
{
	DBG("index %d", index);
	return -ENOSYS;
}

static int mgmt_start_scanning(int index)
{
	DBG("index %d", index);
	return -ENOSYS;
}

static int mgmt_stop_scanning(int index)
{
	DBG("index %d", index);
	return -ENOSYS;
}

static int mgmt_resolve_name(int index, bdaddr_t *bdaddr)
{
	char addr[18];

	ba2str(bdaddr, addr);
	DBG("index %d addr %s", index, addr);

	return -ENOSYS;
}

static int mgmt_set_name(int index, const char *name)
{
	DBG("index %d, name %s", index, name);
	return -ENOSYS;
}

static int mgmt_cancel_resolve_name(int index, bdaddr_t *bdaddr)
{
	char addr[18];

	ba2str(bdaddr, addr);
	DBG("index %d addr %s", index, addr);

	return -ENOSYS;
}

static int mgmt_fast_connectable(int index, gboolean enable)
{
	DBG("index %d enable %d", index, enable);
	return -ENOSYS;
}

static int mgmt_read_clock(int index, bdaddr_t *bdaddr, int which, int timeout,
					uint32_t *clock, uint16_t *accuracy)
{
	char addr[18];

	ba2str(bdaddr, addr);
	DBG("index %d addr %s which %d timeout %d", index, addr, which,
								timeout);

	return -ENOSYS;
}

static int mgmt_read_bdaddr(int index, bdaddr_t *bdaddr)
{
	char addr[18];
	struct controller_info *info = &controllers[index];

	ba2str(&info->bdaddr, addr);
	DBG("index %d addr %s", index, addr);

	if (!info->valid)
		return -ENODEV;

	bacpy(bdaddr, &info->bdaddr);

	return 0;
}

static int mgmt_block_device(int index, bdaddr_t *bdaddr)
{
	char addr[18];

	ba2str(bdaddr, addr);
	DBG("index %d addr %s", index, addr);

	return -ENOSYS;
}

static int mgmt_unblock_device(int index, bdaddr_t *bdaddr)
{
	char addr[18];

	ba2str(bdaddr, addr);
	DBG("index %d addr %s", index, addr);

	return -ENOSYS;
}

static int mgmt_get_conn_list(int index, GSList **conns)
{
	DBG("index %d", index);
	return -ENOSYS;
}

static int mgmt_read_local_version(int index, struct hci_version *ver)
{
	struct controller_info *info = &controllers[index];

	DBG("index %d", index);

	if (!info->valid)
		return -ENODEV;

	memset(ver, 0, sizeof(*ver));
	ver->manufacturer = info->manufacturer;
	ver->hci_ver = info->hci_ver;
	ver->hci_rev = info->hci_rev;

	return 0;
}

static int mgmt_read_local_features(int index, uint8_t *features)
{
	struct controller_info *info = &controllers[index];

	DBG("index %d", index);

	if (!info->valid)
		return -ENODEV;

	memcpy(features, info->features, 8);

	return 0;
}

static int mgmt_disconnect(int index, uint16_t handle)
{
	DBG("index %d handle %u", index, handle);
	return -ENOSYS;
}

static int mgmt_remove_bonding(int index, bdaddr_t *bdaddr)
{
	char addr[18];

	ba2str(bdaddr, addr);
	DBG("index %d addr %s", index, addr);

	return -ENOSYS;
}

static int mgmt_request_authentication(int index, uint16_t handle)
{
	DBG("index %d handle %u", index, handle);
	return -ENOSYS;
}

static int mgmt_pincode_reply(int index, bdaddr_t *bdaddr, const char *pin)
{
	char addr[18];

	ba2str(bdaddr, addr);
	DBG("index %d addr %s pin %s", index, addr, pin);

	return -ENOSYS;
}

static int mgmt_confirm_reply(int index, bdaddr_t *bdaddr, gboolean success)
{
	char addr[18];

	ba2str(bdaddr, addr);
	DBG("index %d addr %s success %d", index, addr, success);

	return -ENOSYS;
}

static int mgmt_passkey_reply(int index, bdaddr_t *bdaddr, uint32_t passkey)
{
	char addr[18];

	ba2str(bdaddr, addr);
	DBG("index %d addr %s passkey %06u", index, addr, passkey);

	return -ENOSYS;
}

static int mgmt_get_auth_info(int index, bdaddr_t *bdaddr, uint8_t *auth)
{
	char addr[18];

	ba2str(bdaddr, addr);
	DBG("index %d addr %s", index, addr);

	return -ENOSYS;
}

static int mgmt_read_scan_enable(int index)
{
	DBG("index %d", index);
	return -ENOSYS;
}

static int mgmt_enable_le(int index)
{
	DBG("index %d", index);
	return -ENOSYS;
}

static int mgmt_encrypt_link(int index, bdaddr_t *dst, bt_hci_result_t cb,
							gpointer user_data)
{
	char addr[18];

	ba2str(dst, addr);
	DBG("index %d addr %s", index, addr);

	return -ENOSYS;
}

static int mgmt_set_did(int index, uint16_t vendor, uint16_t product,
							uint16_t version)
{
	DBG("index %d vendor %u product %u version %u",
					index, vendor, product, version);
	return -ENOSYS;
}

static int mgmt_services_updated(int index)
{
	DBG("index %d", index);
	return -ENOSYS;
}

static int mgmt_disable_cod_cache(int index)
{
	DBG("index %d", index);
	return -ENOSYS;
}

static int mgmt_restore_powered(int index)
{
	DBG("index %d", index);
	return -ENOSYS;
}

static int mgmt_load_keys(int index, GSList *keys, gboolean debug_keys)
{
	DBG("index %d keys %d debug_keys %d", index, g_slist_length(keys),
								debug_keys);
	return -ENOSYS;
}

static struct btd_adapter_ops mgmt_ops = {
	.setup = mgmt_setup,
	.cleanup = mgmt_cleanup,
	.set_powered = mgmt_set_powered,
	.set_connectable = mgmt_set_connectable,
	.set_discoverable = mgmt_set_discoverable,
	.set_pairable = mgmt_set_pairable,
	.set_limited_discoverable = mgmt_set_limited_discoverable,
	.start_inquiry = mgmt_start_inquiry,
	.stop_inquiry = mgmt_stop_inquiry,
	.start_scanning = mgmt_start_scanning,
	.stop_scanning = mgmt_stop_scanning,
	.resolve_name = mgmt_resolve_name,
	.cancel_resolve_name = mgmt_cancel_resolve_name,
	.set_name = mgmt_set_name,
	.set_dev_class = mgmt_set_dev_class,
	.set_fast_connectable = mgmt_fast_connectable,
	.read_clock = mgmt_read_clock,
	.read_bdaddr = mgmt_read_bdaddr,
	.block_device = mgmt_block_device,
	.unblock_device = mgmt_unblock_device,
	.get_conn_list = mgmt_get_conn_list,
	.read_local_version = mgmt_read_local_version,
	.read_local_features = mgmt_read_local_features,
	.disconnect = mgmt_disconnect,
	.remove_bonding = mgmt_remove_bonding,
	.request_authentication = mgmt_request_authentication,
	.pincode_reply = mgmt_pincode_reply,
	.confirm_reply = mgmt_confirm_reply,
	.passkey_reply = mgmt_passkey_reply,
	.get_auth_info = mgmt_get_auth_info,
	.read_scan_enable = mgmt_read_scan_enable,
	.enable_le = mgmt_enable_le,
	.encrypt_link = mgmt_encrypt_link,
	.set_did = mgmt_set_did,
	.services_updated = mgmt_services_updated,
	.disable_cod_cache = mgmt_disable_cod_cache,
	.restore_powered = mgmt_restore_powered,
	.load_keys = mgmt_load_keys,
};

static int mgmt_init(void)
{
	return btd_register_adapter_ops(&mgmt_ops, TRUE);
}

static void mgmt_exit(void)
{
	btd_adapter_cleanup_ops(&mgmt_ops);
}

BLUETOOTH_PLUGIN_DEFINE(mgmtops, VERSION,
		BLUETOOTH_PLUGIN_PRIORITY_LOW, mgmt_init, mgmt_exit)
