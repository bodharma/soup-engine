# Experimental EOS 5D Mark II port

Target: Canon firmware **2.1.2**, Magic Lantern source pinned in
`integrations/magiclantern/5D2.212/SOUP-PIN.txt`. This is a source/build
prototype, not a hardware-validated release.

## Behaviour

1. `Shoot → Soup Film → Enable` starts OFF on every boot. Enable before shooting.
2. New RAW-only shots use the CR2's embedded colour JPEG. JPEG-only shots use
   the JPEG directly. RAW+JPEG pairs prefer the JPEG and produce one output.
3. A separate half-size film JPEG is saved as
   `A:/ML/SOUP/<DCIM-folder>/<original-prefix><number>.JPG`. Original CR2/JPEG
   files are opened read-only and never rewritten or deleted. Existing outputs
   and stale temporary files are left alone.
4. The stock selector reads `ML/FILM/INDEX.TXT`. The first default is Classic
   Chrome. Use sRGB and a colour Canon Picture Style; a monochrome source
   cannot regain colour.

The current file is selected only while Canon's last job state is idle, and
both possible source sizes must stay unchanged for one second. Input JPEG
EOI markers, preview offsets and lengths, dimensions, allocations, read/write
counts and final file size are checked. Completed temporary files are copied
with checked, bounded I/O; the unsafe upstream 5D2 rename fallback is not used.
JPEG pixels are decoded and encoded
in strips using a bounded arena; no whole-image RGB allocation is made.

Existing photos are not backfilled when Film is enabled. The worker finishes
pending numbers in the previous folder before switching to a new folder or
filename prefix. Restarting or disabling Film can leave queued originals
unprocessed; source files remain intact. After three failed attempts a frame
is skipped with an on-screen error so later frames can continue. A stale
`.TMP` or incomplete existing JPEG is preserved for review and causes that
frame to be skipped; existing files are never overwritten.

## Local build

With an Arm GNU `arm-none-eabi` toolchain available:

```sh
ARM_BINPATH=/path/to/toolchain/bin sh src/tools/build-5d2.sh
```

The script checks out the pinned ML source if needed, applies the small
5D2 overlay, and builds `autoexec.bin` plus matching `5D2_212.sym`. It never
formats a card, changes a boot flag, flashes Canon firmware or copies files
to a card. Keep build outputs together; symbols must match their binary.

The actual camera conversion and file-handling routines can be exercised
on a computer with independent camera fixtures:

```sh
sh integrations/magiclantern/5D2.212/tests/run-host-tests.sh /absolute/sample.CR2 /absolute/sample.JPG
```

The checks cover RAW/JPEG input, one-output pair handling, source bytes
remaining unchanged, full output decoding, incomplete files, busy/changing
sources, short/failed reads, failed writes and bounded failed-frame retries.
They execute the shared file pipeline and retry decision. The native worker
loop, directory/prefix transitions and cursor movement have not been executed
by the host tests or validated on hardware.

## Unverified limits

- No physical 5D Mark II boot, shooting, burst, shutdown or card-removal test
  has been performed. Experimental software may hang the camera.
- Output is half the decoded JPEG's linear dimensions, not full 21 MP.
  Output in `ML/SOUP` is for computer viewing; in-camera playback is not
  promised, and the Canon playback preview remains the original image.
- Baseline JPEG only. The current streaming decoder rejects restart-interval,
  progressive and arithmetic-coded JPEGs. Unsupported files are preserved.
- JPEG-only output currently adds camera/look tags but does not copy the
  original exposure, date, GPS or orientation metadata. CR2 input copies the
  metadata fields supported by Soup's existing parser.
- Film names describe approximations, not measured Fujifilm-equivalent colour.
