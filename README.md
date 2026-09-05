# brscan — Open-Source Brother Scanner Driver

Fully open-source SANE backend for Brother MFC/DCP scanners, tested on **Raspberry Pi** and **NanoPi NEO** (ARM). Produces pixel-perfect scans on Brother DCP-1510 without any proprietary binary blobs.

The source code originates from the Brother [download page](http://www.brother.com/cgi-bin/agreement/agreement.cgi?dlfile=http://www.brother.com/pub/bsc/linux/dlf/brscan3-src-0.2.11-5.tar.gz&lang=English_source) and has been significantly cleaned and extended.

## What was done

The original Brother distribution shipped two **proprietary x86-64 binary blobs** (`libbrscandec.so` and `libbrcolm.so`) with no source code. Both have been fully reverse-engineered and replaced with open-source C implementations:

- **libbrcolm** (color matching) — written from scratch by analyzing the original blob's disassembly. Implements 3D trilinear LUT interpolation with optional gamma pre-correction. Verified byte-for-byte against the original blob (21/21 tests).

- **libbrscandec** (scan decoder / resolution changer) — fixed from Ghidra decompilation by verifying every exported function against the original disassembly. Fixed 6 classes of Ghidra decompilation errors. Verified byte-for-byte (6/6 tests across same-reso, upscale, downscale, B&W, color modes).

Additionally, the **DCP-1510 scan protocol** (brscan4) was reverse-engineered from `libsane-brother4.so.1.0.7`:

- **Mono packbits** wire format: `[10-byte wrapper][2-byte LE length][packbits data]`, with the per-block 1-byte status code (`0x80` Page End, `0x81` NextPage, `0x83`/`0xE3` Cancel) appearing inline between frames.
- **24-bit Color** is a single **baseline JPEG stream** framed into per-block wrappers. The driver stages the payload to a temp file and decodes via libjpeg with `out_color_space = JCS_RGB`.
- **EOF detection** propagates the in-stream `0x80` byte to `ProcessMain`'s `GetStatusCode` path so `SCAN_EOF` is raised cleanly (replacing the prior heuristic that lost trailing scanlines).
- **USB session teardown** mirrors the reference `CloseDevice` exactly — `BREQ_GET_CLOSE` then `usb_release_interface(1)`, with no stray `usb_set_altinterface(0)` between them. This eliminates the firmware-side `BCOMMAND_RETURN=0x80` that previously required a power cycle between scans.
- USB endpoint mapping (EP 0x85 IN / EP 0x04 OUT) and I-command response parsing with variable-length headers.

## Supported Models

Tested on **Brother DCP-1510** and **Brother DCP-7060D**. Should work on other Brother MFC/DCP models listed in `data/Brsane.ini`. Models with `seriesNo >= 10` use the brscan4 protocol with the new line framing format.

To add a model, append a line to `data/Brsane.ini` under `[Support Model]`:

```
0x0249,14,2,"DCP-7060D"
```

The fields are product ID, `seriesNo`, model type (1 = MFC, 2 = DCP) and name. Matching is an exact vendor/product ID comparison, so an unlisted ID is simply invisible to `scanimage -L`. Note the optional trailing endpoint columns are parsed with `%d`, i.e. **decimal** — writing `0x85` there silently yields `0`. For `seriesNo` 14 the endpoints (IN `0x85`, OUT `0x04`) are already selected by `ChangeEndpoint[]`, so leave those columns off.

## Prerequisites

```
sudo apt install libsane-dev libusb-dev libjpeg-dev libncurses-dev pkg-config cmake gcc
```

`libusb-dev` is the **0.1** API (`usb.h`), which is what the backend actually calls; `libusb-1.0-0-dev` is additionally required for the CMake `FindLibUSB` probe to succeed.

## Building & Installing

```
mkdir build
cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr ..
make -j4
sudo make install
sudo sh -c "echo brother >> /etc/sane.d/dll.conf"
```

Pre-built binaries for amd64, arm64, and armv7 are published as GitHub releases — see [Releases](https://github.com/dmikushin/brscan/releases).

## USB Permissions

Add a udev rule for your scanner (use the appropriate product ID):

```
lsusb | grep Brother
# Bus 004 Device 009: ID 04f9:02d0 Brother Industries, Ltd DCP-1510
```

A ready-made rule covering every Brother device is in `data/60-brother-scanner.rules`:

```
sudo install -m 0644 data/60-brother-scanner.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --action=add --subsystem-match=usb
sudo usermod -aG scanner "$USER"     # log out and back in
```

The rule is deliberately **additive**: it sets only `libsane_matched=yes`, which triggers the distribution's `99-libsane1.rules` to run `setfacl -m g:scanner:rw` on the device node. It does not assign `GROUP` or `MODE`. Brother MFPs are printer-class devices whose node is already owned by group `lp` for CUPS, and reassigning it risks disturbing printing on a combined print/scan device; an ACL grants scanner access without taking anything away.

Note `--action=add` on the trigger: the rule matches `ACTION=="add"`, and `udevadm trigger` defaults to `change`, so without it nothing is applied to an already-enumerated device.

## Scanning

Verify the scanner is detected:

```
scanimage -L
# device `brother:bus4;dev1' is a Brother DCP-1510 USB scanner
```

Scan a full A4 page in grayscale:

```
scanimage --mode "True Gray" --resolution 200 -x 210 -y 297 --format=pnm > scan.pnm
```

Scan a full A4 page in 24-bit color:

```
scanimage --mode "24bit Color" --resolution 200 -x 210 -y 297 --format=pnm > color.pnm
```

Available scan modes: `Black & White`, `Gray[Error Diffusion]`, `True Gray`, `24bit Color`

## Press-to-Scan (`brscan-skeyd`)

Brother's `brscan-skey`, the daemon that reacts to the SCAN key on the device's own panel, ships only as an i386/amd64 binary with no source, so it cannot run on ARM. `brscan-skeyd` replaces it.

### How the key event is delivered

The device **never pushes** the event. Capturing the vendor interface's bulk IN (`0x85`) and interrupt IN (`0x89`) endpoints across button presses yields nothing at all — the interrupt endpoint exists but is a red herring. Instead the host polls EP0 with a vendor control transfer, recovered by disassembling Brother's `brscan-skey-exe` (which is unstripped) and confirmed against a DCP-7060D:

```
usb_control_msg(h, 0xC0, 0x03, 0, 0, buf, 0xFF, timeout)
```

`bRequest = 0x03` sits alongside the two request codes the backend already knew, `0x01` (open) and `0x02` (close). The reply is:

| Offset | Value |
|---|---|
| 0 | reply length (4 idle, 9 with an event) |
| 1 | `0x10`, descriptor type |
| 2 | `0x03`, request echoed |
| 3 | status: `0x00` idle, `0x10` menu active, `0x20` selection pending |
| 4 | function code for the chosen destination |

Three behaviours are easy to get wrong, and each caused a real bug here:

- **The status is a level, not a pulse.** After a scan the device re-asserts the same event, so it must be drained or one press scans repeatedly.
- **The transaction must be completed by actually scanning.** Consuming the event without starting a scan leaves the panel stuck on "Connecting to PC", and no further key presses are reported until the operator presses Stop. The event survives across daemon restarts until a scan happens.
- **Never claim a USB interface while polling.** EP0 needs no claim, and claiming interface 1 locks out the scan itself. The daemon keeps a device handle open across polls (reopening per poll, as Brother does, missed presses) but closes it for the duration of each scan.

The function codes are **not** the `APPNUM` values Brother uses in its network protocol (1=IMAGE, 2=EMAIL, 3=OCR, 5=FILE) — codes outside that set occur on USB. Treat them as opaque and model-specific; log what your device sends.

### Usage

```
brscan-skeyd --handler /usr/libexec/brscan-skeyd/handler.sh --verbose
```

The handler runs under `/bin/sh` with `BRSCAN_SKEY_CODE` set (and `BRSCAN_DEVICE` if `--device` was given). The bundled `handler.sh` scans one page and writes a PDF. A systemd unit is in `skeyd/brscan-skeyd.service`.

### Keeping it quick

On a 700MHz ARMv6 the press-to-PDF round trip is about 17s for a 300dpi colour A4 page, of which the scan itself is 16.6s. Two things that are easy to get wrong dominated the rest, and both are handled by the bundled handler:

- **Discovery loads every SANE backend.** A stock `/etc/sane.d/dll.conf` lists ~80, and `net`, `escl` and `airscan` block on network probes: `scanimage -L` took 10.4s wall for 1.8s of CPU. The handler points `SANE_CONFIG_DIR` at a one-line backend list, which takes 0.1s. This affects discovery only — opening a device by name was never slow.
- **PDF assembly costs more than it looks.** `img2pdf` is correct and does not re-encode, but spends 4.9s of its 5.8s importing PIL and pikepdf. `brscan-jpeg2pdf`, installed alongside the handler, wraps the JPEG in a single-page PDF directly (identical `MediaBox`, no decode) in 0.05s. `img2pdf` remains the fallback.

The 3s pause after the start-scan command is not worth removing — the device takes that long to produce data anyway. The 2s pause after close *is* load-bearing: without it a scan started immediately after another fails with an I/O error.

The daemon and the SANE backend interlock through a SysV semaphore keyed by `ftok("/var/lib/brscan/skey.lock", 'b')`, so a poll cannot collide with an in-progress scan. Brother derived its key from a path that only exists with the proprietary package installed; when that binary is absent the backend now falls back to the same key this daemon uses, instead of silently disabling the interlock.

## Tests

```
cd build
gcc -o test_integration ../tests/test_integration.c -ldl -lm
./test_integration ./libbrscandec.so.1 ./libbrcolm.so.1 ../libbrcolm/GrayCmData/YL4FB/brlutcm.dat
# === Results: 65 passed, 0 failed ===
```

Tests cover: brscan4 frame structure, packbits decompression (including ARM `signed char` edge cases), ScanDecOpen parameter computation, full decode pipeline, color matching, and end-to-end JPEG color decode (`tests/test_color_jpeg.c`).

## Debug Logging

Set `BROTHER_DEBUG=1` to enable verbose logging to stderr:

```
BROTHER_DEBUG=1 scanimage --mode "True Gray" --resolution 200 -x 210 -y 297 > scan.pnm
```

## Architecture

```
libsane-brother.so.1    SANE backend (talks USB, parses scan protocol)
  ├── libbrscandec.so.1  Scan decoder: packbits decompression, resolution scaling
  ├── libbrcolm.so.1     Color matching: 3D LUT interpolation from .dat/.cm files
  └── libjpeg            24-bit color decode (JPEG-over-USB for brscan4)
```

## Known Gaps

Unmapped panel function codes, missing replay fixtures for the key-decode path,
and the open question of whether the DCP-7060D platen is A4 or Letter width are
tracked in [TODO.md](TODO.md).

## License

GPL v2 (original Brother license preserved).
