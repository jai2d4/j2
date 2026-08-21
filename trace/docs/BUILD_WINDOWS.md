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

## Notes for this workstation class

- **AMD Ryzen 9 9950X3D** — ingestion hashing is single-threaded per file by design
  (streamed, bounded memory). Importing several files in sequence is the current
  behaviour; parallel ingest is a Phase 1 option.
- **NVIDIA RTX 5090** — no GPU code path exists in Phase 0. The decoder is software-only
  so frame timing is identical on every workstation, and the hardware-acceleration
  setting is present but disabled. GPU decode and CUDA/TensorRT inference are introduced
  with the analysis phases behind the `IAnalysisProvider` boundary.
- **128 GB RAM** — TRACE never loads a video into memory; it streams in 1 MiB chunks and
  decodes frame by frame. Large recordings are bounded by disk, not RAM.
- **Long paths** — enable `LongPathsEnabled` if case data lives deep in a directory tree.
  Managed filenames are 7-bit clean, avoid reserved device names and strip trailing dots
  and spaces, so they are safe on NTFS.
- **Antivirus** — real-time scanning of the data directory measurably slows ingestion.
  Exclude `TRACE_DATA` after review by whoever owns the endpoint policy.
