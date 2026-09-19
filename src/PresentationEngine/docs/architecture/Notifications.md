# Notification Service — Architecture

Phase 4. The EventBus transports all engine events; the Notification Service is
the *only* system that decides if/when/where/how the user is informed. It has
zero UI knowledge — providers are the boundary.

```
                EventBus
                   │  (all engine events)
                   ▼
        ┌───────────────────────┐
        │   NotificationService │  subscribes; builds seeds
        └───────────┬───────────┘
                    ▼
        ┌───────────────────────┐
        │   NotificationFactory │  event topic → NotificationSeed
        └───────────┬───────────┘
                    ▼
        ┌───────────────────────┐
        │    NotificationRules  │  policy: show? channel? dismiss? persist?
        └───────────┬───────────┘
                    ▼
        ┌───────────────────────┐
        │   NotificationQueue   │  priority FIFO, rate-limit, dedup, grouping
        └───────────┬───────────┘
                    ▼
        ┌───────────────────────┐
        │ NotificationDispatcher│  capability check, per-provider retry
        └───────────┬───────────┘
                    ▼
        ┌───────────────────────┐
        │   NotificationManager │  provider registry
        └───────────┬───────────┘
                    ▼
        INotificationProvider implementations
        (Console │ Center │ StatusBar │ Toast │ Banner │ Remote │ future)
```

## Key design decisions

- **Channels are strings, not enums.** A provider advertises the channels it
  supports; the dispatcher routes by capability. Adding a channel (Discord,
  Slack, Teams, Email, Webhook, OBS, Mobile) never touches the service.
- **Providers are registered, not hardcoded** — exactly like `IImporter`.
  `NotificationManager` owns the registry; the service coordinates.
- **The service never creates events** (except `NotificationActionInvoked`,
  which is a user-interaction signal published when a UI triggers an action —
  the owning module handles it).
- **Headless-safe**: with only Console/Center providers registered, the engine
  runs fully headless. A WinUI/Qt/Web frontend adds its own providers.
- **Never blocks the EventBus**: queue + ThreadPool/TaskScheduler dispatch.
- **Presenting mode**: `SetPresenting(true)` suppresses non-critical
  notifications (never interrupt a live presentation).

## Severity → default channel mapping (configurable)

| Severity | Default channels | Auto-dismiss |
|---|---|---|
| Critical | center, banner, console | never (persistent) |
| Error | center, banner, console | never (persistent) |
| Warning | center, statusbar, console | 30 s |
| Success | statusbar, center, console | 5 s |
| Information | statusbar, console | 8 s |
| Debug | console only | — |

## Grouping

Repeated notifications sharing a group key merge into one entry with a count
("200 assets imported") — the queue maintains a dedup window.

## Progress

`UpdateProgress(correlationId, fraction)` updates an existing progress
notification instead of creating new ones.

## Persistence

`NotificationStorage` (INotificationStorage): `MemoryStorage` for the live
history + `DatabaseManager` (`notifications` collection) for history that
survives restarts.

## Actions

Notifications carry `{id, label}` action descriptors. The service publishes
`NotificationActionInvoked`; modules subscribe and perform the work. The
service carries identifiers only.
