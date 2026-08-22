# The detection model

What TRACE runs, how a frame reaches it, and how its output becomes a box on screen.

---

## The artefact

| | |
|---|---|
| Model | YOLOX-Tiny |
| Version | 0.1.1rc0 |
| Format | ONNX |
| Licence | Apache-2.0 (Megvii, YOLOX) |
| Source | `https://github.com/Megvii-BaseDetection/YOLOX/releases/download/0.1.1rc0/yolox_tiny.onnx` |
| SHA-256 | `427cc366d34e27ff7a03e2899b5e3671425c262ea2291f88bb942bc1cc70b0f7` |
| Size | 20,219,662 bytes |
| Input | `images`, `float32[1, 3, 416, 416]`, NCHW |
| Output | `output`, `float32[1, 3549, 85]` |
| Classes | the 80 COCO classes |

YOLOX was chosen over YOLOv5/v8 for one reason that matters here: **Apache-2.0**. An
AGPL model in an evidence tool creates a licensing question an agency should not have to
answer.

`yolox-s` (640×640) is in the catalogue as an optional accuracy/speed trade. It is not
installed by default and has no recorded digest, so TRACE reports the digest of whatever
file it actually runs.

Neither is committed to the repository. `scripts/fetch_models.sh` downloads one and
**verifies its SHA-256 against the catalogue before installing it**; a mismatch deletes
the download and installs nothing. TRACE itself never downloads a model — an absent model
is reported, with the path it was expected at and the script that fetches it.

---

## Preprocessing

These are properties of the artefact, not assumptions of the runtime, so they travel
with the descriptor (`ModelDescriptor`) and are handed to the provider in its config.

| Property | YOLOX-Tiny |
|---|---|
| Channel order | **BGR** |
| Scaling | none — raw 0–255 floats, no `/255`, no mean/std |
| Letterbox | aspect-preserving, image at the **top-left**, pad value **114** |
| Layout | NCHW |

The letterbox transform and its inverse are in
`ai/detection/preprocessing/letterbox.{h,cpp}`:

```
scale = min(416 / sourceWidth, 416 / sourceHeight)
drawn = (round(sourceWidth * scale), round(sourceHeight * scale))   at (0, 0)
rest of the 416×416 canvas = 114
```

Because the image sits at the top-left rather than centred, the inverse is just a
division by `scale` — but it is written explicitly (`canvasBoxToSource`,
`canvasCenterBoxToSource`) and unit-tested, because getting this wrong puts every box in
the wrong place and the result still *looks* plausible.

---

## Decoding the output

The exported graph has **no NMS node**. The `[1, 3549, 85]` tensor is a concatenation of
three grids:

| Stride | Grid | Cells |
|---|---|---|
| 8 | 52 × 52 | 2704 |
| 16 | 26 × 26 | 676 |
| 32 | 13 × 13 | 169 |
| | | **3549** |

Each row is `[cx, cy, w, h, objectness, 80 class scores]` in grid units:

```
cx_canvas = (cx + gridX) * stride
cy_canvas = (cy + gridY) * stride
w_canvas  = exp(w) * stride
h_canvas  = exp(h) * stride
score     = objectness * classScore        (the best class wins)
```

Boxes below the confidence threshold are dropped, the survivors go through **class-aware
non-maximum suppression** (`ai/detection/postprocessing/nms.cpp`, default IoU 0.45), and
what remains is mapped back through the inverse letterbox and normalised against the
source frame.

`DetectionCapabilities::performsOwnNms` exists so a model whose graph already applies NMS
does not get it applied twice.

---

## From tensor to screen

```
decoded frame (RGB24, source size)
   └─ letterboxToTensor()        BGR, 0–255, pad 114, top-left, NCHW
        └─ Ort::Session::Run()
             └─ decodeYoloxOutput()   grid → canvas boxes
                  └─ NMS (class-aware, IoU 0.45)
                       └─ canvasBoxToSource()   inverse letterbox
                            └─ NormalizedBox (0–1 of the source frame)
                                 ├─ detections.bbox_x/y/w/h   ← authoritative
                                 └─ detections.bbox_pixel_*   ← convenience
```

**Normalised geometry is authoritative.** It stays correct when the item is drawn at
another size — a resized panel, full screen, a report — and the pixel columns are a
convenience derived from the source dimensions recorded on the run.

The viewer maps a normalised box onto whatever rectangle the frame currently occupies:

```cpp
QRectF detectionBoxToWidget(const NormalizedBox& box, const QRect& frameRect);
```

so a 16:9 frame letterboxed inside a 4:3 panel still gets its boxes on the object rather
than on the black bars.

---

## Class grouping

The model reports 80 fine-grained COCO classes. TRACE groups them so an analyst can ask
for "vehicles" without knowing which class list a given model uses
(`ai/detection/models/class_taxonomy.cpp`):

| Group | COCO classes |
|---|---|
| People | `person` |
| Vehicles | `bicycle`, `car`, `motorcycle`, `bus`, `train`, `truck`, `boat`, `airplane` |
| Objects | everything else |

The group drives the timeline lanes and the filters. The specific class label is always
kept and always shown — the group is a lens, not a replacement.

---

## Devices

| Requested | What happens |
|---|---|
| Automatic | CUDA execution provider if the build and machine have it, otherwise CPU |
| CPU | CPU execution provider |
| GPU | CUDA attempted; **on failure the run continues on CPU and records a warning** |

The device that actually did the work is recorded on the run as `device_used`, separately
from `device_requested`. TRACE reports what ran, not what was asked for.

The CUDA path is compiled and guarded (`OnnxDetectionProvider::cudaAvailable()`), and is
selected from the runtime's own `availableExecutionProviders()`. **It has not been
executed on a GPU in this repository's development environment**, which has no NVIDIA
device — see `docs/PHASE1_TESTING.md` for what was and was not measured.

---

## Measured performance

On the development machine — Intel Xeon @ 2.30 GHz, 4 vCPU, 15 GB RAM, **no GPU**,
ONNX Runtime 1.17.3 CPU execution provider:

| Clip | Frames analysed | Wall clock | Per frame |
|---|---|---|---|
| 768 × 576, 795 frames, every frame (Detailed) | 795 | 20.3 s | **25.5 ms** (≈ 39 analysed frames/s) |

That is the number this environment produced. No GPU figure is quoted because no GPU was
available to produce one.
