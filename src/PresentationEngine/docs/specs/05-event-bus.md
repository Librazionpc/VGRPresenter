# 05 — EventBus Specification

| Field | Value |
|---|---|
| **System** | EventBus |
| **Spec version** | 1.0 |
| **Spec status** | Approved |
| **Implementation status** | **Implemented** — full implementation with unit tests (`tests/unit/main.cpp`); audited in `docs/architecture/Conformance.md` |
| **Source files** | `core/include/core/events/EventBus.hpp`, `core/events/EventBus.cpp`, `interfaces/include/interfaces/IEvent.hpp` |
| **Depends on** | Logger (02), ConfigurationManager (03), ServiceManager (04) |
| **Inherits** | 00 Cross-System Rules |

## 1. Purpose

The EventBus is the **most important system** in the engine: it is the *only* approved
channel for cross-system communication. **No module directly calls another.** When a
slide changes, the presentation module publishes `SlideChanged` and the EventBus fans it
out to whoever cares — Display, Remote, NDI, OBS, Logger — without any of them knowing
about each other. This decoupling is what makes modules hot-reloadable, testable, and
additive.

## 2. Responsibilities

- **Publish** events to any topic with typed payloads.
- **Subscribe / Unsubscribe** with token-based lifecycle (auto-cleanup on death).
- **Broadcast** to all subscribers (or all except a sender).
- **Request/Response** correlation (one-shot query, matched by request ID).
- **Priority ordering** of subscribers within a topic.
- **Sync / Async / Delayed** delivery modes.
- **Sticky events** (last value retained for late subscribers).
- **History + Replay** (bounded ring of past events; replay by filter).
- **Filtering** (by topic, event type, payload predicate, source).
- **Broken-subscriber isolation** — a crashing subscriber must not take down publishers.

## 3. Public API

```cpp
class EventBus final {
public:
    static EventBus& Instance();

    // Publish
    Result<void> Publish(const IEvent& event, PublishOptions opts = {});   // sync default
    Result<void> PublishAsync(const IEvent& event, PublishOptions opts = {});
    Result<void> Broadcast(const IEvent& event, PublishOptions opts = {});
    Result<void> PublishDelayed(const IEvent& event, std::chrono::microseconds delay,
                                PublishOptions opts = {});

    // Request/Response
    template <class Req, class Res>
    Result<Res> Request(const Req& request, std::chrono::microseconds timeout);

    // Subscribe
    template <class EventT>
    Subscription Subscribe(EventCallback<EventT> cb,
                           SubscribeOptions opts = {});     // priority, mode, filter
    Result<void> Unsubscribe(Subscription token);
    size_t       SubscriberCount(std::string_view topic) const noexcept;

    // Sticky / history / replay
    Result<void> SetSticky(const IEvent& event);
    std::optional<IEvent> GetSticky(std::string_view topic) const;
    Result<std::vector<IEvent>> History(const EventQuery& query) const;
    Result<void> Replay(const EventQuery& query, EventCallback<IEvent> cb);

    // Diagnostics
    HealthReport GetHealth() const noexcept;                 // 00 §7
    EventMetrics Metrics() const noexcept;                   // 00 §4
};
```

`PublishOptions`: `{mode: Sync|Async|Delayed, delay, priorityFloor, sticky, sourceTag}`.
`SubscribeOptions`: `{priority (int, higher first), mode, filter (predicate)}`.

## 4. Internal Components

| Component | Role |
|---|---|
| `EventBus` | Facade; topic registry |
| `TopicRegistry` | Topic → subscriber list (sorted by priority) |
| `Subscriber` | Token, callback, mode, filter, dead-letter flag |
| `Dispatcher` | Sync dispatch loop (publisher thread) + async fan-out to ThreadPool (06) |
| `RequestRouter` | Request/response correlation table (request IDs, timeouts) |
| `StickyStore` | Last-event-per-topic retention |
| `HistoryRing` | Bounded ring buffer of published events (configurable size) |
| `ReplayEngine` | Filtered replay over `HistoryRing` |
| `DeadLetterQueue` | Captures events that failed to deliver (subscriber crash/timeout) |
| `IEvent` | Base interface: `topic()`, `payload type id`, `timestamp()`, `sourceTag()` |

## 5. State Machine

```
Idle --> Ready --> Dispatching --> Ready
            |          |
            +--> Quiescing -> Paused   (graceful drain before shutdown / hot reload)
```

| State | Meaning |
|---|---|
| `Idle` | Pre-boot; publishes queued into the pre-boot buffer (01 §7) |
| `Ready` | Normal operation |
| `Dispatching` | A sync dispatch is in flight on the current thread |
| `Paused` | Delivery suspended (draining); used during shutdown and `Replace()` of a subscriber's service (04) |

## 6. Threading Model

- **Multi-threaded, lock-free fast path.** Topic registry is an immutable snapshot
  behind RCU-style swaps; publish reads the snapshot and calls callbacks directly (sync
  mode). Subscriber lists are modified via copy-on-write.
- **Async/Delayed** events are posted to the ThreadPool (06) with a priority floor; the
  TaskScheduler (07) handles `Delayed` timing.
