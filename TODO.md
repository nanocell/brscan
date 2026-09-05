# TODO

Known gaps and loose ends. Each entry says what is missing and, where it matters,
why it was left rather than fixed.

## Press-to-scan (`brscan-skeyd`)

**Map the panel function codes.** Only one code is confirmed: the byte at offset 4
of the status reply when a SCAN destination is chosen on a DCP-7060D. Two other
values (`0x05`, `0x08`) were seen during probing but never attributed to a
specific menu item, so `handler.sh` currently runs the same pipeline for every
destination — "Scan to File", "Scan to Image" and "Scan to E-mail" all produce
the same PDF in the same folder. Mapping them is slow work: the device reports
only one event until a scan actually completes (see the "Connecting to PC" note
in the README), so it is one press per probe run, and the codes are
model-specific — a table for one model must not be assumed to hold for another.

Until they are mapped, treat the code as opaque and log it; `BRSCAN_SKEY_CODE`
is already exported to the handler so a user can branch on it locally.

**No replay tests for the key-decode path.** `tests/eop_hang/usb_stub.c` already
fakes libusb-0.1 and would host fixtures for the status reply, giving offline
regression cover for the drain-until-idle logic and the status/function decode.
Worth adding before upstreaming.

Note what such tests would *not* have caught: the USB read-size bug fixed in
`brother_brscan4.h` lived below the layer the stub replaces, so a replay
harness would have passed happily while every real scan came out blank. Offline
fixtures cover protocol decoding, not transport.

## Scan geometry

**`BRSCAN_HEIGHT_MM=291` is a workaround in the wrong place.** The DCP-7060D
advertises a 297mm A4 platen but its carriage stops around 291mm, and the driver
pads the remainder white. Trimming it from the handler's environment works, but
it belongs in the model configuration — as an optional trailing column in
`data/Brsane.ini` or a per-series clamp in `brother_modelinf.c` — so that plain
`scanimage` users get a correct page too, not just users of this handler.

**Is the platen A4 or Letter?** The device reports `maxScanPixels=2480`,
`maxScanWidth=209mm` and paper size A4, and a full-width scan measures exactly
210.0mm with no right margin. If the physical glass is in fact Letter width
(215.9mm), roughly 6mm of a Letter original is being clipped and the firmware
figure is the limit rather than the truth. Resolving this needs someone to
measure the glass; overriding a device-reported width on a hunch would be worse
than the current behaviour.

**The corrected geometry has not been confirmed through the button path.** The
A4 height fix and the 291mm trim were both verified with direct `scanimage`
runs. The button path was verified before those changes landed, so a press
should now produce a full-bleed A4 PDF with no white band — but that exact
combination is unverified.

## Speed

Measured press-to-PDF on the reference Pi (700MHz ARMv6), 300dpi colour A4:

| Stage | Before | After |
|---|---|---|
| Device discovery (`scanimage -L`) | 10.4s | 0.1s |
| Scan (device + decode + re-encode) | 16.6s | 16.6s |
| JPEG → PDF | 5.8s | 0.05s |
| **Total** | **~33s** | **~17s** |

**Pass the device's JPEG through instead of decoding and re-encoding it.**
The remaining 16.6s contains 6.6s of CPU that does no useful work: the backend
decodes the device's JPEG to RGB with libjpeg, and `scanimage --format=jpeg`
immediately re-encodes that RGB back to JPEG. Besides the time, this costs a
generation of quality for nothing.

This looks tractable because the hard part is already done. `brother_color.c`
collects the whole page into a temp file during its COLLECTING phase and only
then decodes — so a complete baseline JPEG file for the page already exists on
disk before any decoding starts. SANE has a frame type for exactly this,
`SANE_FRAME_JPEG` (0x0B, "complete baseline JPEG file").

What stops it being a small change: `SANE_FRAME_JPEG` is an extension beyond
the five frame types in the core enum, frontend support for it is uneven, and
returning it unconditionally would break every frontend that expects RGB. It
would have to be opt-in, and whether stock `scanimage` writes such a frame out
verbatim is unverified. Worth measuring before committing to it.

**Two sleeps in the scan path were investigated and deliberately left alone.**
The 3s after the start-scan command (`brother_scanner.c`) costs nothing:
removing it made no measurable difference, because the device takes that long
to produce data regardless and the read simply blocks instead. The 2s after
close (`brother_devaccs.c`) is load-bearing on this model, not just the
DCP-1510 it was written for — with it removed, a second scan started
immediately after the first fails with `sane_read: Error during device I/O`.

It could still be moved off the critical path by deferring it: record the close
time and wait at the *next* open only if less than 2s has passed, so the PDF is
delivered 2s sooner and back-to-back scans stay protected. That needs the
timestamp shared between processes (each `scanimage` run is a separate
process), which means a file somewhere writable by every scanning user — and if
that write silently fails the protection is lost and scans start erroring. Not
obviously worth 2s of a 17s budget.

## Driver

**Re-test the DCP-1510 partial-scan quirk.** `brother_scanner.c` carries an EOF
fallback for a firmware quirk where the DCP-1510 "sometimes ships ~184/196 lines
and goes silent" on back-to-back scans. That signature — a scan that starts,
delivers part of a page and stalls — is exactly what the `0x7FFF` read size
produced on the DCP-7060D, where the real cause was one byte lost per bulk read
desyncing the record parser. The two may be the same bug. The fallback should
stay either way (it is a safety net, not a fix), but if it never fires again on
DCP-1510 hardware the comment attributing it to firmware should be corrected.

**The semaphore fallback is untested against the real thing.** When Brother's
proprietary `brscan-skey` is absent the backend now derives the interlock key
itself via `ftok`, which is the only path that has been exercised. The `popen`
path that asks the vendor binary for its semaphore ID was kept so nothing
regresses for users who do have it installed, but no such system was available
to confirm that.

## Packaging

**Single page per press.** The DCP-7060D is flatbed-only with no sheet feeder
and no paper sensor, so each press yields a one-page PDF. Multi-page jobs would
need either a hold-open window that appends further presses to the same document
or an ADF model to test against.
