# Developer Guide — MOQ2TS Publisher

## Purpose

`moq2ts-publisher` is a reference cross-platform Qt application for live
media publishing. It currently provides:

1. Source selection (TS/M2TS video/audio)
2. Direct TS/M2TS source packet validation and objectization
3. Catalog generation for `draft-gregoire-moq-msfts-00`
4. Program-level packet filtering for selected MPEG-TS programs
5. Camera and microphone enumeration through Qt Multimedia
6. In-process camera/microphone capture through libavdevice/libavformat
7. Publication through an adapter that defaults to a deterministic mock path

## Runtime flow

- The UI (`src/app/MainWindow.*`) builds a `PublishConfig` from operator input and
  emits `startRequested`.
- The UI enumerates camera and microphone devices when Qt Multimedia is present.
  These selections are stored in `PublishConfig`.
- Main wires this into `LivePipeline` and `MoqxrPublisher`.
- `LivePipeline` opens one TS/M2TS source and passes the selected program number
  to the packetizer. Program `0` means the first nonzero PAT program.
- If no TS/M2TS file source is provided and a camera or microphone is selected,
  `LivePipeline` opens `LibavCaptureSource` instead.
- `M2tsPacketizer` detects 188-byte TS or 192-byte M2TS source packets and
  validates sync bytes.
- `LibavCaptureSource` opens selected devices through libavdevice, transcodes
  to H.264 plus AAC/Opus, muxes MPEG-TS into an in-memory AVIO sink, extracts
  PAT/PMT as `initData`, and emits 188-byte TS packet objects.
- `M2tsPacketizer` scans PAT/PMT, selects one program, and filters media
  objects to PAT, selected PMT, selected PCR PID, and that program's elementary
  PIDs. Packets from other programs and null packets are not published.
- `MsftsMuxer` builds the MSF catalog with `packaging: "m2ts"`.
- Whole source packets are grouped into MOQT Object payloads and exposed to
  `MoqxrPublisher::publishLiveObjects(...)`.

## File-level map

- `src/app/MainWindow.h/.cpp`
  - Qt UI and user controls.
  - Emits start/stop events and receives status/log callbacks.

- `src/media/LivePipeline.h/.cpp`
  - Packetizer setup, catalog publication, media object loop, and pacing.
  - Uses `std::thread`.

- `src/media/M2tsPacketizer.*`
  - Detects TS/M2TS packet size.
  - Emits object payloads made only from whole source packets.
  - Scans the initial packet window for PAT and PMT packets, preserves those
    packets in source-packet form, and exposes them for catalog `initData`.
  - Filters MPTS inputs down to the selected program's PSI/PCR/elementary PIDs
    before objectization.

- `src/media/LibavCaptureSource.*`
  - Optional direct libavdevice capture path.
  - Uses libavcodec for H.264/AAC/Opus encoding and libavformat for MPEG-TS
    muxing.
  - Emits the same `M2tsObject` shape as the file packetizer.

- `src/media/MsftsMuxer.*`
  - Generates a compact MSF catalog for the `m2ts` packaging value.

- `src/publish/MoqxrPublisher.*`
  - Provides `IMoqOutput` interface.
  - Mock implementation logs payload sends.
  - `MOQ2TS_HAS_MOQXR` compile section is the hook for real SDK wiring.

- `CMakeLists.txt`
  - UI + dependencies + optional feature toggles.

## Building and dependency matrix

| Feature        | Toggle | Required to enable | Code path |
|----------------|--------|-------------------|----------|
| Mock publisher | `MOQ2TS_BUILD_WITH_MOCK_MOQXR` | none | default ON |
| Device listing | auto-detected | Qt Multimedia | UI enumeration only |
| Device capture | auto-detected | libavdevice/libswscale/libswresample | camera/mic to MPEG-TS |
| OpenH264 path  | `MOQ2TS_ENABLE_OPENH264`     | libopenh264          | encoder integration |
| libav path     | `MOQ2TS_ENABLE_LIBAV_AUDIO`  | libavcodec/libavformat | media integration |
| libopus path   | `MOQ2TS_ENABLE_LIBOPUS`      | libopus              | Opus audio path |

## Integrating the real moqxr API

The app uses the local moqxr `Publisher::publish_live_objects` API. The adapter
builds a `LiveObjectSource` with:

- a `catalog` track carrying the MSF catalog JSON
- one `m2ts` media track carrying source-packet object payloads
- caller-supplied Group ID, Subgroup ID, Object ID, media time, and duration
- payloads without CMSF/CMAF parsing

Suggested mapping:

- `connect(cfg)` -> store endpoint, namespace, and stream settings
- `publishLiveObjects(...)` -> call `openmoq::publisher::Publisher::publish_live_objects`
- `stop()` -> close session and flush buffers

## Draft MSFTS conformance notes

The media Object payload is only consecutive source packets. Do not prepend
private headers. Object payload length must be a multiple of `m2tsPacketSize`.

`initData` is Base64 of whole source packets. For 188-byte TS this means PAT
and PMT packets beginning with sync byte `0x47`; for 192-byte M2TS this means
the four-byte timestamp prefix is retained and sync byte `0x47` remains at
offset 4.

For an MPTS input, `m2tsProgramNumber` identifies the selected program. The
published media track is program-filtered; it is not a raw pass-through of every
PID in the source multiplex.

## Runtime operational guidance

- Keep both audio and video sources timestamped from a single master clock if
  low-latency sync matters.
- If using separate files, treat as asynchronous tracks and apply track-level
  offset compensation at the sender.
- For true live capture, replace file-based source URIs with capture endpoints.

## Known limitations in this scaffold

- Audio/video synchronization is intentionally simplified.
- Capture device naming is platform-specific. Linux uses `v4l2` for camera IDs
  and PulseAudio device names for microphones; Windows uses DirectShow-style
  names; macOS uses AVFoundation names.
- The sample MSFTS frameizer is draft-oriented and not the authoritative final
  wire format.
- Program filtering preserves source PAT/PMT packet bytes in `initData`; it does
  not rewrite PAT sections to remove other program entries yet.
- No retransmission/back-pressure strategy is included yet.
- No auth/token refresh path is implemented in the publisher adapter.
