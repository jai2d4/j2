# Building TRACE on Windows 11

Windows 11 Pro is the primary development and deployment target. These instructions were
written for that target; the Phase 0 code itself is built and tested on Linux in CI, and
the platform-specific paths in it (`%LOCALAPPDATA%`, `GetUserNameA`, `GetComputerNameA`,
`_mkgmtime`, `GlobalMemoryStatusEx`) are compiled behind `_WIN32` guards.

> **Status note.** The build steps below have not yet been executed on a Windows machine
> in this phase — no Windows host was available. They are the documented, intended
> procedure; treat the first Windows build as a task to confirm, not as a certainty.

## Prerequisites

| Component | Version | Notes |
|---|---|---|
| Visual Studio 2022 | 17.8+ | "Desktop development with C++" workload, MSVC v143 |
| CMake | 3.21+ | Bundled with Visual Studio, or standalone |
| Ninja | any | Bundled with Visual Studio |
| vcpkg | current | Dependency manager used below |
| Git | any | |

## 1. vcpkg

```powershell
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
setx VCPKG_ROOT C:\vcpkg
```

## 2. Dependencies

```powershell
C:\vcpkg\vcpkg install `
    qtbase[core,gui,widgets,concurrent]:x64-windows `
    ffmpeg[avformat,avcodec,avfilter,swscale,swresample]:x64-windows `
    sqlite3:x64-windows `
    gtest:x64-windows
```

Qt is the largest item; expect a long first build. A prebuilt Qt from the official
online installer works equally well — point CMake at it with
`-DCMAKE_PREFIX_PATH="C:/Qt/6.6.3/msvc2019_64"` and drop `qtbase` from the vcpkg line.

## 3. Configure and build

```powershell
cd <repository>
cmake -S trace -B trace\build -G Ninja `
    -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake `
    -DVCPKG_TARGET_TRIPLET=x64-windows `
    -DCMAKE_BUILD_TYPE=RelWithDebInfo

cmake --build trace\build --parallel
```

Artefacts:

```
trace\build\bin\trace.exe
trace\build\bin\trace_unit_tests.exe
trace\build\bin\trace_integration_tests.exe
trace\build\bin\trace_acceptance_test.exe
```

## 4. Tests

```powershell
ctest --test-dir trace\build --output-on-failure
```

The acceptance test needs a window station. On an interactive desktop it runs as-is; on
a headless agent set `QT_QPA_PLATFORM=offscreen` (already set by the CTest entry).

## 5. Deploy the Qt runtime

`trace.exe` needs the Qt DLLs and platform plugin beside it:

```powershell
C:\Qt\6.6.3\msvc2019_64\bin\windeployqt.exe trace\build\bin\trace.exe
```

With vcpkg's Qt, `applocal.ps1` runs automatically at install time; for a manual copy the
minimum set is `Qt6Core.dll`, `Qt6Gui.dll`, `Qt6Widgets.dll`, `platforms\qwindows.dll`,
plus the FFmpeg and SQLite DLLs from `vcpkg\installed\x64-windows\bin`.

## 6. First run

```powershell
trace\build\bin\trace.exe
trace\build\bin\trace.exe -d D:\TRACE_DATA
```

The default data directory is `%LOCALAPPDATA%\TRACE\TRACE_DATA`. Put it on the fast NVMe
volume for large imports: ingestion reads the source once and writes and re-reads the
managed copy, so throughput is bounded by that volume.

## 7. ONNX Runtime and the detection model (Phase 1)

Detection needs two runtime artefacts. Neither is committed, and TRACE never fetches
either on its own.

```powershell
# ONNX Runtime — CPU package
curl.exe -L -o ort.zip `
  https://github.com/microsoft/onnxruntime/releases/download/v1.17.3/onnxruntime-win-x64-1.17.3.zip
Expand-Archive ort.zip -DestinationPath trace\third_party
Rename-Item trace\third_party\onnxruntime-win-x64-1.17.3 trace\third_party\onnxruntime

# ...or the CUDA package, for GPU inference
curl.exe -L -o ort-gpu.zip `
  https://github.com/microsoft/onnxruntime/releases/download/v1.17.3/onnxruntime-win-x64-gpu-1.17.3.zip

# YOLOX-Tiny (Apache-2.0), 20,219,662 bytes
New-Item -ItemType Directory -Force trace\models | Out-Null
curl.exe -L -o trace\models\yolox_tiny.onnx `
  https://github.com/Megvii-BaseDetection/YOLOX/releases/download/0.1.1rc0/yolox_tiny.onnx

# Verify the digest before using it — a mismatch means it is not the same artefact
Get-FileHash trace\models\yolox_tiny.onnx -Algorithm SHA256
# expected 427CC366D34E27FF7A03E2899B5E3671425C262EA2291F88BB942BC1CC70B0F7
```

Configure with the runtime:

```powershell
cmake -S trace -B trace\build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DTRACE_WITH_ONNXRUNTIME=ON `
  -DTRACE_ONNXRUNTIME_ROOT=trace\third_party\onnxruntime
```

`onnxruntime.dll` (and, for the CUDA package, `onnxruntime_providers_cuda.dll` and
`onnxruntime_providers_shared.dll`) must sit next to `trace.exe` or on `PATH`.

TRACE looks for models in `%LOCALAPPDATA%\TRACE\TRACE_DATA\models`, overridable with
`TRACE_MODEL_DIR`. Point it at a shared read-only model store on a managed workstation.

## Notes for this workstation class

- **AMD Ryzen 9 9950X3D** — ingestion hashing is single-threaded per file by design
  (streamed, bounded memory). Importing several files in sequence is the current
  behaviour; parallel ingest remains a later option. On CPU, detection is bounded by
  inference: ONNX Runtime uses all available cores by default.
- **NVIDIA RTX 5090** — the CUDA execution provider is selected automatically when the
  GPU package of ONNX Runtime is present and the runtime reports CUDA among its
  available providers; if it cannot be acquired the run continues on CPU and records a
  warning, and the device that actually ran is stored on the analysis run.

  Two honest caveats. **No GPU figure is quoted anywhere in this repository**: the
  development environment has no NVIDIA device, so the CUDA path has been written and
  guarded but never executed — see `docs/PHASE1_TESTING.md`. And CUDA support for a
  50-series card requires an ONNX Runtime build against a CUDA 12.x runtime new enough
  for its compute capability; check the release notes of the version you download rather
  than assuming 1.17.3 is right for it.

  TensorRT is not implemented in Phase 1. It would be a new `IDetectionProvider`, and
  nothing in the evidence, storage, timeline or audit code would change.
- **Video decode** is software-only on every platform, so frame timing is identical on
  every workstation and the hardware-acceleration setting is present but disabled.
- **128 GB RAM** — TRACE never loads a video into memory; it streams in 1 MiB chunks and
  decodes frame by frame. Large recordings are bounded by disk, not RAM.
- **Long paths** — enable `LongPathsEnabled` if case data lives deep in a directory tree.
  Managed filenames are 7-bit clean, avoid reserved device names and strip trailing dots
  and spaces, so they are safe on NTFS.
- **Antivirus** — real-time scanning of the data directory measurably slows ingestion.
  Exclude `TRACE_DATA` after review by whoever owns the endpoint policy.
