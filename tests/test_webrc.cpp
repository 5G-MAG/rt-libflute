// libflute - FLUTE/ALC library
//
// Tests for the WEBRC sender schedule, RFC 3738.
//
// RFC 5775 clause 2.2 names WEBRC as the congestion control building block an ALC implementation
// must at minimum support. These cover the schedule arithmetic against the RFC's own formulas.
// They do not cover a running session, and nothing here claims conformance to RFC 3738.

#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>

#include "Webrc.h"

using namespace LibFlute::Webrc;

namespace {

Parameters recommended() {
  Parameters p;                       // RFC 3738 clause 3.1.1 defaults and recommendations
  p.sender_rate_bits_per_second = 1000000;
  p.packet_length_bytes = 1250;       // makes SR_P a round 100
  p.base_channel_rate_packets = 1.0;  // BCR_P default
  p.time_slot_duration_seconds = 10.0;  // TSD recommended
  p.quiescent_duration_seconds = 300.0; // QD recommended
  p.rate_drop_per_slot = 0.75;        // P default
  p.wave_duration_slots = 4;
  return p;
}

}  // namespace

/* "SR_P = SR_b/(8*LENP_B) is the sender transmission rate in packets per second." */
TEST(WebrcDerive, SenderRateInPackets) {
  auto d = derive(recommended());
  EXPECT_DOUBLE_EQ(d.sender_rate_packets, 100.0);
}

/* "BCR_b = 8*LENP_B*BCR_P is the rate of the base channel at the beginning of a time slot in bits
   per second." */
TEST(WebrcDerive, BaseChannelRateInBits) {
  auto d = derive(recommended());
  EXPECT_DOUBLE_EQ(d.base_channel_rate_bits, 10000.0);
}

/* "L = ceil(BCR_P*TSD*(P-1)/log(P)) is the number of base channel packets sent in each time slot." */
TEST(WebrcDerive, BasePacketsPerSlotMatchesTheFormula) {
  auto p = recommended();
  auto d = derive(p);
  const double expected = std::ceil(p.base_channel_rate_packets * p.time_slot_duration_seconds *
                                    (p.rate_drop_per_slot - 1.0) / std::log(p.rate_drop_per_slot));
  EXPECT_EQ(d.base_packets_per_slot, static_cast<uint32_t>(expected));
  EXPECT_EQ(d.base_packets_per_slot, 9u) << "BCR_P=1, TSD=10, P=0.75";
}

/* "Q = ceil(QD/TSD)" and "T = N + Q is the total number of time slots in a cycle. T is also the
   total number of wave channels." */
TEST(WebrcDerive, QuiescentSlotsAndChannelCount) {
  auto d = derive(recommended());
  EXPECT_EQ(d.quiescent_slots, 30u);
  EXPECT_EQ(d.wave_channels, 34u);
}

/* "Recall that CN = T for the base channel and CN = 0,1,...,T-1 for the wave channels." */
TEST(WebrcDerive, BaseChannelTakesChannelNumberT) {
  auto d = derive(recommended());
  EXPECT_EQ(base_channel_number(d), 34);
}

TEST(WebrcDerive, UnusableInputsAreRefused) {
  auto p = recommended();
  p.rate_drop_per_slot = 1.0;   // log(P) is zero, L undefined
  EXPECT_THROW(derive(p), std::invalid_argument);

  p = recommended(); p.rate_drop_per_slot = 0.0;
  EXPECT_THROW(derive(p), std::invalid_argument);

  p = recommended(); p.wave_duration_slots = 0;
  EXPECT_THROW(derive(p), std::invalid_argument);

  p = recommended(); p.packet_length_bytes = 0;
  EXPECT_THROW(derive(p), std::invalid_argument);
}

/* The channel number is 8 bits in the short-format CCI of clause 5.1, and the base channel takes T,
   so T must fit. A long quiescent period is what pushes it over. */
TEST(WebrcDerive, AChannelCountOverTheFieldWidthIsRefused) {
  auto p = recommended();
  p.quiescent_duration_seconds = 10000.0;  // Q = 1000
  EXPECT_THROW(derive(p), std::invalid_argument);
}

/* "wave channel i is active during time slots i-N+1 modulo T, i-N+2 modulo T, ..., i and is
   quiescent for time slots i+1 modulo T, i+2 modulo T, ..., i+Q modulo T." */
TEST(WebrcSchedule, AWaveIsActiveForTheNSlotsEndingAtItsOwnIndex) {
  auto p = recommended();
  auto d = derive(p);  // N = 4, T = 34

  for (uint32_t k = 0; k < p.wave_duration_slots; ++k) {
    EXPECT_TRUE(wave_channel_active(10, 10 - k, p, d)) << "slot " << (10 - k);
  }
  EXPECT_FALSE(wave_channel_active(10, 6, p, d)) << "one slot before the wave starts";
  EXPECT_FALSE(wave_channel_active(10, 11, p, d)) << "the slot after it ends";
}

