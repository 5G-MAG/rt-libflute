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

#include <cmath>
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

}  // namespace LibFlute::Webrc
