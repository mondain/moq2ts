# Sequence Diagrams — MOQ2TS Publisher

## 1) Live publish startup and first fragments

```mermaid
sequenceDiagram
    participant User
    participant UI as MainWindow
    participant App as main.cpp
    participant Pipe as LivePipeline
    participant Pkt as M2tsPacketizer
    participant Mux as MsftsMuxer
    participant Pub as MoqxrPublisher

    User->>UI: Fill endpoint, stream, and source paths
    User->>UI: Click Start
    UI->>App: emit startRequested(config)
    App->>Pub: connect(config)
    Pub-->>App: connectionStateChanged(connected=true)
    App->>Pipe: start(config, publisher)
    Pipe->>Pkt: open M2TS source
    Pkt-->>Pipe: packetSize = 188 or 192
    Pipe->>Mux: catalogJson(track, packetSize, packetsPerObject)
    Pipe->>Pub: publishLiveObjects(source)
    Pipe->>Pkt: readObject(packetsPerObject)
    Pkt-->>Pipe: whole source packets
    Pub-->>Pipe: pull next LiveObject
    Pub-->>Pipe: framePublished(bytes)
    Pipe-->>UI: stats()
    Pipe-->>UI: status("running")
```

## 2) Error path and fallback

```mermaid
sequenceDiagram
    participant UI as MainWindow
    participant Pipe as LivePipeline
    participant Pkt as M2tsPacketizer
    participant Pub as MoqxrPublisher

    Pipe->>Pub: publishLiveObjects(...)
    Pub-->>Pipe: publishError("network")
    Pipe->>UI: error("publish failed")
    UI-->>UI: show error dialog
    UI-->>App: user selects Stop / retry
    App->>Pub: stop()
    App->>Pipe: stop()
    App->>UI: onPublishStatus("Stopped")
```

## 3) Media track handling

```mermaid
sequenceDiagram
    participant Pipe as LivePipeline
    participant Pkt as M2tsPacketizer
    participant Pub as MoqxrPublisher

    Pipe->>Pkt: read packets from multiplexed TS/M2TS source
    Pkt-->>Pipe: object payload = source packet(s)
    Pipe->>Pub: publishLiveObjects(source)
    Pub-->>Pkt: nextObject callback
```

## 4) Reconnect strategy (recommended extension point)

```mermaid
sequenceDiagram
    participant Pub as MoqxrPublisher
    participant App as main.cpp
    participant Pipe as LivePipeline

    Pub-->>App: connectionStateChanged(connected=false)
    App->>Pipe: stop()
    App->>Pub: stop()
    App->>App: apply retry timer (exponential or fixed)
    App->>Pub: connect(config)
    Pub-->>App: connected true
    App->>Pipe: start(config, publisher)
```
