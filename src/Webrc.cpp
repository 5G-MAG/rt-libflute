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

#include "Webrc.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace LibFlute::Webrc {

Derived derive(const Parameters& p)
{
  /* Every input is checked rather than trusted, because each one divides or is a logarithm base
     below and a silently wrong schedule is worse than a refusal at setup. */
  if (p.packet_length_bytes == 0) {
    throw std::invalid_argument("WEBRC: packet length must be non-zero");
  }
  if (p.base_channel_rate_packets <= 0.0) {
    throw std::invalid_argument("WEBRC: base channel rate must be positive");
  }
  if (p.time_slot_duration_seconds <= 0.0) {
    throw std::invalid_argument("WEBRC: time slot duration must be positive");
  }
  if (p.quiescent_duration_seconds <= 0.0) {
    throw std::invalid_argument("WEBRC: quiescent duration must be positive");
  }
  /* P is a multiplicative drop over a slot, so it lies strictly between 0 and 1. At exactly 1 the
     log below is zero and L is undefined. */
  if (!(p.rate_drop_per_slot > 0.0 && p.rate_drop_per_slot < 1.0)) {
    throw std::invalid_argument("WEBRC: the rate drop per slot must lie strictly between 0 and 1");
  }
  if (p.wave_duration_slots == 0) {
    throw std::invalid_argument("WEBRC: a wave must last at least one time slot");
  }

  Derived d;
  /* SR_P = SR_b/(8*LENP_B) */
  d.sender_rate_packets =
      static_cast<double>(p.sender_rate_bits_per_second) / (8.0 * p.packet_length_bytes);
  /* BCR_b = 8*LENP_B*BCR_P */
  d.base_channel_rate_bits = 8.0 * p.packet_length_bytes * p.base_channel_rate_packets;
  /* L = ceil(BCR_P*TSD*(P-1)/log(P)). Both (P-1) and log(P) are negative for 0 < P < 1, so L is
     positive. */
  d.base_packets_per_slot = static_cast<uint32_t>(std::ceil(
      p.base_channel_rate_packets * p.time_slot_duration_seconds *
      (p.rate_drop_per_slot - 1.0) / std::log(p.rate_drop_per_slot)));
  if (d.base_packets_per_slot == 0) d.base_packets_per_slot = 1;
  /* Q = ceil(QD/TSD) */
  d.quiescent_slots = static_cast<uint32_t>(
      std::ceil(p.quiescent_duration_seconds / p.time_slot_duration_seconds));
  /* T = N + Q */
  d.wave_channels = p.wave_duration_slots + d.quiescent_slots;

  /* CN is 8 bits in the short-format Congestion Control Information of clause 5.1, and the base
     channel takes the value T, so T itself must be representable. */
  if (d.wave_channels > 255) {
    throw std::invalid_argument(
        "WEBRC: N + Q exceeds the 255 the short-format channel number can carry; shorten the "
        "quiescent period or the wave");
  }
  return d;
}

bool wave_channel_active(uint32_t cn, uint32_t ctsi, const Parameters& p, const Derived& d)
{
  if (cn >= d.wave_channels) return false;
  const uint32_t T = d.wave_channels;
  /* Wave channel i is active during slots i-N+1 .. i, all modulo T. Counting backwards from i
     avoids the negative intermediate that i-N+1 would take in unsigned arithmetic. */
  for (uint32_t k = 0; k < p.wave_duration_slots; ++k) {
    if (((cn + T - k) % T) == ctsi % T) return true;
  }
  return false;
}

std::vector<uint32_t> active_wave_channels(uint32_t ctsi, const Parameters& p, const Derived& d)
{
  std::vector<uint32_t> active;
  for (uint32_t cn = 0; cn < d.wave_channels; ++cn) {
    if (wave_channel_active(cn, ctsi, p, d)) active.push_back(cn);
  }
  return active;
}

