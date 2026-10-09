/*
 * WynlandOS - Zerp 2.0: newer Wayland clients on Qt's compositor.
 *
 * QtWaylandCompositor offers wl_seat at version 4 and
 * wl_data_device_manager at version 1. Clients of today want more: foot
 * refuses to start without wl_seat 5 (pointer events grouped by
 * wl_pointer.frame) and wl_data_device_manager 3. What those versions add
 * is small, so Zerp offers them itself:
 *
 *  - wl_global_create() is interposed (this executable's definition wins
 *    over libwayland-server's for every library): the seat goes out as
 *    version 5, the data device manager as 3. The request the seat adds,
 *    wl_seat.release, gets Qt's generated default, which keeps the
 *    resource until the client disconnects (a small leak per release;
 *    clients release once).
 *  - wl_resource_create() is interposed: Qt makes data devices, sources and
 *    offers at version 1 whatever the client bound, and a version 3
 *    request on them (wl_data_source.set_actions, wl_data_offer.finish) was
 *    a protocol error that disconnected the client. They are made at 3:
 *    those requests reach Qt's generated do-nothing defaults (drag and drop
 *    actions are not negotiated, the clipboard works), and a client that
 *    bound version 1 never sees an event of a later version -- Qt sends
 *    none.
 *  - wl_resource_post_event() is interposed: after a pointer event to a
 *    version 5 pointer, the wl_pointer.frame event that version promises
 *    follows. The event itself is passed on unchanged, its arguments read
 *    by the message signature as libwayland does.
 *
 * And one for the keyboard: Qt writes the keymap every client gets into a
 * file it makes with mkstemp() (and unlinks), mapped MAP_SHARED; clients
 * map the fd they receive. The kernel's MAP_SHARED of an ext2 file is
 * not shared yet (each process gets its own copy of the pages), so the
 * clients read zeros -- foot crashed on it. mkstemp() is interposed for
 * Qt's "qtwayland-XXXXXX" files and answers with a memfd, whose shared
 * mappings are shared.
 *
 * Copyright (C) 2026 Red_Artem39. GPL-2.0-or-later.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* the parts of libwayland-server's ABI used here (wayland-util.h) */
struct wl_display;
struct wl_global;
struct wl_resource;
struct wl_client;
struct wl_message { const char *name; const char *signature; const void **types; };
struct wl_interface {
    const char *name;
    int version;
    int method_count;
    const struct wl_message *methods;
    int event_count;
    const struct wl_message *events;
};
union wl_argument { int32_t i; uint32_t u; int32_t f; const char *s; void *o; uint32_t n; void *a; int32_t h; };
typedef void (*wl_global_bind_func_t)(struct wl_client *, void *, uint32_t, uint32_t);

/* by name: Qt's code is generated with interface descriptions of its own,
   not libwayland's wl_seat_interface & co. */
static int is(const struct wl_interface *i, const char *name) { return i && i->name && !strcmp(i->name, name); }
int wl_resource_get_version(struct wl_resource *resource);
void wl_resource_post_event_array(struct wl_resource *resource, uint32_t opcode, union wl_argument *args);

#define WL_POINTER_FRAME 5   /* enter 0, leave 1, motion 2, button 3, axis 4, frame 5 */
#define MAX_ARGS 20

struct wl_global *wl_global_create(struct wl_display *display, const struct wl_interface *interface,
                                   int version, void *data, wl_global_bind_func_t bind)
{
    static struct wl_global *(*real)(struct wl_display *, const struct wl_interface *, int, void *,
                                     wl_global_bind_func_t);
    if (!real) real = dlsym(RTLD_NEXT, "wl_global_create");
    if (is(interface, "wl_seat") && version < 5) version = 5;
    else if (is(interface, "wl_data_device_manager") && version < 3) version = 3;
    return real(display, interface, version, data, bind);
}

struct wl_resource *wl_resource_create(struct wl_client *client, const struct wl_interface *interface,
                                       int version, uint32_t id)
{
    static struct wl_resource *(*real)(struct wl_client *, const struct wl_interface *, int, uint32_t);
    if (!real) real = dlsym(RTLD_NEXT, "wl_resource_create");
    if (version < 3 && (is(interface, "wl_data_device") || is(interface, "wl_data_source") ||
                        is(interface, "wl_data_offer")))
        version = 3;
    return real(client, interface, version, id);
}

/* the message of an event, from the resource's interface; and whether the
   resource is a pointer (the only one whose events get a frame) */
static const struct wl_message *event_of(struct wl_resource *r, uint32_t opcode, int *is_pointer)
{
    /* struct wl_resource starts with struct wl_object { interface, impl, id } */
    const struct wl_interface *iface = *(const struct wl_interface **)r;
    *is_pointer = is(iface, "wl_pointer");
    if (!iface || (int)opcode >= iface->event_count) return NULL;
    return &iface->events[opcode];
}

void wl_resource_post_event(struct wl_resource *resource, uint32_t opcode, ...)
{
    int is_pointer = 0;
    const struct wl_message *msg = event_of(resource, opcode, &is_pointer);
    union wl_argument args[MAX_ARGS];
    int n = 0;
    va_list ap;
    va_start(ap, opcode);
    for (const char *sig = msg ? msg->signature : ""; *sig && n < MAX_ARGS; sig++) {
        switch (*sig) {
        case 'i': args[n++].i = va_arg(ap, int32_t); break;
        case 'u': args[n++].u = va_arg(ap, uint32_t); break;
        case 'f': args[n++].f = va_arg(ap, int32_t); break;
        case 's': args[n++].s = va_arg(ap, const char *); break;
        case 'o': args[n++].o = va_arg(ap, void *); break;
        case 'n': args[n++].o = va_arg(ap, void *); break;
        case 'a': args[n++].a = va_arg(ap, void *); break;
        case 'h': args[n++].h = va_arg(ap, int32_t); break;
        default: break;   /* '?' (nullable) and the since-version digits */
        }
    }
    va_end(ap);
    wl_resource_post_event_array(resource, opcode, args);
    if (is_pointer && opcode < WL_POINTER_FRAME && wl_resource_get_version(resource) >= 5)
        wl_resource_post_event_array(resource, WL_POINTER_FRAME, NULL);
}

int mkstemp(char *tmpl)
{
    static int (*real)(char *);
    if (!real) real = (int (*)(char *))dlsym(RTLD_NEXT, "mkstemp");
    const size_t n = tmpl ? strlen(tmpl) : 0;
    if (n >= 16 && !strcmp(tmpl + n - 16, "qtwayland-XXXXXX")) {
        /* a real unique name first: Qt unlinks whatever name it gets
           back, and a made-up one could be someone else's file */
        int named = real(tmpl);
        if (named < 0) return named;
        int fd = memfd_create("qtwayland-keymap", MFD_CLOEXEC);
        if (fd >= 0) { close(named); return fd; }   /* Qt unlinks the empty reserved file */
        return named;
    }
    return real(tmpl);
}
