# Workers & Resources: Soviet Republic — research notes

Session: 2026-10-24, continued 2026-10-25. Current scope: experimental LAN candidate; real two-client construction/demolition, shared save, normal rejoin, 20-minute strict no-resync run and lag/jitter construction passed on one PC. Recovery/package final checks are recorded in the delivered QA_RESULTS.json. Earlier partial/failure notes below are historical; they do not supersede the final report.
Private install paths, machine state and binary observations are in ignored `local/`.

## Observed public evidence

- Playbook pinned at [coop-anything 5b86e5b](https://github.com/cranebeaker/coop-anything/tree/5b86e5bb10ce864ab9872f2589651f73ffcae5db); AGENTS, coop-research, coop-revive, coop-reverse-engineer, coop-qa read. Hub pulled; no Workers & Resources / RepublicMP game entry found by local search.
- Anonymous GitHub issue/PR searches: hub `Soviet` returned only unrelated World in Conflict; `RepublicMP` returned zero. RepublicMP open PR endpoint returned none; 10 open issues inspected, particularly [#54 LAN hosting](https://github.com/E3418/RepublicMP/issues/54), [#55 desyncs](https://github.com/E3418/RepublicMP/issues/55) and #49-53. Branches main and Republics; forks Kruupych/RepublicMP and veryinsanee/RepublicMP enumerated (not exhaustively audited).
- [RepublicMP source 70933ea](https://github.com/E3418/RepublicMP/tree/70933eafd6106b13598c603479448dd20916147e): version 0.7.2, protocol 51. ARCHITECTURE.md explicitly targets 1.1.1.9 / PE 0x6A3EB6AD. GPL-3.0-only. Last source push reported 2026-10-24.
- Latest published release observed: [v0.7.1](https://github.com/E3418/RepublicMP/releases/tag/v0.7.1), 2026-10-23, RepublicMP.dll 1,410,048 bytes; API digest SHA256 `2c0dad02ecdb84cd51a0bf6ee773ac8434113ed205e8aff33ae5a00b335aa67e`. Source HEAD is newer. The release binary was subsequently downloaded and used for isolated startup checks only; this is not a co-op QA result.
- [TesmioLoader 3baa141](https://github.com/MaxLegend/TesmioLoader/tree/3baa141f9f08921aea9c95f0a400289cabd9960a), GPL v3. Native DLL loader/API4; source headers vendored with license. Latest published archive observed b0.3.6, 289,266 bytes, API digest `95835c84e553a72beee02ceb7ce6ab48694871dfbc5fd96eeeb716a31c9a5139`; subsequently used on isolated copies.
- [WRSR Multiplayer](https://www.nexusmods.com/workersandresourcessovietrepublic/mods/42), KobelWSRS, page reports 0.71 updated 2026-08-17. Source/license and exact binary safety unverified; no download/account access. [RepublicLAN provenance](https://github.com/E3418/RepublicMP/blob/70933eafd6106b13598c603479448dd20916147e/README.md#credits) is attributed to SoylentGreed by RepublicMP; no independent matching repository found in web searches.
- [Official modding wiki](https://wiki.hoodedhorse.com/Workers_Resources_Soviet_Republic/General_modding) describes content modding. Native synchronization requires the separate loader; not a Workshop data-only implementation. [TesmioLoader findings](https://github.com/MaxLegend/TesmioLoader/blob/3baa141f9f08921aea9c95f0a400289cabd9960a/docs/02-findings.md) document C3D and build-specific native addresses.

## Searched

Public web: Workers & Resources cooperative multiplayer mod github; Soviet Republic TesmioLoader; RepublicLAN; WRSR Multiplayer Kobel; hub Soviet/RepublicMP. Primary GitHub source, releases, open issues/PRs, branches/forks, Nexus page, official wiki and public community announcements consulted. Research used anonymous public sources. Steam was subsequently used for licensed isolated-game tests with explicit owner authorization; Discord and other private services were not accessed. Search is not a claim of exhaustive discovery.

## Derived decisions

- Reuse RepublicMP's real city transfer and lockstep implementation. It is active upstream, so this is a local compatibility/LAN adaptation, not an abandoned-project revival or unrelated network sample.
- TCP hosting exists through SaveSync_StartHost → NetworkListen (5556), lockstep (5560). #54 does not establish an absent TCP path. Compile-time TCP-only mode prevents this plugin's SteamNet callbacks, relay/P2P initialization and lobby publication; it does not remove the licensed game's Steam requirements.
- The exact EXE/engine allowlist before plugin subsystem binding prevents this plugin using known offsets against a different disk build. Loader-owned hooks are installed earlier and are outside that guard. Binary identity does not establish determinism or remote peer authentication.
- Do not weaken vehicle state hashing to turn an early desync into a hollow pass. Diagnose the native scheduling/movement path and rerun real clients.

## Executed checks

- PASS: 6/6 Node synthetic PE parser tests and 5/5 mutation-inspector checks on private binary copies; expected version and two capture-hook prologues matched. Those are JS/static results.
- PASS: official Microsoft C++ Build Tools/components installed with explicit authorization; MSVC 14.51/v145 and SDK 10.0.26100.0 available. The first UAC attempt was cancelled; the owner accepted retry and installation completed. Official signed bootstrap SHA-256: 994ddaa3c0a5795ad700fafbc6414c1ed5b580c32eb29bbe46f4e21388eb5df3. Toolchain absence is no longer a blocker.
- PASS: native LAN DLL compiled with 0 warnings / 0 errors. Exact changing DLL hashes and run identities remain in private local/; no final DLL is pinned by this note.
- PASS: native guard executed under the loader and refused an altered disposable EXE before plugin hooks; original disposable EXE hash restored afterward. No claim that all native engine-mutation cases ran.
- PASS: two real licensed game clients launched simultaneously on physical isolated copies with loader/hooks/QA bot. Upstream release startup was checked separately and does not establish this branch's co-op correctness.
- Found and fixed: the first own-DLL handshake failed because 0.7.2-local-lan1 exceeded SessionHello.version[16]. Short versions 0.7.2-lan1 / 0.7.2-loc1 and static_assert preserve the wire layout; both peers must use the same DLL.
- PASS for initial transfer: real host saves/sends/reloads the city, real client receives and loads it, then both enter lockstep. Fixture is shipped tutorial_map18 copied to private save/coopqa_fixture, never an owner's save.
- Historical failure: early strict session QA differed at tick 2/3. Later investigation identified collisions of native same-PC worker/render events; isolated short tests subsequently reached at least 300 ticks with matching samples, pause/credit/shared-save/resume checks. That scope does not establish a final full-suite or separate-PC pass.
- The current candidate's construction, rejoin, long-run and lag/jitter results must be read from their completed run reports. Earlier partial runs and modified runner assertions do not establish those final results. Saving for initial transfer is not the shared-save gameplay test.
- PASS for packaging tooling only: PowerShell AST, outside/overlap/existing-path/hash/reparse negative checks and synthetic staging with inert dummy runtime. No final archive or friend-ready distribution is established.
- Private evidence includes game identity, native guard logs, per-client state/status, owned-process tracking, build output and packaging review; machine paths, copied game files/assets, saves, screenshots and private logs stay in ignored local/.

## Pinned game identity

- Store/version/build: Steam, 1.1.1.9, build 23935965, PE timestamp 0x6A3EB6AD, Windows x64; native C3D engine.
- SOVIET64.exe SHA-256: 296644a9f207d609031fc2ae73fed2dcb34619a1d55a35d1c7b51965ce6841b8.
- C3DDLL64.dll SHA-256: 65fcb6845c3cb7c25a22121f25904a855ea88657e31a3b50a15adac26e231516.
- Upstream source/release links and release API digests above are preserved as research evidence; they do not identify an evolving locally compiled DLL.

## Native construction and demolition continuation — 2026-10-25

- Corrected the initial instant-build assumption: native 0x160550 writes the selected currency flags and tail-calls resume 0x161440. Native construction 0x159070 skips its update at zero speed/timestep; paid sites advance during running Late ticks. The host fixture was observed progressing 0 → approximately 0.918 → 1 on both peers, with the same 6788.5 net ruble decrease. This is a bounded observation, not the final candidate suite result or an isolated per-site price attribution.
- Fixed a real capture/replay omission: demolition uses current world+AF8/AFC currency selection. DemolishAsCommand now captures `msg.mode = BuildMode_Read()`; Buildings_ApplyDemolishCommand temporarily applies the sender's mode, invokes native demolition and restores the local mode in `__finally`. This transports gameplay input rather than fabricating progress/removal.
- The QA helper now enters patched native 0x168730 with the real world/object and canonical fast-tool arguments {1,1,0,0,1} while local capture is admissible; it temporarily selects ruble mode and restores it. The detour itself queues the shared command. This exercises API/native capture/replay, not a mouse/UI-click path.
- The strengthened runner requires exactly one fresh actor-log capture record and +1 command-application epoch on both peers, plus preservation of local currency settings. A runner process already in flight before that JavaScript edit does not include the new assertions; a later construction run is required to exercise them.
- Fixture payment flags are phase observations, not an unconditional oracle. An observed demolition site had progress 1, payRub 0 / payUsd 0, inverted EA8=1 and localRub 1 on both peers; earlier native simulation removed the probe objects from both origins after roughly 600 advancing ticks. Requiring payRub 1 at every phase was therefore incorrect for this fixture. Retain actual disappearance, restored counts, matching paused full hashes, captured sender mode and local-mode restoration; do not fake flags or object removal.
- Demolition can require running ticks too. Current bounded gates resume/pause in batches and fail if the real site remains after 1800 advancing ticks; construction completion is bounded separately. No overall construction/rejoin/long-run PASS is claimed while the current run remains incomplete.

## Current unknowns / next work

- Same-PC diagnostics found native `FillingrStart`, `RenderStart` and `RenderFinished` event-name collisions. A startup-only triple-opt-in hook uses PID namespaces for those exact names; it does not change physics formulas. It is confined to marked test copies and does not establish remote multiplayer behavior.
- Final completed gates: construction/rejoin/20-minute strict run, strengthened capture assertions under lag/jitter, one-shot TCP failure/recovery, ordinary packaged startup and final native guard. Read the delivered QA_RESULTS.json for measured details. Keep per-run DLL/runner identities and outcomes separate; do not apply a new assertion retroactively to an old process.
- Unknown: remote friend machine, distinct Steam accounts, DLC parity, Internet/NAT behavior, durations beyond the measured run and unsupported tool synchronization. Keep upstream #55 open; do not claim its vehicle/office/building identity reports fixed.
- Steam networking variant requires its own build/runtime QA; LAN build success does not validate Steam P2P or invitations.
- Installation of official build tools and Steam use on isolated licensed copies are already authorized. No repeated permission request is needed for that scope. Publishing, GitHub/Discord account use and contacting people remain unauthorized; no claim issue/PR/release was published.

## Same-machine QA event collision (continuation)

Native static analysis identified CreateEventW names FillingrStart (constructor 2592EA), RenderStart (47D22D), RenderFinished (47D248). Two clients in the same Windows session open the same manual-reset kernel events. Actual native trace recorded vehicle updates from Filling caller 2594DF outside InStepFrame, explaining a cross-process scheduling route. This is a same-machine isolation issue; it is not proof of an upstream two-PC physics defect. A triple-opt-in qa/instance_events module namespaces only these three names by PID before native constructors, forwarding all other parameters. The pre-first-step held-frame workaround was fully removed; upstream determinism/held_frame code was restored before the successful final 20-minute control. The completed report defines the current measured scope.


## 2026-11-03 LAN2 candidate
- Observed: installed EXE/engine SHA-256 match supported_build.h (Steam 23935965, 1.1.1.9).
- Observed: provided client log contains eight host mismatch reports: buildings+citizens five, buildings two, citizens one. Re-sync follows roughly 60 seconds after each report. Private evidence is in local/evidence, excluded from deliverables.
- Derived: seven-step loading is mismatch recovery, not a periodic 20-minute timer. Specific causative gameplay operation is unknown without paired host evidence.
- Implemented: finite TCP operation deadlines, cancellation of overlapped IO including unlimited idle receives, bounded sends and disconnect ordering, file transfer publication through a temporary file, seven persisted LAN settings and persisted desync recovery choice.
- Validation: MSVC Release x64 LAN build, zero errors/warnings; 12 real loopback TCP checks passed. First backpressure fixture allowed kernel buffering and was corrected to constrain the receive window before connect. Next test exposed cancellation that did not wake an unlimited read; implementation changed and complete suite passed. Six PE inspection tests and 18 gameslot checks plus 13 mutation twins passed.
- Observed: LAN2 repeat passed native construction/demolition from both clients, shared save, disconnect/rejoin, and 1200 seconds under injected lag/jitter with 608 matching samples through tick 29400. Separate injected TCP closure was detected and the client rejoined with matching state. Both copied game processes were closed after each successful scenario.
- Historical incomplete LAN2 run: status unavailable after the last observation at 934 seconds; cleanup did not complete its result report. Cause remains unknown. The successful repeat does not retrospectively turn that run into a pass.
- Unknown: complete UI interaction coverage (window capture timed out), original city root desync, separate PCs. Upstream economy change c14e655 was not imported.
- Sources: supplied LAN1 source archive; https://github.com/E3418/RepublicMP/issues/55 ; https://github.com/cranebeaker/coop-anything/blob/main/AGENTS.md .
