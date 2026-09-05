#!/bin/sh
# brscan-skeyd handler: scan one page and write a PDF into the shared folder.
#
# Invoked by brscan-skeyd when the panel SCAN key is pressed. Receives:
#   BRSCAN_SKEY_CODE  function code from the panel (e.g. 0x03)
#   BRSCAN_DEVICE     SANE device name, if the daemon was given one
#
# Running this is what completes the transaction with the device. Until a scan
# session actually opens, the panel sits at "Connecting to PC" and reports no
# further key presses - so this script must run promptly and must not be
# skipped, even for a code we do not recognise.

set -u

OUTDIR="${BRSCAN_OUTDIR:-/srv/scans}"
RESOLUTION="${BRSCAN_RESOLUTION:-300}"
MODE="${BRSCAN_MODE:-24bit Color}"
TMPDIR_BASE="${BRSCAN_TMPDIR:-/var/tmp}"

# Optional scan length in mm. Left unset the full advertised page is scanned.
#
# Some scanners cannot reach the full length of their own platen: a DCP-7060D
# advertises A4 (297mm) but its carriage stops at ~291mm, so the driver pads
# the last few mm white. Setting this slightly below the platen length removes
# that band. Device-specific, hence configuration rather than a default.
HEIGHT_MM="${BRSCAN_HEIGHT_MM:-}"

stamp=$(date +%Y%m%d-%H%M%S)
work=$(mktemp -d "${TMPDIR_BASE}/brscan-XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT

# Look at only the Brother backend.
#
# `scanimage -L` otherwise loads every backend listed in /etc/sane.d/dll.conf -
# about 80 on a stock install - and several of them (net, escl, airscan) probe
# the network and block on timeouts. Measured on a 700MHz ARMv6: 10.4s wall for
# 1.8s of CPU, which was longer than assembling the PDF and a third of the whole
# press-to-PDF time. With a one-line backend list it is 0.1s.
#
# Safe because the Brother backend keeps its own config (Brsane.ini,
# brsanenetdevice.cfg) under /usr/share/sane/brother, not in the SANE config
# dir, so nothing is lost by not falling back to /etc/sane.d. Naming
# /etc/sane.d as a fallback would undo most of the saving anyway: the dll
# backend then also reads its dll.d/ drop-ins, which costs 1.5s of the 10.4s.
sane_conf="$work/sane"
mkdir -p "$sane_conf" && echo brother > "$sane_conf/dll.conf" || exit 1
export SANE_CONFIG_DIR="$sane_conf"

# Resolve the device. The backend names devices by enumeration order
# (bus%d;dev%d), which is not stable across replugs, so look it up each time
# rather than baking it into the unit file.
device="${BRSCAN_DEVICE:-}"
if [ -z "$device" ]; then
    device=$(scanimage -L 2>/dev/null \
             | sed -n "s/^device \`\\(brother:[^']*\\)'.*/\\1/p" \
             | head -n1)
fi

if [ -z "$device" ]; then
    echo "handler: no Brother scanner found via scanimage -L" >&2
    exit 1
fi

echo "handler: code=${BRSCAN_SKEY_CODE:-?} device=$device mode=$MODE ${RESOLUTION}dpi" >&2

# Scan to JPEG. --format=jpeg keeps the page compressed from the start, which
# matters on a 700MHz ARMv6: a 300dpi colour A4 page is ~26MB as raw RGB.
set -- -d "$device" --mode "$MODE" --resolution "$RESOLUTION" --format=jpeg
[ -n "$HEIGHT_MM" ] && set -- "$@" -y "$HEIGHT_MM"

if ! scanimage "$@" -o "$work/page.jpg" 2>>"$work/scan.err"; then
    echo "handler: scanimage failed" >&2
    sed 's/^/handler: scanimage: /' "$work/scan.err" >&2
    exit 1
fi

mkdir -p "$OUTDIR" || exit 1
out="$OUTDIR/scan-$stamp.pdf"

# Wrap the JPEG in a PDF. Both routes embed the bitstream as a /DCTDecode
# stream with no re-encode - ImageMagick, which would decode and re-encode the
# full-page bitmap, is far slower and loses a generation of quality.
#
# brscan-jpeg2pdf is preferred purely on startup cost: img2pdf is correct but
# spends 4.9 of its 5.8s importing PIL and pikepdf before it looks at the file,
# which is a sixth of the whole press-to-PDF time on slow hardware. img2pdf
# stays as the fallback so an existing install keeps working, and because it
# handles JPEG variants the narrow C helper deliberately rejects.
#
# The helper is installed beside this script, so derive it from $0 rather than
# hardcoding a prefix. BRSCAN_JPEG2PDF overrides that, both for testing and for
# the case where $0 is not a usable path because the script was fed to a shell
# on stdin.
jpeg2pdf="${BRSCAN_JPEG2PDF:-$(dirname "$0")/brscan-jpeg2pdf}"

if [ -x "$jpeg2pdf" ] && "$jpeg2pdf" --dpi "$RESOLUTION" "$work/page.jpg" \
        -o "$out" 2>>"$work/pdf.err"; then
    :
elif command -v img2pdf >/dev/null 2>&1; then
    if [ -x "$jpeg2pdf" ]; then
        echo "handler: brscan-jpeg2pdf declined the file, using img2pdf" >&2
        sed 's/^/handler: jpeg2pdf: /' "$work/pdf.err" >&2
    fi
    if ! img2pdf "$work/page.jpg" -o "$out" 2>>"$work/pdf.err"; then
        echo "handler: img2pdf failed, keeping JPEG" >&2
        cp "$work/page.jpg" "$OUTDIR/scan-$stamp.jpg"
        exit 1
    fi
else
    echo "handler: no PDF writer available, writing JPEG instead" >&2
    cp "$work/page.jpg" "$OUTDIR/scan-$stamp.jpg"
    exit 0
fi

# Readable by anyone who can reach the share.
chmod 0644 "$out" 2>/dev/null || true
echo "handler: wrote $out" >&2