- **Request/Response** uses condition-variable correlation keyed by request ID with a
  timeout; timeouts are reaped by the scheduler.
- **Guarantee:** a sync publish never blocks the publisher on a slow subscriber beyond a
  per-subscriber budget; over-budget subscribers are moved to async with a warning
  (dead-letter accounting).

## 7. Events Published

The bus is *itself* observable. `engine.eventbus.*` topics report health and flow:

| Event | Payload | When |
|---|---|---|
| `engine.eventbus.subscriber_added` | `{topic, priority}` | Subscriber registered |
| `engine.eventbus.subscriber_removed` | `{topic, priority}` | Subscriber unregistered |
| `engine.eventbus.subscriber_slow` | `{topic, p95_ns, budget_ns}` | Subscriber exceeded budget |
| `engine.eventbus.dead_letter` | `{topic, event, reason}` | Undeliverable event |
| `engine.eventbus.history_trimmed` | `{dropped, oldest_ts}` | Ring overflow |

Engine-wide domain event categories (namespaced, e.g. `presentation.slide_changed`,
`media.video_state`, `display.output_changed`, `audio.level`, `input.key`,
`plugin.loaded`, `ai.inference_done`, `network.connected`, `user.action`):

- **System** — `system.*` (boot, health, lifecycle)
- **Presentation** — `presentation.*` (slide, playlist, stage)
- **Media** — `media.*` (video, image, audio asset state)
- **Display** — `display.*` (outputs, monitors, GPU surface)
- **Audio** — `audio.*` (device, levels, routing)
- **Input** — `input.*` (keyboard, mouse, MIDI, remote)
- **Plugin** — `plugin.*` (load/unload/state)
- **AI** — `ai.*` (inference, transcription, suggestions)
- **Network** — `network.*` (connectivity, sync, streaming)
- **User** — `user.*` (intent, undo/redo, preferences)

### Canonical example

```
Presentation module:  Publish(SlideChanged{index: 7})
                              │
                              v
                        [ EventBus ]
                        /    |    |    \
                       v     v    v    v
                   Display Remote NDI  OBS   Logger
```

Each consumer subscribes to `presentation.slide_changed` independently. Adding a new
consumer (e.g. `Streaming`) requires **zero** changes to the publisher.

## 8. Events Consumed

| Event | Action |
|---|---|
| `engine.kernel.state_changed` (01) | Pause delivery during shutdown; flush pre-boot buffer on `booted` |
| `engine.resource.pressure_high` (10) | Trim history ring, reduce async fan-out |
| `engine.config.hot_reload` (03) | Re-apply budgets/history size without restart |

## 9. Dependencies

- **Depends on:** Logger (02), ConfigurationManager (03), ServiceManager (04).
- **Uses after init:** ThreadPool (06) for async fan-out; TaskScheduler (07) for
  delayed delivery and request timeouts.
- **Provides:** the communication backbone for every other system.

## 10. Failure Modes

| Failure | Behavior |
|---|---|
| Subscriber throws / crashes mid-callback | Catch in dispatcher; mark subscriber `Degraded`; route its events to the dead-letter queue; keep publishing. Third crash in 10 s → auto-unsubscribe + `engine.eventbus.dead_letter` |
| Subscriber exceeds time budget | Move to async delivery for that subscriber with a warning metric |
| History ring overflow | Trim oldest, count metric, publish `history_trimmed` |
| Request timeout | `Request` returns `ErrorCode::EventBus_RequestTimeout`; response is dropped if it arrives late |
| Async queue backpressure (06 busy) | Queue on the bus's bounded pending list; if full, drop lowest-priority async events (counted) — never block the publisher |
| Duplicate/corrupt event ID | Dropped with `Error` + dead-letter entry |

## 11. Performance Goals

| Goal | Target |
|---|---|
| Sync publish with 10 subscribers | **< 5 µs** (exclusive of subscriber work) |
| Async fan-out overhead | **< 2 µs** enqueue |
| Dispatch p95 (any mode) | **< 100 µs** including subscriber time (excluding deliberately heavy consumers) |
| History ring | 10 000 events default, configurable, bounded |
| Concurrent topics | 10 k+ topics, no per-topic lock on the hot path |

## 12. Future Extensions

- **Event sourcing replay** — replay the full history to rebuild module state (already
  supported by History + Replay; a recovery service can layer on top).
- **Distributed bus** — bridge topics over the network module with exactly-once
  semantics; the API is identical.
- **Event tracing UI** (timeline inspector) driven by `engine.eventbus.*` metrics.
- **Schema'd event contracts** (protobuf/flatbuffers) behind the same `IEvent` facade.
- **Offline queueing** for disconnected remote controllers.

## 13. Testing Requirements

- **Unit:** subscribe/unsubscribe semantics, priority order, sticky, history, replay
  filters, request/response correlation and timeouts, delayed delivery.
- **Stress:** 1 M events/s across 100 topics with 50 subscribers each; 16 producer
  threads — no drops (within budget), no deadlock.
- **Performance:** assert §11 budgets.
- **Failure:** throwing subscriber, slow subscriber, ring overflow, timeout storms,
  async queue saturation — verify isolation and dead-letter accounting.
