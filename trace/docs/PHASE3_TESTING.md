# Phase 3 testing

What is covered, and — at some length, because it matters here — what was not
exercised.

---

## The suites

| Suite | Cases | Phase 3 additions |
|---|---|---|
| `trace_unit_tests` | 81 | — |
| `trace_ui_unit_tests` | 6 | — |
| `trace_integration_tests` | 72 | **24** (`AudioDecoderTest`, `WaveformTest`, `AudioClockTest`, `PlaybackClockSourceTest`) |
| `trace_acceptance_test` | 1 | — |
| `trace_acceptance_phase1_test` | 1 | — |
| `trace_acceptance_phase2_test` | 1 | — |
| `trace_acceptance_phase3_test` | 1 | **3 workflows** |
| **Total** | **163** | |

Every test from Phases 0, 1 and 2 is unchanged and still passes.

The 24 integration cases break down as **9 decoder, 8 waveform, 5 clock, 2 sync**.

The integration suite went from the 46 recorded in `PHASE2_TESTING.md` to 72. Twenty-four
of those 26 are Phase 3's; the other two are `ReportExportTest` cases added after that
document was written, when the by-hand verification walkthrough found that the two
documented commands could not detect a file *added* to a bundle.

## Decoding (9 cases)

| Case | What it proves |
|---|---|
| `ReportsWhatTheContainerHoldsAndWhatItProduces` | Source format and output format are reported separately, so a resampled copy is never mistaken for the original's properties |
| `DecodesTheWholeTrackWithTimestampsThatOnlyMoveForward` | The whole track decodes, and presentation timestamps never go backwards |
| `TheFirstBlockStartsAtTheBeginningOfTheStream` | The first block is at ~0, matching the convention video frames use |
| `ResamplesToWhateverFormatIsAskedFor` | Three targets — 48000/2, 44100/1, 16000/1 — all produce the requested format |
| `SeekingLandsWhereItSaysItDoes` | A seek arrives at the requested position |
| `SeekingBackToZeroReplaysFromTheStart` | Seeking home replays from the beginning |
| `AnItemWithNoAudioTrackSaysSoRatherThanFailingObscurely` | No audio gives `ErrorCode::NotFound` and a readable message, which is how callers tell "silent" from "broken" |
| `RefusesAnImpossibleOutputFormat` | An unachievable target is rejected rather than producing garbage |
| `DecodingDoesNotModifyTheSourceFile` | Bytes **and** mtime of the managed original are unchanged after a full decode |

## The waveform (8 cases)

| Case | What it proves |
|---|---|
| `BuildsAnEnvelopeThatSpansTheWholeTrack` | The envelope covers the whole item; every value is in range; RMS never exceeds the peak that contains it |
| `BucketCountIsIndependentOfHowLongTheRecordingIs` | A short clip and a long one produce the same number of points, so both fit one timeline row |
| `RefusesABucketCountThatWouldBeUseless` | Out-of-range bucket counts are rejected |
| `CancellationStopsTheBuild` | A cancelled build returns `Cancelled` and registers nothing |
| `SurvivesARoundTripThroughItsStoredForm` | The stored JSON reads back as the envelope that was written |
| `AMalformedWaveformFileIsRejected` | A corrupt stored waveform is rejected rather than drawn |
| `IsGeneratedOnceAndRegisteredWithItsProvenance` | Built once, hashed, reused on the second ask; the original is untouched |
| `AnItemWithNoAudioReportsNothingToDrawRatherThanFailing` | No audio gives `NotFound`, which the UI reads as "no row", not "error" |

## The reference clock (5 cases)

This is the arithmetic that decides where video is drawn relative to what is being
heard. It is tested on its own, because an audio device is not needed to get it wrong.

| Case | What it proves |
|---|---|
| `SaysNothingUntilItIsRunning` | A stopped clock offers no position, so callers know to fall back to their own timing instead of pacing against a value that has quietly stopped advancing |
| `ReportsWhereTheDeviceIsOnTheMediaTimeline` | The device's own count is placed correctly on the media timeline, including after starting part-way in |
| `ASeekMovesTheOriginRatherThanAccumulating` | Restarting resets the origin; already-played time is not carried over |
| `ADeviceCountThatJumpsBackwardsDoesNotDragPlaybackBack` | A backend that reports a backwards jump around an underrun cannot pull video backwards |
| `AudioIsRenderedOnlyAtNormalSpeed` | Every speed the viewer offers except 1.0 silences the track |

