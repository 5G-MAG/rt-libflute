// libflute - FLUTE/ALC library
//
// Copyright (C) 2026 5G-MAG
//
// Licensed under the License terms and conditions for use, reproduction, and
// distribution of 5G-MAG software (the "License").  You may not use this file
// except in compliance with the License.  You may obtain a copy of the License at
// https://www.5g-mag.com/reference-tools.  Unless required by applicable law or
// agreed to in writing, software distributed under the License is distributed on
// an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.
//
// See the License for the specific language governing permissions and limitations
// under the License.

#pragma once
#include <cstdint>
#include <limits>
#include <vector>

namespace LibFlute::Webrc {

 /**
  *  Sender-side schedule of the WEBRC building block, RFC 3738.
  *
  *  RFC 5775 clause 2.2 names this as the congestion control building block an ALC implementation
  *  must at minimum support: "At a minimum, implementations of ALC MUST support [RFC3738]". This is
  *  the sender half of it, and only the schedule: which channels exist, which are active in a given
  *  time slot, and at what rate each is sending. The wire encoding is in AlcPacket and the sending
  *  itself in Transmitter.
  *
  *  Nothing here is used by a 3GPP profile. TS 26.346 clause 7.2.4 excludes congestion control for
  *  MBMS download, and the library refuses to enable this under those profiles.
  */

 /** Inputs a sender supplies, with the values RFC 3738 clause 3.1.1 recommends or defaults. */
  struct Parameters {
    /** SR_b, an upper bound on the aggregate transmission rate to all channels, in bits/second. */
    uint64_t sender_rate_bits_per_second = 0;
    /** LENP_B, packet length in bytes. */
    uint32_t packet_length_bytes = 1400;
    /** BCR_P, base channel rate at the start of a time slot, in packets/second. Default 1. */
    double base_channel_rate_packets = 1.0;
    /** TSD, time slot duration in seconds. Recommended 10. */
    double time_slot_duration_seconds = 10.0;
    /** QD, minimum quiescent period in seconds. Recommended 300. */
    double quiescent_duration_seconds = 300.0;
    /** P, the multiplicative drop in every channel rate over a time slot. Default 0.75. */
    double rate_drop_per_slot = 0.75;
    /** N, wave duration in time slots, and the number of wave channels active at any time. */
    uint32_t wave_duration_slots = 4;
  };

 /**
  *  The fixed quantities RFC 3738 clause 3.1.1 derives from those inputs.
  *
  *  Throws std::invalid_argument if the inputs cannot produce a usable schedule, rather than
  *  yielding one that is quietly wrong.
  */
  struct Derived {
    /** SR_P = SR_b/(8*LENP_B), sender rate in packets/second. */
    double sender_rate_packets = 0.0;
    /** BCR_b = 8*LENP_B*BCR_P, base channel rate at slot start, in bits/second. */
    double base_channel_rate_bits = 0.0;
    /** L = ceil(BCR_P*TSD*(P-1)/log(P)), base channel packets per time slot. */
    uint32_t base_packets_per_slot = 0;
    /** Q = ceil(QD/TSD), quiescent time slots per cycle for a wave channel. */
    uint32_t quiescent_slots = 0;
    /** T = N + Q, total time slots in a cycle, and the number of wave channels. */
    uint32_t wave_channels = 0;
  };

  Derived derive(const Parameters& p);

 /**
  *  Channel number carried in the Congestion Control Information.
  *
  *  RFC 3738 clause 3.1.2: "Recall that CN = T for the base channel and CN = 0,1,...,T-1 for the
  *  wave channels."
  */
  constexpr uint8_t base_channel_number(const Derived& d) {
    return static_cast<uint8_t>(d.wave_channels);
  }

 /**
  *  Whether wave channel `cn` is active during time slot `ctsi`.
  *
  *  RFC 3738 clause 3.1.2: "wave channel i is active during time slots i-N+1 modulo T, i-N+2
  *  modulo T, ..., i and is quiescent for time slots i+1 modulo T, i+2 modulo T, ..., i+Q modulo T."
  */
  bool wave_channel_active(uint32_t cn, uint32_t ctsi, const Parameters& p, const Derived& d);

 /** The wave channels active during a time slot, in ascending channel number. */
  std::vector<uint32_t> active_wave_channels(uint32_t ctsi, const Parameters& p, const Derived& d);

 /**
  *  Rate of the base channel at a point within a time slot, in packets/second.
  *
  *  RFC 3738 clause 3.1.2: "The rate at which packets are sent to the base channel starts at BCR_P
  *  packets per second at the beginning of each time slot and decreases exponentially to P*BCR_P at
  *  the end of that time slot."
  *
  *  @param fraction_through_slot 0.0 at the start of the slot, 1.0 at its end.
  */
  double base_channel_rate(double fraction_through_slot, const Parameters& p);

