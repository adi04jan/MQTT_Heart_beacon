---
name: Heart Beacon
description: The Android companion of a hand-soldered brass heart; made of what the beacon is made of.
colors:
  brass: "#A57C1F"
  brass-ink: "#765710"
  on-brass: "#231700"
  brass-bright: "#E2BC6A"
  brass-container: "#F1DFB0"
  brass-container-dark: "#55410F"
  tin: "#F0F0EE"
  tin-plate: "#FAFAF8"
  tin-high: "#E1E0DC"
  tin-ink: "#1A1917"
  tin-ink-soft: "#55524B"
  tin-hairline: "#D3D1CB"
  felt: "#131211"
  felt-plate: "#1E1D1A"
  felt-high: "#34322E"
  felt-ink: "#EAE5DC"
  felt-ink-soft: "#C6C0B4"
  felt-hairline: "#3B3934"
  stage-ground: "#110C16"
  stage-text: "#F4EEF3"
  stage-text-soft: "#CABDCC"
  stage-label: "#A898AE"
  stage-pip-off: "#3D3145"
  stage-warn: "#F2B8B5"
  stage-day-ground: "#D5CADB"
  stage-day-text: "#1E1722"
  stage-day-text-soft: "#4C4352"
  stage-day-label: "#5D5363"
  stage-day-pip-off: "#BFB3C6"
  stage-day-warn: "#B3261E"
  led-heart: "#FA343C"
  led-sorry: "#EBA117"
  led-annoy: "#17E7EB"
  led-pair: "#0000FA"
  mood-heart-on-tin: "#C81E2B"
  mood-sorry-on-tin: "#B85C00"
  mood-annoy-on-tin: "#00757A"
  error: "#B3261E"
typography:
  display:
    fontFamily: "Jost, sans-serif"
    fontSize: "40sp"
    fontWeight: 300
    lineHeight: "46sp"
    letterSpacing: "-0.4sp"
  headline:
    fontFamily: "Jost, sans-serif"
    fontSize: "30sp"
    fontWeight: 400
    lineHeight: "36sp"
    letterSpacing: "-0.2sp"
  title:
    fontFamily: "Jost, sans-serif"
    fontSize: "19sp"
    fontWeight: 500
    lineHeight: "26sp"
  body:
    fontFamily: "Roboto, system-ui, sans-serif"
    fontSize: "16sp"
    fontWeight: 400
    lineHeight: "24sp"
  label:
    fontFamily: "Jost, sans-serif"
    fontSize: "16sp"
    fontWeight: 500
    letterSpacing: "0.2sp"
rounded:
  xs: "6dp"
  sm: "10dp"
  md: "16dp"
  lg: "22dp"
  xl: "28dp"
  full: "999dp"
spacing:
  xs: "4dp"
  s: "8dp"
  m: "12dp"
  l: "16dp"
  xl: "24dp"
  xxl: "32dp"
  screen: "16dp"
components:
  button-primary:
    backgroundColor: "{colors.brass}"
    textColor: "{colors.on-brass}"
    typography: "{typography.label}"
    rounded: "{rounded.full}"
    height: "52dp"
  button-primary-on-stage:
    backgroundColor: "{colors.brass-bright}"
    textColor: "#2E2100"
    rounded: "{rounded.full}"
    height: "48dp"
  plate:
    backgroundColor: "{colors.tin-plate}"
    textColor: "{colors.tin-ink}"
    rounded: "{rounded.lg}"
  day-toggle-on:
    backgroundColor: "{colors.brass}"
    textColor: "{colors.on-brass}"
    rounded: "{rounded.full}"
    size: "40dp"
  stage:
    backgroundColor: "{colors.stage-ground}"
    textColor: "{colors.stage-text}"
    rounded: "{rounded.xl}"
---

# Design System: Heart Beacon

## Overview

**Creative North Star: "Made of what it's made of"**

The beacon is a heart of thin brass rods, joined by hand with blobs of tin solder, photographed on grey felt. The app
uses those three materials and nothing else. Brass is the one accent: every control you can press, every selected
state and the primary action. Tin, the solder's pale neutral grey, is the light ground. Felt, the charcoal cloth, is
the dark ground. The stage the beacon stands on is violet cloth, brass's complement: aubergine felt in the dark
theme, pale lilac in the light one.

The object leads. Home opens on the beacon itself, drawn from the real build: rods, soft uneven joints, battery
block, board, LEDs and wire. Everything else is quiet Material 3 on tin or felt, with section rows gathered on
rounded plates. Saturated colour exists only where the firmware puts it, in the LEDs, so colour always means a mood
or the battery.

Nothing moves at rest. The stage draws a frame only while an LED effect, a gesture or a transition runs.

