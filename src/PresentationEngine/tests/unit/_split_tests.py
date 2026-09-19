#!/usr/bin/env python3
"""ONE-SHOT migration script — DO NOT RE-RUN on the split tree.

This split tests/unit/main.cpp (the old 5649-line single TU) into per-phase
files. tests/unit/main.cpp is now the small suite runner, so running this
script again without restoring the original would clobber it and the split
files. To regenerate, first restore the pre-split source:
    cp /tmp/main.cpp.bak tests/unit/main.cpp
then run this script. /tmp/main.cpp.bak holds the pre-split source.

Splits tests/unit/main.cpp into per-phase files + shared harness.
"""
import re
from pathlib import Path

BASE = Path("tests/unit")
main = (BASE / "main.cpp").read_text()
lines = main.split("\n")

# ---------------------------------------------------------------------------
# 1. Anchor scan
# ---------------------------------------------------------------------------
RULES = [
    ("test",     re.compile(r"^static void (Test\w+)\(")),
    ("anon_ns",  re.compile(r"^namespace \{$")),
    ("alias",    re.compile(r"^namespace (\w+) = bps::")),
    ("counters", re.compile(r"^static int g_(checks|failures)")),
    ("checkmacro", re.compile(r"^#define CHECK")),
    ("fake",     re.compile(r"^class Fake(Driver|Display|Renderer)")),
    ("helper",   re.compile(r"^(static |inline )?(struct|class|enum|union)\b")),
    ("helper",   re.compile(r"^(static |inline )?[A-Za-z_][A-Za-z0-9_:<>\[\],*& ]+\([^;]*$")),
    ("atomic",   re.compile(r"^std::atomic<int>")),
]

main_idx = next(i for i, l in enumerate(lines) if l.startswith("int main()"))

anchors = []  # (line_idx, type, name)
for i in range(main_idx):
    line = lines[i]
    for typ, rx in RULES:
        m = rx.match(line)
        if m:
            anchors.append((i, typ, m.group(1) if m.lastindex else ""))
            break

# Chunk each region from its anchor to the next anchor / int main.
chunks = []  # (type, name, content)
for k, (i, typ, name) in enumerate(anchors):
    end = anchors[k + 1][0] if k + 1 < len(anchors) else main_idx
    chunks.append((typ, name, "\n".join(lines[i:end]).rstrip("\n") + "\n"))

# Next-test-after-k for helper assignment.
next_test = [None] * len(chunks)
nxt = None
for k in range(len(chunks) - 1, -1, -1):
    next_test[k] = nxt
    if chunks[k][0] == "test":
        nxt = chunks[k][1]

# ---------------------------------------------------------------------------
# 2. Phase mapping
# ---------------------------------------------------------------------------
phase_of = {}
def add(names, phase):
    for n in names:
        phase_of[n] = phase

add(["TestResult","TestVersion","TestJson","TestLogger","TestConfig","TestServices",
     "TestEventBus","TestThreadPool","TestScheduler","TestMemory","TestResource",
     "TestPlatform","TestPal","TestPalPerfStress","TestAssets","TestDrivers",
     "TestDatabase","TestDisplay","TestRenderer","TestIpc","TestLifecycle",
     "TestModules","TestAutoplay","TestSongs","TestMedia","TestKernel"], "core")
add(["TestCamsUuid","TestCamsVfs","TestCamsZip","TestCamsDatabase","TestCamsRegistryCache",
     "TestCamsLoader","TestCamsImportExport","TestCamsIndexerSearch","TestCamsWatcherValidator",
     "TestCamsSerializerCompressor","TestCamsThumbnail","TestCamsContentManager"], "cams")
add(["TestPluginManager","TestIpcLogStream"], "modules")
add(["TestNotifyQueue","TestNotifyRules","TestNotifyPipeline"], "notify")
add(["TestProjectManager","TestProjectUndoRedo","TestProjectWorkspace",
     "TestProjectSessionSnapshot","TestProjectPackage","TestDataManager"], "project")
add(["TestAdaptiveHardware","TestAdaptiveProfiler","TestAdaptiveFeatures","TestAdaptiveQuality",
     "TestAdaptiveBudgets","TestAdaptiveOptimizer","TestAdaptivePressure","TestAdaptiveLearning",
     "TestAdaptiveRuntime"], "adaptive")
add(["TestRenderTypes","TestRenderBackends","TestRenderSceneGraph","TestRenderObjects",
     "TestRenderText","TestRenderAnimation","TestRenderEffects","TestRenderPipeline",
     "TestRenderGpu","TestRenderOutputs","TestRenderEngine","TestRenderStress"], "rendering")
add(["TestDisplayProviders","TestDisplayDevices","TestDisplayOutputs","TestDisplayScaling",
     "TestDisplaySelfTest","TestDisplayRecovery"], "display")
