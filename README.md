# moq2ts — Cross-platform MOQ Live Publisher

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
- **Catalog**: the catalog uses `packaging: "m2ts"` plus `m2tsPacketSize`,
  `m2tsPacketsPerObject`, `m2tsProgramNumber`, and `m2tsTimestampMode`.
- **Initialization data**: the packetizer scans the start of the source for
  PAT and PMT packets and emits them as Base64 `initData`, preserving 188-byte
  or 192-byte source-packet form.
- **Program filtering**: the selected program is discovered from PAT/PMT.
  Media objects include PAT, the selected PMT, the selected PCR PID, and the
  elementary PIDs for that program. Packets for other programs and null packets
  are dropped before publication.
- **Timeline track**: each media track gets a `<stream>.timeline` side track
  with `packaging: "msf-timeline"`. Timeline objects map media object
  `mediaTimeUs` to Unix wall-clock microseconds without modifying M2TS payloads.
- **Encoding**: OpenH264/libav/libopus dependencies are present for in-process
  encoder work. The current live path expects a conforming multiplexed M2TS
  source and never shells out to the `ffmpeg` application.
- **Capture devices**: camera and microphone devices are listed in the UI through
  libavdevice/platform capture APIs, with Qt Multimedia fallback where enabled.
  If no file/pipe source is provided, selected devices are opened through
  libavdevice, encoded in-process, muxed to MPEG-TS through libavformat, and
  published as MSFTS M2TS objects.
- **Preview**: the Preview tab can open selected camera/microphone devices before
  publishing. During live capture publishing, the Preview tab is driven from the
  same decoded frames and audio samples used by the encoder path. Audio meters
  display RMS levels on a dBFS scale so normal speech is visible.

## moqxr integration

`src/publish/MoqxrPublisher.cpp` wires the MSFTS object stream into the local
moqxr `Publisher::publish_live_objects` API. Build against the neighboring
checkout with:

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
