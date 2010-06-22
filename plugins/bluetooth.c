/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010  ProFUSION embedded systems
 *  Copyright (C) 2010 Gustavo F. Padovan <gustavo@padovan.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
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
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <glib.h>
#include <gdbus.h>
#include <ofono.h>

#include <ofono/dbus.h>

#include "bluetooth.h"

static DBusConnection *connection;
static GHashTable *uuid_hash = NULL;
static GHashTable *adapter_address_hash = NULL;

void bluetooth_create_path(const char *dev_addr, const char *adapter_addr, char *buf, int size)
{
	int i, j;

	for (i = 0, j = 0; adapter_addr[j] && i < size - 1; j++)
		if (adapter_addr[j] >= '0' && adapter_addr[j] <= '9')
			buf[i++] = adapter_addr[j];
		else if (adapter_addr[j] >= 'A' && adapter_addr[j] <= 'F')
			buf[i++] = adapter_addr[j];

	if (i < size - 1)
		buf[i++] = '_';

	for (j = 0; dev_addr[j] && i < size - 1; j++)
		if (dev_addr[j] >= '0' && dev_addr[j] <= '9')
			buf[i++] = dev_addr[j];
		else if (dev_addr[j] >= 'A' && dev_addr[j] <= 'F')
			buf[i++] = dev_addr[j];

	buf[i] = '\0';
}

int bluetooth_send_with_reply(const char *path, const char *interface,
				const char *method,
				DBusPendingCallNotifyFunction cb,
				void *user_data, DBusFreeFunction free_func,
				int timeout, int type, ...)
{
	DBusMessage *msg;
	DBusPendingCall *call;
	va_list args;
	int err;

	msg = dbus_message_new_method_call(BLUEZ_SERVICE, path,
						interface, method);
	if (!msg) {
		ofono_error("Unable to allocate new D-Bus %s message", method);
		err = -ENOMEM;
		goto fail;
	}

	va_start(args, type);

	if (!dbus_message_append_args_valist(msg, type, args)) {
		va_end(args);
		err = -EIO;
		goto fail;
	}

	va_end(args);

	if (timeout > 0)
		timeout *= 1000;

	if (!dbus_connection_send_with_reply(connection, msg, &call, timeout)) {
		ofono_error("Sending %s failed", method);
		err = -EIO;
		goto fail;
	}

	dbus_pending_call_set_notify(call, cb, user_data, free_func);
	dbus_pending_call_unref(call);
	dbus_message_unref(msg);

	return 0;

fail:
	if (free_func && user_data)
		free_func(user_data);

	if (msg)
		dbus_message_unref(msg);

	return err;
}

typedef void (*PropertyHandler)(DBusMessageIter *iter, gpointer user_data);

struct property_handler {
	const char *property;
	PropertyHandler callback;
	gpointer user_data;
};

static gint property_handler_compare(gconstpointer a, gconstpointer b)
{
	const struct property_handler *handler = a;
	const char *property = b;

	return strcmp(handler->property, property);
}

void bluetooth_parse_properties(DBusMessage *reply, const char *property, ...)
{
	va_list args;
	GSList *prop_handlers = NULL;
	DBusMessageIter array, dict;

	va_start(args, property);

	while (property != NULL) {
		struct property_handler *handler =
					g_new0(struct property_handler, 1);

		handler->property = property;
		handler->callback = va_arg(args, PropertyHandler);
		handler->user_data = va_arg(args, gpointer);

		property = va_arg(args, const char *);

		prop_handlers = g_slist_prepend(prop_handlers, handler);
	}

	va_end(args);

	if (dbus_message_iter_init(reply, &array) == FALSE)
		goto done;

	if (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_ARRAY)
		goto done;

	dbus_message_iter_recurse(&array, &dict);

	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, value;
		const char *key;
		GSList *l;

		dbus_message_iter_recurse(&dict, &entry);

		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
			goto done;

		dbus_message_iter_get_basic(&entry, &key);

		dbus_message_iter_next(&entry);

		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT)
			goto done;

		dbus_message_iter_recurse(&entry, &value);

		l = g_slist_find_custom(prop_handlers, key,
					property_handler_compare);

		if (l) {
			struct property_handler *handler = l->data;

			handler->callback(&value, handler->user_data);
		}

		dbus_message_iter_next(&dict);
	}

done:
	g_slist_foreach(prop_handlers, (GFunc)g_free, NULL);
	g_slist_free(prop_handlers);
}

static void has_uuid(DBusMessageIter *array, gpointer user_data)
{
	gboolean *profiles = user_data;
	DBusMessageIter value;

	if (dbus_message_iter_get_arg_type(array) != DBUS_TYPE_ARRAY)
		return;

	dbus_message_iter_recurse(array, &value);

	while (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_STRING) {
		const char *uuid;

		dbus_message_iter_get_basic(&value, &uuid);

		if (!strcasecmp(uuid, HFP_AG_UUID))
			*profiles |= HFP_AG;

		dbus_message_iter_next(&value);
	}
}

static void parse_string(DBusMessageIter *iter, gpointer user_data)
{
	char **str = user_data;
	int arg_type = dbus_message_iter_get_arg_type(iter);

	if (arg_type != DBUS_TYPE_OBJECT_PATH && arg_type != DBUS_TYPE_STRING)
		return;

	dbus_message_iter_get_basic(iter, str);
}

