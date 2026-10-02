/* SPDX-License-Identifier: MIT
 * Real Windows clipboard + X11 integration in a PRIVATE window station.
 * Never fall back to the user's window station if isolation cannot be created.
 * Host definitions below replace only server logging/lifecycle, not clipboard,
 * WIC, Windows messages, XCB, or selection behavior.
 */
#define COBJMACROS
#include "../winclipboard.h"
#include "../internal.h"
#include "os/ddx_priv.h"
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <wincodec.h>

Bool g_fClipboardStarted = FALSE;
Bool g_fClipboardLaunched = FALSE;
HWND g_hwndClipboard = NULL;
void *g_pClipboardDisplay = NULL;
Window g_iClipboardWindow = 0;
static HDESK desktop;
static DWORD workerId;
static xcb_connection_t *client;
static xcb_window_t requestor;
static xcb_atom_t clipboard, png, targets, utf8, prop, incr;

void winDebug(const char *format, ...)
{
    if (getenv("VCXSRV_TEST_VERBOSE")) {
        va_list ap; va_start(ap,format); vfprintf(stderr,format,ap); va_end(ap);
    }
}
void ErrorF(const char *format, ...)
{
    va_list ap; va_start(ap,format); vfprintf(stderr,format,ap); va_end(ap);
}
CARD32 GetTimeInMillis(void) { return GetTickCount(); }
void ddxGiveUp(enum ExitCode error) { (void)error; abort(); }

