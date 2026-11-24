---
target: Heart Beacon Android app UI
total_score: 24
max_score: 40
na_heuristics: 
p0_count: 0
p1_count: 3
target_identity: "file:D:\\Personal\\Project\\MQTT_Heart_beacon\\android\\app\\src\\main\\kotlin\\com\\heartbeacon\\app\\ui"
timestamp: 2026-11-24T15-30-06Z
slug: android-app-src-main-kotlin-com-heartbeacon-app-ui
---
Method: dual-agent (A: design review · B: detector + native audit)

Verdict: the stage is the product's own and colour/contrast hold in both themes; what breaks is behaviour — the stage eats the scroll, settings fail silently, and the moment a heart arrives (the point of the app) usually isn't shown. Critique 24/40 Acceptable · audit 13/20 Acceptable · no P0.

## Design health (Nielsen)
| # | Heuristic | Score | Key issue |
|---|---|---|---|
| 1 | System status | 2 | Settings write optimistically and revert silently (Log.w only); an incoming heart plays only if the app is open within 30 s |
| 2 | Real-world match | 3 | HB-756268, -58 dBm, "admin", "circle", "Unreachable" leak through |
| 3 | User control | 2 | Tap commits a Heart in 400 ms, no undo; Forget Wi-Fi instant; stage swallows scroll |
| 4 | Consistency | 2 | Brass "Shown" breaks Brass Is Pressable; Appearance's selected segment grey, not brass |
| 5 | Error prevention | 2 | Dead Update button; edits allowed while Asleep/Offline with no "applies on wake" |
| 6 | Recognition | 3 | Gesture key always visible but has no verb |
| 7 | Flexibility | 2 | Gestures only; no reply from the notification |
| 8 | Minimalist | 3 | Stage focused; device id, dBm, 7-row "This phone" plate are noise |
| 9 | Error recovery | 2 | Add-beacon errors are excellent; Home settings failures invisible |
| 10 | Help | 3 | Good inline explanations (schedule, nap, permission rationale) |
| Total | | 24/40 | Acceptable |

## Audit health (native)
| Dimension | Score | Key finding |
|---|---|---|
| Accessibility | 3 | Sorry/Annoy only via timed multi-tap or TalkBack actions; LED fx ignore Remove animations |
| Performance | 3 | Per-frame allocations while fx run; felt shader repainted each LED frame |
| Theming | 3 | Stage/brass/mood colours are luminance-picked hex, not theme tokens |
| Conformance | 3 | Dead Update button; Back exits the whole Add flow |
| Adaptivity | 1 | Landscape Home overlaps; nav stack lost on rotation |
| Total | 13/20 | Acceptable |

## Design specificity
- Stage: authored. Cage from the real build geometry, firmware LED colours and timings, 1/2/3-dot notation = the button.
- Below the stage: generic M3 settings in brass. Settings never preview on the cage (brightness slider doesn't light it, schedule toggle doesn't show the asleep look).
- Detector: does not apply. Engine scans html/css/jsx/vue/svelte/astro only; skipped all 29 .kt files, printed [], exit 0. No browser overlay (native).

## What's working
1. Honest, calm twin: LEDs are the only saturated colour; frame clock only runs during an effect; stale values say "Last known".
2. Dot notation shared by button, gesture key and Recent discs; meaning never rests on colour alone; TalkBack custom actions send moods.
3. Contrast passes AA everywhere incl. stage (text 11.1:1 lilac / 10.6:1 aubergine, labels >= 4.6:1); holds at font scale 1.3.

## Priority issues
1. [P1] Stage swallows the page scroll (verified on emulator). detectDragGestures on the full-stage Canvas (HeartStage.kt:332); vertical swipe on 66% of the screen tips the cage on its side. Fix: yaw-only horizontal drag, vertical passes to LazyColumn. Command: adapt.
2. [P1] Settings don't tell the truth. RealRepo.update reverts silently (RealRepo.kt:183); Forget removes before BLE confirms, no confirm; Update button onClick = {} (BeaconSettings.kt:135); no "applies when it wakes" under an Asleep stage. Fix: pending/applied supporting line, snackbar + Retry on failure, Forget confirm, wire or drop Update. Command: harden, clarify.
3. [P1] Heart arrival rarely reaches the phone. Plays only if the app is open within 30 s (HeartStage.kt:238); opening from the notification shows a static cage + 8dp dot. Fix: on open, play the unseen mood once, captioned as a replay; tap the foot line to replay. Command: delight, animate.
4. [P2] Accidental sends + timing-only Sorry/Annoy. Gesture key reads as a legend; "tap the heart" only in contentDescription; a stray poke lights the other desk. Fix: key entries become 48dp send chips; brief Cancel while pending. Command: clarify, harden.
5. [P2] Themes don't carry states equally. Light Asleep dim 0.8 barely differs from awake; light glow faint between beats (verified); "Shown" in brass; light brass fill 2.88:1 on plate. Fix: paper asleep ~0.6 + cool cast, stronger light glow, Shown in onSurfaceVariant, light brass ~#A57C1F. Command: colorize, polish.

## Persona red flags
- BulBul glancing after a notification: no heart playing; Recent sits below Remove beacon; HB-id under the name; "Moods wait and go out after" (after what?).
- First-timer: nothing visible says tap the heart; Near vs Wi-Fi -58 dBm mixes proximity and transport; "admin"/"circle" wrong words for two people.
- One-handed: swipe up on the heart tilts instead of scrolling; login keyboard hides Sign in, 372dp hero never collapses; Add step 3 keyboard covers password + Join, Done does nothing.
- TalkBack: failed save never announced; every Forget reads "Forget" with no SSID; lock icon silent; no Show battery action.

## Minor observations
- Rotation drops to Home and can resurrect a stale AddDeviceVm (MainActivity.kt:126-130 remember, not rememberSaveable); landscape Home overlaps; Back exits the whole Add flow.
- Twin.waiting is a plain var, so "N waiting" goes stale.
- Notification switches all on while "Notifications: Off" in red below.
- Add step 2 spinner above its heading; step 1 pulses blue before the user pressed anything.
- Light nav-bar scrim is a white band; dark settings text runs under the nav buttons.
- Mixed filled/outlined icon styles.
- Per-frame allocations (ColorFilter/Stroke/Brush) during fx; fine at rest.

## Questions to consider
- When you send, the real question is "will she see it now?"; the stage only shows your own beacon. Should it answer that?
- Should Home be just the heart + Recent, with beacon settings behind the gear?
