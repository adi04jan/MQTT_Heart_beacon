// Hardware-free logic for the Heart Beacon. No Arduino includes: host-tested by test/test_logic.cpp.
#pragma once
#include <stdint.h>

enum class BtnEvent : uint8_t { None, Short, Long };

// Debounced short/long press classifier. Feed it the raw "pressed" level and millis().
// Short fires on release (if Long did not fire); Long fires once while still held.
struct Button {
  static const uint32_t DEBOUNCE_MS = 30;
  static const uint32_t LONG_MS = 600;
  bool raw = false, stable = false, long_fired = false;
  uint32_t raw_since = 0, pressed_at = 0;

  // Call once at boot. A press already in progress (e.g. the wake-up press) is swallowed.
  void begin(bool pressed, uint32_t now) {
    raw = stable = long_fired = pressed;
    raw_since = pressed_at = now;
  }

  BtnEvent update(bool pressed, uint32_t now) {
    if (pressed != raw) {
      raw = pressed;
      raw_since = now;
    }
    if (raw != stable && now - raw_since >= DEBOUNCE_MS) {
      stable = raw;
      if (stable) {
        pressed_at = now;
        long_fired = false;
      } else if (!long_fired) {
        return BtnEvent::Short;
      }
    }
    if (stable && !long_fired && now - pressed_at >= LONG_MS) {
      long_fired = true;
      return BtnEvent::Long;
    }
    return BtnEvent::None;
  }
};

// Battery millivolts -> number of lit pixels, 0..count, linear between min and max.
inline int battery_pixels(uint32_t mv, uint32_t min_mv, uint32_t max_mv, int count) {
  if (mv <= min_mv) return 0;
  if (mv >= max_mv) return count;
  return (int)((uint64_t)(mv - min_mv) * (uint32_t)count / (max_mv - min_mv));
}

// True if dotted version `candidate` (e.g. "0.1.10") is strictly newer than `current`.
// Non-digit junk (captive-portal HTML, empty body) parses as 0 and is never newer than a real version.
inline bool version_is_newer(const char *candidate, const char *current) {
  while (*candidate || *current) {
    unsigned long a = 0, b = 0;
    while (*candidate >= '0' && *candidate <= '9') a = a * 10 + (unsigned long)(*candidate++ - '0');
    while (*current >= '0' && *current <= '9') b = b * 10 + (unsigned long)(*current++ - '0');
    if (a != b) return a > b;
    if (*candidate) candidate++;
    if (*current) current++;
  }
  return false;
}

// millis()-wraparound-safe periodic timer: true (and re-arms) once `interval` has passed since `last`.
inline bool every(uint32_t now, uint32_t &last, uint32_t interval) {
  if (now - last < interval) return false;
  last = now;
  return true;
}
