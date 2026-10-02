/* SPDX-License-Identifier: MIT
 * Exercise real Windows GDI/WIC and binary buffers without changing or reading
 * the user's clipboard. Include the implementation to test its private helpers.
 */
#include <assert.h>
#include <stdio.h>
#include "../image.c"

static void checkBitmap(int width, int height, int bottom_up, int bitfields, int ddb)
{
    BITMAPINFO *info = calloc(1, sizeof(BITMAPINFOHEADER) + 3 * sizeof(DWORD));
    unsigned char *pixels, *png;
    size_t size, fallback_size;
    unsigned char *fallback;
    HBITMAP bitmap;
    IWICImagingFactory *factory;
    IWICStream *stream;
    IWICBitmapDecoder *decoder;
    IWICBitmapFrameDecode *frame;
    IWICFormatConverter *converter;
    UINT w, h;
    unsigned char *decoded;
    int x, y;
    info->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info->bmiHeader.biWidth = width;
    info->bmiHeader.biHeight = bottom_up ? height : -height;
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biBitCount = 32;
    info->bmiHeader.biCompression = bitfields ? BI_BITFIELDS : BI_RGB;
    if (bitfields) {
        DWORD *masks = (DWORD *)info->bmiColors;
        masks[0] = 0x00ff0000; masks[1] = 0x0000ff00; masks[2] = 0x000000ff;
    }
    bitmap = CreateDIBSection(NULL, info, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
    assert(bitmap);
    for (y=0; y<height; ++y) for (x=0; x<width; ++x) {
        size_t offset = ((size_t)(bottom_up ? height-1-y : y)*width+x)*4;
        pixels[offset] = (unsigned char)(x*7);
        pixels[offset+1] = (unsigned char)(y*11);
        pixels[offset+2] = (unsigned char)(x+y);
        pixels[offset+3] = 0; /* Unspecified DDB alpha must not make it transparent. */
    }
    /* Exercise the bounded fallback without accessing the desktop clipboard. */
    {
        size_t masks = bitfields ? 12 : 0;
        size_t length = sizeof(BITMAPINFOHEADER) + masks + (size_t)width*height*4;
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, length);
        unsigned char *bytes = GlobalLock(mem);
        assert(bytes);
        memcpy(bytes, info, sizeof(BITMAPINFOHEADER) + masks);
        memcpy(bytes + sizeof(BITMAPINFOHEADER) + masks, pixels, (size_t)width*height*4);
        GlobalUnlock(mem);
        fallback = dibPNG(mem, &fallback_size);
        assert(fallback && fallback_size > 8); GlobalFree(mem);
    }
    if (ddb) {
        HDC dc = GetDC(NULL);
        HBITMAP device = CreateDIBitmap(dc, &info->bmiHeader, CBM_INIT,
                                       pixels, info, DIB_RGB_COLORS);
        assert(device); ReleaseDC(NULL, dc); DeleteObject(bitmap); bitmap = device;
    }
    /* The conversion must initialize COM itself, including on a fresh thread. */
    png = bitmapPNG(bitmap, &size);
    assert(png && size > 8);
    assert(size == fallback_size && !memcmp(png, fallback, size)); free(fallback);
    assert(SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED)));
    assert(SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory, NULL,
        CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory)));
    assert(SUCCEEDED(IWICImagingFactory_CreateStream(factory, &stream)));
    assert(SUCCEEDED(IWICStream_InitializeFromMemory(stream, png, (DWORD)size)));
    assert(SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(factory,
        (IStream *)stream, NULL, WICDecodeMetadataCacheOnLoad, &decoder)));
    assert(SUCCEEDED(IWICBitmapDecoder_GetFrame(decoder, 0, &frame)));
    assert(SUCCEEDED(IWICBitmapFrameDecode_GetSize(frame, &w, &h)));
    assert(w == (UINT)width && h == (UINT)height);
    assert(SUCCEEDED(IWICImagingFactory_CreateFormatConverter(factory, &converter)));
    assert(SUCCEEDED(IWICFormatConverter_Initialize(converter, (IWICBitmapSource *)frame,
        &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0,
        WICBitmapPaletteTypeCustom)));
    decoded = malloc((size_t)width*height*4);
    assert(decoded);
    assert(SUCCEEDED(IWICFormatConverter_CopyPixels(converter, NULL, width*4,
        width*height*4, decoded)));
    for (y=0; y<height; ++y) for (x=0; x<width; ++x) {
        size_t offset = ((size_t)y*width+x)*4;
        assert(decoded[offset] == (unsigned char)(x*7));
        assert(decoded[offset+1] == (unsigned char)(y*11));
        assert(decoded[offset+2] == (unsigned char)(x+y));
        assert(decoded[offset+3] == 255);
    }
    /* The native PNG path must preserve every byte, including embedded NULs. */
    {
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
        void *data = GlobalLock(mem);
        size_t copied_size;
        unsigned char *copied;
        assert(data); memcpy(data, png, size); GlobalUnlock(mem);
        copied = copyPNG(mem, &copied_size);
        assert(copied && copied_size >= size && !memcmp(copied, png, size));
        free(copied); GlobalFree(mem);
    }
    free(decoded); free(png); free(info); DeleteObject(bitmap);
    IWICFormatConverter_Release(converter);
    IWICBitmapFrameDecode_Release(frame);
    IWICBitmapDecoder_Release(decoder);
    IWICStream_Release(stream);
    IWICImagingFactory_Release(factory);
    CoUninitialize();
}

