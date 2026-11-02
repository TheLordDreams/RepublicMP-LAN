# Isolated game automation

Implementation reference updated **25 October 2026**. The commands and runner gates below describe available tests, not their results. Read the per-run report for actual PASS/FAIL; construction, rejoin and the long run are not declared passed by this document.

The local file mailbox is inert unless `RepublicMP.qa.enabled` exists beside the plugin DLL **and** `TEST-COPY.txt` exists beside the game executable. Use disposable physical copies with separate save/config trees. There is no arbitrary-function, RVA or arbitrary-path command. The mailbox adds no network listener; the separately enabled TCP test proxy is described below.

Bind `QaAutomation_Bind(host)` after the required module binds. `QaAutomation_Tick()` polls the mailbox and publishes status from Present. Bind registers `OnStep` and `OnFrame` with the determinism observer APIs. Native construction queries/actions and paused-world snapshots run in the main-thread frame observer; status publication reads their cached POD data under the module lock. Register the construction/frame-capture/instance-event helpers at their respective integration points; initialization of event isolation must precede native worker creation.

## Mailbox commands

Atomically replace `RepublicMP.qa.command` beside each plugin with one UTF-8/ASCII line `sequence|verb|argument`. Start increasing positive decimal sequences at 1 for each process. Wait for `lastSequence` before sending another command; malformed lines and duplicate/old sequences are ignored.

```text
1|load|coopqa_fixture
2|host|
3|state|
```

The second process uses `1|join|127.0.0.1`. This QA command accepts only loopback; the ordinary LAN UI supports the host's IPv4. `load` requires an idle session, initialized root menu or loaded world, and an existing `media_soviet/save/coopqa_*` directory. Save names accept ASCII letters, digits, underscore and hyphen. All save operations remain inside each private game copy.

| Verb | Argument | Behavior |
| --- | --- | --- |
| `state` | empty | Request status without a gameplay action. |
| `host` | empty | Host the loaded city in Co-op mode. |
| `join` | `127.0.0.1` | Join the local host. |
| `pause`, `resume` | empty | Use the existing queued session pause-toggle request. |
| `disconnect` | empty | Request session disconnect. |
| `credit-test` | empty | Queue the fixed +100-ruble, zero-dollar `LS_CMD_MONEY` fixture. |
| `save` | `coopqa_*` name | Request the existing shared-save capture path. |
| `probe-prime` | empty | Prime both bounded construction targets on the main thread. |
| `build-test` | `0` or `1` | Queue `LS_CMD_PLACE` for the chosen probe slot. |
| `demolish-test` | `0` or `1` | Enter the patched native demolition tool function; its capture hook queues `LS_CMD_DEMOLISH`. |

An acknowledgement means request handling, not completion or success of asynchronous joining, pause propagation, saving or construction. Wait for status and verify the corresponding world-state oracle. Probe commands have a second deferred main-thread stage: wait for `probeCommandPending:false`, `probeValid:true` and empty `probeError`, as well as the desired actual state.

## Paused action oracles

`credit-test` requires a loaded paused running lockstep session. Capture both balances, same stopped tick and `commandsApplyEpoch`, send the command only to the client, then require exactly +100 rubles on both, unchanged dollars, unchanged tick and advancing application epochs. Sample again after settling to catch repeated application. The fixture uses `Commands_Submit`; it does not write a local balance directly.

`save|coopqa_result` calls `TriggerSaveGame`. `SharedSave_BeforeLate` intercepts it before native save I/O, clears the local trigger and queues `LS_CMD_SAVE`; the command saves on both PCs at a boundary, including held/paused frames. Verify shared SAVE requested/saved log records and nonempty headers on both sides. Reload/rejoin is a separate persistence gate. File existence alone proves neither equivalent state nor completed persistence. Choose a fresh `coopqa_` name; that name may be overwritten within the private copies. Do not issue a concurrent save-and-exit UI action.

