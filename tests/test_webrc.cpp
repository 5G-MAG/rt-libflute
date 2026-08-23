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

#include <boost/asio.hpp>

#include "AlcPacket.h"
#include "Transmitter.h"
#include "EncodingSymbol.h"
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


/* RFC 3738 clause 5.1 lays the short-format Congestion Control Information out as CTSI (8 bits),
   Channel Number (8) and Packet Sequence Number (16). The CCI sits immediately after the first
   32-bit LCT header word, which is where C=0 puts it. */
namespace {

std::vector<LibFlute::EncodingSymbol> one_symbol(const char* data, size_t len) {
  return {LibFlute::EncodingSymbol(0, 0, const_cast<char*>(data), len,
                                   LibFlute::FecScheme::CompactNoCode)};
}

LibFlute::FecOti plain_oti() {
  LibFlute::FecOti oti{};
  oti.encoding_id = LibFlute::FecScheme::CompactNoCode;
  oti.encoding_symbol_length = 1400;
  oti.max_source_block_length = 64;
  return oti;
}

}  // namespace

TEST(WebrcCci, AbsentLeavesTheFieldZero) {
  const char payload[] = "no congestion control";
  LibFlute::AlcPacket pkt(/*tsi*/ 1, /*toi*/ 3, plain_oti(), one_symbol(payload, sizeof payload),
                          1400, /*fdt_instance_id*/ 0);
  const auto* b = reinterpret_cast<const unsigned char*>(pkt.data());
  EXPECT_EQ(b[4], 0); EXPECT_EQ(b[5], 0); EXPECT_EQ(b[6], 0); EXPECT_EQ(b[7], 0);
}

TEST(WebrcCci, PresentIsEncodedInFieldOrder) {
  const char payload[] = "with congestion control";
  LibFlute::CongestionControlInfo cci;
  cci.current_time_slot_index = 17;
  cci.channel_number = 34;             // the base channel, T, for a 34 wave-channel session
  cci.packet_sequence_number = 0xBEEF;

  LibFlute::AlcPacket pkt(/*tsi*/ 1, /*toi*/ 3, plain_oti(), one_symbol(payload, sizeof payload),
                          1400, /*fdt_instance_id*/ 0, false, false, cci);
  const auto* b = reinterpret_cast<const unsigned char*>(pkt.data());
  EXPECT_EQ(b[4], 17)   << "CTSI";
  EXPECT_EQ(b[5], 34)   << "CN";
  EXPECT_EQ(b[6], 0xBE) << "PSN high byte, network order";
  EXPECT_EQ(b[7], 0xEF) << "PSN low byte";
}

/* "the last packet sent to the channel before the channel goes quiescent with PSN = 2^16-1" */
TEST(WebrcCci, TheFinalPsnOfAWaveIsRepresentable) {
  const char payload[] = "last of the wave";
  LibFlute::CongestionControlInfo cci;
  cci.packet_sequence_number = 65535;
  LibFlute::AlcPacket pkt(/*tsi*/ 1, /*toi*/ 3, plain_oti(), one_symbol(payload, sizeof payload),
                          1400, /*fdt_instance_id*/ 0, false, false, cci);
  const auto* b = reinterpret_cast<const unsigned char*>(pkt.data());
  EXPECT_EQ(b[6], 0xFF);
  EXPECT_EQ(b[7], 0xFF);
}


/* Wiring the schedule to a real Transmitter. */
namespace {

std::vector<std::pair<std::string, unsigned short>> wave_addresses(uint32_t count) {
  std::vector<std::pair<std::string, unsigned short>> v;
  for (uint32_t i = 0; i < count; ++i) {
    v.emplace_back("239.30." + std::to_string(i / 250) + "." + std::to_string(1 + i % 250),
                   static_cast<unsigned short>(6000 + i));
  }
  return v;
}

std::unique_ptr<LibFlute::Transmitter> unprofiled_tx(boost::asio::io_context& io, const char* addr) {
  return std::make_unique<LibFlute::Transmitter>(
      addr, 5000, /*tsi*/ 1, /*mtu*/ 1400, /*rate_limit*/ 0, io, std::nullopt,
      LibFlute::FileDeliveryTable::FDT_NS_NONE, /*active*/ false, std::nullopt,
      LibFlute::Profile::Unprofiled);
}

}  // namespace

