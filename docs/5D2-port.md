# Experimental EOS 5D Mark II port

Target: Canon firmware **2.1.2**, with Magic Lantern source pinned in
`integrations/magiclantern/5D2.212/SOUP-PIN.txt`. This version updates existing
camera playback images after processing. Its new playback and recovery paths
have not been validated on a physical camera.

## Shooting and playback

1. Highlight `Shoot → Soup Film` and press SET to open its controls. `Enable`
   starts OFF on every boot; set it ON before taking a new photo. `Stock` opens
   the film-name picker. On the 5D Mark II, Pict.Style returns from that picker
   to the Enable/Stock controls. Classic Chrome is the initial stock. Use sRGB
   and a colour Canon Picture Style; a monochrome source cannot regain colour.
2. Full RAW uses its embedded JPEG; JPEG-only uses the camera JPEG. RAW+JPEG
   prefers the JPEG and applies one look to both the indexed JPEG and the
   CR2's main JPEG preview and thumbnail.
3. Byte-exact originals are retained at
   `A:/ML/SOUP/ORIGINALS/<DCIM-folder>/<original-basename>`. Existing backups
   are never overwritten. Updated files keep their original DCIM paths, so
   no new filename or image-library registration is required.
4. Canon Large images decode at half size and are centre-cropped from
   2808×1872 to the native Small size, 2784×1856. Medium 4080×2720 and Small
   2784×1856 JPEGs retain their native dimensions. Real styled 160×120 JPEG
   thumbnails include black letterboxing. Full RAW is supported when its
   embedded JPEG is 5616×3744; unrecognised geometries, including typical
   sRAW1 previews, are skipped without changing the source.
5. PLAY should show the style after processing completes. A best-effort
   reload is requested only if the camera is already displaying a photo in
   PLAY and is idle. Soup never forces PLAY while shooting. The automatic
   two-second review may finish before rendering; cache behaviour, actual
   camera decoding and processing time still require hardware testing.

## File updates and recovery

Soup first waits for Canon's capture job to become idle and for both possible
source sizes to remain unchanged for one second. Before creating backups it
checks exclusive-create semantics on a disposable private probe file. If the
camera does not honour exclusive creation, processing stops and reports that
backups cannot be safely created. The probe never touches a photo.
New files use the 5D2.212 firmware's native create/exclusive flags (`0x100`
and `0x400`), verified from its file-opening code; ML's generic POSIX flag
values do not match this camera.

Each member of a RAW+JPEG pair gets an immutable backup, verified by complete
byte comparison. Candidate images are prepared in
`ML/SOUP/STAGED/<DCIM-folder>/`. A complete generated JPEG (`.NEW`) is decoded
again, its embedded styled thumbnail is checked, and encoded versus read-back
CRC32 values must agree. Staged copies are checked against this candidate and
the original backup before indexed files can be changed.

During the indexed update and any recovery, Soup requests a broader Canon UI
lock, rechecks capture state and identities, and refuses to proceed if the
lock is unavailable. This is a hardware-dependent UI exclusion mechanism,
not a filesystem mutex. Normal busy collisions defer the job rather than
consuming a failed-frame retry.

JPEG updates retain the original file length and allocation: the new JPEG
must fit, with bounded quality reductions if necessary. Bytes after its new
EOI remain from the original file and are ignored by tested computer JPEG
decoders. **Canon's acceptance of this trailing-data shortcut is unverified.**
Orientation and available date information are carried into generated EXIF;
JPEG-only exposure/GPS and proprietary metadata are not all copied. The
byte-exact backup retains all original metadata.

A staged CR2 keeps its original size and offsets. Only its existing main JPEG
preview, 160×120 thumbnail, and their dimension/count fields may be patched.
The compressed RAW sensor payload is never opened for writing through those
ranges. All bytes outside the six validated patch spans must equal the backup,
including the sensor payload and the separate IFD2 RGB preview. IFD2 is left
unchanged because it is not a display-ready sRGB image. Unsupported or
non-fitting candidates leave the indexed photo unchanged.

Before the first indexed write, Soup closes and reads back a checksummed
`ML/SOUP/RECOVER.BIN` receipt containing the pair membership, canonical paths,
lengths, CRC32 values and allowed patch spans. Indexed files are opened for
bounded updates; the destructive `FIO_CreateFile` and unsafe rename fallback
are never used on them. Backups are never deleted.

If an update fails, both members are rolled back from their verified backups.
A pending receipt is checked at the next startup **even with Enable OFF and
without film-stock files**. Recovery waits for an idle camera, validates the
receipt, backups and target identity, restores the pair, and verifies full
byte equality before removing the receipt. Interrupted rollback can be retried.
An invalid receipt, mismatching backup or uncertain target is preserved for
review; indexed processing is disabled with a recovery notification.

**Close/reopen and byte comparison establish logical verification, not proven
write durability.** Canon's `O_SYNC` behaviour and CF ordering are unverified;
there is no proven flush/barrier API here. Power loss, removal or a damaged
card can still corrupt files or backups. Recovery is best effort, and backups
on the same card do not replace an off-card copy.

Existing photos are not backfilled when Enable is switched on. Staged copies
are retained as completion evidence, so repeat processing does not apply the
look twice. After three permanent processing failures a frame is skipped with
an on-screen error; a recovery problem disables processing instead. Restarting
or disabling Film can leave queued originals unprocessed.

## Local build and tests

With an Arm GNU `arm-none-eabi` toolchain available:

```sh
ARM_BINPATH=/path/to/toolchain/bin sh src/tools/build-5d2.sh
```

The script obtains the pinned ML source if needed, applies the 5D2 overlay,
and builds `autoexec.bin` plus matching `5D2_212.sym`. It never formats a card,
changes a boot flag, flashes Canon firmware or copies files to a card. Keep
matching binary and symbols together.

The real file-processing and transaction functions can be tested locally with
independent camera fixtures:

```sh
sh integrations/magiclantern/5D2.212/tests/run-host-tests.sh /absolute/sample.CR2 /absolute/sample.JPG
```

Host checks exercise RAW/JPEG/pair processing, full JPEG and styled-thumbnail
decoding, byte-exact backups, RAW non-patch byte preservation, exclusive-create
failures, bounded I/O, staged corruption, interrupted publication and repeated
rollback, and receipt validation. Native worker directory/prefix/cursor
transitions and physical Canon UI exclusion have not been executed by these
tests. Film names describe approximations, not measured Fujifilm-equivalent
colour.