Before construction, issue `probe-prime` on **both** paused clients and compare both slots' x/y/z. Repeat after a common world load: target caches are keyed to the local load generation. Targets are selected in fixed order from two disjoint bounded regions using the engine's terrain-height query, flat/dry checks and clearance from existing buildings. No camera, mouse, RNG, fake insertion or desktop UI input is involved. Availability of an empty-looking candidate does not override native placement validation.

`build-test` uses the built-in `monument_emblem` type with ruble-paid construction, zero auto-purchase/fill and the canonical `BuildingMsg`/`Commands_Submit` path. Paid construction is **not instant**: native `160550` selects payment and resumes the site; simulation update `159070` advances construction and charges it during running ticks. At pause, a newly placed site can legitimately have progress 0 and no debit. First observe one actual new site and count +1 on both peers; then resume bounded simulation, compare exact-tick hashes and pause for observations. The current runner allows up to 1200 advancing ticks for completion. Require `progress >= 1`, a positive equal **net** ruble decrease, count +1 and equal full `pausedState.all` hashes. Net balance change during simulation is not an isolated per-site price measurement. Use slot 0 from host and slot 1 from client to exercise both command origins. Native refusal or failure to complete remains a failure.

`demolish-test` requires exactly one observed probe building and a paused main-thread boundary where `Commands_TakeLocal()` accepts local capture. It calls the **patched** native entry `168730` with the real world/object pointers and the stock fast-tool ABI/flags `{1,1,0,0,1}`. A temporary ruble `BuildMode` is installed and restored in `__finally`. The demolition detour captures that mode and queues the command; replay applies the sender's mode on each peer and restores that peer's local selection. The test does not call the unhooked native bypass or submit a handcrafted demolition message directly.

The strengthened runner counts exactly one fresh `LOCKSTEP: building demolition captured` record from the actor and exactly +1 `commandsApplyEpoch` on both peers. It also verifies local ruble/dollar mode restoration. These assertions apply only to a runner process that loaded this strengthened revision; an already running Node process does not pick up edits to the source file.

Native demolition can leave an inverted-progress site (`demolishing == 1`, native `EA8`) until simulation runs. Allow bounded resume/pause batches, currently up to 1800 advancing ticks, then require actual disappearance on both peers, restored global counts and matching full paused hashes. Do not require `payRub == 1` unconditionally: the fixture has been observed with `EA8=1`, zero pay flags and preserved local ruble mode, while an earlier run removed its objects through native updates. Flags diagnose the current phase; they do not replace the removal oracle. A site still listed at timeout is not a pass.

These tests inject calls through game/mod APIs and exercise native capture/replay. They do **not** verify a human mouse click, the bulldozer's visual selection, confirmation-dialog presentation or the full UI input path. No blind desktop clicks or fake progress/removal writes are used.

## Status and sampling

`RepublicMP.qa.status.json` is replaced atomically every 500 ms and after a new valid sequence. `commandError` reports mailbox refusal; `sessionError` and `desync` report session faults. States: 0 idle, 1 host listening, 2 host joining, 3 host in game, 4 client joining, 5 client in game. Check PID and advancing uptime; leftover files are stale.

`PausedSample` is captured by main-thread `OnFrame`, including the stopped tick, load generation, command epoch and `StateSummary`. Present does not walk live vectors for these paused fields. `moneyRubles`, `moneyDollars`, `commandsApplyEpoch` and `pausedState` are published only when that cache is valid for the current paused running session, tick and local load generation; otherwise values are JSON null. Money retains double precision; nonfinite balances are null. `pausedState` exposes full/subsystem hashes and object counts. Compare matched stopped ticks and wait for the command to propagate before asserting.

`probeSlots` contains each target's `chosen`, `exists`, `demolished`, `nearby`, `index`, `progress` and x/y/z, plus diagnostic payment/stop/demolition and local-currency fields in the current schema. `demolished` means a previously observed slot object is now absent. `probeValid` and `probeError` distinguish an actual observation from an unreadable/unready probe. These are main-thread cached observations, not fake world data.