/* TS 26.346 V18.2.0 clause 7.2.4: "For simplicity of congestion control, FLUTE channelization shall
   be provided by a single FLUTE channel with single rate transport." */
TEST(WebrcTransmitter, RefusedUnderThe3gppProfiles) {
  boost::asio::io_context io;
  LibFlute::Transmitter tx("239.31.0.1", 5000, 1, 1400, 0, io, std::nullopt,
                           LibFlute::FileDeliveryTable::FDT_NS_NONE, false, std::nullopt,
                           LibFlute::Profile::Ts26517);
  auto d = derive(recommended());
  EXPECT_THROW(tx.enable_webrc(recommended(), wave_addresses(d.wave_channels)),
               std::runtime_error);
  EXPECT_FALSE(tx.webrc_enabled());
}

TEST(WebrcTransmitter, EnablingOpensOneChannelPerWaveChannel) {
  boost::asio::io_context io;
  auto tx = unprofiled_tx(io, "239.31.0.2");
  auto d = derive(recommended());

  tx->enable_webrc(recommended(), wave_addresses(d.wave_channels));
  EXPECT_TRUE(tx->webrc_enabled());
  // The session's own destination is the base channel, so T wave channels plus it.
  EXPECT_EQ(tx->channel_count(), d.wave_channels + 1);
}

TEST(WebrcTransmitter, TheWrongNumberOfAddressesIsRefused) {
  boost::asio::io_context io;
  auto tx = unprofiled_tx(io, "239.31.0.3");
  auto d = derive(recommended());
  EXPECT_THROW(tx->enable_webrc(recommended(), wave_addresses(d.wave_channels - 1)),
               std::runtime_error);
  EXPECT_FALSE(tx->webrc_enabled());
}

/* "CN for the base channel is T, and the CNs for the wave channels are 0 through T-1." */
TEST(WebrcTransmitter, ChannelZeroIsTheBaseChannelAndTakesCnEqualToT) {
  boost::asio::io_context io;
  auto tx = unprofiled_tx(io, "239.31.0.4");
  auto d = derive(recommended());
  tx->enable_webrc(recommended(), wave_addresses(d.wave_channels));

  auto base = tx->webrc_cci_for(0);
  ASSERT_TRUE(base.has_value());
  EXPECT_EQ(base->channel_number, d.wave_channels) << "the base channel takes CN = T";

  auto first_wave = tx->webrc_cci_for(1);
  ASSERT_TRUE(first_wave.has_value());
  EXPECT_EQ(first_wave->channel_number, 0) << "added channel 1 is wave channel 0";

  auto last_wave = tx->webrc_cci_for(d.wave_channels);
  ASSERT_TRUE(last_wave.has_value());
  EXPECT_EQ(last_wave->channel_number, d.wave_channels - 1);
}

TEST(WebrcTransmitter, NoCciBeforeTheBuildingBlockIsEnabled) {
  boost::asio::io_context io;
  auto tx = unprofiled_tx(io, "239.31.0.5");
  EXPECT_FALSE(tx->webrc_cci_for(0).has_value());
}


/* The receiver control loop of RFC 3738 clause 3.2. Pure logic, driven by events, producing a
   decision. Nothing in the library calls it; these check the formulas, not a running session. */
namespace {

ReceiverController fresh_controller() {
  auto p = recommended();
  return ReceiverController(p, derive(p));
}

}  // namespace

/* "For each packet event (whether it is a received packet or a lost packet), W = W + 1" and
   "At the beginning of each loss event, update W, X, and Y". With no loss at all, LOSSP stays at
   its floor, because max{Z1,Z2,1} keeps the reciprocal at or below 1. */
TEST(WebrcReceiver, LossProbabilityStaysBoundedWithoutLossEvents) {
  auto rc = fresh_controller();
  for (int i = 0; i < 1000; ++i) rc.on_packet_event();
  rc.on_epoch_end();
  EXPECT_GT(rc.average_loss_probability(), 0.0);
  EXPECT_LE(rc.average_loss_probability(), 1.0);
}

TEST(WebrcReceiver, LossEventsRaiseTheLossProbability) {
  auto rc = fresh_controller();
  for (int i = 0; i < 200; ++i) rc.on_packet_event();
  rc.on_epoch_end();
  const double quiet = rc.average_loss_probability();

  for (int e = 0; e < 20; ++e) {
    for (int i = 0; i < 5; ++i) rc.on_packet_event();
    rc.on_loss_event_begin();
    rc.on_loss_event_end();
    rc.on_epoch_end();
  }
  EXPECT_GT(rc.average_loss_probability(), quiet)
      << "frequent loss events must raise the estimate";
}

