# Architecture Documentation

This directory holds the architectural home documents for Believers Presentation
Software. Behavior-level contracts live in [`../specs/`](../specs/README.md); these
documents describe *structure and conformance*.

| Document | Content |
|---|---|
| [`SystemArchitecture.md`](./SystemArchitecture.md) | The layered architecture: Frontends → Communication → Core → Managers → Feature Modules → Platform. The **core purity rule** (the core never knows about Presentation/Bible/Songs/AI) and the **engine-wide manager contract** live here. |
| [`Conformance.md`](./Conformance.md) | Audit of every architecture expectation against the implemented code (✅ Met / ◑ Partial / ○ Deferred), including the lifecycle state-machine delta (13 vs 12) and the v1 implementation footprint. |

Legacy placeholders (`CoreArchitecture.md`, `EventSystem.md`, `ModuleLifecycle.md`,
`PluginSystem.md`, `RenderingPipeline.md`, `ResourceManager.md`, `StartupSequence.md`)
are superseded by the spec set — see `docs/specs/` for the authoritative per-system
behavior. They are kept as empty stubs to preserve paths; new detailed architecture
content should be added here only if it is cross-system (per-system content belongs in
the specs).
