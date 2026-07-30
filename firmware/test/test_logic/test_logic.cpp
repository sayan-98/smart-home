// ---------------------------------------------------------------------------
//  Native unit tests for the hardware-free logic.
//
//  These are the pieces where a subtle bug is expensive and hard to see on a
//  bench: contact debouncing, press-gesture decoding, and reconnect backoff.
//  Run with:  pio test -e native
// ---------------------------------------------------------------------------
#include <unity.h>

#include "util/Backoff.h"
#include "util/Crc32.h"
#include "util/Debouncer.h"
#include "util/PressDecoder.h"

using sh::Backoff;
using sh::Debouncer;
using sh::PressDecoder;
using sh::PressResult;

// Unity links these unconditionally. Every fixture here is constructed inside
// its own test, so there is nothing to do per test.
void setUp() {}
void tearDown() {}

// --- Debouncer -------------------------------------------------------------

void test_debounce_accepts_first_edge_immediately() {
  Debouncer d;
  d.configure(25, true);
  TEST_ASSERT_FALSE(d.update(true, 0));  // priming reports no change

  // The whole point of leading-edge debounce: no waiting.
  TEST_ASSERT_TRUE(d.update(false, 1));
  TEST_ASSERT_FALSE(d.level());
}

void test_debounce_ignores_bounce_inside_lockout() {
  Debouncer d;
  d.configure(25, true);
  d.update(true, 0);
  TEST_ASSERT_TRUE(d.update(false, 10));  // accepted

  // Contact chatter over the next few ms must be swallowed.
  TEST_ASSERT_FALSE(d.update(true, 12));
  TEST_ASSERT_FALSE(d.update(false, 15));
  TEST_ASSERT_FALSE(d.update(true, 20));
  TEST_ASSERT_FALSE(d.level());
}

void test_debounce_self_corrects_after_lockout() {
  Debouncer d;
  d.configure(25, true);
  d.update(true, 0);
  TEST_ASSERT_TRUE(d.update(false, 10));

  // If the line really settled back HIGH, we must notice once the window ends
  // rather than staying stuck reporting LOW forever.
  TEST_ASSERT_FALSE(d.update(true, 20));
  TEST_ASSERT_TRUE(d.update(true, 40));
  TEST_ASSERT_TRUE(d.level());
}

void test_debounce_no_phantom_change_when_stable() {
  Debouncer d;
  d.configure(25, false);
  d.update(false, 0);
  for (uint32_t t = 1; t < 500; t += 7) {
    TEST_ASSERT_FALSE(d.update(false, t));
  }
}

// --- PressDecoder ----------------------------------------------------------

void test_press_instant_mode_fires_on_press_edge() {
  PressDecoder p;
  p.configure(0, 0);  // both gestures off -> zero added latency
  TEST_ASSERT_TRUE(p.instantMode());

  TEST_ASSERT_EQUAL(static_cast<int>(PressResult::Short),
                    static_cast<int>(p.onEdge(true, 100)));
  // Release must not produce a second toggle.
  TEST_ASSERT_EQUAL(static_cast<int>(PressResult::None),
                    static_cast<int>(p.onEdge(false, 180)));
}

