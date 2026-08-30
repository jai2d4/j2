# Audio

Phase 3. What TRACE does with the sound on a recording: how it is decoded, how the
waveform is produced and stored, how playback is kept in step with the picture, and —
in the last section — exactly which of those claims has been verified and which has not.

---

## 1. Decoding

`media/ffmpeg/audio_decoder.{h,cpp}` — no Qt.

Audio is decoded with libavcodec and converted through libswresample to **interleaved
signed 16-bit PCM**, because that is the one format every output backend accepts.
`AudioDecoder::open(file, sampleRate, channels)` takes the rate and channel count the
caller needs and does the conversion once, in the decoder.

`AudioStreamInfo` reports the source and the output **separately**:

| Source (what was recorded) | Output (what this decoder produced) |
|---|---|
| `codecName`, `codecLongName` | — |
| `sourceSampleRate`, `sourceChannels` | `sampleRate`, `channels` |
| `sourceSampleFormat`, `sourceBitRate` | — |

That separation is deliberate. An analyst looking at the inspector needs to see that a
recording is mono 8 kHz telephone-quality audio even while TRACE is playing it back as
stereo 48 kHz. Collapsing the two would let a resampled copy be mistaken for the
original's own properties.

Timestamps are microseconds, normalised so the first block of the stream is zero — the
same convention video frames use, which is what lets the two line up.

An item with **no audio track** fails with `ErrorCode::NotFound` and the message
*"This item has no audio track"*. Callers treat that as "nothing to draw" or "nothing to
play", never as a broken file.

The managed original is opened read-only. Decoding audio reads evidence and writes
nothing.

---

## 2. The waveform

`media/audio/waveform.{h,cpp}` and `waveform_service.{h,cpp}` — no Qt.

### What is stored

A `Waveform` is a fixed number of evenly spaced buckets across the whole item, with two
series per bucket:

- **`peaks`** — the loudest sample in the bucket. This is what makes a transient
  visible: a door, a shout, a gunshot.
- **`rms`** — the average energy. This is what tracks how loud a passage actually
  sounds.

Both are kept because they answer different questions. Drawing peaks alone exaggerates
an isolated click into a wall; drawing RMS alone hides it entirely.

### Not normalised

Values are 0–1 **relative to full scale**, not relative to the loudest point in this
file. A quiet recording draws quiet.

This matters more than it looks. Every consumer audio editor normalises its display,
because for music the shape is what you want. For evidence, *how loud the audio is* is
itself information about the recording — whether a microphone was clipping, whether a
room was silent, whether one passage is louder than another in the same file. A
normalised envelope silently destroys that comparison. The stored provenance records
`"normalised": false` so a reader of the derived asset can tell.

### Fixed bucket count

`buckets` is a count, not a duration. A thirty-second clip and a three-hour recording
both produce 2000 points and both fit the same timeline row. The widget maps each pixel
column back through the timeline's own time axis, so the envelope stays correct under
zoom and scroll without being rebuilt.

Analysis is done at **mono 8 kHz** — enough to place energy in time, and a great deal
cheaper than decoding at full rate for a picture that is a few hundred pixels wide.

### As a derived asset

`WaveformService::ensureWaveform()` builds it **once** and registers it like any other
derivation:

- written into the case's working directory as JSON
- hashed, with the digest stored on the asset
- filed with a `waveform_generation` operation carrying the method, the bucket count,
  the source rate and channel count, `normalised: false`, and the FFmpeg library
  versions in effect
- audited as `DerivedAssetCreated`

Asking again returns the existing asset. It is rebuilt only if the file has gone
missing. A cancelled or failed build registers **nothing** — no partial asset, no
half-written file.

---

## 3. Playback, and which clock is right

`ui/audio/audio_output.{h,cpp}` (Qt) and `media/audio/audio_clock.{h,cpp}` (no Qt).

### The problem

Before Phase 3, video was paced against a steady clock: each frame's presentation
timestamp, measured from an anchor taken when playback started. With no audio that is
exactly right.