static void *worker(void *unused)
{
    (void)unused;
    assert(SetThreadDesktop(desktop));
    workerId=GetCurrentThreadId();
    winClipboardProc(getenv("DISPLAY"),NULL);
    return NULL;
}
static xcb_atom_t atom(const char *name)
{
    xcb_intern_atom_reply_t *r=xcb_intern_atom_reply(client,
        xcb_intern_atom(client,0,strlen(name),name),NULL);
    xcb_atom_t a; assert(r); a=r->atom; free(r); return a;
}
static void pumpWindows(void)
{
    MSG msg;
    while(PeekMessage(&msg,NULL,0,0,PM_REMOVE)) {
        TranslateMessage(&msg); DispatchMessage(&msg);
    }
}
static unsigned char *fetch(xcb_atom_t target, size_t *size)
{
    uint64_t deadline=GetTickCount64()+10000;
    int notified=0, incremental=0;
    unsigned char *result=NULL;
    xcb_generic_event_t *event;
    *size=0;
    xcb_convert_selection(client,requestor,clipboard,target,prop,XCB_CURRENT_TIME);
    xcb_flush(client);
    while(GetTickCount64()<deadline) {
        pumpWindows();
        event=xcb_poll_for_event(client);
        if(!event) { Sleep(1); continue; }
        if((event->response_type&127)==XCB_SELECTION_NOTIFY) {
            xcb_selection_notify_event_t *n=(void *)event;
            if (n->property != prop)
                fprintf(stderr, "Selection refused: target=%u PNG=%u TARGETS=%u UTF8=%u\n", target,png,targets,utf8);
            assert(n->property==prop); notified=1;
        } else if((event->response_type&127)==XCB_PROPERTY_NOTIFY) {
            xcb_property_notify_event_t *p=(void *)event;
            if(!notified || !incremental || p->state!=XCB_PROPERTY_NEW_VALUE) {
                free(event); continue;
            }
        } else { free(event); continue; }
        free(event);
        {
            xcb_get_property_reply_t *r=xcb_get_property_reply(client,
                xcb_get_property(client,1,requestor,prop,XCB_GET_PROPERTY_TYPE_ANY,
                                 0,WIN_CLIPBOARD_IMAGE_LIMIT/4),NULL);
            size_t len; assert(r);
            len=xcb_get_property_value_length(r);
            if(r->type==incr) { incremental=1; free(r); xcb_flush(client); continue; }
            assert(r->type==target || (target==targets && r->type==XCB_ATOM_ATOM));
            assert(*size+len<=WIN_CLIPBOARD_IMAGE_LIMIT);
            result=realloc(result,*size+len+1); assert(result);
            memcpy(result+*size,xcb_get_property_value(r),len); *size+=len;
            free(r); xcb_flush(client);
            if(!incremental || !len)return result;
        }
    }
    assert(!"selection timeout"); return NULL;
}
static int hasTarget(xcb_atom_t expected)
{
    size_t size,i; unsigned char *bytes=fetch(targets,&size); int found=0;
    for(i=0;i<size/sizeof(xcb_atom_t);++i)
        if(((xcb_atom_t *)bytes)[i]==expected)found=1;
    free(bytes); return found;
}
static void waitOwner(int present)
{
    uint64_t deadline=GetTickCount64()+5000;
    while(GetTickCount64()<deadline) {
        xcb_get_selection_owner_reply_t *r=xcb_get_selection_owner_reply(client,
            xcb_get_selection_owner(client,clipboard),NULL);
        int ok; assert(r); ok=(r->owner!=XCB_NONE)==present; free(r);
        if(ok)return;
        pumpWindows(); Sleep(1);
    }
    assert(!"clipboard ownership timeout");
}
static unsigned pixel(int x, int y, int width)
{
    unsigned v=(unsigned)(x+width*y)*1664525u+1013904223u;
    v ^= v >> 16; v *= 2246822519u; v ^= v >> 13;
    v *= 3266489917u; v ^= v >> 16;
    return v;
}
static void verifyPNG(unsigned char *bytes, size_t size, int width, int height)
{
    IWICImagingFactory *factory;
    IWICStream *stream;
    IWICBitmapDecoder *decoder;
    IWICBitmapFrameDecode *frame;
    IWICFormatConverter *converter;
    UINT w,h;
    unsigned char *decoded=malloc((size_t)width*height*4);
    int x,y;
    assert(decoded && SUCCEEDED(CoInitializeEx(NULL,COINIT_MULTITHREADED)));
    assert(SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory,NULL,CLSCTX_INPROC_SERVER,
        &IID_IWICImagingFactory,(void **)&factory)));
    assert(SUCCEEDED(IWICImagingFactory_CreateStream(factory,&stream)));
    assert(SUCCEEDED(IWICStream_InitializeFromMemory(stream,bytes,(DWORD)size)));
    assert(SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(factory,(IStream *)stream,
        NULL,WICDecodeMetadataCacheOnLoad,&decoder)));
    assert(SUCCEEDED(IWICBitmapDecoder_GetFrame(decoder,0,&frame)));
    assert(SUCCEEDED(IWICBitmapFrameDecode_GetSize(frame,&w,&h)));
    assert(w==(UINT)width && h==(UINT)height);
    assert(SUCCEEDED(IWICImagingFactory_CreateFormatConverter(factory,&converter)));
    assert(SUCCEEDED(IWICFormatConverter_Initialize(converter,(IWICBitmapSource *)frame,
        &GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,NULL,0,WICBitmapPaletteTypeCustom)));
    assert(SUCCEEDED(IWICFormatConverter_CopyPixels(converter,NULL,width*4,width*height*4,decoded)));
    for(y=0;y<height;++y)for(x=0;x<width;++x) {
        unsigned v=pixel(x,y,width);
        size_t o=((size_t)y*width+x)*4;
        assert(decoded[o]==(unsigned char)v && decoded[o+1]==(unsigned char)(v>>8) &&
               decoded[o+2]==(unsigned char)(v>>16) && decoded[o+3]==255);
    }
    free(decoded);
    IWICFormatConverter_Release(converter); IWICBitmapFrameDecode_Release(frame);
    IWICBitmapDecoder_Release(decoder); IWICStream_Release(stream);
    IWICImagingFactory_Release(factory); CoUninitialize();
}
static void setDIB(HWND owner,int width,int height)
{
    size_t size=sizeof(BITMAPINFOHEADER)+(size_t)width*height*4;
    HGLOBAL h=GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT,size);
    BITMAPINFOHEADER *b=GlobalLock(h); unsigned char *p; int x,y;
    assert(b); b->biSize=sizeof(*b); b->biWidth=width; b->biHeight=-height;
    b->biPlanes=1; b->biBitCount=32; b->biCompression=BI_RGB;
    p=(unsigned char *)(b+1);
    for(y=0;y<height;++y)for(x=0;x<width;++x) {
        size_t o=((size_t)y*width+x)*4;
        /* Deterministic pixel noise ensures the large fixture exercises INCR. */
        unsigned v=pixel(x,y,width);
        p[o]=(unsigned char)v; p[o+1]=(unsigned char)(v>>8); p[o+2]=(unsigned char)(v>>16);
    }
    GlobalUnlock(h); assert(OpenClipboard(owner)); assert(EmptyClipboard());
    assert(SetClipboardData(CF_DIB,h)); CloseClipboard(); waitOwner(1);
}
static void setPNG(HWND owner,const unsigned char *pngBytes,size_t size,int withText)
{
    HGLOBAL h=GlobalAlloc(GMEM_MOVEABLE,size); void *p=GlobalLock(h);
    assert(p); memcpy(p,pngBytes,size); GlobalUnlock(h);
    assert(OpenClipboard(owner)); assert(EmptyClipboard());
    assert(SetClipboardData(RegisterClipboardFormatW(L"PNG"),h));
    if(withText) {
        const WCHAR text[]={ 'h','e','l','l','o',' ',0x03bb,'\r','\n',0 };
        h=GlobalAlloc(GMEM_MOVEABLE,sizeof(text)); p=GlobalLock(h);
        assert(p); memcpy(p,text,sizeof(text)); GlobalUnlock(h);
        assert(SetClipboardData(CF_UNICODETEXT,h));
    }
    CloseClipboard(); waitOwner(1);
}
int main(int argc, char **argv)
{
    WCHAR name[80]; HWINSTA station,oldStation; HDESK oldDesktop;
    HWND owner; pthread_t thread; int screenNumber; xcb_screen_t *screen;
    uint32_t mask=XCB_EVENT_MASK_PROPERTY_CHANGE;
    unsigned char *small,*large,*again,*text; size_t smallSize,largeSize,size;
    FILE *out;
    int ci = argc == 2 && !strcmp(argv[1], "--ci-clipboard") &&
        getenv("GITHUB_ACTIONS") && !strcmp(getenv("GITHUB_ACTIONS"), "true");
    if (argc == 2 && !strcmp(argv[1], "--serve-current-clipboard")) {
        /* Explicit manual acceptance mode: bridge only, no synthetic writes. */
        winClipboardProc(getenv("DISPLAY"), NULL);
        return 0;
    }
    if (argc != 1 && !ci) return 2;
    oldStation=GetProcessWindowStation(); oldDesktop=GetThreadDesktop(GetCurrentThreadId());
    swprintf(name,80,L"vcxsrv-clipboard-test-%lu",(unsigned long)GetCurrentProcessId());
    station=NULL;
    if (ci) { desktop=oldDesktop; } else {
    station=CreateWindowStationW(name,0,WINSTA_ALL_ACCESS,NULL);
    if (!station) { fprintf(stderr, "CreateWindowStation failed: %lu\n", (unsigned long)GetLastError()); return 77; }
    if (!SetProcessWindowStation(station)) { fprintf(stderr, "SetProcessWindowStation failed: %lu\n", (unsigned long)GetLastError()); CloseWindowStation(station); return 77; }
    desktop=CreateDesktopW(L"test",NULL,NULL,0,GENERIC_ALL,NULL);
    assert(desktop && SetThreadDesktop(desktop));
    }
    owner=CreateWindowExW(0,L"STATIC",L"fixture",0,0,0,1,1,NULL,NULL,NULL,NULL);
    assert(owner);
    client=xcb_connect(NULL,&screenNumber); assert(!xcb_connection_has_error(client));
    screen=xcb_setup_roots_iterator(xcb_get_setup(client)).data;
    requestor=xcb_generate_id(client);
    xcb_create_window(client,XCB_COPY_FROM_PARENT,requestor,screen->root,0,0,1,1,0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,screen->root_visual,XCB_CW_EVENT_MASK,&mask);
    clipboard=atom("CLIPBOARD"); png=atom("image/png"); targets=atom("TARGETS");
    utf8=atom("UTF8_STRING"); prop=atom("INTEGRATION_RESULT"); incr=atom("INCR");
    assert(pthread_create(&thread,NULL,worker,NULL)==0);
    /* Wait for the listener to initialize before setting the first fixture. */
    { uint64_t end=GetTickCount64()+5000; while(!g_fClipboardStarted && GetTickCount64()<end)Sleep(1); }
    assert(g_fClipboardStarted);
    fprintf(stderr,"STAGE: bitmap-only\n");
    setDIB(owner,3,2); assert(hasTarget(png) && !hasTarget(utf8));
    small=fetch(png,&smallSize); assert(smallSize>8);
    verifyPNG(small,smallSize,3,2);
    out=fopen("build-tests/integration-small.png","wb"); assert(out);
    assert(fwrite(small,1,smallSize,out)==smallSize); fclose(out);
    fprintf(stderr,"STAGE: native PNG\n");
    setPNG(owner,small,smallSize,0); again=fetch(png,&size);
    assert(size>=smallSize && !memcmp(small,again,smallSize)); free(again);
    fprintf(stderr,"STAGE: mixed PNG and text\n");
    setPNG(owner,small,smallSize,1); assert(hasTarget(png) && hasTarget(utf8));
    text=fetch(utf8,&size); assert(size==9 && !memcmp(text,"hello \xce\xbb\n",9)); free(text);
    fprintf(stderr,"STAGE: large bitmap\n");
    setDIB(owner,1024,1024); large=fetch(png,&largeSize); assert(largeSize>65536);
    verifyPNG(large,largeSize,1024,1024);
    out=fopen("build-tests/integration-large.png","wb"); assert(out);
    assert(fwrite(large,1,largeSize,out)==largeSize); fclose(out);
    assert(OpenClipboard(owner)); assert(EmptyClipboard()); CloseClipboard(); waitOwner(0);
    free(small); free(large);
    assert(PostThreadMessage(workerId,WM_QUIT,0,0)); assert(pthread_join(thread,NULL)==0);
    xcb_disconnect(client); DestroyWindow(owner);
    if (!ci) {
        assert(SetThreadDesktop(oldDesktop)); assert(SetProcessWindowStation(oldStation));
        CloseDesktop(desktop); CloseWindowStation(station);
    }
    puts("PASS: isolated Windows clipboard -> actual clipboard library -> X11 PNG, bitmap, mixed Unicode text, INCR, empty clipboard");
    return 0;
}
