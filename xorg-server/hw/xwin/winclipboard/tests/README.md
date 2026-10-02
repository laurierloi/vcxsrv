# Windows image clipboard forwarding

This change implements **Windows -> X11 `CLIPBOARD` images**. It does not add
X11 -> Windows image conversion and does not close every case in upstream issue
[marchaesen/vcxsrv#51](https://github.com/marchaesen/vcxsrv/issues/51).

## Behavior

- Advertise `image/png` when Windows has a PNG, bitmap, DIB, or DIBV5.
- Copy the registered Windows `PNG` format unchanged, preserving transparency.
- Otherwise ask Windows for its synthesized bitmap and encode it with WIC.
  Bitmap-only copies are opaque because Windows DDB alpha is unspecified.
- Keep image-only copies out of `PRIMARY` (middle-click text paste).
- Preserve text targets when the clipboard contains both text and an image.
- Snapshot and close the Windows clipboard before sending over X11/SSH.
- Use ICCCM INCR for payloads above 64 KiB (or a smaller server request limit).
  Four concurrent requestor windows are supported, with a 30-second idle timeout;
  repeated requests on an active window are rejected rather than overwriting data.
- Limit encoded payloads to 64 MiB and bitmap conversion to 16,777,216 pixels.
  Timeouts, closed requestor windows, failed requests and shutdown release buffers.

## Tests

The native test uses synthetic GDI bitmaps and WIC decoders. It never opens the
user's clipboard. Run from a Visual Studio developer command prompt:

```bat
cl /nologo /W4 /WX /std:c11 /Od /Fe:image-test.exe xorg-server\hw\xwin\winclipboard\tests\image-test.c /link ole32.lib windowscodecs.lib gdi32.lib user32.lib uuid.lib
image-test.exe
```

The X11 test uses a real isolated X server, not an XCB mock. On Linux:

```sh
cc -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -g \
  -fsanitize=address,undefined \
  xorg-server/hw/xwin/winclipboard/tests/outgoing-test.c \
  xorg-server/hw/xwin/winclipboard/outgoing.c -lxcb -o outgoing-test
xvfb-run -a ./outgoing-test
```

The same tests also compile with Cygwin GCC; link the native image test against
`-lole32 -lwindowscodecs -lgdi32 -luuid`. Use `-lxcb` for the transfer test and
point `DISPLAY`/`XAUTHORITY` at an isolated, authenticated X server with clipboard
integration disabled. The transport test does not access any desktop clipboard.

The Windows integration target links the actual clipboard library and uses a
real X server. It checks bitmap-only, registered PNG, mixed Unicode text/image,
large INCR transfers, and empty clipboard, decoding and comparing bitmap pixels.
Its default mode requires a private Windows window station and exits with code
77 if Windows denies that isolation. It must not fall back to the user's clipboard.
The `--ci-clipboard` mode requires `GITHUB_ACTIONS=true` and is intended only for
a disposable CI runner. Do not set that environment variable to bypass isolation
on a personal desktop.

An explicitly requested manual test may use `--serve-current-clipboard` to run
the bridge without writing synthetic fixtures. Use an isolated authenticated X
display: the bridge itself retains the normal bidirectional text clipboard behavior.

## End-to-end acceptance (separate from unit/protocol tests)

1. Start a build containing this change with clipboard integration enabled and
   access control enabled. Keep the listener local; do not use `-ac` or expose
   an unauthenticated X11 port to the network.
2. From Windows, use SSH trusted forwarding to your own workstation. Let SSH
   assign the remote `DISPLAY`; never set it to the Windows display manually.
3. Copy a synthetic screenshot on Windows. On Linux, check that
   `xclip -selection clipboard -target TARGETS -out` contains `image/png`, then
   retrieve it with `xclip -selection clipboard -target image/png -out > image.png`.
   Decode the PNG and verify pixel values/dimensions, not just its signature.
4. Repeat with a bitmap-only Windows source, a PNG with transparency, a large
   image requiring INCR, Unicode text, empty clipboard, and repeated copies.
5. Start Codex in that fresh SSH environment and attempt image paste. A PNG
   successfully retrieved by xclip does not by itself establish Codex acceptance.
6. Reconnect and create a new tmux window for the new SSH `DISPLAY`. Existing
   processes retain their old environment and cannot be repaired by updating
   tmux's session environment alone.

CI jobs cover native conversion, X11 protocol behavior, and the integrated
Windows clipboard library. Record their actual results in the pull request.
Full-server compilation, interactive desktop acceptance, SSH and Codex acceptance
remain separate checks.
