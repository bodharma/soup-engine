# B&W JPEG experiment — EOS 5D Mark II

Target: Canon firmware **2.1.2**. This is an experimental camera build,
not a measured reproduction of Fujifilm ACROS.

Canon's native **Monochrome Picture Style** supplies the black-and-white
conversion and tone. Soup adds **grain, neutral halation and bloom** to the
camera JPEG, then updates that JPEG at its original DCIM path. The intended
result is a finished JPEG available in Canon PLAY without computer editing.

## First test

1. Select JPEG recording in Canon's menu. For the first disposable test,
   **Small/Fine** keeps the native 2784×1856 dimensions and reduces the
   amount of card I/O. Large JPEGs currently become 2784×1856 after Soup
   processing; Medium JPEGs retain 4080×2720. This build does not remove
   that existing resolution limit.
2. Set Soup Enable OFF, then open **Shoot → Soup Film → B&W test**. This selects native Monochrome
   and the effects-only experiment. It must confirm the camera setting;
   a busy camera or failed confirmation must leave processing disabled.
3. Check **Mode → Effects only**, then review the controls under **Effects**.
   Canon's contrast, sharpness and monochrome filter remain normal Canon
   Picture Style settings. Start with toning OFF so the base is neutral.
4. Take one disposable photo containing a bright window, lamp or reflection,
   some midtone texture and a dark area. Release the shutter and let the
   card-access light finish. Check Soup's Status after processing.
5. Open PLAY and magnify the photo. Native Monochrome alone already makes
   it B&W: look for the added grain and highlight spill, and check the
   processing status before treating the effects as successfully applied.

## Effects

| Control | Purpose | First-test defaults |
| --- | --- | --- |
| Grain strength / size | Amount and coarseness of the simulated grain | 20 / 2 pixels |
| Halation strength / radius | A compact neutral glow around highlights | 15 / 3 pixels |
| Bloom strength / radius | A broader neutral highlight glow | 8 / 6 pixels |

In this first experiment both glows are neutral, preserving B&W. They are
two independently adjustable highlight spreads, rather than a coloured
halation fringe. Grain is added after the glows. Its pattern varies with
the capture number while remaining repeatable when the same shot is retried.

Effects-only bypasses Soup's curves, hue corrections, saturation, split
toning and chroma rolloff. It also works with a colour Canon Picture Style;
the B&W test action is the explicit way to select Monochrome. The older
**Film stock** mode remains available for the existing Soup stock files.

The B&W test action resets these six effect controls to the first-test
defaults and enables Soup. To keep customised values on later camera
starts, use ordinary Enable instead. Enable starts OFF on every boot;
the effect controls and selected processing mode are saved normally.

With all three effect strengths at zero, effects-only leaves the photo
bytes alone. With effects enabled, JPEG decoding and recompression still
occur; output is not a lossless copy of Canon's original JPEG.

## What still needs camera verification

The earlier build displayed “developing” without a visibly updated photo.
Its latest diagnostic must be read before installing this experiment.
An effects-only path does not establish the cause of that earlier failure.
The worker requests Magic Lantern's standard temporary auto-power-off
protection during processing. It saves diagnostic checkpoints after backup,
after rendering and before publication, as well as the final timing summary.
These changes improve interruption handling and diagnosis; they do not prove
that automatic power-off caused the earlier failure.
Measured camera processing time, native menu/Monochrome confirmation,
automatic PLAY updates and complete temporary-file cleanup need an actual
camera capture. Host tests cannot establish these hardware behaviours.

Originals remain temporary until the indexed update verifies successfully,
then Soup removes its owned original/stage/render files. Failed updates
retain recovery material. Do not delete it to retry a failed shot.

Do not start a 10–20-photo burst as the first test. The backlog is not
persistent across Disable or restart, and burst behaviour is unqualified.
See [the port notes](5D2-port.md) for the existing storage and recovery limits.