**Key Characteristics:**
- One accent (brass), two grounds (tin, felt), firmware LED colours as the only saturated colour.
- A violet stage that follows the theme (aubergine / lilac); the object and its light are the picture.
- Jost for names, headings, figures and buttons; the system face for reading.
- Sections on rounded plates with hairlines; no shadows.
- The twin shows only what the desk beacon does. Unknown is shown as unknown.

## Colors

Two metals and a cloth, plus the light the LEDs give.

### Primary
- **Brass** (#A57C1F on tin and on the lilac stage, #E2BC6A on felt and on the aubergine stage), with dark lettering
  (#231700, 4.6:1) on it: the fill of main buttons, switches that are on, the slider, chosen day toggles, the awake
  span of the day bar, the step bars. On tin plate it holds 3.6:1, so a fill reads as a shape on its own.
- **Brass ink** (#765710 on tin, #684C0D on the lilac stage; the bright brass on felt): brass as text and icons, for
  text buttons and the add mark, where a fill colour would not have the contrast. Nothing that cannot be pressed or is not a selection is
  brass.

### Neutral
- **Tin** (#F0F0EE): the light ground. **Tin plate** (#FAFAF8): section plates on it. **Tin high** (#E1E0DC): tracks,
  discs behind mood icons. **Tin ink** (#1A1917) and **soft ink** (#55524B): text. **Hairline** (#D3D1CB).
- **Felt** (#131211): the dark ground. **Felt plate** (#1E1D1A), **felt high** (#34322E), **felt ink** (#EAE5DC),
  **soft ink** (#C6C0B4), **hairline** (#3B3934).
- **Stage, dark** (aubergine felt: #110C16 under a procedural lamp pool that lifts to about #3F2D52, with visible
  grain, darkened again behind the name and the foot): text #F4EEF3, soft text #CABDCC, labels #A898AE, unlit pips
  #3D3145, warnings #F2B8B5.
- **Stage, light** (lilac cloth: #D5CADB lifting to about #F6EBFD under the pool, finer grain): text #1E1722, soft
  text #4C4352, labels #5D5363, unlit pips #BFB3C6, warnings #B3261E; mood marks use the on-tin mood colours. The
  cage's far side fades into the cloth instead of darkening, and LED light is laid over the cloth instead of added.
  Asleep, the cage dims part way, cools and thins into the cloth, so it reads apart from awake as clearly as on felt.

### LED colours (data, not decoration)
- **Heart** 250,0,0 (drawn #FA343C as a mark on the stage, #C81E2B on tin), **Sorry** 235,161,23 (#B85C00 on tin, kept apart from brass),
  **Annoy** 23,231,235 (#00757A on tin), **Pair** 0,0,250, and the battery bar's 9 steps from red through yellow to
  green. The values come from `core/Fx.kt`, which mirrors the firmware.

### Named Rules
**The Firmware Colour Rule.** A saturated colour on screen is a colour the LEDs can show, and it means that mood or
the battery level. No other red, amber, cyan or blue appears, except Material's error role.

**The Brass Is Pressable Rule.** Brass marks controls and selections only. Headings and facts are ink.

## Typography

**Display Font:** Jost (bundled variable font, SIL OFL), weights 300, 400, 500
**Body Font:** the system face (Roboto)

**Character:** Jost is the geometric lettering of instrument dials: it sets the beacon's name, section headings,
the large readings and button labels. Reading text, field labels and supporting lines stay on the system face.

### Hierarchy
- **Display** (300, 40sp, 46sp): the app name on sign-in; the battery figure on the stage (40sp, tabular).
- **Headline** (400, 30sp and 26sp): the beacon's name on the stage; step headings in Add beacon.
- **Title** (500, 23sp and 19sp): top bar titles and time values (23sp); section headings (19sp). Stage readings use
  Jost 400 at 23sp.
- **Body** (system, 16sp and 14sp): row titles, supporting text, explanations.
- **Label** (Jost 500, 16sp, +0.2sp): buttons, day letters. Small labels (system, 12sp): stage readout labels, ticks.

### Named Rules
**The Tabular Rule.** Battery, times, dBm, versions and step counts use tabular figures so values do not jitter.

## Layout

One column, phone first. Screen gutter 16dp; plates sit inside it and pad their rows 16dp; long-form steps (Sign in,
Add beacon) use 24dp. Spacing steps are 4, 8, 12, 16, 24, 32dp: tight inside a plate, 28dp above a section heading
and 8dp below it.

Home is a single scroll: the stage at 66% of the height with its bottom corners rounded 28dp, then Recent right under
it, then one plate per settings section. A vertical swipe anywhere scrolls; on the stage only a sideways drag turns the
cage. The stage stacks its name bar, the dashboard and the foot in one column, the dashboard (a 164dp column over the
cage's left lobe) centred in the space between, so they never overlap; the cage is centred at 60% of the width. Once
the stage scrolls away, a standard top bar with the beacon's name pins. Every touch target is at least 48dp. Layout
holds at font scale 1.3 with the gesture keys on one line; past that they flow to a second.

## Elevation & Depth

Flat, by tone. Plates are one tonal step off their ground (tin plate on tin, felt plate on felt) with no shadow.
Depth belongs to the stage, where it is physical: the cage's far side sinks into the dark, the beacon casts a soft
shadow with a dark core where its tip touches the cloth, and lit LEDs throw coloured light on the rods, the block and the cloth.

### Named Rules
**The Light Is Real Rule.** Glow appears only where an LED is lit, in that LED's colour. No decorative glows, no
glass, no gradient text.

## Shapes

Round and soft, like solder. Plates 22dp; the stage's lower corners and picture panels 28dp; buttons, toggles, pips
and step bars fully round or near it; text fields keep Material's outlined shape. Hairlines (1dp, inset 16dp)
separate rows inside a plate. On the cage itself joints are rounded beads that swallow the rod ends, never sharp
corners.

## Components

### Buttons
- **Shape:** full pill, 52dp high for a screen's main action (48dp on the stage).
- **Primary:** brass with dark lettering on tin; bright brass with dark lettering on felt and on the stage.
- **Secondary:** tonal (neutral) for inline actions such as Allow; text buttons in brass for Forget, Scan again;
  destructive actions are text buttons in the error role.

### Plates (section containers)
- **Corner Style:** 22dp. **Background:** tin plate / felt plate. **Shadow:** none. **Border:** none.
- **Rows:** Material list items on a transparent ground, a hairline between them, heading above the plate in Jost.

### Inputs / Fields
- **Style:** Material outlined fields on the ground colour; brass focus; error role with a supporting line.

### Navigation
- A top bar with a Back arrow and a Jost title on separate screens; Home's bar is transparent over the stage and
  pins as a tonal bar after scrolling. System Back always works.

### The stage (signature)
The beacon on felt: `CageRenderer` draws `Cage`'s tested geometry as brass rods with a wide soft highlight, tin
beads of uneven size (about 1.4 rod widths in radius, one smooth dome with a wet point of light) drawn over the
rod ends so the rods run into the tin, the battery block, board, USB-C socket, button, LED packages
and copper chain wire. LED frames come from `Fx` through `Twin`, so colours and timings are the firmware's. Tap,
double tap, triple tap and hold mirror the physical button; a sideways drag turns the cage with inertia and it settles
back. A mood waits 1.5 s at the foot ("Sending your Heart", Cancel) before it goes out, so a stray poke can be taken
back; a newer one sends the waiting one at once. A sent mood leaves as a heart-shaped wave in its colour and never
lights the user's own LEDs. The newest mood from the other side plays once when the app opens, if the beacon is up
(it came while the app was closed, the usual case); the foot says when it came and offers Play again. Asleep is dim
and cool; offline is a desaturated ghost.

### Dashboard readouts
A small label, then a Jost reading, then at most one supporting line. Battery is a large figure with 9 pips in the
battery bar's own colours; Wi-Fi strength is four rising bars. Stale values say so ("Last known") and dim.

### Gesture keys
One, two and three dots and a bar, labelled Heart, Sorry, Annoy, Battery, each a key with a thin ring on the cloth
(48dp to touch): the dots send that mood as that many taps on the heart do, the bar plays the battery as a hold does.
The key being tapped or waiting to send lights its ring and dots in its mood's colour. The same dot counts, in the
mood's colour on a quiet disc, mark each row of Recent; a delivery mark ("Shown") is a fact, in soft ink, not brass.

### Settings that cannot land yet
Settings save at once and say so plainly when they fail (a snackbar: "Not saved." with the reason and Retry; the
control goes back). While the beacon sleeps or is offline, a line under the first heading says changes reach it when
it wakes. Wi-Fi goes to the beacon over Bluetooth only, so Forget and Add network wait for the phone to be near, and
Forget asks first.

### Day plan
A midnight-to-midnight bar with the awake hours filled in brass, ticks at 6, 12 and 18 hours, the two times beneath
as large tappable values, and seven round day toggles.

## Do's and Don'ts

### Do:
- **Do** keep brass for things that can be pressed or are selected.
- **Do** take every LED colour and timing from `Fx`; show stale or missing values as stale or unknown.
- **Do** keep the stage on violet cloth that follows the theme, and draw frames only while something is happening.
- **Do** group a section's rows on one plate with hairlines, heading above in Jost.
- **Do** keep joints on the cage rounded and uneven, rods thin, highlights wide and soft.

### Don't:
- **Don't** add a second accent or use an LED colour for decoration.
- **Don't** play a sent mood on the user's own heart.
- **Don't** add shadows under plates, glass panels, gradient text, emoji icons or small labels above headings.
- **Don't** animate the stage at rest.
