# moq2ts — Cross-platform MOQ Live Publisher

[![Build](https://github.com/mondain/moq2ts/actions/workflows/build.yml/badge.svg)](https://github.com/mondain/moq2ts/actions/workflows/build.yml)

This repository contains a Qt-based application that publishes live media to MOQ
using a moqxr-oriented publishing adapter.
The media pipeline is designed around M2TS input and produces
`draft-gregoire-moq-msfts-00` media objects: each object payload is a
concatenation of whole 188-byte TS packets or 192-byte M2TS source packets.

The application targets:

- H.264 video encoding via `openh264`
- AAC or Opus audio at 48kHz stereo via direct libav/libopus integration points
- M2TS stream ingestion for live audio/video paths
- Cross-platform desktop UI (Qt Widgets)
- CMake build with a Debian Bookworm Docker build entrypoint

## Prebuilt binaries

GitHub Actions builds Linux, macOS, and Windows packages on every push and pull
request (see the [Build workflow](.github/workflows/build.yml)):

- **Linux** — `moq2ts-publisher-linux-x86_64.AppImage` (self-contained; `chmod +x`
  and run)
- **macOS** — `moq2ts-publisher-macos.zip` (a bundled `.app`)
- **Windows** — `moq2ts-publisher-windows.zip` (the `.exe` with its Qt and codec
  DLLs)

Per-commit packages are attached to each run on the Actions tab. Tagged releases
(`v*`) additionally publish these assets to a GitHub Release.

These CI binaries are **real, relay-capable builds**: each links the prebuilt
openmoq publisher SDK from the public `mondain/moqxr` releases (pinned via the
`MOQXR_VERSION` workflow variable) together with the full capture and transcode
stack (Qt6 + ffmpeg/libav + openh264 + opus), so they can publish to a real MOQ
relay endpoint. The SDK ships static libraries (publisher + picoquic/picotls);
OpenSSL is linked from the platform. On Windows the build uses MSVC + vcpkg to
match the SDK's ABI; Linux and macOS use the system GCC/Clang toolchains.

A mock build (UI, file packetization, and object generation against `mock://`
endpoints only) is still available locally with
`-DMOQ2TS_BUILD_WITH_MOCK_MOQXR=ON`. See [moqxr integration](#moqxr-integration)
for linking the SDK (`MOQXR_SDK_DIR`) or a local `../moqxr` source checkout.

To cut a release, push a version tag:

```bash
git tag v0.1.0
git push origin v0.1.0
```

## Project layout

- `src/main.cpp`: Qt application bootstrap and signal wiring
- `src/app/*`: UI and publish configuration model
- `src/media/*`: media pipeline, capture preview, TS/M2TS packetizer, and draft MSFTS catalog helpers
- `src/publish/*`: MOQ publisher adapter (mock + local moqxr `publish_live_objects`)
- `docker/*`: Bookworm container definition
- `scripts/*`: build automation
- `docs/*`: sequence diagrams and developer documentation

## UI feature set

- Live configuration of endpoint/namespace/stream
- Separate video and audio source selectors (M2TS file/pipe)
- Camera and microphone enumeration through libavdevice/platform capture APIs,
  with Qt Multimedia fallback where enabled
- Preview tab with decoded video preview and dB-scaled left/right audio meters
- In-process libavdevice capture path for selected camera/microphone devices
- M2TS program selection for publishing one program/track from an MPTS source
- MSF timeline side track for wall-clock correlation
- Tunable codec and bitrate settings
- Fragment size control and pacing
- Status/log output and basic counters

## Build prerequisites

- CMake >= 3.24
- Qt Widgets (Qt 5 or Qt 6)
- Optional (recommended for real encode path):
  - `libopenh264-dev`
  - `libav*` dev packages
  - `libavdevice-dev`
  - `libswscale-dev`
  - `libopus-dev`

If third-party moqxr headers are unavailable, the app still builds using mock
publisher defaults. Mock publisher builds only accept `mock://...` endpoints.

## Build locally

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DMOQ2TS_BUILD_WITH_MOCK_MOQXR=ON
cmake --build build -j"$(nproc)"
```

Run:

```bash
./build/moq2ts-publisher
```

## Docker (Debian Bookworm)

```bash
./scripts/build-debian-bookworm.sh
```

The script builds a Bookworm container image, compiles the app, and installs
artifacts under `build-bookworm/install`.

The default Bookworm build uses the mock publisher. It is intended for local
capture, muxing, preview, and object-generation testing:

```text
mock://local
```

Real relay URLs such as `https://moq-relay.red5.net:4433/moq` require a
real-linked moqxr build:

```bash
MOQ2TS_BUILD_WITH_MOCK_MOQXR=OFF ./scripts/build-debian-bookworm.sh
```

The build script mounts a sibling `../moqxr` checkout into the container when it
exists. Launch the packaged app with:

```bash
./build-bookworm/install/moq2ts-publisher-bookworm
```

## Notes on codec paths

- **MSFTS packaging**: media object payloads are raw source packets only. The app
  does not add a private wrapper before publication.
- **Packet size**: source packets are validated as either 188-byte TS or 192-byte
  M2TS packets, with sync byte checks required by the draft.
- **Catalog**: the media track uses `packaging: "m2ts"` with MSF common fields
  (`isLive`, `role`, `mimeType`, `targetLatency`) and MSFTS m2ts fields
  (`m2tsPacketSize`, `m2tsPacketsPerObject`, `m2tsProgramNumber`, optional
  `m2tsPmtPid`/`m2tsPcrPid`, and `m2tsRandomAccess` when every group starts on a
  random-access point). `m2tsTimestampMode` is emitted only for 192-octet
  source packets.
- **Initialization data**: the packetizer scans the start of the source for
  PAT and PMT packets and emits them as Base64 `initData`, preserving 188-byte
  or 192-byte source-packet form.
- **Program filtering**: the selected program is discovered from PAT/PMT.
  Media objects include PAT, the selected PMT, the selected PCR PID, and the
  elementary PIDs for that program. Packets for other programs and null packets
  are dropped before publication.
- **Timeline track**: each media track gets a `<stream>.timeline` side track of
  MSF `type: "mediatimeline"` that `depends` on the media track. Timeline objects
  are a compact `[mediaTimeMs, [groupId, objectId], wallclockMs]` record array
  mapping media presentation time to Unix wall-clock milliseconds without
  modifying M2TS payloads.
- **Encoding**: OpenH264/libav/libopus dependencies are present for in-process
  encoder work. The current live path expects a conforming multiplexed M2TS
  source and never shells out to the `ffmpeg` application.
- **Capture devices**: camera and microphone devices are listed in the UI through
  libavdevice/platform capture APIs, with Qt Multimedia fallback where enabled.
  If no file/pipe source is provided, selected devices are opened through
  libavdevice, encoded in-process, muxed to MPEG-TS through libavformat, and
  published as MSFTS M2TS objects. On Linux the capture path probes the camera's
  V4L2 modes and prefers an MJPEG mode at the requested resolution/framerate
  (so e.g. 1080p30 is honored where raw YUYV would be capped), reconciling the
  encoder to the framerate the driver actually negotiates. Transient corrupt
  decode frames are skipped rather than aborting capture.
- **Preview**: the Preview tab can open selected camera/microphone devices before
  publishing. During live capture publishing, the Preview tab is driven from the
  same decoded frames and audio samples used by the encoder path. Audio meters
  display RMS levels on a dBFS scale so normal speech is visible.

## moqxr integration

`src/publish/MoqxrPublisher.cpp` wires the MSFTS object stream into the moqxr
`Publisher::publish_live_objects` API. There are two ways to link the real
publisher (both require OpenSSL on the system).

Link a prebuilt SDK from the `mondain/moqxr` releases (what CI uses) — extract the
archive for your platform and point `MOQXR_SDK_DIR` at it:

```bash
cmake -S . -B build \
  -DMOQXR_SDK_DIR=/path/to/openmoq-publisher-v0.3.2-Linux \
  -DMOQ2TS_BUILD_WITH_MOCK_MOQXR=OFF
```

Or build against a neighboring moqxr source checkout:

```bash
cmake -S . -B build \
  -DMOQXR_SOURCE_DIR=/media/mondain/terrorbyte/workspace/github-moq/moqxr \
  -DMOQ2TS_BUILD_WITH_MOCK_MOQXR=OFF
```

Mock mode remains available with `MOQ2TS_BUILD_WITH_MOCK_MOQXR=ON`.

Mock mode is explicit: endpoints must start with `mock://`. Entering a real
relay URL in a mock build fails with an error instead of pretending to connect.

## Sequence diagrams

See [`docs/sequence-diagrams.md`](docs/sequence-diagrams.md).

## Developer docs

See [`docs/DEVELOPER.md`](docs/DEVELOPER.md).
