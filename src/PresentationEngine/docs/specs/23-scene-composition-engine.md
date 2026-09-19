# 23. Scene Composition Engine (Phase 11)

## Objective

The definitive architecture for the platform: **everything is a Scene**. Not
slides, not songs, not Bible verses — everything. A scene is a composition of
regions, layers, widgets, and components. Each output renders the same scene
through its own **layout**. Content, style, and layout are fully separated.

```text
Workspace → Project → Presentation → Scene
                                      ├─ Regions → Layers → Widgets → Components
                                      └─ Output Rules → Layouts (Audience/Stage/Stream/...)
```

## Widget philosophy

Do not create special systems for Songs, Bible, Images, Videos, Lower Thirds,
Countdowns, or Logos. Everything is a **Widget** deriving from `IWidget`:

```cpp
class IWidget {
public:
    virtual ~IWidget() = default;
    virtual const char* Id() const noexcept = 0;
    virtual const char* Type() const noexcept = 0;   // "lyrics", "clock", "logo"...
    virtual void Update(double dt) = 0;
    virtual void Compose(rendering::RenderEngine& engine,
                         const std::string& sceneId) = 0;
};
```

Built-in widgets: ClockWidget, CountdownWidget, LogoWidget, LyricsWidget,
BibleWidget, MediaWidget, VideoWidget, ImageWidget, TickerWidget, WeatherWidget,
QRWidget, CameraWidget, ChatWidget, DonationWidget, ScoreWidget — plus plugin
widgets. Widgets carry components (Text Component, Animation Component, Shadow
Component, Interaction Component) — composition, not inheritance explosion.

## Regions & layouts

A scene contains **regions** (Background, Main, Lower Third, Overlay, Safe Area,
Notification). Regions don't know what they contain — only widgets. A **Layout**
tells regions where to appear (position, size, anchors, percentage/absolute).
Every output picks a layout: Audience Layout, Stage Layout, Stream Layout,
Recording Layout, Preview Layout. One presentation → multiple renderings.

## Templates & themes

Templates are **Scene Blueprints** (Song Scene Template, Bible Template,
Announcement Template) — never presentation objects. Themes control fonts,
colors, spacing, animations, effects, transitions, and safe areas; widgets never
own these values. Changing one theme updates thousands of songs. Template
inheritance: a base Song template is overridden only where children differ.
Templates and themes are stored as `.vgr` documents (internal types), so they
are searched, versioned, shared, synced, and inherited with existing
infrastructure.

## Rule engine

Instead of hardcoding "if Song → show Lyrics", a Rule Engine decides:

```text
IF ContentType == Song AND Output == Audience THEN Use Song Layout
IF Output == Stage THEN Hide Background, Show Chords, Show Next Verse
IF Output == Stream THEN Show Lower Third, Hide Chords, Show Logo
```

Rules are data — configurable, never scattered through code.

## Dynamic layout resolution

```text
Content → Determine Type → Apply Rules → Select Template → Apply Theme
       → Resolve Widgets → Build Scene → Render → Output
```

## Component tree

```text
Scene → Region → Layer → Widget → Component → Scene Graph → Rendering Engine
```

## Providers everywhere

Template Provider, Theme Provider, Layout Provider, Widget Provider, Animation
Provider, Transition Provider, Output Provider, Import Provider, Search
Provider — every subsystem is provider-registered, exactly like Importers,
Notifications, and Display providers.

## Native format

Everything is stored inside `.vgr` (docs/specs/22): Document Type → Scene →
Layout → Theme → Presentation → Workspace. One format, many document types.

## EventBus

Consumes: `presentation.slide_changed`, `ConfigHotReload`,
`engine.resource.pressure_changed`.
Publishes: `scene.composed`, `scene.layout_applied`, `scene.rule_applied`,
`scene.theme_applied`, `scene.widget_added`, `scene.widget_removed`.

## Definition of Done

- [x] Scene tree: Regions → Layers → Widgets → Components.
- [x] IWidget interface + built-in widgets (clock, countdown, logo, lyrics,
      bible, media, image, video, ticker).
- [x] Layouts per output (Audience/Stage/Stream/Preview/Recording) with anchors
      and responsive scaling.
- [x] Themes (fonts, colors, spacing, safe area) — content never owns styling.
- [x] Scene Templates with inheritance (base + overrides).
- [x] Rule Engine (IF/THEN, data-driven, per output + content type).
- [x] Dynamic resolution pipeline (type → rules → template → theme → widgets →
      scene → render).
- [x] Live editing: moving a widget updates the audience output without restart.
- [x] Widget visibility rules (which outputs see a widget).
- [x] Provider registry for templates/themes/layouts/widgets.
- [x] EventBus integration + Adaptive Runtime budget consultation.
- [x] Tests: composition, layouts, rules, themes, template inheritance, live
      edits, per-output visibility.
