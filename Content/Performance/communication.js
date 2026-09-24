/* The same fixture drives Chromium's real UE binding and RmlUi's URmlUiJSContext. */
(function () {
    'use strict';
    const isWeb = typeof window !== 'undefined' && typeof document !== 'undefined';
    let service, bridge, cachedObject;
    const pending = new Map();
    let nextId = 1;
    let stringifyMs = 0, parseMs = 0;
    function report(result) {
        if (isWeb) {
            result.display_text = document.getElementById('result').textContent;
            return service.report(JSON.stringify(result));
        }
        const text = result.error || `${result.mode}: count=${result.count}; value=${result.last}; sum=${result.sum}`;
        bridge.SetText(bridge.FindNode('result'), text);
        result.display_text = bridge.GetText(bridge.FindNode('result'));
        service.Report(JSON.stringify(result));
    }
    if (isWeb) window.addEventListener('ue-response', event => {
        const start = performance.now();
        const result = JSON.parse(event.detail);
        parseMs += performance.now() - start;
        const callback = pending.get(result.id);
        if (!callback) throw new Error(`Unexpected response ${result.id}`);
        pending.delete(result.id);
        callback(result.value);
    });
    function eventRead(kind) {
        const id = nextId++;
        return new Promise((resolve, reject) => {
            pending.set(id, resolve);
            const start = performance.now();
            const request = JSON.stringify({ id, kind });
            stringifyMs += performance.now() - start;
            service.request(request).catch(reject);
        });
    }
    async function runWeb(config) {
        try {
            const latencies = [];
            let issued = 0, sum = 0, last = 0, mismatches = 0;
            stringifyMs = parseMs = 0;
            const start = performance.now();
            async function worker() {
                while (issued < config.count) {
                    ++issued;
                    const before = performance.now();
                    const value = config.mode.startsWith('event_')
                        ? await eventRead(config.mode.includes('property') ? 'property' : 'float')
                        : await (config.mode.includes('property') ? service.getobjectvalue() : service.getfloat());
                    latencies.push(performance.now() - before);
                    if (value !== config.expected) ++mismatches;
                    sum += value;
                    last = value;
                }
            }
            await Promise.all(Array.from({ length: config.concurrency }, () => worker()));
            const elapsed = performance.now() - start;
            document.getElementById('result').textContent =
                `${config.mode}: count=${issued}; value=${last}; sum=${sum}`;
            await report({ mode: config.mode, count: issued, sum, last, mismatches,
                elapsed_ms: elapsed, latencies_ms: latencies, stringify_ms: stringifyMs, parse_ms: parseMs });
        } catch (error) { await report({ error: String(error.stack || error) }); }
    }
    function runPuerts(json) {
        const config = JSON.parse(json);
        try {
            let sum = 0, last = 0;
            service.BeginBatch();
            // No JS-to-C++ clock call or JSON operation inside these tight loops.
            if (config.mode === 'typed_float') {
                for (let i = 0; i < config.count; ++i) { last = service.GetFloat(); sum += last; }
            } else if (config.mode === 'typed_property_getter') {
                for (let i = 0; i < config.count; ++i) { last = service.GetObjectValue(); sum += last; }
            } else if (config.mode === 'typed_property_cached') {
                for (let i = 0; i < config.count; ++i) { last = cachedObject.Value; sum += last; }
            } else if (config.mode === 'typed_property_resolve') {
                for (let i = 0; i < config.count; ++i) { last = service.GetObject().Value; sum += last; }
            } else { throw new Error(`Unknown mode ${config.mode}`); }
            const elapsed = service.EndBatch();
            report({ mode: config.mode, count: config.count, sum, last,
                mismatches: sum === config.expected * config.count && last === config.expected ? 0 : 1,
                elapsed_ms: elapsed, latencies_ms: [] });
        } catch (error) { report({ error: String(error.stack || error) }); }
    }
    if (isWeb) {
        window.runCommunicationBenchmark = runWeb;
        function connect() {
            if (!window.ue || !window.ue.bench) return setTimeout(connect, 20);
            service = window.ue.bench;
            service.ready();
        }
        connect();
    } else {
        service = require('puerts').argv.getByName('bench');
        bridge = require('puerts').argv.getByName('bridge');
        cachedObject = service.GetObject();
        service.OnRun.Add(runPuerts);
        service.Ready();
        bridge.ReportReady();
    }
})();