/* "REQN = 1/(ARTT*sqrt{LOSSP}(0.816 + 7.35*LOSSP*(1+32*LOSSP^2)))" */
TEST(WebrcReceiver, TheRateEquationMatchesTheClause) {
  auto rc = fresh_controller();
  rc.on_join_measured(0.100, /*is_base_channel*/ true);   // ARTT = 100 ms
  for (int i = 0; i < 50; ++i) { rc.on_packet_event(); }
  rc.on_loss_event_begin(); rc.on_loss_event_end();
  rc.on_epoch_end();

  const double artt = rc.average_round_trip_seconds();
  const double l = rc.average_loss_probability();
  const double expected = 1.0 / (artt * std::sqrt(l) * (0.816 + 7.35 * l * (1.0 + 32.0 * l * l)));
  EXPECT_NEAR(rc.rate_equation(), expected, expected * 1e-12);
}

/* "If it is the base channel that has been joined, ARTT is set to FirstTime-JoinTime" */
TEST(WebrcReceiver, TheBaseChannelSetsTheRoundTripDirectly) {
  auto rc = fresh_controller();
  rc.on_join_measured(0.250, true);
  EXPECT_DOUBLE_EQ(rc.average_round_trip_seconds(), 0.250);
}

/* "ARTT is updated to max{P*ARTT,(1-Rho)*ARTT+Rho*MRTT}" -- the floor is what keeps a negative
   MRTT, which the clause says can happen, from dragging ARTT down without limit. */
TEST(WebrcReceiver, ANegativeMeasurementCannotDragTheRoundTripBelowItsFloor) {
  auto p = recommended();
  ReceiverController rc(p, derive(p));
  rc.on_join_measured(0.200, true);
  const double before = rc.average_round_trip_seconds();
  rc.on_join_measured(-5.0, /*wave channel*/ false);
  EXPECT_GE(rc.average_round_trip_seconds(), p.rate_drop_per_slot * before);
}

/* "When SSR_P = infinity, TRATE is computed as TRATE = min{4*TRR_P, MRR_P}." SSR_P starts at
   infinity, per clause 3.2.2.6. */
TEST(WebrcReceiver, TargetRateDuringStartUpIsFourTimesTheTargetReceptionRate) {
  auto p = recommended();
  Tuning t; t.max_reception_rate_packets = 1e9;
  ReceiverController rc(p, derive(p), t);
  rc.set_reception_rates(/*ARR_P*/ 10.0, /*TRR_P*/ 25.0);
  EXPECT_DOUBLE_EQ(rc.target_rate(), 100.0);
}

TEST(WebrcReceiver, TargetRateIsCappedByTheMaximumReceptionRate) {
  auto p = recommended();
  Tuning t; t.max_reception_rate_packets = 40.0;
  ReceiverController rc(p, derive(p), t);
  rc.set_reception_rates(10.0, 25.0);
  EXPECT_DOUBLE_EQ(rc.target_rate(), 40.0) << "MRR_P caps it";
}

/* Clause 3.2.3.6's mandatory refusals. */
TEST(WebrcReceiver, NoJoinBeforeTheFirstBaseChannelPacket) {
  auto rc = fresh_controller();
  rc.set_reception_rates(1.0, 1000.0);
  EXPECT_FALSE(rc.may_join_next_layer());
}

TEST(WebrcReceiver, NoJoinDuringALossEventOrAnOutstandingJoin) {
  auto rc = fresh_controller();
  rc.on_first_base_packet();
  rc.set_reception_rates(1.0, 1000.0);
  ASSERT_TRUE(rc.may_join_next_layer()) << "otherwise the refusals below prove nothing";

  rc.on_loss_event_begin();
  EXPECT_FALSE(rc.may_join_next_layer()) << "loss event in progress";
  rc.on_loss_event_end();

  rc.set_joining(true);
  EXPECT_FALSE(rc.may_join_next_layer()) << "join in progress";
  rc.set_joining(false);
  EXPECT_TRUE(rc.may_join_next_layer());
}

