# Connecting TRACE to cameras

Cable, WiFi and Bluetooth — what each of them actually is, what TRACE does with
it, and what has not been run.

---

## 0. What has and has not been exercised

The machine TRACE was built on has **no camera of any kind**: no USB device
under `/dev/video*`, no Bluetooth adapter, no IP camera on the network. That
shapes what the tests can honestly claim.

| Path | Status |
|---|---|
| Network camera over TCP — open, remux, close, hash, decode | **Executed.** The tests serve the fixture over a real loopback socket and record it. |
| A link dropping mid-recording, and reconnecting | **Executed.** The test server hangs up mid-stream and comes back. |
| Operator stop, progress cancellation, refusals | **Executed.** |
| Rolling to a new file at a size limit without dropping the camera | **Executed.** |
| Filing a capture as evidence with capture provenance | **Executed.** |
| RTSP handshake specifically | **Not executed.** No RTSP server was available here. The transport reaches TRACE through the same `avformat_open_input` call as the HTTP stream that is tested, but the RTSP-specific negotiation is not covered. |
| USB cameras and capture cards (`video4linux2` / DirectShow / AVFoundation) | **Not executed.** No device. libavdevice is linked and registered; the code path has never opened one. |
| Bluetooth control | **Not implemented.** No adapter, and no backend written against one. Every entry point refuses with a reason. |

No throughput, latency or frame-rate figure appears in this document, because
none has been measured against a real camera.

---

## 1. Cable, WiFi and Bluetooth are not three equivalent options

The feature was asked for as "hook up to any camera system via solid cable,
WiFi, Bluetooth". Two of those three carry video and one does not, and the
implementation says so rather than offering three buttons of which one cannot
work.

**Cable means two different things.**

- An **Ethernet cable to an IP camera** is a *network* transport. TRACE reaches
  it exactly as it reaches the same camera over WiFi: an address and RTSP.
- A **USB cable to a webcam, or HDMI/SDI into a capture card**, is a *local
  device*. Different code path entirely — `video4linux2` on Linux, DirectShow on
  Windows, AVFoundation on macOS, all via libavdevice.

**WiFi is Ethernet, above the network stack.** The camera has an address and
speaks RTSP either way. What genuinely differs is discovery and how often the
link drops — not the protocol. TRACE does not pretend to have two
implementations where it has one. What it does do is record which link carried a
recording, when that is known, because "captured over WiFi" and "captured over a
cable" are different claims about how interruptible the capture was.

**Bluetooth cannot carry video.** There is no Bluetooth video profile in general
use — the published profiles cover audio, serial data, input devices and file
transfer, and the one video specification (VDP over Bluetooth Classic) is not
implemented by cameras anyone deploys. Bluetooth Low Energy's practical
application throughput is on the order of a megabit per second, often much less;
heavily compressed 1080p is several megabits per second. The gap is one to two
orders of magnitude and it is not a matter of tuning.

So `carriesVideo(CameraTransport::BluetoothControl)` returns `false`, and every
path that opens a stream checks it before creating a file.

### What Bluetooth is genuinely for

Action cameras, body-worn cameras and dashcams advertise over BLE and use it as
a **control channel**: wake the camera, read its battery and storage, change
settings, start and stop recording, and — the step that matters here — **tell it
to bring up its WiFi**. The video then arrives over that WiFi link as an
ordinary network stream, through the same code as a wired IP camera.

That is the shape `bluetooth.h` models: `ControlLink::beginStreaming()` issues
the enable-WiFi command and returns a `CameraSource` that `CaptureSession`
records like any other.

The platform backends are **not implemented**. BlueZ, WinRT and CoreBluetooth
are three separate integrations against three unrelated APIs, and nothing
written against them could have been run even once here. `bluetooth::scan()` and
`bluetooth::connect()` therefore return `ErrorCode::Unsupported` with the reason
— never an empty list, because "no backend" and "nothing nearby" are different
facts and only one of them is about the room.

---

## 2. Capturing is not ingesting

This is the part that matters most for evidence.

Every other path into TRACE receives a file that already exists. It is copied
into managed storage, hashed, and that hash is what an outside party checks the
copy against. The chain of custody starts before TRACE.

**A live camera has no original.** TRACE is the recording device, and the chain
of custody starts with this process, on this machine, at this clock. Three
things follow:

1. **There is nothing to compare the hash against.** It is computed when the
   segment closes and attests that the file has not changed *since* — not that
   it matches anything that existed before.