double base_channel_rate(double fraction_through_slot, const Parameters& p)
{
  if (fraction_through_slot < 0.0) fraction_through_slot = 0.0;
  if (fraction_through_slot > 1.0) fraction_through_slot = 1.0;
  /* BCR_P at the slot's start falling exponentially to P*BCR_P at its end. */
  return p.base_channel_rate_packets * std::pow(p.rate_drop_per_slot, fraction_through_slot);
}

double wave_channel_rate(uint32_t slots_remaining, double fraction_through_slot,
                         const Parameters& p)
{
  if (fraction_through_slot < 0.0) fraction_through_slot = 0.0;
  if (fraction_through_slot > 1.0) fraction_through_slot = 1.0;
  /* The wave ends its final active slot at BCR_P, and drops by a factor of P per slot, so a slot
     with `slots_remaining` still to run after it starts a factor of P^-(slots_remaining+1) above
     that floor and falls by P across the slot. */
  const double exponent = -(static_cast<double>(slots_remaining) + 1.0) + fraction_through_slot;
  return p.base_channel_rate_packets * std::pow(p.rate_drop_per_slot, exponent);
}

ReceiverController::ReceiverController(const Parameters& p, const Derived& d, Tuning t)
  : _p(p), _d(d), _t(t)
{
  /* SSMINR_P, clause 3.2.2.6: "The recommended value for SSMINR_P is BCR_P*(1+1/P+1/P^2)." Kept as
     the floor a finite slow start threshold is held to; SSR_P itself starts at infinity, as the
     same clause requires. */
  (void)_d;
}

void ReceiverController::on_packet_event()
{
  /* "For each packet event (whether it is a received packet or a lost packet), W = W + 1" */
  _w += 1.0;
}

void ReceiverController::on_loss_event_begin()
{
  /* "At the beginning of each loss event, update W, X, and Y as follows: X = X + W, W = 0,
     Y = Y + 1" */
  _x += _w;
  _w = 0.0;
  _y += 1.0;
  _loss_event = true;
}

void ReceiverController::on_epoch_end()
{
  /* Clause 3.2.2.1, verbatim in structure:
       G = Nu*EL/TSD
       Z = Z*(1-Delta)^(G*Y) + G*X/(G*Y+1)*(1-(1-Delta)^(G*Y+1))
       X = X*(1-G)
       Y = Y*(1-G)
       Z1 = Z*(1-Delta)^Y + X/(Y+1)*(1-(1-Delta)^(Y+1))
       Z2 = Z*(1-Delta)^(Y+1) + (X+W+1)/(Y+2)*(1-(1-Delta)^(Y+2))
       LOSSP = 1/max{Z1,Z2,1} */
  const double G = _t.nu * _t.epoch_seconds / _p.time_slot_duration_seconds;
  const double one_minus_delta = 1.0 - _t.delta;

  _z = _z * std::pow(one_minus_delta, G * _y)
       + G * _x / (G * _y + 1.0) * (1.0 - std::pow(one_minus_delta, G * _y + 1.0));
  _x *= (1.0 - G);
  _y *= (1.0 - G);

  const double z1 = _z * std::pow(one_minus_delta, _y)
                    + _x / (_y + 1.0) * (1.0 - std::pow(one_minus_delta, _y + 1.0));
  const double z2 = _z * std::pow(one_minus_delta, _y + 1.0)
                    + (_x + _w + 1.0) / (_y + 2.0) * (1.0 - std::pow(one_minus_delta, _y + 2.0));

  _lossp = 1.0 / std::max({z1, z2, 1.0});
}

