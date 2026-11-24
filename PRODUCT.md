# Product

<!-- impeccable:product-schema 1 -->

## Platform

android

## Users

Two people, Aditya and BulBul, each with one Heart Beacon on their own desk (usually at the office) and an Android 14+
phone. The app is private to the two of them: no store listing, no strangers, no generic onboarding. Each person uses
it to see their own beacon, send the other a heart, and set their beacon up.

## Product Purpose

A Heart Beacon is a battery-powered desk toy: a heart of 9 LEDs and one button. A tap on one beacon lights the other
person's beacon. The app is the beacon's companion: it mirrors **your own** beacon live, lets you send a mood from the
phone, and holds the beacon's settings. Success: the 3D heart on the phone is a faithful, real-time twin of the heart
on the desk, and sending a heart from it feels as direct as pressing the real button.

## Positioning

The phone is not a remote control for a gadget; it is a second body of the same heart. What the desk beacon does
(heartbeat in red when a heart arrives, sleeping after office hours, low battery) the phone heart does at the same
moment, and the same gestures work on both.

## Operating Context

- Office desk toy. Office hours Mon-Sat 09:00-19:00 IST; outside them the beacon sleeps and missed hearts play once
  on wake (coalesced).
- Phone and beacon talk over BLE when near (phone relays to the server) and the beacon uses WiFi when the phone is
  away. Server: own MQTT + HTTPS API reached by a public IP with a pinned private CA.
- The app runs an always-on foreground service (`connectedDevice`) with auto/fast reconnect; the UI is opened
  occasionally, glanced at, and closed.

## Capabilities and Constraints

- Moods (`protocol/PROTOCOL.md`): `heart`, `sorry`, `annoy`. On the beacon: tap = heart, double tap = sorry,
  triple tap = annoy. Incoming moods carry the sender's display name (`mood.from`); sent moods get a `shown` receipt
  when the other beacon actually played them.
- Beacon status reaching the phone: `state` (lowercase power states such as on / nap / sleep), `tele` (battery,
  RSSI, firmware), link state (BLE near vs WiFi vs offline).
- Beacon settings (signed config from the server): brightness, schedule (auto on/off timer, can run past midnight),
  idle NAP (off by default), WiFi networks (add/forget, sent over BLE, never in the signed config), remove device.
- App structure (decided 2026-11-24): the 3D heart screen **replaces Home**: the heart with a small overlay dashboard
  on its left (battery, network, link), the beacon's settings scrolling below. App settings and Add device stay
  separate screens.
- Kotlin + Jetpack Compose, minSdk 34 / target 36. Hardware: 9 WS2812 LEDs, button on GPIO0, no hardware change
  planned. Undecided: FCM (after measurement).

## Brand Commitments

- Names: "Heart Beacon"; people are shown by display name ("Aditya", "BulBul").
- Copy is personal and plain, written for two people who know each other, not for customers.

## Evidence on Hand

- Real protocol, server and firmware in this repo (`protocol/`, `server/`, `fw/`); existing app screens and data
  layer in `android/`.
- No marketing, testimonials or metrics exist; none are needed.

## Product Principles

1. **Twin, not remote.** The phone heart shows what the desk heart is doing now; never invent a state the beacon is
   not in. Unknown is shown as unknown.
2. **Same gestures everywhere.** Tap / double / triple mean heart / sorry / annoy on the phone exactly as on the
   button.
3. **Glance, then go.** Battery, network and link are readable in one look without leaving the heart.
4. **Quiet by default.** The beacon and app stay calm after hours and on battery; nothing nags.
