/* SPDX-License-Identifier: MIT
 * Wire-level tests against a real, isolated X server (no clipboard access).
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../outgoing.h"
#include "../image.h"
static xcb_connection_t *sender, *receiver;
static xcb_screen_t *screen;
static xcb_atom_t png, incr, property, clipboard;
static ClipboardOutgoing state;
static uint64_t now = 100;

static xcb_atom_t atom(const char *name)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(sender,
        xcb_intern_atom(sender,0,(uint16_t)strlen(name),name),NULL);
    xcb_atom_t result; assert(r); result=r->atom; free(r); return result;
}
static void syncConnection(xcb_connection_t *c)
{
    xcb_get_input_focus_reply_t *r=xcb_get_input_focus_reply(c,xcb_get_input_focus(c),NULL);
    assert(r); free(r);
}
static void pump(void)
{
    xcb_generic_event_t *e;
    syncConnection(receiver); syncConnection(sender);
    while ((e=xcb_poll_for_event(sender))) {
        assert((e->response_type & 127)!=0);
        winClipboardOutgoingEvent(&state,sender,e,++now); free(e);
    }
    xcb_flush(sender);
}
static xcb_window_t window(void)
{
    xcb_window_t w=xcb_generate_id(receiver);
    xcb_create_window(receiver,XCB_COPY_FROM_PARENT,w,screen->root,
        0,0,1,1,0,XCB_WINDOW_CLASS_INPUT_OUTPUT,screen->root_visual,0,NULL);
    syncConnection(receiver); return w;
}
static int sendBytes(xcb_window_t w, size_t size, int no_property)
{
    xcb_selection_request_event_t r={0};
    unsigned char *bytes=malloc(size ? size : 1); size_t i;
    assert(bytes);
    for(i=0;i<size;++i)bytes[i]=(unsigned char)(i*13+7);
    r.requestor=w; r.selection=clipboard; r.target=png;
    r.property=no_property ? XCB_NONE : property;
    return winClipboardSend(&state,sender,&r,incr,bytes,size,now);
}
static xcb_get_property_reply_t *get(xcb_window_t w, xcb_atom_t prop, int delete)
{
    xcb_get_property_reply_t *r=xcb_get_property_reply(receiver,
        xcb_get_property(receiver,delete,w,prop,XCB_GET_PROPERTY_TYPE_ANY,0,
                         WIN_CLIPBOARD_IMAGE_LIMIT/4),NULL);
    assert(r); return r;
}
static void noTransfers(void)
{
    int i; for(i=0;i<WIN_CLIPBOARD_TRANSFERS;++i)assert(!state.slots[i]);
}
static void maskRestored(xcb_window_t w)
{
    xcb_get_window_attributes_reply_t *r=xcb_get_window_attributes_reply(sender,
        xcb_get_window_attributes(sender,w),NULL);
    assert(r && r->your_event_mask==0); free(r);
}
static void transfer(size_t size, int no_property)
{
    xcb_window_t w=window();
    xcb_atom_t prop=no_property?png:property;
    xcb_get_property_reply_t *r; size_t total=0, i; int incremental;
    assert(sendBytes(w,size,no_property));
    r=get(w,prop,1); incremental=r->type==incr;
    if(incremental) {
        assert(r->format==32 && r->value_len==1);
        assert(*(uint32_t *)xcb_get_property_value(r)==size); free(r);
    }
    for (;;) {
        if(incremental) {
            int tries=0;
            do {
                pump(); r=get(w,prop,1);
                if(r->type!=XCB_NONE)break;
                free(r); usleep(1000);
            } while(++tries<1000);
            assert(tries<1000);
        }
        assert(r->type==png && r->format==8);
        for(i=0;i<(size_t)xcb_get_property_value_length(r);++i)
            assert(((unsigned char *)xcb_get_property_value(r))[i]==
                   (unsigned char)((total+i)*13+7));
        total+=xcb_get_property_value_length(r);
        i=r->value_len; free(r);
        if(!incremental || !i)break;
    }
    assert(total==size); noTransfers(); maskRestored(w);
    xcb_destroy_window(receiver,w); pump();
}
int main(void)
{
    int s, i; xcb_window_t windows[WIN_CLIPBOARD_TRANSFERS+1];
    sender=xcb_connect(NULL,&s); receiver=xcb_connect(NULL,NULL);
    assert(!xcb_connection_has_error(sender) && !xcb_connection_has_error(receiver));
    screen=xcb_setup_roots_iterator(xcb_get_setup(receiver)).data;
    png=atom("image/png"); incr=atom("INCR");
    property=atom("TEST_IMAGE_TRANSFER"); clipboard=atom("CLIPBOARD");
    transfer(37,0); transfer(65536,0); transfer(65537,0);
    transfer(3*1024*1024+17,0); transfer(99,1); transfer(131077,1);
    for(i=0;i<WIN_CLIPBOARD_TRANSFERS+1;++i)windows[i]=window();
    for(i=0;i<WIN_CLIPBOARD_TRANSFERS;++i)assert(sendBytes(windows[i],131072,0));
    assert(!sendBytes(windows[WIN_CLIPBOARD_TRANSFERS],131072,0));
    assert(!sendBytes(windows[0],131072,0)); /* Do not overwrite an active stream. */
    {
        xcb_property_notify_event_t fake={0};
        xcb_get_property_reply_t *r;
        fake.response_type=XCB_PROPERTY_NOTIFY|0x80;
        fake.window=windows[0]; fake.atom=property; fake.state=XCB_PROPERTY_DELETE;
        winClipboardOutgoingEvent(&state,sender,(xcb_generic_event_t *)&fake,now);
        r=get(windows[0],property,0); assert(r->type==incr); free(r);
    }
    winClipboardOutgoingExpire(&state,sender,now+WIN_CLIPBOARD_TRANSFER_TIMEOUT);
    noTransfers(); for(i=0;i<WIN_CLIPBOARD_TRANSFERS;++i)maskRestored(windows[i]);
    assert(sendBytes(windows[0],131072,0));
    xcb_destroy_window(receiver,windows[0]); pump(); noTransfers();
    assert(!sendBytes(windows[1],0,0)); noTransfers();
    assert(sendBytes(windows[1],131072,0));
    winClipboardOutgoingClear(&state,NULL); noTransfers();
    xcb_disconnect(receiver); xcb_disconnect(sender);
    puts("PASS: binary fidelity, chunk boundaries, 3 MiB INCR, property=None, concurrent/duplicate limits, synthetic event rejection, timeout, destroyed requestor, shutdown");
    return 0;
}
