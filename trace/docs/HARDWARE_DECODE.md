# Hardware-accelerated decode

What it does, why it is off by default, and — first, because it changes how to
read everything else here — what has not been run.

---

## 0. None of this has run on a GPU

The machine TRACE was built on has no accelerator of any kind. Every hardware
type FFmpeg knows about was probed and every one failed to open:

```
vaapi          VA-API (Intel / AMD on Linux)     unavailable — Generic error in an external library
cuda           NVIDIA CUDA / NVDEC               unavailable — Operation not permitted
qsv            Intel Quick Sync                  unavailable — Generic error in an external library
vdpau          VDPAU                             unavailable — Unknown error occurred
drm            DRM                               unavailable — Bad address
opencl         OpenCL                            unavailable — No such device
vulkan         Vulkan                            unavailable — Generic error in an external library
```

So: the enumeration, the fallback, and the guarantee that requesting hardware on
a machine without it changes nothing are **executed and tested**. The accelerated
decode path itself — device attach, format negotiation, frame transfer — is
**written and unexecuted**.

**No performance figure appears anywhere in this codebase or these documents,**
because none has been observed. Not a frame rate, not a multiplier, not a
comparison against software decode. Hardware decode is normally much faster;
TRACE does not say by how much, on what, because it has not measured it.

---

## 1. What it is for

Software decode of a 4K recording runs at a few frames a second on one core. An
analyst scrubbing through six hours of it, or an analysis pass sampling every
frame, is waiting on the decoder and nothing else. Handing that to a GPU or a
fixed-function decoder is the difference between a review that happens and one
that does not.

---

## 2. Why it is off by default

Not caution about speed. A hardware decoder is a **different implementation of
the same standard**.

H.264 and HEVC are specified so that conformant decoders produce bit-exact
output, and in practice hardware decoders generally are conformant. But
"generally" and "this chip, with this driver version" are different claims, and
TRACE cannot verify the second from inside itself. If a hardware path produced
even slightly different pixels, then:

- a frame exported as an exhibit would depend on which decoder happened to be
  enabled on that workstation, and
- two analysts examining the same recording could be looking at different images
  with nothing in the record to explain the difference.

That is not a risk worth accepting by default in exchange for speed. So the
setting is off until an operator turns it on, and three things make that a
decision they can actually reason about:

1. **Exported exhibits never use it** (§4).
2. **Everything records which decoder produced it** (§5).
3. **TRACE ships the check** rather than asking to be trusted (§6).

---

## 3. What the setting does

Settings → *Acceleration and analysis* → **Use hardware-accelerated decoding**.

The checkbox is enabled only when a device actually opened on this machine. When
none did, it is disabled and the note lists every accelerator that was tried and
the error each returned — because "unavailable" on a workstation that has a GPU
is usually a driver or permissions problem the operator can act on, and a greyed
checkbox alone would not tell them that.

Enumeration works by **opening each device**, not by asking what FFmpeg was
compiled with. An FFmpeg build contains nearly every accelerator it can compile,
so a list drawn from the build would offer hardware the machine has not got.

---

## 4. Where it is used, and where it deliberately is not

| | Decoder |
|---|---|
| Playback in the viewer | Accelerated when enabled |
| Analysis sampling | Accelerated when enabled |
| **Frame export ("Save current frame")** | **Always software** |
| **Report and exhibit frames** | **Always software** |
| Thumbnails | Always software |

The split is the point. Playback is a *view* — a difference of one pixel value
costs nothing there, and it is the place an accelerator earns its keep. An
exported frame is *evidence*, and it has to be reproducible on any workstation
regardless of what hardware happens to be in it.

**A consequence worth stating plainly:** if a hardware decoder on some machine
were not bit-exact, the frame an analyst sees during accelerated playback could
differ very slightly from the frame that same moment exports to. This is why §6
exists, and why accelerated playback should be confirmed against software on
your own footage before it is relied on for close visual examination.

---

## 5. What gets recorded

