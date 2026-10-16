/*
 * WynlandOS - bluez-stub: a STAND-IN for BlueZ, NOT Bluetooth.
 * ============================================================
 * WynlandOS has no Bluetooth: no USB stack, no HCI, no AF_BLUETOOTH
 * sockets, so the real BlueZ daemon (bluetoothd) cannot run. Desktop
 * shells (Caelestia/Quickshell) ask the system bus for org.bluez and log
 * errors when nobody answers. This program owns the name org.bluez and
 * says the truth in BlueZ's own terms: there are no adapters (an empty
 * ObjectManager). The shell then shows Bluetooth as a machine without
 * an adapter.
 *
 * Real Bluetooth later = xHCI (USB) + an HCI driver + AF_BLUETOOTH in the
 * kernel + Ubuntu's bluez instead of this file (and its wynrc service
 * "bluez-stub" and /etc/dbus-1/system.d/wynland-bluez-stub.conf).
 *
 * Answers, on "/": org.freedesktop.DBus.ObjectManager.GetManagedObjects
 * (empty), org.freedesktop.DBus.Introspectable.Introspect; anything else
 * gets UnknownMethod / UnknownObject.
 */
#include <dbus/dbus.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *INTROSPECT =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    " <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "  <method name=\"Introspect\"><arg name=\"xml\" type=\"s\" direction=\"out\"/></method>\n"
    " </interface>\n"
    " <interface name=\"org.freedesktop.DBus.ObjectManager\">\n"
    "  <method name=\"GetManagedObjects\"><arg name=\"objects\" type=\"a{oa{sa{sv}}}\" direction=\"out\"/></method>\n"
    "  <signal name=\"InterfacesAdded\"><arg name=\"object\" type=\"o\"/><arg name=\"interfaces\" type=\"a{sa{sv}}\"/></signal>\n"
    "  <signal name=\"InterfacesRemoved\"><arg name=\"object\" type=\"o\"/><arg name=\"interfaces\" type=\"as\"/></signal>\n"
    " </interface>\n"
    "</node>\n";

static DBusHandlerResult on_message(DBusConnection *c, DBusMessage *m, void *data)
{
    (void)data;
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char *path = dbus_message_get_path(m);
    DBusMessage *reply = NULL;

    if (path && !strcmp(path, "/") &&
        dbus_message_is_method_call(m, "org.freedesktop.DBus.ObjectManager", "GetManagedObjects")) {
        /* no adapters, no devices: an empty a{oa{sa{sv}}} */
        reply = dbus_message_new_method_return(m);
        DBusMessageIter it, dict;
        dbus_message_iter_init_append(reply, &it);
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{oa{sa{sv}}}", &dict);
        dbus_message_iter_close_container(&it, &dict);
    } else if (path && !strcmp(path, "/") &&
               dbus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {
        reply = dbus_message_new_method_return(m);
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &INTROSPECT, DBUS_TYPE_INVALID);
    } else if (path && strcmp(path, "/")) {
        reply = dbus_message_new_error(m, "org.freedesktop.DBus.Error.UnknownObject",
                                       "no such object: this system has no Bluetooth (bluez-stub)");
    } else {
        reply = dbus_message_new_error(m, "org.freedesktop.DBus.Error.UnknownMethod",
                                       "not supported: this system has no Bluetooth (bluez-stub)");
    }
    if (reply) {
        dbus_connection_send(c, reply, NULL);
        dbus_message_unref(reply);
    }
    return DBUS_HANDLER_RESULT_HANDLED;
}

int main(void)
{
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *c = NULL;
    /* the system bus may still be coming up at boot */
    for (int i = 0; i < 60 && !c; i++) {
        c = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
        if (!c) { dbus_error_free(&err); sleep(1); }
    }
    if (!c) { fprintf(stderr, "bluez-stub: no system bus\n"); return 1; }
    int r = dbus_bus_request_name(c, "org.bluez", DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (r != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "bluez-stub: could not own org.bluez: %s\n", dbus_error_is_set(&err) ? err.message : "taken");
        return 1;
    }
    dbus_connection_add_filter(c, on_message, NULL, NULL);
    fprintf(stderr, "bluez-stub: org.bluez answered by a stand-in (no Bluetooth on this system)\n");
    while (dbus_connection_read_write_dispatch(c, -1)) {}
    return 0;
}