add(["TestPresentationStateMachine","TestPresentationNavigator","TestPresentationTimeline",
     "TestPresentationEngine"], "presentation")
add(["TestSearchIndexing","TestSearchRanking"], "search")
add(["TestMediaEngine"], "media")
add(["TestVgrFormat"], "vgr")
add(["TestSceneComposition"], "scene")
add(["TestBibleResolver","TestBibleProviders","TestBibleEngine"], "bible")
add(["TestSongChords","TestSongProviders","TestSongEngine"], "song")
add(["TestFlowModel","TestFlowEngine"], "flow")

PHASE_FILES = ["core","cams","modules","notify","project","adaptive","rendering",
               "display","presentation","search","media","vgr","scene","bible","song","flow"]

harness = []
phase_bodies = {p: [] for p in PHASE_FILES}

for k, (typ, name, content) in enumerate(chunks):
    if typ == "test":
        if name not in phase_of:
            raise SystemExit(f"no phase for {name}")
        body = content.replace("static void Test", "void Test", 1)
        phase_bodies[phase_of[name]].append(body)
    elif typ in ("counters", "checkmacro", "fake", "alias"):
        harness.append((typ, content))
    else:  # helper / anon_ns / atomic
        nxt = next_test[k]
        phase = phase_of.get(nxt)
        if not phase:
            raise SystemExit(f"helper {name!r} has no following test")
        phase_bodies[phase].append(content)

# ---------------------------------------------------------------------------
# 3. TestHarness.hpp
# ---------------------------------------------------------------------------
header_region = "\n".join(lines[:anchors[0][0]]) + "\n"

counters = "inline int g_checks = 0;\ninline int g_failures = 0;\n"

harness_ordered = []
for typ, content in harness:
    if typ == "counters":
        continue  # replaced by inline counters above
    harness_ordered.append(content)

harness_text = (
    "#pragma once\n"
    "// Shared unit-test harness (docs/specs/00 §8). Split from tests/unit/main.cpp\n"
    "// so each phase compiles and runs independently: ./bps_unit_tests [phase]\n"
    + header_region
    + counters
    + "".join(harness_ordered)
)
(BASE / "TestHarness.hpp").write_text(harness_text)
print("wrote TestHarness.hpp")

# ---------------------------------------------------------------------------
# 4. Phase files
# ---------------------------------------------------------------------------
descriptions = {
    "core": "Core Engine + PAL (Phases 1-8, docs/specs/01-15) and the whole-engine Kernel test",
    "cams": "CAMS / content / asset management (docs/specs/13)",
    "modules": "Plugin manager + IPC log streaming",
    "notify": "Notification service (docs/specs/14)",
    "project": "Project & Data system (docs/specs/15)",
    "adaptive": "Adaptive Runtime (docs/specs/16)",
    "rendering": "Rendering Engine (docs/specs/17)",
    "display": "Display Engine (docs/specs/18)",
    "presentation": "Presentation Engine (docs/specs/19)",
    "search": "Search & Indexing Engine (docs/specs/20)",
    "media": "Media Engine (docs/specs/21)",
    "vgr": "Native .vgr format (docs/specs/22)",
    "scene": "Scene Composition Engine (docs/specs/23)",
    "bible": "Bible Engine (docs/specs/24)",
    "song": "Song & Lyrics Engine (docs/specs/25)",
    "flow": "Service Flow & Automation Engine (docs/specs/26)",
}
for phase in PHASE_FILES:
    body = "".join(phase_bodies[phase])
    text = (
        "// Unit tests: " + descriptions[phase] + ".\n"
        "// Split from tests/unit/main.cpp so a single phase can run alone:\n"
        "//   ./bps_unit_tests " + phase + "\n"
        '#include "TestHarness.hpp"\n'
        "\n"
        + body
    )
    (BASE / f"tests_{phase}.cpp").write_text(text)
    print(f"wrote tests_{phase}.cpp")

# ---------------------------------------------------------------------------
# 5. TestDecls.hpp
# ---------------------------------------------------------------------------
test_order = [c[1] for c in chunks if c[0] == "test"]
decls = "#pragma once\n// Generated declarations for every phase test suite.\n"
for t in test_order:
    decls += f"void {t}();\n"
(BASE / "TestDecls.hpp").write_text(decls)
print("wrote TestDecls.hpp")

# ---------------------------------------------------------------------------
# 6. Suite table (phase + label + functions, from the original MARK block)
# ---------------------------------------------------------------------------
suites = []  # (label, [fns])
cur_label = None
in_main = False
for line in lines:
    if line.startswith("int main()"):
        in_main = True
        continue
    if in_main and line.startswith("}"):
        break
    if not in_main:
        continue
    m = re.search(r'MARK\("([^"]+)"\)', line)
    if m:
        cur_label = m.group(1)
    for fn in re.findall(r"\b(Test\w+)\s*\(", line):
        if fn.startswith("Test"):
            if suites and suites[-1][0] == cur_label:
                suites[-1][1].append(fn)
            else:
                suites.append((cur_label, [fn]))