It stops being right the moment a sound card is involved. A sound card consumes samples
on its own crystal. It is close to the system clock but not equal to it, and over a long
recording the difference accumulates into visible lip-sync error. It is also the audio a
person notices first: a video frame shown a few milliseconds early passes unremarked, a
click or a stutter does not.

### The answer

**When audio is playing, the device is the reference and video follows it.**

`PlaybackController::setClockSource()` takes a callback returning
`std::optional<Microseconds>`. Before pacing each frame, the engine asks it. If it
answers, the engine re-anchors its steady clock to that position and paces the frame
from there.

Re-anchoring **every frame** is what bounds the error: the two can never drift apart by
more than one frame's worth, and no correction filter or drift estimate is needed.

The callback answers with **nothing** when:

- the item has no audio track
- the machine has no output device
- playback is paused, stepping or stopped
- the speed is not 1.0 (see below)
- the audio track has ended while the video continues

Nothing means the steady clock keeps its anchor, and the engine behaves exactly as it
did before audio existed. A file with no sound plays as it always has.

`AudioClock` holds the arithmetic that turns the device's own count — how much it has
played since it was told to start — into a position on the media timeline. Getting that
offset wrong is what desynchronises a seek, so it lives in a Qt-free class that can be
tested on a machine with no audio hardware. It also clamps the device's count to move
forward only: some backends report a count that jumps backwards around an underrun, and
video must not be dragged back with it. A real backwards move on the media timeline is a
seek, and a seek restarts the clock.

### Speed

**Audio plays at normal speed only.**

Reaching another speed without resampling shifts the pitch. A pitch-shifted voice on a
recording that may be evidence misrepresents it: an operator would hear someone who does
not sound like the person who was in the room. A video frame shown early is still the
frame that was recorded, so video can honestly be sped up. Audio cannot.

So at any speed other than 1.0 the track is **silenced** rather than altered, the clock
stops answering, and the viewer says so in the transport tooltip and in Settings.
Returning to 1.0 brings the track back.

