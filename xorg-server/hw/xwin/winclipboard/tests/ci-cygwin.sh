#!/bin/bash
# SPDX-License-Identifier: MIT
set -euo pipefail
# This uses the disposable GitHub runner clipboard, never a developer's clipboard.
if [[ "${GITHUB_ACTIONS:-}" != true ]]; then
    echo 'This integration runner is restricted to the ephemeral CI environment.' >&2
    exit 2
fi
mkdir -p build-tests
meson setup build-cygwin xorg-server -Dxwin=true -Dxorg=false -Dxwayland=false \
    -Dxnest=false -Dxvfb=false -Dglx=false -Dglamor=false -Ddocs=false \
    -Ddevel-docs=false -Dhyperv=false -Dxkb_dir=/usr/share/X11/xkb \
    -Dxkb_bin_dir=/usr/bin >build-tests/configure.log 2>&1
ninja -C build-cygwin -j4 hw/xwin/winclipboard/clipboard-integration-test.exe \
    >build-tests/clipboard-build.log 2>&1
export DISPLAY=:93
export XAUTHORITY="$PWD/build-tests/Xauthority"
# A private cookie; never print it in CI logs.
python3 -c 'import secrets,subprocess,os; subprocess.run(["xauth","-f",os.environ["XAUTHORITY"],"add",":93",".",secrets.token_hex(16)],check=True)'
XWin :93 -multiwindow -noclipboard -notrayicon -nolisten tcp -noreset \
    -auth "$XAUTHORITY" -logfile "$PWD/build-tests/XWin.log" \
    >build-tests/server.log 2>&1 &
server_pid=$!
trap 'kill "$server_pid" 2>/dev/null || true' EXIT
sleep 3
./build-cygwin/hw/xwin/winclipboard/clipboard-integration-test.exe --ci-clipboard \
    >build-tests/integration.log 2>&1
cat build-tests/integration.log
