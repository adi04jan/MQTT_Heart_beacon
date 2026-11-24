---
version: 1
slug: "tlin-com-heartbeacon-app-ui-homescreen-kt-2dec60b4"
primary_target: "android/app/src/main/kotlin/com/heartbeacon/app/ui/HomeScreen.kt"
related_targets: []
---

# Home (beacon twin) and the app around it

Scope: the app's main screen, plus Login, Add beacon and App settings so the whole app is one world. Mode: Operate,
with one experiential element (the heart). Audience: Aditya or BulBul, glancing at their own beacon, sending a mood,
tweaking settings. Build path: code-led (no image generation on this machine). Raised 2026-11-24 after the user called
the first build "too basic", and again for "softer at joint".

## Direction contract

THESIS: The app is made of what the beacon is made of. Home is the brass heart itself on its cloth, alive and in
sync; it refuses the category default of a device card, a status list and a big "Send" button.

OWN-WORLD: Two metals and a cloth. Brass is the one accent (controls, selection, the primary action); tin, the
solder's pale grey, is the light ground; felt, the charcoal cloth in the photos, is the dark ground and the stage in
both themes (lamp pool, fine grain). Saturated colour is only the firmware's LED colours (heart 250,0,0; sorry
235,161,23; annoy 23,231,235; pair 0,0,250; the 9-step battery ramp) and always means a mood or the battery. Jost, a
dial-lettering geometric, sets names, headings, figures and buttons; body stays on the system face. Settings sit on
rounded plates with hairlines, one plate per section.

STORY: One glance says the beacon's state: dark and still is awake and idle, dimmed and cold is asleep (with the wake
time), a grey ghost is offline with its battery marked last known. Tap the heart like the button: once Heart, twice
Sorry, three times Annoy; hold for the battery bar. A mood arriving plays the exact firmware effect at the moment the
desk heart plays it; a mood you send never lights your own heart, it leaves as a heart-shaped wave.

FIRST VIEWPORT: Full-bleed felt stage, 66% of the height, under a transparent bar (beacon name in Jost, did, settings
icon). The cage is centred at 60% across, about 70% of the width: thin brass rods running into soft, uneven tin beads,
black battery block, blue board and USB-C socket under it, nine white LED packages chained by copper wire, a contact
shadow on the cloth. The dashboard is a left column over the left lobe: Battery (a large figure and 9 pips), Network,
Beacon. Stage foot: the last event in one line and the drawn gesture key. The first settings plate starts below.

FORM: pinned by the user's brief (heart stage, overlapping left dashboard, config below). World roll: seed 8c795dac,
assigned grounded candidate 4 of 7 (the engraved instrument: brass, dial lettering, readouts). Challengers declined;
kept from the emission-line rail: state shown by form before hue (pips, bars, the ghost). Signature interaction: tap
squash, the gesture key lighting the count in the mood's colour, then the heart-shaped wave. Motion grammar: nothing
moves at rest; the intro turn, drag with inertia and a spring home, firmware-timed LED effects.

FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, DESIGN.md, and every shipping raster carrying its provenance