- **Every exported frame's provenance** carries `"decoder": "software"` or
  `"decoder": "hardware:<device>"`.
- **`DecoderStreamInfo::hardwareDevice`** reports what actually decoded, never
  what was requested. A request that fell back reads as software, because that
  is what happened.
- **An analysis run** records `decoder_requested` in its configuration, and adds
  a warning when the accelerator it asked for did not take the file — the run
  still completes and its results are sound, but nobody should be left with the
  impression it ran on hardware.

A fallback is an ordinary condition on a mixed fleet of workstations, not an
error worth refusing to play a file over. It is logged, recorded, and otherwise
invisible.

---

## 6. Checking it on your own hardware

`hwaccel::verifyMatchesSoftware(file, device, frameCount)` decodes the same
frames both ways and compares them byte for byte. It reports:

- how many frames were compared and how many were identical;
- the **largest** absolute difference of any single channel value;
- the **mean** absolute difference — reported alongside the maximum because one
  stray pixel and a uniformly shifted image are very different findings.

`identical()` is true only when every compared frame matched exactly.

The integration test `HardwareDecodeOnRealHardware.DecodedFramesMatchSoftwareOrTheDifferenceIsReported`
runs this against every available device. On a machine with no accelerator it
skips; on one with a GPU it does real work and **fails with the measured
difference** if the two disagree. It is written to be skipped here and to be
useful elsewhere, rather than left out because this machine cannot run it — a
test that does not exist is not going to be written later.

Run it against your own footage, not only the sample clip: codec, profile and
bit depth all affect which path a decoder takes internally.

---

## 7. How it works

1. `hwaccel::devices()` probes every `AVHWDeviceType` by calling
   `av_hwdevice_ctx_create`, once per process, and caches the result.
2. `preferredDeviceFor(codec)` picks an available device that also has an
   `AVCodecHWConfig` for this codec — a machine can have a working VA-API device
   and no VA-API support for the codec in hand.
3. `Session::attach` opens the device and sets `hw_device_ctx` and a
   `get_format` callback **before** `avcodec_open2`. On failure it changes
   nothing, so the caller opens the same context for software decoding without
   rebuilding it. A fallback that needed a second allocation would be a fallback
   nobody trusted.
4. `get_format` returns the hardware format when FFmpeg offers it and the first
   software format otherwise. Returning a format outside the offered list aborts
   the decode, so this is the one place that must not be clever.
5. `Session::transfer` moves each frame into system memory with
   `av_hwframe_transfer_data` and copies its timestamps across with
   `av_frame_copy_props` — timing lives on the hardware frame and does not
   travel with the pixels. A transfer failure is a **hard error**: quietly
   returning the previous frame would show an analyst one moment of a recording
   while telling them it was another.

The device context is declared before the codec context in the decoder's
internals so it is destroyed after it; releasing the device first would leave
`avcodec_free_context` walking a dangling reference.

---

## 8. What is verified, and what is not

**Verified by test:**

- Enumeration answers for this machine rather than for the build, and every
  unavailable device says why.
- The device list is cached, so the settings dialog and the decoder cannot give
  different answers.
- Requesting an accelerator that is absent, or naming one that does not exist,
  never stops a file playing.
- A decoder that fell back does **not** report a hardware device — the
  provenance story depends entirely on this.
- Frames decoded through the fallback are identical, byte for byte, to frames
  from a plain software open.
- The comparison harness refuses to return a result when there was no
  accelerator to compare against, rather than reporting "identical" from a
  comparison that never ran.

**Not verified:**

- **The accelerated path has never executed.** No device attach, no format
  negotiation, no frame transfer has run anywhere.
- **No throughput measurement of any kind**, on any hardware.
- **No claim that any hardware decoder is bit-exact.** §6 is the mechanism for
  finding out; TRACE asserts nothing about the answer.
- Windows and macOS accelerators (`d3d11va`, `dxva2`, `videotoolbox`) are coded
  and have never been built against, since CI builds Windows without running
  hardware and there is no macOS job at all.
