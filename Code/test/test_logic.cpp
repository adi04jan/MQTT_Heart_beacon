// Host test for beacon_logic.h. Run: see CLAUDE.md ("Host tests").
#include <cassert>
#include <cstdio>

#include "../beacon_logic.h"

static BtnEvent feed(Button &b, bool pressed, uint32_t from, uint32_t to, uint32_t step = 10) {
  BtnEvent last = BtnEvent::None;
  for (uint32_t t = from; t <= to; t += step) {
    BtnEvent e = b.update(pressed, t);
    if (e != BtnEvent::None) {
      assert(last == BtnEvent::None && "at most one event per segment");
      last = e;
    }
  }
  return last;
}

static void test_button() {
  Button b;
  b.begin(false, 0);
  // Short tap 150 ms -> Short on release.
  assert(feed(b, true, 100, 250) == BtnEvent::None);
  assert(feed(b, false, 260, 400) == BtnEvent::Short);
  // A 400 ms press is still a Short (old firmware treated >200 ms as "battery").
  assert(feed(b, true, 1000, 1400) == BtnEvent::None);
  assert(feed(b, false, 1410, 1600) == BtnEvent::Short);
  // Hold -> exactly one Long while held, nothing on release.
  assert(feed(b, true, 2000, 4000) == BtnEvent::Long);
  assert(feed(b, false, 4010, 4500) == BtnEvent::None);
  // Bounce shorter than debounce does nothing.
  assert(b.update(true, 5000) == BtnEvent::None);
  assert(b.update(false, 5010) == BtnEvent::None);
  assert(feed(b, false, 5020, 5500) == BtnEvent::None);
  // Release bounce after a long hold must not send a Short.
  assert(feed(b, true, 6000, 7000) == BtnEvent::Long);
  b.update(false, 7010);
  b.update(true, 7015);
  b.update(false, 7020);
  assert(feed(b, false, 7030, 7500) == BtnEvent::None);
  // Wake-up press held through boot is swallowed.
  Button w;
  w.begin(true, 0);
  assert(feed(w, true, 0, 3000) == BtnEvent::None);
  assert(feed(w, false, 3010, 3500) == BtnEvent::None);
  assert(feed(w, true, 4000, 4100) == BtnEvent::None);
  assert(feed(w, false, 4110, 4300) == BtnEvent::Short);
}

static void test_battery() {
  assert(battery_pixels(3000, 3300, 4150, 9) == 0);
  assert(battery_pixels(3300, 3300, 4150, 9) == 0);
  assert(battery_pixels(4150, 3300, 4150, 9) == 9);
  assert(battery_pixels(4215, 3300, 4150, 9) == 9);
  assert(battery_pixels(3725, 3300, 4150, 9) == 4);  // half way
  assert(battery_pixels(4149, 3300, 4150, 9) == 8);
}

static void test_every() {
  uint32_t last = 0;
  assert(!every(999, last, 1000));
  assert(every(1000, last, 1000) && last == 1000);
  assert(!every(1999, last, 1000));
  last = 0xFFFFFF00u;  // millis() wrap
  assert(!every(0x00000010u, last, 1000));
  assert(every(0x00000300u, last, 1000));
}

static void test_version() {
  assert(version_is_newer("0.1.0", "0.0.9"));
  assert(!version_is_newer("0.0.9", "0.1.0"));  // server behind a bench build: no downgrade
  assert(!version_is_newer("0.1.0", "0.1.0"));
  assert(version_is_newer("0.1.10", "0.1.9"));  // numeric, not string compare
  assert(version_is_newer("1.0", "0.9.9"));
  assert(!version_is_newer("0.1", "0.1.0"));
  assert(version_is_newer("0.1.1", "0.1"));
  assert(!version_is_newer("", "0.1.0"));
  assert(!version_is_newer("<html>", "0.1.0"));
}

int main() {
  test_version();
  test_button();
  test_battery();
  test_every();
  std::puts("test_logic: all passed");
  return 0;
}
