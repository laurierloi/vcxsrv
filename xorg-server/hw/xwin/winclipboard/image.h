/* SPDX-License-Identifier: MIT */
#ifndef WINCLIPBOARD_IMAGE_H
#define WINCLIPBOARD_IMAGE_H
#include <stddef.h>
/* PNG transport and decoded bitmap limits, independent of X request size. */
#define WIN_CLIPBOARD_IMAGE_LIMIT (64u * 1024u * 1024u)
/* Caller holds the Windows clipboard open. Returned storage uses malloc. */
int winClipboardHasImage(void);
unsigned char *winClipboardReadPNG(size_t *size);
#endif