static void device_properties_cb(DBusPendingCall *call, gpointer user_data)
{
	DBusMessage *reply;
	int have_uuid = 0;
	const char *path = user_data;
	const char *adapter = NULL;
	const char *adapter_addr = NULL;
	const char *device_addr = NULL;
	const char *alias = NULL;
	struct bluetooth_profile *profile;

	reply = dbus_pending_call_steal_reply(call);

	if (dbus_message_is_error(reply, DBUS_ERROR_SERVICE_UNKNOWN)) {
		DBG("Bluetooth daemon is apparently not available.");
		goto done;
	}

	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
		if (!dbus_message_is_error(reply, DBUS_ERROR_UNKNOWN_METHOD))
			ofono_info("Error from GetProperties reply: %s",
					dbus_message_get_error_name(reply));

		goto done;
	}

	bluetooth_parse_properties(reply, "UUIDs", has_uuid, &have_uuid,
				"Adapter", parse_string, &adapter,
				"Address", parse_string, &device_addr,
				"Alias", parse_string, &alias, NULL);

	if (adapter)
		adapter_addr = g_hash_table_lookup(adapter_address_hash,
							adapter);

	if ((have_uuid & HFP_AG) && device_addr && adapter_addr) {
		profile = g_hash_table_lookup(uuid_hash, HFP_AG_UUID);
		if (!profile || !profile->create)
			goto done;

		profile->create(path, device_addr, adapter_addr, alias);
	}

done:
	dbus_message_unref(reply);
}

static void parse_devices(DBusMessageIter *array, gpointer user_data)
{
	DBusMessageIter value;
	GSList **device_list = user_data;

	DBG("");

	if (dbus_message_iter_get_arg_type(array) != DBUS_TYPE_ARRAY)
		return;

	dbus_message_iter_recurse(array, &value);

	while (dbus_message_iter_get_arg_type(&value)
			== DBUS_TYPE_OBJECT_PATH) {
		const char *path;

		dbus_message_iter_get_basic(&value, &path);

		*device_list = g_slist_prepend(*device_list, (gpointer) path);

		dbus_message_iter_next(&value);
	}
}

static void adapter_properties_cb(DBusPendingCall *call, gpointer user_data)
{
	const char *path = user_data;
	DBusMessage *reply;
	GSList *device_list = NULL;
	GSList *l;
	const char *addr;

	reply = dbus_pending_call_steal_reply(call);

	if (dbus_message_is_error(reply, DBUS_ERROR_SERVICE_UNKNOWN)) {
		DBG("Bluetooth daemon is apparently not available.");
		goto done;
	}

	bluetooth_parse_properties(reply, "Devices", parse_devices, &device_list,
				"Address", parse_string, &addr, NULL);

	DBG("Adapter Address: %s, Path: %s", addr, path);
	g_hash_table_insert(adapter_address_hash,
				g_strdup(path), g_strdup(addr));

	for (l = device_list; l; l = l->next) {
		const char *device = l->data;

		bluetooth_send_with_reply(device, BLUEZ_DEVICE_INTERFACE,
				"GetProperties", device_properties_cb,
				g_strdup(device), g_free, -1, DBUS_TYPE_INVALID);
	}

done:
	g_slist_free(device_list);
	dbus_message_unref(reply);
}

static void parse_adapters(DBusMessageIter *array, gpointer user_data)
{
	DBusMessageIter value;

	DBG("");

	if (dbus_message_iter_get_arg_type(array) != DBUS_TYPE_ARRAY)
		return;

	dbus_message_iter_recurse(array, &value);

	while (dbus_message_iter_get_arg_type(&value)
			== DBUS_TYPE_OBJECT_PATH) {
		const char *path;

		dbus_message_iter_get_basic(&value, &path);

		DBG("Calling GetProperties on %s", path);

		bluetooth_send_with_reply(path, BLUEZ_ADAPTER_INTERFACE,
				"GetProperties", adapter_properties_cb,
				g_strdup(path), g_free, -1, DBUS_TYPE_INVALID);

		dbus_message_iter_next(&value);
	}
}

static void manager_properties_cb(DBusPendingCall *call, gpointer user_data)
{
	DBusMessage *reply;

	reply = dbus_pending_call_steal_reply(call);

	if (dbus_message_is_error(reply, DBUS_ERROR_SERVICE_UNKNOWN)) {
		DBG("Bluetooth daemon is apparently not available.");
		goto done;
	}

	DBG("");

	bluetooth_parse_properties(reply, "Adapters", parse_adapters, NULL, NULL);

done:
	dbus_message_unref(reply);
}

int bluetooth_register_uuid(const char *uuid, struct bluetooth_profile *profile)
{
	if (uuid_hash)
		goto done;

	connection = ofono_dbus_get_connection();

	uuid_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
						g_free, NULL);

	adapter_address_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
						g_free, g_free);

done:
	g_hash_table_insert(uuid_hash, g_strdup(uuid), profile);

	bluetooth_send_with_reply("/", BLUEZ_MANAGER_INTERFACE, "GetProperties",
				manager_properties_cb, NULL, NULL, -1,
				DBUS_TYPE_INVALID);

	return 0;
}

void bluetooth_unregister_uuid(const char *uuid)
{
	g_hash_table_remove(uuid_hash, uuid);

	if (g_hash_table_size(uuid_hash))
		return;

	g_hash_table_destroy(uuid_hash);
	g_hash_table_destroy(adapter_address_hash);
	uuid_hash = NULL;
}

OFONO_PLUGIN_DEFINE(bluetooth, "Bluetooth Utils Plugins", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, NULL, NULL)
