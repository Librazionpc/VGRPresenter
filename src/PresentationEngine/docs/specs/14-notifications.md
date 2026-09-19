# 14 — Notification Service

Phase 4. The only system responsible for deciding **if, when, where, and how**
the user is informed. Converts engine events (EventBus) into user notifications
without coupling the backend to any UI.

**Status**: Implemented — `modules/notification/` (namespace `bps::notification`).
Architecture: `docs/architecture/Notifications.md`. Acceptance: Conformance §17.

## Purpose

- Be the central nervous system's *presentation* layer: EventBus transports all
  engine events; the Notification Service subscribes, filters, and decides which
  become user-visible.
- Keep the backend headless-capable: providers are the only UI touchpoint.
- Never create events; it only listens (except `NotificationActionInvoked`, which
  is a *user-interaction* signal — the UI triggers it, modules handle it).

## Responsibilities

Subscribe to EventBus → filter → decide visibility (rules) → queue → dispatch →
providers. Supports grouping, dedup, priority, auto-expiry, persistence, actions,
progress updates, scheduling (quiet hours / presenting suppression).

Never: call the UI directly, create dialogs, load media, perform business logic,
change engine state.

## Pipeline

```
EventBus → NotificationFactory → NotificationRules → NotificationQueue
         → NotificationDispatcher → NotificationManager → INotificationProvider
                                                        (Toast/Center/StatusBar/Console/…)
```

## Public API (NotificationService facade)

- `Initialize/Start/Stop/Shutdown/Reload/Reset/GetHealth/MetricsSnapshot` (IService).
- `Publish(const NotificationSeed&)` — programmatic entry (equivalent to an event).
- `NotifyNow(Notification)` — enqueue for immediate dispatch.
- `NotifyAfter(Notification, delay)` — schedule for later (NotificationScheduler).
- `RegisterProvider(shared_ptr<INotificationProvider>)` / `UnregisterProvider` /
  `SetProviderEnabled(name, bool)`.
- `RegisterRule(shared_ptr<INotificationRule>)` / rule management.
- `RegisterFormatter(topic, formatter)`.
- `RegisterChannelMapping(topic, channel, severity)`.
- `History()` / `SearchHistory(query)` / `MarkRead(id)` / `Dismiss(id)` /
  `ClearHistory()` / `ExportHistory(path)`.
- `UpdateProgress(correlationId, fraction)` — updates a progress notification.
- `SetPresenting(bool)` — presentation-mode suppression.
- `InvokeAction(id, actionId)` — publishes `NotificationActionInvoked`.

## Notification Object

`NotificationID, CorrelationID, Timestamp, SourceModule, SourceEvent, Severity,
Category, Priority, Title, Message, Payload, Progress, Actions, Persistent,
Dismissed, Expiry, Channels, Tags`.

Severity: `Critical > Error > Warning > Success > Information > Debug`.
Priority: `Critical > High > Normal > Low` (queue ordering).
Channels: strings (`"toast"`, `"statusbar"`, `"center"`, `"console"`, `"banner"`,
`"remote"`, …) — never a hardcoded enum; providers advertise supported channels.

## Internal Components

| Component | File | Role |
|---|---|---|
| NotificationService | `NotificationService.hpp/.cpp` | Facade, EventBus pipeline, lifecycle |
| NotificationManager | `NotificationManager.hpp/.cpp` | Provider registry + dispatch |
| NotificationFactory | `NotificationFactory.hpp/.cpp` | Event → Notification seeds |
| NotificationQueue | `NotificationQueue.hpp/.cpp` | Priority FIFO, rate limit, dedup, grouping |
| NotificationDispatcher | `NotificationDispatcher.hpp/.cpp` | Route to providers, capability check, retry |
| NotificationHistory | `NotificationHistory.hpp/.cpp` | Searchable, filterable, exportable |
| NotificationRules | `NotificationRules.hpp/.cpp` | Policy evaluation (configurable) |
| NotificationScheduler | `NotificationScheduler.hpp/.cpp` | Delayed + quiet-hours + presenting |
| NotificationStorage | `NotificationStorage.hpp/.cpp` | Memory + persistent (DatabaseManager) |
| NotificationFormatter | `NotificationFormatter.hpp/.cpp` | Topic → user string templates |
| NotificationProviderRegistry | `NotificationManager.hpp` | Provider registry |

## Interfaces

`INotificationProvider`, `INotificationRule`, `INotificationStorage`,
`INotificationFormatter`, `INotificationFilter` — everything interface-driven
(Manager → Registry → Interface → Providers).

## Events Consumed

`content.asset_*`, `project.*` (opened/saved/closed/deleted), `engine.resource.*`,
`engine.kernel.*`, `platform.monitor_*`, `engine.module.*`, `engine.plugin.*`,
`presentation.*`, `songs.*` — mapped to notification seeds by the factory.

## Events Produced

`notification.action_invoked` (user interaction only).

## Configuration (`ConfigurationManager`)

- `notify.enabled` (bool)
- `notify.severity.{critical,error,warning,success,info,debug}.enabled`
- `notify.autoDismissMs.{success,info,warning}` (e.g. 5000/8000/30000)
- `notify.history.maxEntries` (default 1000)
- `notify.channels.{toast,statusbar,center,console,banner}.enabled`
- `notify.quietHours.enabled` + `{start, end}` (HH:MM)
- `notify.presenting.suppressNonCritical` (default true)

## Failure Modes

- Provider Show fails → `Notification_ProviderFailed`, other providers unaffected.
- Unknown channel in a notification → routed to `"console"` only, logged.
- Queue full → oldest low-priority evicted (bounded, default 4096).
- History full → oldest trimmed (default 1000).

## Performance Goals

Never blocks the EventBus; all dispatch via ThreadPool/TaskScheduler; hundreds of
notifications/sec handled; dedup + grouping bound the UI work.

## Testing Expectations

Queue ordering, severity filtering, grouping, auto-expiry, history persistence,
progress updates, configuration rules, concurrent delivery, large bursts
(`TestNotify*`).

## Future Extensions

Email/Webhook/Discord/Slack/Teams/OBS/Mobile providers (implement
`INotificationProvider`, register — no engine change). Notification Policy Engine
exposed as configurable rule scripts. Replay of notification history after restart.
