---
version: 1
slug: "tlin-com-heartbeacon-app-ui-homescreen-kt-2dec60b4"
primary_target: "android/app/src/main/kotlin/com/heartbeacon/app/ui/HomeScreen.kt"
related_targets: []
---

# Home (beacon twin)

Scope: the app's main screen, replacing the old Home and the separate Device settings route. Mode: Operate, with one
experiential element (the heart). Audience: Aditya or BulBul, glancing at their own beacon, sending a mood, tweaking
settings. Build path: code-led (no image generation on this machine).

## Direction contract

THESIS: Home is a display case for one object, your brass heart, alive and in sync. It refuses the category default
of a device card, a status list and a big "Send" button.

OWN-WORLD: Below the stage, the app's incumbent world unchanged (rose-wine primary on near-neutral grounds, Material
3, system face, tabular numerals). The stage is a fixed dark vitrine in both themes: warm near-black, a soft floor
pool of light under the heart. Its only colours are brass rods, silver solder nodes, a dark core block, and the
firmware's LED colours (heart 250,0,0; sorry 235,161,23; annoy 23,231,235; the 9-step red-to-green battery ramp).

STORY: One glance says the beacon's state: dark and still is awake and idle, dimmed and cold is asleep (with the wake
time), a grey ghost is offline. Battery reads as 9 pips in the beacon's own battery-bar colours. Tap the heart like
the button: once Heart, twice Sorry, three times Annoy; hold for the battery bar. A mood arriving plays the exact
firmware effect on the phone heart at the moment the desk heart plays it.

FIRST VIEWPORT: Full-bleed dark stage, about 60% of the height, under a transparent top bar (beacon name, settings
icon). The cage fills ~78% of the stage width, centred a little right. The dashboard is a narrow left column over the
left lobe: Battery (pips + %), Network (near / Wi-Fi dBm / offline), State. Stage foot: the last event in one line and
the gesture hint. The first settings section starts just below the stage edge.

FORM: pinned by the user's brief (heart stage, overlapping left dashboard, config below); no concept roll, seed none.
Signature interaction: tap squash with a tap-count readout, then the firmware pulse; drag turns the cage with
inertia; rods catch the LEDs' light.

FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, DESIGN.md, and every shipping raster carrying its provenance