void test_press_long_press_fires_while_held() {
  PressDecoder p;
  p.configure(800, 0);

  TEST_ASSERT_EQUAL(static_cast<int>(PressResult::None),
                    static_cast<int>(p.onEdge(true, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(PressResult::None),
                    static_cast<int>(p.tick(500)));
  TEST_ASSERT_EQUAL(static_cast<int>(PressResult::Long),
                    static_cast<int>(p.tick(800)));
  // Only once, and the release is swallowed so it does not also toggle.
  TEST_ASSERT_EQUAL(static_cast<int>(PressResult::None),
                    static_cast<int>(p.tick(1200)));
  TEST_ASSERT_EQUAL(static_cast<int>(PressResult::None),
                    static_cast<int>(p.onEdge(false, 1300)));
}

void test_press_short_press_deferred_when_long_enabled() {
  PressDecoder p;
  p.configure(800, 0);
  TEST_ASSERT_FALSE(p.instantMode());

  p.onEdge(true, 0);
  TEST_ASSERT_EQUAL(static_cast<int>(PressResult::None),
                    static_cast<int>(p.tick(100)));
  // Released before the long threshold -> short, decided at release.
  TEST_ASSERT_EQUAL(static_cast<int>(PressResult::Short),
                    static_cast<int>(p.onEdge(false, 200)));
}

void test_press_double_press_detected() {
  PressDecoder p;
  p.configure(0, 300);

  p.onEdge(true, 0);
  TEST_ASSERT_EQUAL(static_cast<int>(PressResult::None),
                    static_cast<int>(p.onEdge(false, 80)));
  TEST_ASSERT_EQUAL(static_cast<int>(PressResult::Double),
                    static_cast<int>(p.onEdge(true, 200)));
  TEST_ASSERT_EQUAL(static_cast<int>(PressResult::None),
                    static_cast<int>(p.onEdge(false, 260)));
}

void test_press_single_emitted_after_double_window_expires() {
  PressDecoder p;
  p.configure(0, 300);

  p.onEdge(true, 0);
  p.onEdge(false, 80);
  TEST_ASSERT_EQUAL(static_cast<int>(PressResult::None),
                    static_cast<int>(p.tick(300)));
  TEST_ASSERT_EQUAL(static_cast<int>(PressResult::Short),
                    static_cast<int>(p.tick(400)));
  // And not repeatedly.
  TEST_ASSERT_EQUAL(static_cast<int>(PressResult::None),
                    static_cast<int>(p.tick(500)));
}

// --- Backoff ---------------------------------------------------------------

void test_backoff_doubles_and_saturates() {
  Backoff b;
  b.configure(1000, 8000, 0);  // no jitter, so the sequence is exact

  TEST_ASSERT_EQUAL_UINT32(1000, b.next(0));
  TEST_ASSERT_EQUAL_UINT32(2000, b.next(0));
  TEST_ASSERT_EQUAL_UINT32(4000, b.next(0));
  TEST_ASSERT_EQUAL_UINT32(8000, b.next(0));
  TEST_ASSERT_EQUAL_UINT32(8000, b.next(0));
  TEST_ASSERT_EQUAL_UINT32(8000, b.next(0));

  b.reset();
  TEST_ASSERT_EQUAL_UINT32(1000, b.next(0));
}

void test_backoff_jitter_stays_in_band() {
  Backoff b;
  b.configure(1000, 60000, 20);
  for (uint32_t seed = 0; seed < 500; ++seed) {
    Backoff x;
    x.configure(1000, 60000, 20);
    const uint32_t v = x.next(seed);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(800, v);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(1200, v);
  }
}

// --- CRC32 -----------------------------------------------------------------

void test_crc32_known_vector() {
  // "123456789" -> 0xCBF43926 for CRC-32/ISO-HDLC.
  const char* s = "123456789";
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, sh::crc32(s, 9));
}

void test_crc32_detects_single_bit_flip() {
  uint8_t a[16];
  for (uint8_t i = 0; i < 16; ++i) a[i] = i;
  const uint32_t before = sh::crc32(a, sizeof(a));
  a[7] ^= 0x01;
  TEST_ASSERT_NOT_EQUAL(before, sh::crc32(a, sizeof(a)));
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_debounce_accepts_first_edge_immediately);
  RUN_TEST(test_debounce_ignores_bounce_inside_lockout);
  RUN_TEST(test_debounce_self_corrects_after_lockout);
  RUN_TEST(test_debounce_no_phantom_change_when_stable);

  RUN_TEST(test_press_instant_mode_fires_on_press_edge);
  RUN_TEST(test_press_long_press_fires_while_held);
  RUN_TEST(test_press_short_press_deferred_when_long_enabled);
  RUN_TEST(test_press_double_press_detected);
  RUN_TEST(test_press_single_emitted_after_double_window_expires);

  RUN_TEST(test_backoff_doubles_and_saturates);
  RUN_TEST(test_backoff_jitter_stays_in_band);

  RUN_TEST(test_crc32_known_vector);
  RUN_TEST(test_crc32_detects_single_bit_flip);

  return UNITY_END();
}