## Sync (2 cases)

| Case | What it proves |
|---|---|
| `VideoFollowsTheReferenceClockWhenThereIsOne` | With a clock source standing ahead, video reaches three seconds of media in ~38 ms of wall time — it is pacing against the clock, not against the frames' own timestamps |
| `WithoutAClockSourceFramesKeepTheirOwnPace` | With a source that declines to answer, the media position never runs ahead of the wall time spent reaching it |

The first is the load-bearing one. It measures the mechanism rather than asserting it
exists. Note what it uses: a **fake clock**, not a sound card.

## Acceptance coverage

Driven through the real `MainWindow`, `ViewerPanel` and `TimelineWidget`.

| Workflow | What it proves |
|---|---|
| `TheWaveformIsBuiltAsADerivedAssetAndReachesTheTimeline` | Opening an item with sound builds the waveform off the GUI thread, hashes it, records the operation with its parameters and library versions, leaves the original's digest unchanged, and puts an envelope row named "Audio" on the timeline whose values are all in range |
| `TheAudioTransportReportsWhatThisMachineCanDoAndWhy` | The transport's enabled state matches whether this machine has a device; when it does not, the reason names the **missing device** and does **not** claim the item has no audio track |
| `VideoPlaysWhetherOrNotThereIsAnythingToPlayItThrough` | Play, seek, frame step and speed changes all work with no device present, and returning to normal speed does not leave playback wedged |

The second workflow branches on `AudioOutput::deviceAvailable()`. On this machine it
takes the no-device branch. On a machine with a device it takes the other, which
additionally re-hashes the managed original after moving the volume and mute controls
and asserts the digest is unchanged.

---

## Not exercised

### Audio actually leaving a sound card

**This is the significant gap in Phase 3.**

The container this was built and tested in reports **zero audio output devices**
(`QMediaDevices::audioOutputs()` is empty; `/dev/snd` does not exist; PulseAudio's
context connection fails). `QAudioSink` is therefore never constructed, and the
buffer-feeding path in `AudioOutput::Engine::feed()` never executes.

Everything the tests establish about *sync* is established against a fake clock. What
they show is that the mechanism works: given a clock source, video follows it. What they
cannot show is that a real device's reported position is the right thing to follow.

Specifically unverified:

1. That the negotiated format is accepted by a real backend, and that the fallback chain
   finds a working one when the device's preferred format is not Int16.
2. That `QAudioSink::processedUSecs()` on a given backend reports **played-out** audio
   rather than audio merely written into the buffer. If a backend reports the latter,
   video will run ahead of sound by roughly the buffer depth (200 ms as configured).
   **This is the single most likely thing to be wrong.**
3. That a 200 ms buffer topped up every 20 ms avoids audible gaps under load.
4. That audio and video stay locked over a long recording — the drift the whole design
   exists for is precisely what cannot be observed without a device.
5. That stopping and restarting the sink on every seek is fast enough not to be heard.

`docs/AUDIO.md` §5 lists what to check on real hardware, in order.

Treat audio playback as **written and unproven**, exactly as `docs/PHASE1_TESTING.md`
treats the CUDA path. Do not quote sync figures that have not been measured on the
hardware being described.

### Other gaps

- **Audio-only evidence items.** The decoder and waveform handle them; the viewer does
  not display them, because it draws a picture and there is none. It says so rather than
  failing.
- **Time-stretched playback.** Other speeds silence the track rather than pitch-shift
  it. Nothing tests `atempo`, because `libavfilter` is not a dependency.
- **A device that disappears mid-playback** (a USB interface unplugged, a Bluetooth
  headset dropping). Not simulated.
- **Formats beyond the committed fixture.** The sample is AAC mono 44.1 kHz. Decoding
  it to stereo 48 kHz exercises format conversion, channel upmixing and rate conversion
  at once, but only for that one input codec.
- **Very long recordings.** The longest fixture is 8 seconds. Bucket assignment
  arithmetic is exercised, but not against a multi-hour file.