/* "If NWC = N the receiver MUST not join." */
TEST(WebrcReceiver, NoJoinOnceEveryActiveWaveIsJoined) {
  auto p = recommended();
  ReceiverController rc(p, derive(p));
  rc.on_first_base_packet();
  rc.set_reception_rates(1.0, 1000.0);
  rc.set_wave_channels_joined(p.wave_duration_slots);
  EXPECT_FALSE(rc.may_join_next_layer());
}

/* "If ... TRATE < ARR_P*((1/P)^{NWC+2}-1)/((1/P)^{NWC+1}-1), the receiver MUST not join." */
TEST(WebrcReceiver, NoJoinWhenTheTargetRateIsBelowTheThreshold) {
  auto p = recommended();
  ReceiverController rc(p, derive(p));
  rc.on_first_base_packet();
  // A large average reception rate puts the threshold above any target rate.
  rc.set_reception_rates(/*ARR_P*/ 1e6, /*TRR_P*/ 1.0);
  EXPECT_FALSE(rc.may_join_next_layer());

  // A target rate well above the threshold permits it.
  rc.set_reception_rates(/*ARR_P*/ 1.0, /*TRR_P*/ 1e6);
  EXPECT_TRUE(rc.may_join_next_layer());
}


/* RFC 3926 clause 4, recommendation 1: "The layers to which packets for FDT Instances are sent
   SHOULD NOT be biased towards those layers to which lower rate receivers are not joined."

   A
   receiver holds no object until it holds the FDT describing it, so an FDT spread over wave
   channels denies the session to anyone who has not climbed to those layers. The FDT therefore goes
   to the base channel only, which is the case the clause explicitly calls ok. */
TEST(WebrcTransmitter, TheFdtGoesOnlyToTheBaseChannel) {
  boost::asio::io_context io;
  auto tx = unprofiled_tx(io, "239.31.0.6");
  auto d = derive(recommended());
  tx->enable_webrc(recommended(), wave_addresses(d.wave_channels));

  /* The base channel's Congestion Control Information is what an FDT packet must carry, whatever
     the round-robin over content channels has reached. */
  auto base = tx->webrc_cci_for(0);
  ASSERT_TRUE(base.has_value());
  EXPECT_EQ(base->channel_number, d.wave_channels)
      << "the FDT rides the base channel, which takes CN = T";
}


/* RFC 3450 clause 4.4: "The ALC sender MUST obey the rules for filling in the CCI field in the
   packet headers and MUST send packets at the appropriate rates to the channels associated with the
   session as dictated by the multiple rate congestion control building block."

   The credit scheme the sender uses to honour that is checked here directly on the rates, since the
   rates are what decide the shares: a wave with more active slots left is sending faster than one
   about to end, and both are above the base channel, so packets must be biased towards the younger
   wave. */
TEST(WebrcRates, AYoungerWaveOutrunsAnOlderOneAndBothOutrunTheBase) {
  auto p = recommended();
  const double youngest = wave_channel_rate(p.wave_duration_slots - 1, 0.0, p);
  const double oldest = wave_channel_rate(0, 0.0, p);
  const double base = base_channel_rate(0.0, p);

  EXPECT_GT(youngest, oldest) << "a wave with more slots to run sends faster";
  EXPECT_GT(oldest, base) << "even a wave in its last slot starts above the base channel";

  /* Over one slot the shares are the ratio of the rates, which is what the credit scheme converges
     on. A wave one slot younger than another is exactly 1/P times its rate. */
  EXPECT_NEAR(wave_channel_rate(1, 0.0, p) / wave_channel_rate(0, 0.0, p),
              1.0 / p.rate_drop_per_slot, 1e-12);
}


/* Slow start, RFC 3738 clause 3.2.2.6 and clause 3.2.3.4. SSR_P begins infinite and becomes finite
   at the first event that ends the start-up period. */
TEST(WebrcReceiver, StartUpBeginsWithAnInfiniteThreshold) {
  auto rc = fresh_controller();
  EXPECT_TRUE(rc.in_start_up());
  EXPECT_TRUE(std::isinf(rc.slow_start_threshold()));
}

/* "The recommended value for SSMINR_P is BCR_P*(1+1/P+1/P^2)." */
TEST(WebrcReceiver, TheSlowStartFloorMatchesTheClause) {
  auto p = recommended();
  ReceiverController rc(p, derive(p));
  const double inv = 1.0 / p.rate_drop_per_slot;
  EXPECT_NEAR(rc.slow_start_floor(),
              p.base_channel_rate_packets * (1.0 + inv + inv * inv), 1e-12);
}

