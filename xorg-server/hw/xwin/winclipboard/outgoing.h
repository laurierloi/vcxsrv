/* SPDX-License-Identifier: MIT */
#ifndef WINCLIPBOARD_OUTGOING_H
#define WINCLIPBOARD_OUTGOING_H
#include <stddef.h>
#include <stdint.h>
#include <xcb/xcb.h>
/* Bounded, per-connection state. One active transfer per requestor window. */
#define WIN_CLIPBOARD_TRANSFERS 4
#define WIN_CLIPBOARD_TRANSFER_TIMEOUT 30000u
struct winClipboardTransfer;
typedef struct {
    struct winClipboardTransfer *slots[WIN_CLIPBOARD_TRANSFERS];
} ClipboardOutgoing;
/* Always consumes bytes, including on error. Sends SelectionNotify itself. */
int winClipboardSend(ClipboardOutgoing *state, xcb_connection_t *conn,
                     const xcb_selection_request_event_t *request,
                     xcb_atom_t incr, unsigned char *bytes, size_t size,
                     uint64_t now);
void winClipboardOutgoingEvent(ClipboardOutgoing *state, xcb_connection_t *conn,
                               const xcb_generic_event_t *event, uint64_t now);
void winClipboardOutgoingExpire(ClipboardOutgoing *state, xcb_connection_t *conn,
                                uint64_t now);
void winClipboardOutgoingClear(ClipboardOutgoing *state, xcb_connection_t *conn);
#endif