Proper time-stretching (FFmpeg's `atempo`, which preserves pitch) would make other
speeds honest. It needs libavfilter, which is not currently a dependency.

### Seeks

Audio is stopped and started fresh at the new position rather than left running. A
device holding a fifth of a second of already-buffered audio would otherwise keep
playing the passage the operator has just left.

### Volume and muting

Both change what leaves the sound card. Neither touches the recording, the stored hash,
or anything TRACE has recorded about the item. The Phase 3 acceptance test re-hashes the
managed original after moving both controls and asserts the digest is unchanged.

The values persist in `playback.volume` and `playback.muted`.

### Saying which reason

An item with no audio track and a workstation with no sound card both leave the
transport disabled. **An analyst must not read the second as the first.** TRACE reports
them separately:

| Condition | What the tooltip says |
|---|---|
| No audio track in this item | "This item has no audio track." |
| No output device on this machine | "This machine has no audio output device." |
| Track present but undecodable | "The audio track could not be decoded." |

`PlaybackBridge::hasAudio()` and `audioUnavailableReason()` expose the same distinction
to any other caller.

---

## 4. Layering

| Piece | Layer | Qt? |
|---|---|---|
| `AudioDecoder` | `media/ffmpeg/` | no |
| `Waveform`, `WaveformBuilder` | `media/audio/` | no |
| `WaveformService` | `media/audio/` | no |
| `AudioClock`, `audioPlaysAtSpeed()` | `media/audio/` | no |
| `PlaybackController::setClockSource()` | `media/playback/` | no |
| `AudioOutput` (`QAudioSink`) | `ui/audio/` | yes |
| Transport controls | `ui/viewer/` | yes |

The sink is the only Qt piece. Everything that decides *where playback is* sits below
it with no Qt, which is what makes the sync logic testable on a machine with no audio
hardware — and is why waveform generation works on a build agent with no sound card at
all.

### A note on dependencies

`Qt6::Multimedia` brings **`libQt6Network`** into the binary's dependency graph
transitively — it is not a direct link of any TRACE target, and TRACE's own code uses no
network API. Nothing in TRACE opens a socket. (`libcurl` and `libssl` were already
present the same way, via `libavformat`.)

---

## 5. What has been verified, and what has not

This section is the point of this document.

### Verified here

Run `ctest` in the build directory. The audio work is covered by 24 tests in
`tests/integration/audio_test.cpp` (9 decoder, 8 waveform, 5 clock, 2 sync) and 3 in
`tests/integration/acceptance_phase3_test.cpp`.

**Decoding** — source and output format reported separately; whole-track decode with
forward-only timestamps; first block at ~0; three resample targets (48000/2, 44100/1,
16000/1); seek accuracy; seek to origin; no-audio detection; invalid format rejection;
and that decoding leaves the source file's bytes and mtime untouched.

**Waveform** — the envelope spans the whole track; RMS never exceeds the peak that
contains it; bucket count is independent of duration; bucket-count validation;
cancellation; JSON round-trip; malformed file rejection; generated once and registered
with its provenance; no-audio returns `NotFound`.

**The reference clock** — a clock that is not running offers no position; the device's
count is placed correctly on the media timeline; a seek moves the origin rather than
accumulating; a backwards jump from the device does not drag playback back; audio is
rendered only at 1.0×.

**Sync** — with a clock source standing ahead of playback, video reaches three seconds
of media in about 38 ms of wall time (reproducible across runs). With a source that
declines to answer, the media position never runs ahead of the wall time spent reaching
it, which is real-time pacing against the frames' own timestamps. That is the sync
mechanism working, measured — on a fake clock, not a sound card.

**Acceptance** — opening an item with sound builds the waveform as a derived asset,
hashes it, records its provenance, and puts it on the timeline as an envelope row; the
transport reports what this machine can do and why; and video plays, seeks, steps and
changes speed normally.

### Not verified here

**Sound actually leaving a sound card, and staying in step with the picture.**

The container this was built and tested in reports **zero audio devices**
(`QMediaDevices::audioOutputs()` is empty; there is no `/dev/snd`). `QAudioSink` is
therefore never constructed and the buffer-feeding path never executes. Everything above
about the *sink* is written to Qt's documented contract, not measured.

Specifically unverified:

1. That the negotiated format is accepted by a real backend, and that the fallback chain
   picks a working one when the device's preferred format is not Int16.
2. That `QAudioSink::processedUSecs()` on a given backend reports **played-out** audio
   rather than audio merely written into the buffer. If a backend reports the latter,
   video will run slightly ahead of sound by roughly the buffer depth (200 ms as
   configured). This is the single most likely thing to be wrong.
3. That a 200 ms buffer topped up every 20 ms is enough to avoid audible gaps under
   load, and that the tick interval is right.
4. That audio and video remain locked over a long recording — the drift the design is
   for is precisely what cannot be observed without a device.
5. That stopping and restarting the sink on every seek is fast enough not to be heard as
   a stumble.

### What to check on real hardware

In rough order of what is most likely to need adjusting:

1. Play a clip with a clear sync reference — a clapperboard, a hand slap, speech with
   visible lips. Look for a constant offset. A constant offset with video *ahead* of
   sound points at (2) above; the fix is to subtract the sink's still-buffered bytes
   from the reported position in `AudioOutput::Engine::feed()`.
2. Play a recording several minutes long and check sync at the end as well as the
   start. Drift that grows means the clock source is not being consulted; drift that is
   constant means the offset is wrong.
3. Seek repeatedly during playback and listen for stumbles or repeated passages.
4. Play on a machine whose default device does not support Int16 at 48 kHz — some
   professional interfaces do not — and confirm the fallback chain finds a format.
5. Confirm that at 0.5× and 4× the track is silent, not pitch-shifted, and that
   returning to 1× brings it back at the right position.
6. Load the machine (a large analysis run alongside playback) and listen for gaps.

Until those are done, treat audio playback as **written and unproven**, in the same way
`docs/PHASE1_TESTING.md` treats the CUDA path. Do not quote sync accuracy figures that
have not been measured on the hardware being described.
