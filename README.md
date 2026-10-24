# VST Streaming Record / Playback (NI PXIe-5644R)

**Project home:** <https://github.com/oopsdump/vst-pxie-5644r-streaming-in-c>
**License:** Apache License 2.0 ([LICENSE](LICENSE))

A native C implementation of the NI *Disk Streaming / Streaming Host Example for the
PXIe-5644R*: continuous IQ capture to disk at full rate, and paced IQ playback from disk,
driven by the VST's streaming FPGA personality instead of the instrument-driver image.

The behaviour is modelled one-to-one on the LabVIEW example that ships with NI Streaming
(the same FIFO configuration, the same register sequence, the same priming order); the
LabVIEW sources are the reference and remain the authority where they disagree with this
document.

```
  RF IN  ──► FPGA capture ──► input FIFO (DMA) ──► ring buffer ──► file
  RF OUT ◄── FPGA playback ◄── output FIFO ◄── read-ahead queue ◄── file
```

## What it does

| Capability         | Notes                                                                                                                             |
| ------------------ | --------------------------------------------------------------------------------------------------------------------------------- |
| Streaming record   | 120 MS/s sustained, 457 MB/s to disk, zero dropped samples over a 60 s run                                                        |
| Streaming playback | Paced by the configured IQ rate; FIFO pre-filled to 80% before the engine starts, so a run begins without gaps (`underflows = 0`) |
| Control UI         | Plain Win32 GUI (no runtime dependencies) for both directions plus live buffer telemetry                                          |
| File format        | VSTIQ v1 container, plus TDMS support for interchange with the NI tools                                                           |

## Requirements

| Item             | Detail                                                                                                                                     |
| ---------------- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| OS               | Windows 10/11 x64                                                                                                                          |
| Compiler         | Visual Studio 2015 SP2 … 2022 (any edition, or Build Tools)                                                                                |
| Drivers          | NI-RFSA and NI-RFSG, plus the *NI Streaming for VST* FPGA image                                                                            |
| FPGA image       | `C:\Users\Public\Documents\National Instruments\FPGA Extensions Bitfiles\NI PXIe-5644R\NI Streaming for VST.lvbitx` (selectable in the UI) |
| Runtime location | **The executables must live on the `C:` volume.** NI-PAL fails to initialise otherwise (`0xBFFA0000`).                                     |

## Build

```bat
build.bat            :: builds everything under src\ into build\
build.bat clean      :: same, after wiping build\obj and build\bin
```

Produces:

```
build\bin\vst_stream_record.exe
build\bin\vst_stream_playback.exe
build\bin\vst_gui.exe
build\obj\*.obj
```

The script locates the toolchain itself (via `vswhere`, then known VS2015–VS2022 layouts),
and never writes object files outside `build\`. PowerShell equivalents live in
`tools\build\` for the test programs and the GUI.

Deploy to the `C:` volume with:

```powershell
powershell -ExecutionPolicy Bypass -File tools\run\stage_exes.ps1   # -> C:\vst_rf\bin
```

## Usage

### GUI

```
C:\vst_rf\bin\vst_gui.exe
```

The window has three groups:

```
[ Common ]   Device, File, Stop
             Bitfile
             Center (Hz), IQ Rate (Hz), Ref Clock
[ Record ]   Ref (dBm), Seconds, Max File Size (MB), Record
             Host Depth (Blocks), Read Block (MB), Ring Buffer (MB)
[ Playback ] Output Power (dBm), Play
             Host Depth (Blocks), Write Block (MB), Ring Buffer (MB)
```

Below them: playback progress, two buffer bars (`DMA FPGA Buffer` and
`File Write Buffer` while recording, `File Read Buffer` while playing), and a log pane
that mirrors the child process output.

### Command line

Record 10 s at 120 MS/s:

```
vst_stream_record.exe --out D:\capture.vstiq --seconds 10 --rate 120e6 ^
                      --center 1e9 --ref -10 --record-ring-mb 512
```

Play it back at -5 dBm:

```
vst_stream_playback.exe --in D:\capture.vstiq --power -5 --read-block-mb 4
```

Useful options:

| Option                  | Applies to | Meaning                                                              |
| ----------------------- | ---------- | -------------------------------------------------------------------- |
| `--resource RIO0`       | both       | RIO resource name                                                    |
| `--bitfile <path>`      | both       | streaming FPGA image (full path; the driver gets only the file name) |
| `--ref-clock <name>`    | both       | `OnboardClock`, `RefIn`, `PXI_Clk`, `ClkIn`                          |
| `--host-depth-blocks N` | both       | FIFO depth in blocks (16 blocks ≈ 8,000,000 elements for playback)   |
| `--read-block-mb N`     | both       | transfer size per FIFO access (playback default 4 MB)                |
| `--record-ring-mb N`    | record     | write-back ring buffer, default 512 MB                               |
| `--playback-ring-mb N`  | playback   | read-ahead queue, default 32 MB                                      |
| `--max-file-mb N`       | record     | stop once the file reaches this size                                 |
| `--seconds S`           | both       | recording duration / playback limit                                  |

## Throughput expectations

Recording is not throttled: it drains the input FIFO as fast as it can, so the capture
window equals the configured duration and the data lands in the ring buffer.
Playback is paced by the IQ rate stored in the file, and therefore needs a volume that can
sustain it:

| IQ rate  | Disk bandwidth needed |
| -------- | --------------------- |
| 20 MS/s  | 80 MB/s               |
| 60 MS/s  | 240 MB/s              |
| 120 MS/s | 480 MB/s              |

A volume that only sustains ~150 MB/s will still complete a short capture (the OS write
cache absorbs the burst) but will spend a long time flushing afterwards; the recorder
reports this as a `tail` phase and prints `[flush]` lines while it drains.

## Status

Validated on hardware:

- record: 120 MS/s, 7,200,000,000 samples, 26.82 GB, 0 errors, 0 overruns in one 60 s run;
- playback: 1,080,000,000 samples from a 4.47 GB file with `underflows = 0`;
- longer playback at 20 MS/s: 560,000,000 samples, `underflows = 0` throughout.

Known limits: playback above roughly 90 MS/s is bounded by the storage device, not by the
FPGA or the host code.

## License

Copyright (c) 2026 the vst-pxie-5644r-streaming-in-c authors.

Licensed under the **Apache License, Version 2.0**; see [LICENSE](LICENSE) for the full text,
or <http://www.apache.org/licenses/LICENSE-2.0>.

The three source files under `src\` carry an SPDX identifier and the standard Apache-2.0
notice in their file header:

```
SPDX-License-Identifier: Apache-2.0
```

NI-RFSA, NI-RFSG and the NI Streaming FPGA image are products of National Instruments /
Emerson and are used here through their public driver APIs; they are **not** covered by this
license and must be obtained and licensed separately.