TEST(WebrcSchedule, ActivityWrapsAroundTheCycle) {
  auto p = recommended();
  auto d = derive(p);  // T = 34
  // Channel 1's wave runs slots 32, 33, 0, 1.
  EXPECT_TRUE(wave_channel_active(1, 32, p, d));
  EXPECT_TRUE(wave_channel_active(1, 33, p, d));
  EXPECT_TRUE(wave_channel_active(1, 0, p, d));
  EXPECT_TRUE(wave_channel_active(1, 1, p, d));
  EXPECT_FALSE(wave_channel_active(1, 31, p, d));
  EXPECT_FALSE(wave_channel_active(1, 2, p, d));
}

/* "N is the duration in time slots for each wave. N is also the number of wave channels active at
   any time." */
TEST(WebrcSchedule, ExactlyNWaveChannelsAreActiveInEverySlot) {
  auto p = recommended();
  auto d = derive(p);
  for (uint32_t slot = 0; slot < d.wave_channels; ++slot) {
    EXPECT_EQ(active_wave_channels(slot, p, d).size(), p.wave_duration_slots) << "slot " << slot;
  }
}

TEST(WebrcSchedule, NoChannelIsActiveOutsideTheWaveChannelRange) {
  auto p = recommended();
  auto d = derive(p);
  EXPECT_FALSE(wave_channel_active(d.wave_channels, 0, p, d)) << "that number is the base channel";
  EXPECT_FALSE(wave_channel_active(d.wave_channels + 5, 0, p, d));
}

/* "The rate at which packets are sent to the base channel starts at BCR_P packets per second at the
   beginning of each time slot and decreases exponentially to P*BCR_P at the end of that time slot." */
TEST(WebrcRates, TheBaseChannelFallsFromBcrToPTimesBcrAcrossASlot) {
  auto p = recommended();
  EXPECT_DOUBLE_EQ(base_channel_rate(0.0, p), p.base_channel_rate_packets);
  EXPECT_DOUBLE_EQ(base_channel_rate(1.0, p),
                   p.rate_drop_per_slot * p.base_channel_rate_packets);
  EXPECT_LT(base_channel_rate(0.5, p), base_channel_rate(0.0, p));
  EXPECT_GT(base_channel_rate(0.5, p), base_channel_rate(1.0, p));
}

/* "the packet rate of the wave MUST decrease exponentially by a factor of P per TSD seconds, down
   to a rate of BCR_P at the end of the last active time slot." */
TEST(WebrcRates, AWaveEndsItsFinalSlotAtTheBaseChannelRate) {
  auto p = recommended();
  EXPECT_DOUBLE_EQ(wave_channel_rate(/*slots_remaining*/ 0, /*end of slot*/ 1.0, p),
                   p.base_channel_rate_packets);
}

TEST(WebrcRates, EachEarlierSlotIsAFactorOfPHigher) {
  auto p = recommended();
  const double final_slot = wave_channel_rate(0, 1.0, p);
  const double one_before = wave_channel_rate(1, 1.0, p);
  const double two_before = wave_channel_rate(2, 1.0, p);
  EXPECT_NEAR(final_slot / one_before, p.rate_drop_per_slot, 1e-12);
  EXPECT_NEAR(one_before / two_before, p.rate_drop_per_slot, 1e-12);
}

TEST(WebrcRates, AWaveNeverIncreasesThroughItsLife) {
  auto p = recommended();
  double previous = 1e18;
  for (uint32_t remaining = p.wave_duration_slots - 1;; --remaining) {
    for (double f : {0.0, 0.25, 0.5, 0.75, 1.0}) {
      const double r = wave_channel_rate(remaining, f, p);
      EXPECT_LE(r, previous) << "remaining " << remaining << " fraction " << f;
      previous = r;
    }
    if (remaining == 0) break;
  }
}

/* The exponential is continuous across a slot boundary: the rate at the end of one active slot is
   the rate at the start of the next. A discontinuity there would be a step in the sending rate that
   the clause's "decrease exponentially by a factor of P per TSD seconds" does not describe. */
TEST(WebrcRates, TheRateIsContinuousAcrossASlotBoundary) {
  auto p = recommended();
  for (uint32_t remaining = p.wave_duration_slots - 1; remaining > 0; --remaining) {
    EXPECT_NEAR(wave_channel_rate(remaining, 1.0, p),
                wave_channel_rate(remaining - 1, 0.0, p), 1e-12)
        << "boundary after the slot with " << remaining << " remaining";
  }
}

/* Strictly decreasing within any one slot, which is what the exponential does between boundaries. */
TEST(WebrcRates, AWaveDecreasesStrictlyWithinASlot) {
  auto p = recommended();
  for (uint32_t remaining = 0; remaining < p.wave_duration_slots; ++remaining) {
    EXPECT_GT(wave_channel_rate(remaining, 0.0, p), wave_channel_rate(remaining, 0.5, p));
    EXPECT_GT(wave_channel_rate(remaining, 0.5, p), wave_channel_rate(remaining, 1.0, p));
  }
}