2. **The provenance says captured, not ingested.** Filing a capture through
   `EvidenceService::ingest` alone would record `ingestedAt`, `ingestedBy` and a
   `sourcePath` pointing at a staging file TRACE itself wrote moments earlier,
   and a reader would reasonably conclude the material came from somewhere.
   `CaptureService` writes a `camera_capture` operation on top, carrying
   `"no_original_exists": true` and the sentence that says why.
3. **The recording is only as trustworthy as the link that carried it**, which
   is why a dropped connection is recorded rather than papered over.

The copy still goes through `ingest`, because copy-hash-verify is the right path
and duplicating it would be worse. What is added is the record of where the
material came from.

### Gaps are recorded, never closed

When a network camera drops, TRACE does **not** stitch the two halves into a
file that looks continuous. It ends the segment, records the gap with its
position, duration and cause, and starts a new segment when the camera returns.
Each segment is filed as its own exhibit.

A recording that silently omits ninety seconds while presenting continuous
timestamps is worse than one that stops, because nothing about it looks wrong.

**A gap is a hole between two recorded segments** — time an examiner would
otherwise read straight across. A camera that goes away and never comes back is
an *ending*, not a hole: nothing follows it, and recording it as a gap would
mark every capture that ends with a camera being switched off as internally
discontinuous, putting that claim into the provenance of segments that have no
missing time in them at all. That case sets `failureReason` instead, and the
retry time is the difference between `wallClockMs` and the recorded duration.

**Splitting at a size limit is not a gap either**, and does not drop the camera.
Rolling to a new file closes the output only; the connection stays open, because
closing and reopening it to start a file would lose whatever arrived in between
— a real gap, caused by TRACE, that nobody asked for. A test asserts the split
produces several files, one connection, and no gaps.

`CaptureOutcome` reports two durations that differ exactly when the link
dropped:

- `recordedDurationUs()` — the sum of what is actually in the files
- `wallClockMs` — first packet to last, gaps included

Every segment's provenance carries `capture_continuous` and the gap count for
the capture as a whole, so an examiner holding one exhibit can see the recording
had gaps without having to find the others.

### The recording is the camera's own bitstream

Packets are **remuxed, not re-encoded**: copied from the camera's bitstream into
a container without being decoded. Re-encoding would make every frame a lossy
derivative of material that has no other copy, and no quality setting makes that
acceptable for a recording that may be examined frame by frame. A test asserts
the codec and dimensions of a captured file match the source.

Segments are **Matroska**, not MP4. An MP4 whose `moov` atom was never written —
which is what a capture killed by a power cut produces — is unplayable, while a
truncated Matroska file plays up to the point it stopped. For a recording that
may be interrupted, that is the difference between partial evidence and none.

---

## 3. Finding cameras

Three mechanisms, because none of them covers the ground alone.

**Network cameras** answer **ONVIF WS-Discovery**: a SOAP probe multicast to
`239.255.255.250:3702`. Most professional IP cameras implement it; consumer ones
often do not, which is why entering an address by hand is a first-class path
rather than a fallback for when discovery "fails".

The multicast TTL is **1**. Discovery is for the local segment; a higher value
would send probes to networks the operator did not intend to scan, which on a
shared corporate network is not a neutral act.

**Attached cameras** are enumerated from the platform device list. On Linux that
is `/dev/video*`, sorted numerically so `video10` does not sort between `video1`
and `video2`. A node existing does not mean it is a capture device — many
`/dev/video*` entries are metadata or output nodes — so they are listed as
*candidates* and `probeCamera()` decides. On Windows, enumeration is not
implemented: libavdevice's DirectShow listing writes to the log rather than
offering an API, and rather than parse that, `findLocalDevices()` returns
`Unsupported` with instructions to add the camera by name. Said plainly instead
of returning an empty list that reads as "no cameras attached".

**Discovery reports what it could not look for.** `Outcome::unavailable` carries
`(transport, reason)` pairs. An empty list because nothing answered and an empty
list because the machine has no network are identical to a caller that only gets
a vector, and they need different actions from an operator.

Discovery finds candidates; it does not verify any of them will stream. That is
`probeCamera()`, and keeping them separate means a camera that appears in the
list and then refuses to open produces a specific error rather than vanishing
from the results.

---

## 4. Credentials

An RTSP URL routinely carries `user:password@`. TRACE keeps exactly one copy of
that, in `CameraSource::uri`, because it has to be handed to FFmpeg to open the
camera at all. Everything else is redacted:

- `CameraSource::address` — never has credentials
- `CameraSource::redactedUri()` — what gets displayed
- `CameraSource::toJson()` — what goes into provenance and logs
- `CameraSource::id` — derived from the redacted address, so a credential never
  reaches an identifier that appears in every log line

The redaction looks for `@` **before the first path slash**, because a path or
query may legitimately contain one and cutting there would mangle the URI rather
than redact it.

A capture record is read by people who are not the operator, and it is exported
with the case. A password in it would be published.

---

## 5. Reconnection, and what is not retried

| Setting | Default | What it does |
|---|---|---|
| `openTimeoutSeconds` | 10 | How long FFmpeg waits for the stream to open or for data to arrive |
| `reconnectWindowMs` | 30 000 | How long to keep retrying a dropped camera |
| `reconnectDelayMs` | 1 000 | Wait between attempts |
| `segmentBytes` | 0 (one segment per connection) | Roll to a new file at this size |
| `maximumDurationMs` | 0 (until stopped) | Stop after this long |

**A source that has never connected is not retried.** A wrong address should
fail now, not thirty seconds from now, and the code distinguishes the two: only
a camera that has worked at least once is worth waiting for.

**RTSP is carried over TCP, not the default UDP.** A dropped UDP packet is a
corrupt frame that nothing downstream can distinguish from camera noise; TCP
turns the same event into a stall or a clean disconnect, which TRACE can record
honestly as a gap.

**Stop is observed during blocking calls.** `requestStop()` sets a flag that
FFmpeg polls through an interrupt callback from inside `avformat_open_input` and
`av_read_frame`. Without it, an operator who pressed Stop during a ten-second
connection attempt would wait out the whole timeout. A stop during reconnection
is recorded as an operator stop — **not** as a failure, and **not** as a gap:
nothing was missed, because nothing was being recorded.

---

## 6. What happens to the files

A capture writes into `cases/<case-id>/capture/` while it is still recording.
That is deliberately not `originals/` — nothing belongs there until it has been
hashed and filed, and a segment still being written has no digest.

When a segment closes it is hashed, ingested into managed storage (which copies,
re-hashes and re-reads to prove the copy matches), given its capture provenance,
and the staged copy is then removed. Keeping it would leave a second copy of the
recording outside managed storage — and in an encrypted workspace, that copy
would be in the clear.

**A segment that fails to register is kept.** If the database write fails, the
staged file stays on disk and is reported in `CaptureRegistrationOutcome::unregistered`.
It is at that moment the only copy of material that was genuinely recorded, and
deleting it to tidy up an error would be destroying evidence. The staging
directory is removed only when it is empty; one left behind is a signal, not
litter.

A zero-byte segment — a connection that opened and delivered nothing — is
deleted rather than filed. An empty file would look like a recording that
captured silence rather than one that captured nothing at all.

---

## 7. Building

`libavdevice` is optional and degrades:

```
-DTRACE_WITH_AVDEVICE=ON     # default
```

It ships separately on Debian-family systems (`libavdevice-dev`) and some FFmpeg
builds omit it. Without it, **network cameras are unaffected** — they need only
libavformat — and attached cameras are refused with a reason naming the missing
library rather than failing later inside FFmpeg as an opaque format error. The
configure summary says which:

```
--   Attached cameras ... libavdevice 60.3.100
```

There is no `TRACE_WITH_BLUETOOTH` yet. `bluetooth::availability()` reports the
backend and the adapter as two separate facts, because they need different
things from an operator: install a build with a backend, or plug in an adapter.

---

## 8. What TRACE still will not say

Live capture changes nothing about the rules that hold everywhere else:

- **The original is never modified** — and for a capture there is no original,
  which the provenance states outright rather than leaving to be inferred.
- **A failed capture never appears as a completed one.** A capture with a gap is
  audited as `capture.interrupted` even when it eventually finished, because
  "completed" alongside a missing ninety seconds is the misleading half of the
  truth.
- **Nothing is identified.** A camera reports its manufacturer and model or it
  does not; TRACE does not read "axis" out of a hostname and write it into a
  record as fact. `reportedProfiles` being empty means "not reported", never
  "none".
- **The link is not guessed.** A camera found by multicast is on this segment;
  that says nothing about whether the last hop was a cable or WiFi, so
  `CameraLink` stays `Unknown` rather than becoming a wrong claim in the
  provenance of every recording from it.
