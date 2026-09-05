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

# img2pdf embeds the JPEG bitstream directly with no re-encode. ImageMagick
# would decode and re-encode the full-page bitmap, which is far slower here.
if command -v img2pdf >/dev/null 2>&1; then
    if ! img2pdf "$work/page.jpg" -o "$out" 2>>"$work/pdf.err"; then
        echo "handler: img2pdf failed, keeping JPEG" >&2
        cp "$work/page.jpg" "$OUTDIR/scan-$stamp.jpg"
        exit 1
    fi
else
    echo "handler: img2pdf not installed, writing JPEG instead" >&2
    cp "$work/page.jpg" "$OUTDIR/scan-$stamp.jpg"
    exit 0
fi

# Readable by anyone who can reach the share.
chmod 0644 "$out" 2>/dev/null || true
echo "handler: wrote $out" >&2