`samples` retains the last 128 summaries in the current local world generation. Ticks 1–120 are sampled individually after Early+Late on the simulation thread; later samples occur every 60 ticks. A tick-zero baseline is captured after a paused join before the first step/resume; keep it separate from completed simulation ticks. Compare exact samples at the **same tick in the same shared session**, after the common join/re-sync load. Local `worldLoad` counters need not equal across processes. Never compare arbitrary latest running snapshots as if they represented one tick.

Each sample includes `vehicleRecordsValid` and vehicle records from the same boundary, using existing `StateHash_VehicleRecords`: stable index, position and cargo hash, with no pointers exported. Nonfinite coordinates become null. Failed record reads set validity false; they must not silently count as a matching empty world. Match records by index to locate the earliest disagreement. Diagnostics do not weaken upstream hashes or change the pass/fail oracle.

## Two instances on one PC

The pinned game uses named worker/render events `FillingrStart`, `RenderStart` and `RenderFinished`; sharing these names across processes can couple two clients on one Windows session. The QA-only `CreateEventW` hook namespaces **only those three names** as `Local\RepublicMP.QA.<PID>.<name>`, retaining reuse inside one process. Native physics/simulation formulas are unchanged.

This isolation requires all three markers **before injection**: `TEST-COPY.txt` beside EXE, plus `RepublicMP.qa.enabled` and `RepublicMP.qa.isolate-events.enabled` beside DLL. Late/unknown native worker initialization is refused. Check logs for hook installation and isolated creation of all three events in both processes. Once handles exist, removing a marker cannot safely rename them; restart the process to change this mode. Same-PC tests using this namespace are not evidence that separate-PC networking has been tested.

## Optional loopback network proxy

Before starting the client, add `RepublicMP.qa.proxy.enabled` beside its DLL, alongside the copy/QA markers. The opt-in is read at bind. Only a lockstep connection whose host is exactly `127.0.0.1` and destination port is exactly **5560** is redirected to **5561**. Other hosts/ports, the host listener and city-transfer port 5556 are untouched.

`tools/tcp-chaos-proxy.mjs` listens on loopback 5561 and forwards to loopback 5560. CLI profiles are `clean` and `lag-jitter`; delay/fragmentation preserve byte order and every byte. This is TCP stream impairment, not OS packet-loss emulation, public hosting, NAT traversal or a firewall change. The runner owns the proxy process and records its shutdown summary. Use `Run-LiveQA.ps1 -ChaosProfile clean` or `-ChaosProfile lag-jitter` to select the profile; testing the proxy code alone does not establish in-game chaos behavior.

The runner also accepts `-Construction`, `-Rejoin` and `-LongMinutes` (0–60), plus the explicit Python path used by the local gameslot lease. Those switches select additional assertions; only a completed run report establishes whether they passed. All run-specific evidence remains under ignored `local/`.

`-ChaosProfile clean -DisconnectAfterMs 15000` selects a separate recovery test. The proxy injects **one** TCP closure per run, then leaves rejoined connections available. The runner requires detection by both real clients, surviving processes/worlds, a fresh city transfer and at least 60 strict matching ticks after rejoin. An earlier per-connection timer repeatedly broke rejoin and failed this test; that failure is retained in private evidence and is not a passed recovery result.

## Frame capture and revocation

A one-shot `RepublicMP.qa.screenshot.request` beside DLL, with copy/QA markers, captures only the game's own D3D11 backbuffer to `RepublicMP.qa.frame.bmp`. It uses an atomic publish, bounded dimensions and supported framebuffer formats. Failure produces `RepublicMP.qa.screenshot.error` once; remove the request and let a poll observe its absence before retrying. No desktop capture occurs.

Removing either main copy/QA marker stops mailbox handling/status writes on the next Present frame. Startup-only event namespace and proxy choices have their own lifetime rules above. Do not ship QA-enabled markers in a normal friend package.