static void checkInvalidDIB(void)
{
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, 144);
    BITMAPV5HEADER *h = GlobalLock(memory);
    size_t size;
    assert(h);
    h->bV5Size = 40; h->bV5Width = 8; h->bV5Height = 8;
    h->bV5Planes = 1; h->bV5BitCount = 32;
    GlobalUnlock(memory);
    assert(!dibPNG(memory, &size) && !size); /* Truncated pixel buffer. */
    h = GlobalLock(memory); h->bV5Width = 0x7fffffff; GlobalUnlock(memory);
    assert(!dibPNG(memory, &size) && !size); /* Excessive dimensions. */
    h = GlobalLock(memory); h->bV5Width = 1; h->bV5Height = 1;
    h->bV5Size = sizeof(*h); h->bV5ProfileData = 0xffffffff;
    h->bV5ProfileSize = 100; GlobalUnlock(memory);
    assert(!dibPNG(memory, &size) && !size); /* Invalid profile extent. */
    h = GlobalLock(memory); h->bV5ProfileData = h->bV5ProfileSize = 0;
    h->bV5ClrUsed = 257; GlobalUnlock(memory);
    assert(!dibPNG(memory, &size) && !size); /* Invalid palette. */
    h = GlobalLock(memory); h->bV5ClrUsed = 0; h->bV5SizeImage = 0xffffffff;
    GlobalUnlock(memory);
    assert(!dibPNG(memory, &size) && !size); /* Claimed data exceeds allocation. */
    GlobalFree(memory);
}

int main(void)
{
    size_t size=999;
    HGLOBAL bad;
    checkInvalidDIB();
    checkBitmap(3, 2, 0, 0, 0);
    checkBitmap(3, 2, 1, 0, 0);
    checkBitmap(17, 13, 0, 1, 0);
    checkBitmap(1920, 1080, 1, 1, 0);
    checkBitmap(3, 2, 0, 0, 1);
    assert(!bitmapPNG(NULL, &size) && size == 0);
    assert(!copyPNG(NULL, &size) && size == 0);
    assert(!dibPNG(NULL, &size) && size == 0);
    bad=GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, 32);
    assert(!copyPNG(bad,&size) && size==0);
    assert(!dibPNG(bad,&size) && size==0); GlobalFree(bad);
    puts("PASS: bitmap colors, orientation, BI_BITFIELDS, opaque alpha, 1080p, PNG byte preservation, invalid input");
    return 0;
}