void ReceiverController::on_join_measured(double join_to_first_packet_seconds, bool is_base_channel)
{
  /* Clause 3.2.2.2. The base channel sets ARTT outright; a wave channel contributes an MRTT that is
     filtered in with a weight derived from the variance of previous measurements. */
  if (is_base_channel) {
    _artt = join_to_first_packet_seconds;
    _v = _artt * _artt;
    _k = 0;
    return;
  }

  const double P = _p.rate_drop_per_slot;
  /* "MRTT is set to (FirstTime - JoinTime) - log(1/P)/2/(1-P)/BCR_P * P^NWC" -- which the clause
     notes "can be negative". */
  const double mrtt = join_to_first_packet_seconds
                      - std::log(1.0 / P) / 2.0 / (1.0 - P) / _p.base_channel_rate_packets
                        * std::pow(P, static_cast<double>(_nwc));

  if (_v <= 0.0) { _artt = std::max(mrtt, 0.0); _v = _artt * _artt; _k = 1; return; }

  /* "Let Omega = Alpha*ARTT*ARTT/V, and at the Kth MRTT measurement let Rho =
     Omega/(1-(1-Omega)^(K+1))." */
  const double omega = _t.alpha * _artt * _artt / _v;
  const double denom = 1.0 - std::pow(1.0 - omega, static_cast<double>(_k) + 1.0);
  const double rho = denom > 0.0 ? omega / denom : omega;

  _v = (1.0 - rho) * _v + rho * mrtt * mrtt;
  /* "ARTT is updated to max{P*ARTT,(1-Rho)*ARTT+Rho*MRTT}" -- the floor is what stops a negative
     MRTT dragging ARTT below a fraction of its previous value. */
  _artt = std::max(P * _artt, (1.0 - rho) * _artt + rho * mrtt);
  ++_k;
}

void ReceiverController::set_reception_rates(double average_packets, double target_packets)
{
  _arr = average_packets;
  _trr = target_packets;
}

double ReceiverController::rate_equation() const
{
  /* "REQN = 1/(ARTT*sqrt{LOSSP}(0.816 + 7.35*LOSSP*(1+32*LOSSP^2)))" */
  if (_artt <= 0.0 || _lossp <= 0.0) return std::numeric_limits<double>::infinity();
  const double l = _lossp;
  return 1.0 / (_artt * std::sqrt(l) * (0.816 + 7.35 * l * (1.0 + 32.0 * l * l)));
}

double ReceiverController::target_rate() const
{
  /* "TRATE = min{max{SSR_P, REQN}, MRR_P}. When SSR_P = infinity, TRATE is computed as
     TRATE = min{4*TRR_P, MRR_P}." */
  if (std::isinf(_ssr)) return std::min(4.0 * _trr, _t.max_reception_rate_packets);
  return std::min(std::max(_ssr, rate_equation()), _t.max_reception_rate_packets);
}

bool ReceiverController::may_join_next_layer() const
{
  /* Clause 3.2.3.6's mandatory refusals, in the order it gives them. */
  if (!_base_packet_seen) return false;   // "the first base channel packet has not yet arrived"
  if (_loss_event) return false;          // "there is a loss event in progress"
  if (_joining) return false;             // "a join of a channel is in progress"
  if (_nwc >= _p.wave_duration_slots) return false;  // "If NWC = N the receiver MUST not join."

  /* "If the sender is not sending at constant aggregate rate and
     TRATE < ARR_P*((1/P)^{NWC+2}-1)/((1/P)^{NWC+1}-1), the receiver MUST not join."

     The constant-aggregate-rate variant, which adds a second condition on SR_P, is not
     distinguished here: this sender does not signal which mode it is in, so the stricter of the two
     is applied. */
  const double inv_p = 1.0 / _p.rate_drop_per_slot;
  const double numerator = std::pow(inv_p, static_cast<double>(_nwc) + 2.0) - 1.0;
  const double denominator = std::pow(inv_p, static_cast<double>(_nwc) + 1.0) - 1.0;
  if (denominator <= 0.0) return false;
  if (target_rate() < _arr * numerator / denominator) return false;

  /* The optional checks of the same clause, on RR_P against its maximum since the last join and on
     TRR_P being greatly below ARR_P, are not applied. Both are RECOMMENDED rather than required,
     and both need measurements this class is not given. */
  return true;
}

}  // namespace LibFlute::Webrc
