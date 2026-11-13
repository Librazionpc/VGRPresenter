const fs=require('fs');
function edit(p,fn){ let s=fs.readFileSync(p,'utf8'); const crlf=s.includes('\r\n'); if(crlf) s=s.replace(/\r\n/g,'\n'); s=fn(s); fs.writeFileSync(p,crlf?s.replace(/\n/g,'\r\n'):s); }
function rep(s,a,b){ if(!s.includes(a)) throw new Error('missing: '+a.slice(0,70)); return s.replace(a,b); }
edit('qml/models/OutputListModel.cpp',s=>rep(s,`        { "styleName", styleName },
        { "content", content },`,`        { "styleName", styleName },
        // What the style does to a slide (the same line the StyleLayoutRole carries), for readers of the map.
        { "styleLayout", styleLayoutNote(item.styleId) },
        { "content", content },`));
edit('qml/Main.qml',s=>{
 s=rep(s,`    function outsSample() {
        const found = { rows: 0, styled: 0, described: 0, reported: 0, lines: [] }
        for (let i = 0; i < 24; ++i) {
            const item = SelfTest.findItem("selfTestOutputRow_" + i)
            if (!item)
                break
            ++found.rows
            const styleName = String(item.styleName)
            const note = String(item.styleLayout)`,`    // ONE OUTPUT'S STATE, from the engine's own facts (LiveOutputService) — the probes read outputs from the roster and
    // the engine, not from a settings page: outputs are made, wired and configured on Routing, which carries no
    // per-output status line of its own. The words are the ones the operator-facing views use.
    function outputStateLine(o) {
        const name = String(o.name)
        const enabled = o.isEnabled !== false
        const runtime = LiveOutputService.outputRuntime[name] || null
        const stalled = LiveOutputService.live && enabled && LiveOutputService.outputFeedStalled(name)
        const delivering = !!runtime && runtime.presented === true && !stalled
        let text
        if (!enabled) text = "Switched off"
        else if (!LiveOutputService.live) text = "Ready (go live to send)"
        else if (delivering) text = "Live \u00b7 " + runtime.frames + " frames delivered"
        else text = "Warming up \u00b7 waiting for the first frame"
        if (enabled && LiveOutputService.blackedOut && text.indexOf("blackout") < 0) text += " \u00b7 blackout up"
        if (enabled && stalled && text.indexOf("no feed") < 0) text += " \u00b7 no feed"
        return text
    }
    function outsSample() {
        const found = { rows: 0, styled: 0, described: 0, reported: 0, lines: [] }
        for (let i = 0; i < OutputListModel.rowCount(); ++i) {
            const item = OutputListModel.getOutput(i)
            if (!item)
                break
            ++found.rows
            const styleName = String(item.styleName || "")
            const note = String(item.styleLayout || "")`);
 s=rep(s,`            const status = String(item.statusText)
            const reports = status.indexOf(String(item.kind) + " \u00b7 ") === 0
            if (reports)
                ++found.reported`,`            const status = window.outputStateLine(item)
            // Every output, of every kind, has to be able to say what it is doing.
            if (status !== "")
                ++found.reported`);
 // phase 0 probe
 s=rep(s,`                const probe = SelfTest.findItem("selfTestOutputRow_0")
                if (!probe) {`,`                const probe = SelfTest.findItem("routingScreen")
                if (!probe || OutputListModel.rowCount() < 1) {`);
 // live sample
 s=rep(s,`        for (let i = 0; i < 24; ++i) {
            const item = SelfTest.findItem("selfTestOutputRow_" + i)
            if (!item)
                break
            ++rows
            const status = String(item.statusText)
            const kind = String(item.kind)
            // A row REPORTS when its line opens with its own kind. A row that
            // says nothing (the NDI/SDI-only line this replaced) fails here.
            if (status.indexOf(kind + " \u00b7 ") === 0 || status.indexOf("Live \u00b7 ") === 0)
                ++reporting`,`        for (let i = 0; i < OutputListModel.rowCount(); ++i) {
            const item = OutputListModel.getOutput(i)
            // Switched-off outputs send nothing and say so; the checks below are about the outputs that are on.
            if (!item || item.isEnabled === false)
                continue
            ++rows
            const status = window.outputStateLine(item)
            const kind = String(item.kind)
            // An output REPORTS when it can say its own state: live and sending, or warming up.
            if (status.indexOf("Live \u00b7 ") === 0 || status.indexOf("Warming up") === 0)
                ++reporting`);
 s=s.split('window.openSettings("outputs")').join('window.openSettings("routing")');
 return s;});
