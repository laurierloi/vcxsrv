/* SPDX-License-Identifier: MIT
 * Windows clipboard -> PNG. Keep binary data out of the text conversion path.
 */
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "image.h"
#ifdef _MSC_VER
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#endif

/* Diagnostics contain API names/error codes only, never clipboard bytes. */
static void imageError(const char *operation, unsigned long error)
{
    fprintf(stderr, "clipboard image: %s failed (0x%08lx)\n", operation, error);
}

static UINT
pngFormat(void)
{
    return RegisterClipboardFormatW(L"PNG");
}

int
winClipboardHasImage(void)
{
    UINT png = pngFormat();
    return (png && IsClipboardFormatAvailable(png)) ||
        IsClipboardFormatAvailable(CF_BITMAP) ||
        IsClipboardFormatAvailable(CF_DIB) ||
        IsClipboardFormatAvailable(CF_DIBV5);
}

/* Windows synthesizes CF_BITMAP from DIB/DIBV5, including BI_BITFIELDS.
 * Let Windows interpret those layouts rather than parsing untrusted DIBs here.
 * The bitmap belongs to the clipboard and must not be deleted by this code.
 */
static unsigned char *
bitmapPNG(HBITMAP source, size_t *size)
{
    IWICImagingFactory *factory = NULL;
    IWICBitmap *bitmap = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IStream *stream = NULL;
    HGLOBAL memory = NULL;
    unsigned char *result = NULL;
    void *bytes;
    STATSTG stat;
    UINT width, height;
    BITMAP dimensions;
    HRESULT hr, init;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;

    *size = 0;
    if (!source || GetObject(source, sizeof(dimensions), &dimensions) != sizeof(dimensions) ||
        dimensions.bmWidth <= 0 || dimensions.bmHeight <= 0 ||
        (unsigned long long)dimensions.bmWidth * dimensions.bmHeight >
        WIN_CLIPBOARD_IMAGE_LIMIT / 4u)
        { imageError("bitmap dimensions", GetLastError()); return NULL; }
    init = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE)
        { imageError("CoInitializeEx", (unsigned long)init); return NULL; }
#define CHECK(call) do { hr = (call); if (FAILED(hr)) { imageError(#call, (unsigned long)hr); goto done; } } while (0)
    CHECK(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                           &IID_IWICImagingFactory, (void **)&factory));
    /* DDB alpha is unspecified; treat bitmap-only screenshots as opaque.
     * Native PNG is passed through separately, preserving its alpha exactly.
     */
    CHECK(IWICImagingFactory_CreateBitmapFromHBITMAP(factory, source, NULL,
                                                    WICBitmapIgnoreAlpha, &bitmap));
    CHECK(IWICBitmap_GetSize(bitmap, &width, &height));
    if (!width || !height || (unsigned long long)width * height >
        WIN_CLIPBOARD_IMAGE_LIMIT / 4u)
        { imageError("WIC dimensions", 0); goto done; }
    CHECK(CreateStreamOnHGlobal(NULL, TRUE, &stream));
    CHECK(IWICImagingFactory_CreateEncoder(factory, &GUID_ContainerFormatPng,
                                          NULL, &encoder));
    CHECK(IWICBitmapEncoder_Initialize(encoder, stream, WICBitmapEncoderNoCache));
    CHECK(IWICBitmapEncoder_CreateNewFrame(encoder, &frame, NULL));
    CHECK(IWICBitmapFrameEncode_Initialize(frame, NULL));
    CHECK(IWICBitmapFrameEncode_SetSize(frame, width, height));
    CHECK(IWICBitmapFrameEncode_SetPixelFormat(frame, &format));
    CHECK(IWICBitmapFrameEncode_WriteSource(frame, (IWICBitmapSource *)bitmap, NULL));
    CHECK(IWICBitmapFrameEncode_Commit(frame));
    CHECK(IWICBitmapEncoder_Commit(encoder));
    CHECK(IStream_Stat(stream, &stat, STATFLAG_NONAME));
    if (!stat.cbSize.QuadPart || stat.cbSize.QuadPart > WIN_CLIPBOARD_IMAGE_LIMIT)
        { imageError("encoded size", (unsigned long)stat.cbSize.QuadPart); goto done; }
    CHECK(GetHGlobalFromStream(stream, &memory));
    bytes = GlobalLock(memory);
    if (!bytes)
        { imageError("GlobalLock", GetLastError()); goto done; }
    result = malloc((size_t)stat.cbSize.QuadPart);
    if (result) {
        *size = (size_t)stat.cbSize.QuadPart;
        memcpy(result, bytes, *size);
    }
    GlobalUnlock(memory);
done:
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (stream) IStream_Release(stream);
    if (bitmap) IWICBitmap_Release(bitmap);
    if (factory) IWICImagingFactory_Release(factory);
    if (SUCCEEDED(init)) CoUninitialize();
    return result;
#undef CHECK
}

static unsigned char *
copyPNG(HGLOBAL memory, size_t *size)
{
    static const unsigned char signature[8] = {137,80,78,71,13,10,26,10};
    size_t length = memory ? GlobalSize(memory) : 0;
    void *bytes;
    unsigned char *result = NULL;
    *size = 0;
    if (length < sizeof(signature) || length > WIN_CLIPBOARD_IMAGE_LIMIT)
        return NULL;
    bytes = GlobalLock(memory);
    if (!bytes) return NULL;
    if (!memcmp(bytes, signature, sizeof(signature))) {
        result = malloc(length);
        if (result) {
            memcpy(result, bytes, length);
            *size = length;
        }
    }
    GlobalUnlock(memory);
    return result;
}

unsigned char *
winClipboardReadPNG(size_t *size)
{
    UINT png = pngFormat();
    unsigned char *result = NULL;
    *size = 0;
    if (png && IsClipboardFormatAvailable(png))
        result = copyPNG(GetClipboardData(png), size);
    if (result) return result;
    return bitmapPNG((HBITMAP)GetClipboardData(CF_BITMAP), size);
}
