#!/bin/sh
# /dev/video0 prototype (#44 part 2): static wiring checks for the
# kernel patch, the cam-dump -S holder and the v4l2-probe tool,
# plus a -Wall -Wextra -Werror host compile of v4l2-probe.c.
# Needs no board.
set -u

repo_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
srcdir="$repo_root/linux/buildroot-external/package/cam-dump/src"
patchf="$repo_root/linux/tab5/0141-esp32p4-cam-v4l2.patch"
kcfg="$repo_root/linux/linux-tab5.config"
mkf="$repo_root/linux/buildroot-external/package/cam-dump/cam-dump.mk"
tmp=${TMPDIR:-/tmp}/cdp-v4l2-test.$$
trap 'rm -rf -- "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp"

fails=0
checks=0
ok() { echo "ok: $1"; checks=$((checks + 1)); }
bad() { echo "FAIL: $1"; fails=$((fails + 1)); checks=$((checks + 1)); }
has() { # name file pattern
	if grep -q "$3" "$2"; then ok "$1"; else bad "$1"; fi
}
hasnt() { # name file pattern
	if grep -q "$3" "$2"; then bad "$1"; else ok "$1"; fi
}

# 1-4: kernel config enables the minimal V4L2 core, Tab5 only
has "config: MEDIA_SUPPORT" "$kcfg" "CONFIG_MEDIA_SUPPORT=y"
has "config: FILTER" "$kcfg" "CONFIG_MEDIA_SUPPORT_FILTER=y"
has "config: CAMERA_SUPPORT" "$kcfg" "CONFIG_MEDIA_CAMERA_SUPPORT=y"
has "config: VIDEO_DEV" "$kcfg" "CONFIG_VIDEO_DEV=y"

# 5-13: the 0141 patch registers a read()-only video device
has "patch: exists" "$patchf" "video_device"
has "patch: video_register_device" "$patchf" "video_register_device"
has "patch: READWRITE cap" "$patchf" "V4L2_CAP_READWRITE"
hasnt "patch: no STREAMING cap" "$patchf" "^+.*V4L2_CAP_STREAMING"
has "patch: fixed SBGGR8" "$patchf" "V4L2_PIX_FMT_SBGGR8"
has "patch: fixed sizeimage" "$patchf" "CAM_FRAME_BYTES"
has "patch: v4l2_fh_open" "$patchf" "v4l2_fh_open"
has "patch: remove unregisters" "$patchf" "video_unregister_device"
has "patch: IS_ENABLED guard" "$patchf" "IS_ENABLED(CONFIG_VIDEO_DEV)"

# 14-15: probe/remove free what they take
has "patch: err path frees fb" "$patchf" "err_fb:"
has "patch: remove frees v4l2" "$patchf" "v4l2_device_unregister"

# 16-18: cam-dump -S holds the rail for video0 readers
has "cam-dump: -S flag" "$srcdir/cam-dump.c" '"-S"'
has "cam-dump: usage shows -S" "$srcdir/cam-dump.c" '\[-S\]'
has "cam-dump: holder pauses" "$srcdir/cam-dump.c" "pause()"

# 19-20: v4l2-probe built and installed by the package
has "mk: builds v4l2-probe" "$mkf" "v4l2-probe.c"
has "mk: installs v4l2-probe" "$mkf" "usr/bin/v4l2-probe"

# 21: v4l2-probe.c compiles warning-free on the host (or in docker)
ccbin=""
if command -v cc >/dev/null 2>&1; then ccbin=cc
elif command -v gcc >/dev/null 2>&1; then ccbin=gcc
elif command -v docker >/dev/null 2>&1 &&
     docker ps --format '{{.Names}}' 2>/dev/null | grep -q .; then
	ct=$(docker ps --format '{{.Names}}' | head -1)
	mkdir -p "$tmp/d"
	docker cp "$srcdir/v4l2-probe.c" "$ct:$tmp/d/" 2>/dev/null \
		|| docker cp "$srcdir/v4l2-probe.c" "$ct:/tmp/v4l2-probe.c"
	if MSYS_NO_PATHCONV=1 docker exec "$ct" cc -Wall -Wextra -Werror \
		-c -o /tmp/v4l2-probe.o /tmp/v4l2-probe.c 2>"$tmp/err"; then
		ok "v4l2-probe.c -Werror clean (docker)"
	else
		cat "$tmp/err"; bad "v4l2-probe.c -Werror clean (docker)"
	fi
	ccbin="docker-done"
fi
if [ "$ccbin" != "docker-done" ]; then
	if [ -n "$ccbin" ]; then
		if "$ccbin" -Wall -Wextra -Werror -c \
			-o "$tmp/v4l2-probe.o" "$srcdir/v4l2-probe.c" \
			2>"$tmp/err"; then
			ok "v4l2-probe.c -Werror clean (host)"
		else
			cat "$tmp/err"; bad "v4l2-probe.c -Werror clean (host)"
		fi
	else
		echo "SKIP: no host compiler and no docker container"
	fi
fi

echo "test-cam-dump-v4l2: $((checks - fails))/$checks checks passed"
[ "$fails" -eq 0 ] || exit 1
echo "PASS"
