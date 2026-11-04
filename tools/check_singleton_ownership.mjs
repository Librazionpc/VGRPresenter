#!/usr/bin/env node
// Singleton-ownership guard.
//
// A QML_SINGLETON whose static `create()` hands the engine a pointer to an object it does not
// own is deleted by the engine when it is destroyed. For a function-local static (every
// `instance()` in qml/) that is `free()` on memory that was never on the heap, and ntdll ends
// the process with STATUS_HEAP_CORRUPTION (0xC0000374) — on EVERY exit: FontService did exactly
// that and left ~130 Windows crash reports and not one line in the app's own log.
//
// The cure is one line in create(): QJSEngine::setObjectOwnership(obj, QJSEngine::CppOwnership).
// This gate reads every `::create(QQmlEngine …)` definition under qml/ and fails when its body
// returns something without having set that ownership. A create() that builds a heap object
// with `new` for the engine to own (the engine deleting it is correct) is exempt.
//
//   node tools/check_singleton_ownership.mjs          # the gate
//   node tools/check_singleton_ownership.mjs --list   # compact file:line list, still exits 1

import { readdirSync, readFileSync, statSync } from "node:fs";
import { join, relative, sep } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(fileURLToPath(new URL(".", import.meta.url)), "..");
const list = process.argv.includes("--list");

function* walk(dir) {
    for (const name of readdirSync(dir)) {
        const full = join(dir, name);
        if (statSync(full).isDirectory()) yield* walk(full);
        else if (full.endsWith(".cpp")) yield full;
    }
}

const findings = [];
let checked = 0;
for (const file of walk(join(root, "qml"))) {
    const text = readFileSync(file, "utf8");
    const re = /(\w+)\s*\*\s*\1::create\s*\(\s*QQmlEngine[^)]*\)\s*\{/g;
    let m;
    while ((m = re.exec(text)) !== null) {
        // The body: from the opening brace to its matching close.
        let depth = 1;
        let i = re.lastIndex;
        while (i < text.length && depth > 0) {
            if (text[i] === "{") depth++;
            else if (text[i] === "}") depth--;
            i++;
        }
        const body = text.slice(re.lastIndex, i - 1);
        const line = text.slice(0, m.index).split("\n").length;
        ++checked;
        if (/setObjectOwnership\s*\(/.test(body)) continue;
        if (/\bnew\s+\w+/.test(body)) continue;   // a heap object the engine is meant to own
        // A heap object built elsewhere (instance() does the `new`) that the engine is MEANT to
        // own and delete: say so in create(), with the reason — `// engine-owned: <why>`.
        if (/\/\/\s*engine-owned:\s*\S/.test(body)) continue;
        findings.push({ file: relative(root, file).split(sep).join("/"), line, cls: m[1] });
    }
}

if (findings.length === 0) {
    console.log(`check_singleton_ownership: clean (${checked} singleton create() bodies checked)`);
    process.exit(0);
}
if (list) {
    for (const f of findings) console.log(`${f.file}:${f.line}`);
} else {
    console.log(`check_singleton_ownership: ${findings.length} singleton(s) hand the QML engine an object it will delete:\n`);
    for (const f of findings)
        console.log(`  ${f.file}:${f.line}  ${f.cls}::create() returns an object without setting C++ ownership`);
    console.log(
        "\nThe engine deletes a singleton's create() result when it is destroyed. If the object is a\n" +
        "static (instance()), that frees non-heap memory and the app dies on exit with\n" +
        "STATUS_HEAP_CORRUPTION. Add, before the return:\n" +
        "    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);");
}
process.exit(1);