 /**
  *  Rate of a wave channel, in packets/second, given how many active slots of its wave remain.
  *
  *  RFC 3738 clause 3.1.2: "the packet rate of the wave MUST decrease exponentially by a factor of
  *  P per TSD seconds, down to a rate of BCR_P at the end of the last active time slot." So a wave
  *  in its final active slot ends at BCR_P, and each earlier slot is a factor of P higher.
  *
  *  The clause permits the first two slots of a wave to deviate so the aggregate rate stays
  *  constant; that deviation is not implemented, and the exponential is used throughout.
  *
  *  @param slots_remaining 0 during the wave's final active slot, N-1 during its first.
  *  @param fraction_through_slot 0.0 at the start of the slot, 1.0 at its end.
  */
  double wave_channel_rate(uint32_t slots_remaining, double fraction_through_slot,
                           const Parameters& p);

 /**
  *  Receiver-side control loop of WEBRC, RFC 3738 clause 3.2.
  *
  *  Pure logic: it is fed events and produces a decision, and performs no I/O and joins nothing.
  *  That is deliberate. A rate-control loop cannot be validated against itself, and RFC 3738's own
  *  receiver depends on measurements this library has no way to reproduce in a test, so the
  *  decision is exposed for a caller to act on rather than wired into Receiver's joins. Nothing in
  *  the library calls it.
  *
  *  Not for a 3GPP session: TS 26.346 clause 7.2.4 excludes congestion control for MBMS download.
  */
 /** Constants of clause 3.2, at the values the RFC recommends. */
  struct Tuning {
    double nu = 0.3;         //< time-based smoothing, clause 3.2.2.1
    double delta = 0.3;      //< loss-based smoothing, clause 3.2.2.1
    double alpha = 0.25;     //< round-trip variance smoothing, clause 3.2.2.2
    double epoch_seconds = 0.5;   //< EL, clause 3.2.2.1
    double max_reception_rate_packets = 1e9;  //< MRR_P, clause 3.2.1
  };

  class ReceiverController {
   public:
    ReceiverController(const Parameters& p, const Derived& d, Tuning t = Tuning());

   /** "For each packet event (whether it is a received packet or a lost packet), W = W + 1" */
    void on_packet_event();

   /** "At the beginning of each loss event, update W, X, and Y" */
    void on_loss_event_begin();

   /** Clears the in-progress loss event, so a join may again be considered. */
    void on_loss_event_end() { _loss_event = false; };

   /** The end-of-epoch filter of clause 3.2.2.1, which is what updates LOSSP. */
    void on_epoch_end();

   /**
    *  Record the round-trip measurement a join produces, per clause 3.2.2.2.
    *
    *  @param join_to_first_packet_seconds FirstTime - JoinTime.
    *  @param is_base_channel True for the base channel, which sets ARTT directly.
    */
    void on_join_measured(double join_to_first_packet_seconds, bool is_base_channel);

   /** Marks the base channel's first packet as arrived; no join is allowed before it. */
    void on_first_base_packet() { _base_packet_seen = true; };

   /** Reception rates of clause 3.2.2.5, supplied by the caller's own measurement. */
    void set_reception_rates(double average_packets, double target_packets);

   /** Number of wave channels currently joined, NWC. */
    void set_wave_channels_joined(uint32_t nwc) { _nwc = nwc; };
    uint32_t wave_channels_joined() const { return _nwc; };

   /** True while a join is outstanding, which blocks another. */
    void set_joining(bool joining) { _joining = joining; };

    double average_loss_probability() const { return _lossp; };
    double average_round_trip_seconds() const { return _artt; };

   /**
    *  REQN, the TCP throughput equation.
    *
    *  RFC 3738 clause 3.2.2.3: "REQN = 1/(ARTT*sqrt{LOSSP}(0.816 + 7.35*LOSSP*(1+32*LOSSP^2)))"
    */
    double rate_equation() const;

   /**
    *  TRATE, the target rate.
    *
    *  RFC 3738 clause 3.2.2.7: "TRATE = min{max{SSR_P, REQN}, MRR_P}. When SSR_P = infinity, TRATE
    *  is computed as TRATE = min{4*TRR_P, MRR_P}."
    */
    double target_rate() const;

   /**
    *  Whether the next higher layer may be joined, clause 3.2.3.6.
    *
    *  Applies the mandatory refusals of that clause and its rate inequality. The optional checks it
    *  describes are not applied, and are named in the implementation.
    */
    bool may_join_next_layer() const;

   private:
    Parameters _p;
    Derived _d;
    Tuning _t;
    // clause 3.2.2.1
    double _w = 0.0, _x = 0.0, _y = 0.0, _z = 0.0, _lossp = 0.0;
    // clause 3.2.2.2
    double _artt = 0.0, _v = 0.0;
    uint32_t _k = 0;
    // clause 3.2.2.5 and 3.2.2.6
    double _arr = 0.0, _trr = 0.0;
    double _ssr = std::numeric_limits<double>::infinity();
    uint32_t _nwc = 0;
    bool _loss_event = false, _joining = false, _base_packet_seen = false;
  };

}  // namespace LibFlute::Webrc
