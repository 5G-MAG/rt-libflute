// libflute - FLUTE/ALC library
//
// Copyright (C) 2026 5G-MAG Association (Jordi J. Gimenez <gimenez@5g-mag.com>)
//
// Licensed under the License terms and conditions for use, reproduction, and
// distribution of 5G-MAG software (the “License”).  You may not use this file
// except in compliance with the License.  You may obtain a copy of the License at
// https://www.5g-mag.com/reference-tools.  Unless required by applicable law or
// agreed to in writing, software distributed under the License is distributed on
// an “AS IS” BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.
//
// See the License for the specific language governing permissions and limitations
// under the License.
//
#pragma once
#include <cstdint>
#include "fec/RaptorQTables.h"

// GF(256) octet arithmetic, RFC 6330 Section 5.7.2, transcribed directly
// from the RFC text. Addition/subtraction is XOR (free); multiplication and
// division go through the OCT_EXP/OCT_LOG tables in RaptorQTables.h.

namespace LibFlute {
namespace RaptorQ {

inline uint8_t gf_add(uint8_t u, uint8_t v) { return u ^ v; }
inline uint8_t gf_sub(uint8_t u, uint8_t v) { return u ^ v; }

// u * v = 0 if either is 0, else OCT_EXP[OCT_LOG[u] + OCT_LOG[v]].
// OCT_LOG entries are <= 254, so the sum is <= 508 -- within OCT_EXP's
// 510-entry range (that's exactly why the table has 510, not 255, entries).
inline uint8_t gf_mul(uint8_t u, uint8_t v) {
  if (u == 0 || v == 0) return 0;
  return kOctExp[kOctLog[u - 1] + kOctLog[v - 1]];
}

// u / v (v != 0) = 0 if u == 0, else OCT_EXP[OCT_LOG[u] - OCT_LOG[v] + 255].
inline uint8_t gf_div(uint8_t u, uint8_t v) {
  if (u == 0) return 0;
  return kOctExp[kOctLog[u - 1] - kOctLog[v - 1] + 255];
}

// Multiplicative inverse of a non-zero octet: OCT_EXP[255 - OCT_LOG[u]].
inline uint8_t gf_inv(uint8_t u) {
  return kOctExp[255 - kOctLog[u - 1]];
}

// alpha^^i for 0 <= i < 256, where alpha is the octet 2.
inline uint8_t gf_alpha_pow(uint32_t i) {
  return kOctExp[i];
}

} // namespace RaptorQ
} // namespace LibFlute
