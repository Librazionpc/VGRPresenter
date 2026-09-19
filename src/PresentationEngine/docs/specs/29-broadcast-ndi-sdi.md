# 29 — Broadcast Engine (NDI & SDI)

## Objective

Add first-class NDI (Network Device Interface) and SDI (Serial Digital
Interface) support to the platform as **outputs and inputs of the Phase 15
production graph** — not as a separate rendering system.

The Broadcast Engine provides:

* NDI **senders** (publish the program video/audio bus as an NDI source)
* NDI **receivers** (ingest NDI sources as production-graph inputs)
* NDI **source discovery** (find remote NDI sources on the network)
* SDI **capture** (DeckLink-style devices feed the production graph)
* SDI **output** (DeckLink-style devices present the program bus)
* A **software loopback provider** so the architecture is fully testable and
  demonstrable without any hardware or SDK installed

## 1. Core Principle — Providers, Not Core Code

NDI (NewTek/Vizrt SDK) and SDI (Blackmagic DeckLink SDK) are **proprietary
runtime libraries**. The engine never hard-codes them.

```text
NDI SDK (libndi) ──┐
DeckLink SDK ──────┤
Software Loopback ─┤
Future Provider ───┤
                   ▼
         BroadcastEngine
                   │
   ┌───────────────┼───────────────┐
   ▼               ▼               ▼
 NDI Send       NDI Receive     SDI Capture/Output
   │               │               │
   └───────┬───────┴───────┬───────┘
           ▼               ▼
     Production Graph  Recording Engine
     (sources/buses/    (capture sources,
      outputs)          ISO/archival)
```

Adding a new broadcast technology (e.g. SRT ingest, a capture card, a virtual
camera) means implementing `IBroadcastProvider` and registering it — the
Broadcast Engine and the production graph do not change.

## 2. Runtime SDK Loading

Both SDKs are resolved at runtime with `dlopen` / `LoadLibrary`:

* `NdiSdk` resolves the NDI v5 C API function table
  (`NDIlib_initialize`, `NDIlib_send_create`, `NDIlib_recv_create_v3`,
  `NDIlib_find_create_v2`, …).
* `DeckLinkSdk` resolves `CreateDeckLinkIteratorInstance` and binds the
  minimal COM interfaces needed to enumerate and drive DeckLink devices.

If a library is absent, symbols fail to resolve, or initialization fails,
the matching provider reports `Err::Unsupported` with a readable message.
The engine and the rest of the platform keep working — this is the same
contract the AdaptiveRuntime already applies to the `"ndi"` feature
(`RecommendState` disables heavy features on machines that cannot host them).

## 3. Provider Model

```text
IBroadcastProvider          (probe / availability / kind)
├── INdiProvider            (discover, create sender, create receiver)
├── ISdiProvider            (enumerate devices, capture/output)
└── ISoftwareProvider       (loopback sender/receiver for tests + demo)
```

Every provider implements a stable `Probe()` that returns:

* `Available` — SDK present and healthy
* `Degraded` — SDK present but partial (e.g. no encoder)
* `Unsupported` — SDK/hardware not installed

Providers are registered with the `BroadcastEngine` and can be added by
plugins without modifying the engine.

## 4. NDI Model

### NDI Source

```text
NdiSource
├── name        ("CAM 1 (192.168.1.10)")
├── urlAddress  ("ndi://192.168.1.10/CAM%201")
```

Discovery via the NDI finder is polled on demand (`DiscoverSources`) and
published on the EventBus (`broadcast.ndi_sources_changed`).

### NDI Sender

`CreateNdiSender(name, config)` returns a `BroadcastSenderId`. Frames are
pushed with `SendVideoFrame` / `SendAudioFrame`:

```text
program video bus ──► NDI video frame (BGRA/UYVY) ──► libndi sender
program audio bus ──► NDI audio frame (float, interleaved) ──► libndi sender
```

