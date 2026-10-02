/* SPDX-License-Identifier: MIT
 * ICCCM 2.7.2 incremental outgoing binary selections. The Windows clipboard
 * is closed before entering this state machine; slow clients cannot hold it.
 */
#include <stdlib.h>
#include <string.h>
#include "outgoing.h"
#include "image.h"

struct winClipboardTransfer {
    xcb_window_t window;
    xcb_atom_t property, target;
    uint32_t old_mask;
    unsigned char *bytes;
    size_t size, offset, chunk;
    uint64_t touched;
};

static int checked(xcb_connection_t *conn, xcb_void_cookie_t cookie)
{
    xcb_generic_error_t *error = xcb_request_check(conn, cookie);
    int ok = !error && !xcb_connection_has_error(conn);
    free(error);
    return ok;
}

static void release(ClipboardOutgoing *state, xcb_connection_t *conn, int slot,
                    int destroyed)
{
    struct winClipboardTransfer *t = state->slots[slot];
    if (!t) return;
    if (conn && !destroyed && !xcb_connection_has_error(conn))
        checked(conn, xcb_change_window_attributes_checked(conn, t->window,
                  XCB_CW_EVENT_MASK, &t->old_mask));
    free(t->bytes);
    free(t);
    state->slots[slot] = NULL;
}

void winClipboardOutgoingClear(ClipboardOutgoing *state, xcb_connection_t *conn)
{
    int i;
    for (i = 0; i < WIN_CLIPBOARD_TRANSFERS; ++i) release(state, conn, i, 0);
}

void winClipboardOutgoingExpire(ClipboardOutgoing *state, xcb_connection_t *conn,
                                uint64_t now)
{
    int i;
    for (i = 0; i < WIN_CLIPBOARD_TRANSFERS; ++i)
        if (state->slots[i] && now - state->slots[i]->touched >=
            WIN_CLIPBOARD_TRANSFER_TIMEOUT)
            release(state, conn, i, 0);
}

int winClipboardSend(ClipboardOutgoing *state, xcb_connection_t *conn,
                     const xcb_selection_request_event_t *request,
                     xcb_atom_t incr, unsigned char *bytes, size_t size,
                     uint64_t now)
{
    xcb_selection_notify_event_t notify = {0};
    xcb_atom_t property = request->property ? request->property : request->target;
    uint64_t max_bytes = (uint64_t)xcb_get_maximum_request_length(conn) * 4;
    size_t chunk = max_bytes > 32 ? (size_t)(max_bytes - 32) : 0;
    int slot = -1, i, ok = 0;
    struct winClipboardTransfer *t = NULL;
    xcb_get_window_attributes_reply_t *attrs;
    uint32_t mask, length;
    /* Small chunks keep memory and individual wire requests predictable. */
    if (chunk > 65536) chunk = 65536;
    winClipboardOutgoingExpire(state, conn, now);
    for (i = 0; i < WIN_CLIPBOARD_TRANSFERS; ++i) {
        if (state->slots[i] && state->slots[i]->window == request->requestor)
            goto notify;
        if (!state->slots[i]) slot = i;
    }
    if (!bytes || !size || size > WIN_CLIPBOARD_IMAGE_LIMIT || !chunk)
        goto notify;
    if (size <= chunk) {
        ok = checked(conn, xcb_change_property_checked(conn, XCB_PROP_MODE_REPLACE,
                     request->requestor, property, request->target, 8,
                     (uint32_t)size, bytes));
        goto notify;
    }
    if (slot < 0 || !incr) goto notify;
    attrs = xcb_get_window_attributes_reply(conn,
                xcb_get_window_attributes(conn, request->requestor), NULL);
    if (!attrs) goto notify;
    t = calloc(1, sizeof(*t));
    if (!t) { free(attrs); goto notify; }
    t->old_mask = attrs->your_event_mask;
    free(attrs);
    t->window = request->requestor;
    t->property = property;
    t->target = request->target;
    t->size = size;
    t->chunk = chunk;
    t->touched = now;
    mask = t->old_mask | XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    if (!checked(conn, xcb_change_window_attributes_checked(conn, t->window,
                                                           XCB_CW_EVENT_MASK, &mask))) {
        free(t); t = NULL; goto notify;
    }
    t->bytes = bytes;
    bytes = NULL;
    state->slots[slot] = t;
    length = (uint32_t)size;
    ok = checked(conn, xcb_change_property_checked(conn, XCB_PROP_MODE_REPLACE,
                 t->window, t->property, incr, 32, 1, &length));
    if (!ok) { release(state, conn, slot, 0); t = NULL; }
notify:
    free(bytes);
    notify.response_type = XCB_SELECTION_NOTIFY;
    notify.requestor = request->requestor;
    notify.selection = request->selection;
    notify.target = request->target;
    notify.property = ok ? property : XCB_NONE;
    notify.time = request->time;
    if (!checked(conn, xcb_send_event_checked(conn, 0, notify.requestor,
                                             0, (const char *)&notify))) {
        if (t) release(state, conn, slot, 0);
        ok = 0;
    }
    xcb_flush(conn);
    return ok;
}

void winClipboardOutgoingEvent(ClipboardOutgoing *state, xcb_connection_t *conn,
                               const xcb_generic_event_t *event, uint64_t now)
{
    int i;
    uint8_t type = event->response_type & 0x7f;
    /* Ignore synthetic property/deletion events: only the server advances INCR. */
    if (event->response_type & 0x80) return;
    for (i = 0; i < WIN_CLIPBOARD_TRANSFERS; ++i) {
        struct winClipboardTransfer *t = state->slots[i];
        if (!t) continue;
        if (type == XCB_DESTROY_NOTIFY &&
            ((const xcb_destroy_notify_event_t *)event)->window == t->window) {
            release(state, conn, i, 1);
        } else if (type == XCB_PROPERTY_NOTIFY) {
            const xcb_property_notify_event_t *p = (const xcb_property_notify_event_t *)event;
            size_t count;
            if (p->window != t->window || p->atom != t->property ||
                p->state != XCB_PROPERTY_DELETE) continue;
            count = t->size - t->offset;
            if (count > t->chunk) count = t->chunk;
            if (!checked(conn, xcb_change_property_checked(conn, XCB_PROP_MODE_REPLACE,
                         t->window, t->property, t->target, 8, (uint32_t)count,
                         t->bytes + t->offset)) || !count) {
                release(state, conn, i, 0);
            } else {
                t->offset += count;
                t->touched = now;
            }
        }
    }
}