flat = [(label, fn) for label, fns in suites for fn in fns]
flat_names = [fn for _, fn in flat]
# Call order in main() is authoritative (e.g. TestKernel is defined mid-file but
# runs last because it boots the whole engine). Definitions must cover the same
# set exactly once each.
if sorted(flat_names) != sorted(test_order):
    missing = sorted(set(test_order) - set(flat_names))
    extra = sorted(set(flat_names) - set(test_order))
    raise SystemExit(
        "suite set mismatch: missing=%s extra=%s" % (missing, extra))
if len(flat_names) != len(test_order):
    raise SystemExit("suite count mismatch: %d != %d" % (len(flat_names), len(test_order)))

# Suite carries {phase, name, fn}: the filter matches phase OR label so every
# documented phase name (core, project, rendering, presentation, ...) works
# even though the labels in main() are abbreviated (kernel, proj-mgr, rtypes,
# present, ...).
table = "static const Suite kSuites[] = {\n"
for label, fn in flat:
    table += f'    {{"{phase_of[fn]}", "{label}", {fn}}},\n'
table += "};\n"

runner = (
    "// Engine unit test runner (docs/specs/00 §8).\n"
    "// Usage:\n"
    "//   ./bps_unit_tests                 run every phase\n"
    "//   ./bps_unit_tests flow            run only the 'flow' phase\n"
    "//   ./bps_unit_tests bible song      run suites in phases/labels matching 'bible' or 'song'\n"
    "//   ./bps_unit_tests proj-mgr        run a single suite label\n"
    '//\n'
    '// Build: `cmake -B build && cmake --build build && ./build/bps_unit_tests [phase]`\n'
    '#include "TestHarness.hpp"\n'
    '#include "TestDecls.hpp"\n'
    "\n"
    "#include <cstring>\n"
    "\n"
    "struct Suite {\n"
    "    const char* phase;\n"
    "    const char* name;\n"
    "    void (*fn)();\n"
    "};\n"
    "\n"
    + table
    +
    "static bool Matches(const char* phase, const char* name,\n"
    "                    const std::vector<std::string>& filters) {\n"
    "    if (filters.empty()) return true;\n"
    "    for (const auto& f : filters) {\n"
    "        if (std::string(phase).find(f) != std::string::npos) return true;\n"
    "        if (std::string(name).find(f) != std::string::npos) return true;\n"
    "    }\n"
    "    return false;\n"
    "}\n"
    "\n"
    "int main(int argc, char** argv) {\n"
    "    std::vector<std::string> filters;\n"
    "    for (int i = 1; i < argc; ++i) filters.emplace_back(argv[i]);\n"
    "    if (!filters.empty()) {\n"
    "        std::fprintf(stderr, \"running suites matching\");\n"
    "        for (const auto& f : filters) std::fprintf(stderr, \" '%s'\", f.c_str());\n"
    "        std::fprintf(stderr, \"\\n\");\n"
    "    }\n"
    "    size_t ran = 0;\n"
    "    const char* lastPhase = \"\";\n"
    "    const char* last = \"\";\n"
    "    for (const auto& s : kSuites) {\n"
    "        if (!Matches(s.phase, s.name, filters)) continue;\n"
    "        if (std::strcmp(s.phase, lastPhase) != 0) {\n"
    "            std::fprintf(stderr, \"[phase] %s\\n\", s.phase);\n"
    "            lastPhase = s.phase;\n"
    "            last = \"\";\n"
    "        }\n"
    "        if (std::strcmp(last, s.name) != 0) {\n"
    "            std::fprintf(stderr, \"[mark] %s\\n\", s.name);\n"
    "            last = s.name;\n"
    "        }\n"
    "        s.fn();\n"
    "        ++ran;\n"
    "    }\n"
    "    std::printf(\"\\n%d checks, %d failures\\n\", g_checks, g_failures);\n"
    "    if (ran == 0) {\n"
    "        std::fprintf(stderr, \"no suites matched; available phases:\");\n"
    "        lastPhase = \"\";\n"
    "        for (const auto& s : kSuites)\n"
    "            if (std::strcmp(s.phase, lastPhase) != 0) {\n"
    "                std::fprintf(stderr, \" %s\", s.phase);\n"
    "                lastPhase = s.phase;\n"
    "            }\n"
    "        std::fprintf(stderr, \"\\n\");\n"
    "        return 2;\n"
    "    }\n"
    "    return g_failures == 0 ? 0 : 1;\n"
    "}\n"
)
(BASE / "main.cpp").write_text(runner)
print("wrote main.cpp (runner)")
print("DONE")