The sender carries the standard NDI metadata (`program_name`, `source_url`,
`resolution`, `fps`, `audio` config) so downstream tools can identify the
feed.

### NDI Receiver

`CreateNdiReceiver(source)` returns a `BroadcastReceiverId`. `ReceiveFrame`
pulls the latest video+audio frame (blocking with a timeout) and the engine
registers a matching **virtual source** in the production graph
(`AddVirtualSource`) so incoming NDI can be routed like any camera.

## 5. SDI Model

### SDI Device

```text
SdiDevice
├── deviceIndex
├── modelName   ("DeckLink Mini Recorder")
├── displayName ("DeckLink Mini Recorder 1")
├── capabilities (capture/output/dual)
└── state
```

`EnumerateSdiDevices()` lists the DeckLink devices installed. A connected
device becomes a production-graph source (`sdi.capture_connected` event).

### SDI Capture

```text
DeckLink device ──► IDeckLinkInput ──► video/audio callback ──► graph source
```

The capture callback receives frames on the DeckLink thread and hands them
to the engine, which publishes `broadcast.frame_received` and feeds the
bound graph node.

### SDI Output

```text
program bus ──► IDeckLinkOutput ──► schedule frame ──► DeckLink device
```

Scheduled playback keeps the device clock; the engine passes the program
bus frames with their master-clock timestamps.

## 6. EventBus Integration

Consumes: `production.output_changed`, `recording.capture_connected`,
`adaptive.feature_changed`.

Publishes:

```text
broadcast.provider_registered
broadcast.provider_unavailable
broadcast.ndi_sources_changed
broadcast.sender_started
broadcast.sender_stopped
broadcast.receiver_connected
broadcast.receiver_disconnected
broadcast.frame_sent
broadcast.frame_received
broadcast.sdi_devices_changed
broadcast.sdi_capture_started
broadcast.sdi_capture_stopped
broadcast.error
```

## 7. Notification Integration

`BroadcastEngine` publishes events; the Notification Service decides how to
surface them (NDI source appeared, DeckLink device disconnected, provider
unavailable, sender started, receiver connected).

## 8. Adaptive Runtime Integration

The engine reports NDI/SDI availability to the AdaptiveRuntime feature
registry:

* `"ndi"` feature enabled when the NDI SDK is present and the machine can
  host it
* SDI hardware is reported through the hardware/device providers

This keeps heavy broadcast workloads out of small machines automatically.

## 9. Security

* NDI metadata contains only public identifiers (names, URLs, format info).
* DeckLink/NDI credentials are never stored; no secrets enter `.vgr` files.
* Remote NDI sources are discoverable but never auto-connected without an
  explicit operator action.

## 10. Performance

* Frames are handed to the SDK with no copies where possible (the graph
  already owns decoded buffers).
* `ReceiveFrame` and source discovery run on the caller thread; the engine
  never blocks the render or display threads.
* Senders/receivers can be torn down independently without stopping the
  production.

## 11. Software Loopback Provider

For development, CI, and operator rehearsal, a built-in `software` provider
implements the same sender/receiver contract over in-memory buffers:

```text
software sender ──► ring buffer ──► software receiver
```

This makes the entire Broadcast Engine testable without the NDI or DeckLink
SDKs and is used by `tests_broadcast` and the CLI demo.

## 12. Acceptance Test

```text
BroadcastEngine::Initialize()
RegisterProvider(software)
CreateNdiSender("Worship Cam")          ──► sender id
SendVideoFrame(...)                     ──► frame_sent
DiscoverSources()                       ──► finds loopback source
CreateNdiReceiver(loopback source)      ──► receiver id
ReceiveFrame(timeout)                   ──► returns the sent frame
EnumerateSdiDevices()                   ──► [] when no DeckLink (Unsupported)
NdiSdk::Load() on a machine without NDI ──► false, provider = Unsupported
Shutdown()                              ──► all providers torn down
```

The same flow works against real NDI/DeckLink when their SDKs are installed
— no engine changes required.