/* "When a start of a loss event is detected, the value of SSR_P is updated to
   max{SSMINR_P, P*TRR_P}." */
TEST(WebrcReceiver, ALossEventEndsStartUpAndSetsTheThreshold) {
  auto p = recommended();
  ReceiverController rc(p, derive(p));
  rc.set_reception_rates(/*ARR_P*/ 50.0, /*TRR_P*/ 40.0);
  ASSERT_TRUE(rc.in_start_up());

  rc.on_loss_event_begin();
  EXPECT_FALSE(rc.in_start_up());
  EXPECT_NEAR(rc.slow_start_threshold(),
              std::max(rc.slow_start_floor(), p.rate_drop_per_slot * 40.0), 1e-12);
}

TEST(WebrcReceiver, TheThresholdNeverFallsBelowItsFloor) {
  auto p = recommended();
  ReceiverController rc(p, derive(p));
  rc.set_reception_rates(0.0, 0.0);   // P*TRR_P would be zero
  rc.on_loss_event_begin();
  EXPECT_NEAR(rc.slow_start_threshold(), rc.slow_start_floor(), 1e-12);
}

/* "When SSR_P = infinity, if (P^(-NWC-2)-1)/(P^(-NWC-1)-1)*ARR_P exceeds MRR_P or SR_P, the receiver
   MUST set SSR_P to max{SSMINR_P, TRR_P}." */
TEST(WebrcReceiver, ReachingTheMaximumReceptionRateEndsStartUp) {
  auto p = recommended();
  Tuning t; t.max_reception_rate_packets = 1.0;   // any real rate exceeds this
  ReceiverController rc(p, derive(p), t);
  rc.set_reception_rates(/*ARR_P*/ 100.0, /*TRR_P*/ 90.0);
  ASSERT_TRUE(rc.in_start_up());

  rc.on_epoch_end();
  EXPECT_FALSE(rc.in_start_up());
  EXPECT_NEAR(rc.slow_start_threshold(), std::max(rc.slow_start_floor(), 90.0), 1e-12);
}

/* Once the threshold is finite the target rate follows the other branch of clause 3.2.2.7. */
TEST(WebrcReceiver, TargetRateSwitchesBranchWhenStartUpEnds) {
  auto p = recommended();
  Tuning t; t.max_reception_rate_packets = 1e9;
  ReceiverController rc(p, derive(p), t);
  rc.set_reception_rates(50.0, 40.0);
  EXPECT_DOUBLE_EQ(rc.target_rate(), 160.0) << "4*TRR_P while SSR_P is infinite";

  rc.on_loss_event_begin();
  EXPECT_NE(rc.target_rate(), 160.0) << "now max{SSR_P, REQN} capped by MRR_P";
}

/* ------------------------------------------------------------------------------------------- */
/* The receiver side: a session with no congestion control is refused, and a received packet's
   Congestion Control Information is read back in WEBRC's short format.                         */

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <boost/asio.hpp>

#include "AlcPacket.h"
#include "Receiver.h"

using namespace std::chrono_literals;

namespace {
  LibFlute::Webrc::SessionChannels session_channels() {
    LibFlute::Webrc::SessionChannels sc;
    sc.params = recommended();
    const auto d = derive(sc.params);
    for (uint32_t i = 0; i < d.wave_channels; i++) {
      sc.wave_channels.emplace_back("232.0.1." + std::to_string(i + 1), 40085);
    }
    return sc;
  }
}

/* RFC 3450 clause 4.5: "If a receiver is not able to implement the multiple rate congestion control
   building block it MUST NOT join the session." */
TEST(WebrcReceiverWiring, UnprofiledSessionIsRefusedWithoutCongestionControl) {
  boost::asio::io_context io;
  EXPECT_THROW(
      LibFlute::Receiver("0.0.0.0", "232.0.0.1", 40085, 0, io, "", LibFlute::Profile::Unprofiled),
      std::runtime_error);
}

/* The count of wave channel addresses is not free: T comes out of the parameters. */
TEST(WebrcReceiverWiring, WaveChannelAddressesMustMatchTheDerivedCount) {
  boost::asio::io_context io;
  auto sc = session_channels();
  sc.wave_channels.pop_back();
  EXPECT_THROW(
      LibFlute::Receiver("0.0.0.0", "232.0.0.1", 40085, 0, io, "", LibFlute::Profile::Unprofiled,
                         sc),
      std::runtime_error);
}

