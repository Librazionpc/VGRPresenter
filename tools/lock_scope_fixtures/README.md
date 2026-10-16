# lock_scope_fixtures — the shapes the guard claims to catch

Text only. **Nothing here is compiled** (`tools/` is not a source root, and the
build's guard scans `src/PresentationEngine/{modules,core/resources,platform}`),
so the deliberate violations below cannot reach the app.

They exist because two of the guard's rules are the kind that rot silently: both
report *nothing* on a healthy tree, so a "clean" run cannot tell a working rule
from a dead one. `node tools/check_lock_scope.mjs --self-test` (part of every
build, via the `lock_scope_check` CMake target) runs the guard over THIS
directory and compares what it reports against `expectations.json`, which names
what must be flagged and what must stay quiet.

| fixture | shape | expected |
|---|---|---|
| `cross_tu_caller.cpp` + `cross_tu_helper.cpp` | a locked line calls a helper that is **defined in another translation unit** and reaches the PAL | flagged, naming the helper and its file |
| `relock_same_class.cpp` | a locked method calls a sibling that locks the **same mutex object** | flagged as a self-deadlock |
| `relock_ok_recursive.cpp` | the same shape on a `std::recursive_mutex` | quiet (re-locking is legal by construction) |
| `relock_ok_other_class.cpp` | the callee locks **another class's** equally-named `mutex_` | quiet (different object) |
| `ambiguous_caller.cpp` + `ambiguous_one.cpp` + `ambiguous_two.cpp` | the callee's name has **two definitions** in scope | quiet, and the summary reports the call as unresolved — the scan refuses to guess which definition a call binds to |

Each fixture's `// expect:` comment states its own verdict so the intent survives
even without reading this file.