/* RFC 3738 clause 5.1 gives the short format the C=0 header selects: one byte of CTSI, one byte of
   channel number, two bytes of packet sequence number. */
TEST(WebrcReceiverWiring, ShortFormatCongestionControlInformationIsReadBack) {
  /* An LCT header with V=1, C=0, a 32-bit TSI and a 32-bit TOI, so the CCI sits where the short
     format puts it: immediately after the fixed four bytes. */
  unsigned char pkt[24] = {0};
  pkt[0] = 0x10;   // V=1, C=0, PSI=0
  pkt[1] = 0xa0;   // S=1 (32-bit TSI), O=1 (32-bit TOI), no half-word, no SCT/ERT, no Close flags
  pkt[2] = 0x04;   // LCT header length, in 32-bit words: fixed, CCI, TSI, TOI
  pkt[3] = 0x00;   // codepoint
  pkt[4] = 0x2a;   // CTSI
  pkt[5] = 0x05;   // channel number
  pkt[6] = 0x01;   // PSN, high byte
  pkt[7] = 0x02;   // PSN, low byte
  pkt[11] = 0x01;  // TSI
  pkt[15] = 0x00;  // TOI, the FDT

  LibFlute::AlcPacket alc(reinterpret_cast<char*>(pkt), sizeof(pkt));
  const auto cci = alc.congestion_control_info();
  ASSERT_TRUE(cci.has_value());
  EXPECT_EQ(cci->current_time_slot_index, 0x2a);
  EXPECT_EQ(cci->channel_number, 0x05);
  EXPECT_EQ(cci->packet_sequence_number, 0x0102);
}

/* Step 2 of the receiver procedure disposes of a packet whose session does not match, and step 3,
   the congestion control step, comes after it.
   RFC 3450 clause 4.5: "If there is not a match then the packet MUST be discarded without further
   processing."
   A foreign session's packets on the same group must therefore leave this session's rate alone. */
TEST(WebrcReceiverWiring, AForeignSessionsPacketsDoNotDriveCongestionControl) {
  boost::asio::io_context io;
  auto sc = session_channels();
  LibFlute::Receiver rx("0.0.0.0", "239.9.9.21", 19601, /*tsi*/ 4242, io,
                        "", LibFlute::Profile::Unprofiled, sc);
  std::thread io_thread([&io]() { io.run(); });

  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(sock, 0);
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(19601);
  inet_pton(AF_INET, "239.9.9.21", &dst.sin_addr);

  /* Twenty packets on this group, all carrying a different session's TSI and a sequence number
     jumping by more than one, which is what the loss estimator reads as a loss event. */
  for (uint16_t i = 0; i < 20; i++) {
    unsigned char pkt[24] = {0};
    pkt[0] = 0x10;
    pkt[1] = 0xa0;
    pkt[2] = 0x04;
    pkt[5] = 0x00;                                        // channel number
    pkt[6] = static_cast<unsigned char>((i * 7) >> 8);    // PSN, gapped on purpose
    pkt[7] = static_cast<unsigned char>((i * 7) & 0xFF);
    pkt[11] = 0x63;                                       // TSI 99, not this session's 4242
    ASSERT_EQ(sendto(sock, pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)),
              static_cast<ssize_t>(sizeof(pkt)));
  }
  ::close(sock);
  std::this_thread::sleep_for(150ms);

  EXPECT_EQ(rx.webrc_packets_noted(), 0u)
      << "a foreign session's CCI reached this session's congestion control loop";

  /* The same packets carrying this session's own TSI do reach it, so the counter is measuring the
     session check and not simply never incrementing. */
  int sock2 = socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(sock2, 0);
  for (uint16_t i = 0; i < 5; i++) {
    unsigned char pkt[24] = {0};
    pkt[0] = 0x10;
    pkt[1] = 0xa0;
    pkt[2] = 0x04;
    pkt[7] = static_cast<unsigned char>(i);
    pkt[10] = 0x10;                                       // TSI 4242 = 0x1092
    pkt[11] = 0x92;
    ASSERT_EQ(sendto(sock2, pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)),
              static_cast<ssize_t>(sizeof(pkt)));
  }
  ::close(sock2);
  std::this_thread::sleep_for(150ms);

  EXPECT_EQ(rx.webrc_packets_noted(), 5u) << "this session's own CCI did not reach the loop";

  rx.stop();
  io.stop();
  io_thread.join();
}
