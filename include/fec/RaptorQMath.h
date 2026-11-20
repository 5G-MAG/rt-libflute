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
#include <cstddef>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include "fec/RaptorQTables.h"
#include "fec/RaptorTables.h" // shares Raptor's kRandTableV0/V1 -- see RaptorQTables.h

// The scalar building blocks of RFC 6330 ("RaptorQ Forward Error Correction
// Scheme for Object Delivery"): the 4-way Rand(), the degree generator, the
// six-element tuple generator, and source block parameter derivation.
// Transcribed directly from the RFC text, section numbers cited throughout.

namespace LibFlute {
namespace RaptorQ {

/// RFC 6330 Section 5.3.5.1: Rand[y, i, m] using all four V-tables.
inline uint32_t Rand(uint32_t y, uint32_t i, uint32_t m) {
  uint32_t x0 = (y + i) % 256;
  uint32_t x1 = (y / 256 + i) % 256;
  uint32_t x2 = (y / 65536 + i) % 256;
  uint32_t x3 = (y / 16777216 + i) % 256;
  uint32_t v = LibFlute::Raptor::kRandTableV0[x0] ^ LibFlute::Raptor::kRandTableV1[x1] ^
               kRandTableV2[x2] ^ kRandTableV3[x3];
  return v % m;
}

inline uint32_t smallest_prime_at_least(uint32_t n) {
  if (n <= 2) return 2;
  auto is_prime = [](uint32_t v) {
    if (v < 2) return false;
    if (v % 2 == 0) return v == 2;
    for (uint32_t d = 3; (uint64_t)d * d <= v; d += 2) {
      if (v % d == 0) return false;
    }
    return true;
  };
  uint32_t v = (n % 2 == 0) ? n + 1 : n;
  while (!is_prime(v)) v += 2;
  return v;
}

/// Parameters derived from K' (RFC 6330 Section 5.3.3.3), via a lookup into
/// Table 2 (Section 5.6) for J/S/H/W, then straightforward arithmetic for
/// the rest.
struct PreCodeParams {
  uint32_t Kprime = 0;
  uint32_t J = 0, S = 0, H = 0, W = 0;
  uint32_t L = 0, P = 0, P1 = 0, U = 0, B = 0;
};

/// Looks up the smallest supported K' >= k (RFC 6330 Section 5.3.2: "K' MUST
/// be selected as the smallest value of K' from the table of Section 5.6
/// that is greater than or equal to K").
inline uint32_t smallest_supported_Kprime(uint32_t k) {
  size_t n = sizeof(kTable2_Kprime) / sizeof(kTable2_Kprime[0]);
  auto it = std::lower_bound(kTable2_Kprime, kTable2_Kprime + n, k);
  if (it == kTable2_Kprime + n) {
    throw std::invalid_argument("RaptorQ: K exceeds the largest supported K' (56403)");
  }
  return *it;
}

inline PreCodeParams derive_precode_params(uint32_t Kprime) {
  size_t n = sizeof(kTable2_Kprime) / sizeof(kTable2_Kprime[0]);
  auto it = std::lower_bound(kTable2_Kprime, kTable2_Kprime + n, Kprime);
  if (it == kTable2_Kprime + n || *it != Kprime) {
    throw std::invalid_argument("RaptorQ: K' is not one of Table 2's supported values -- "
                                 "call smallest_supported_Kprime(K) first");
  }
  size_t row = it - kTable2_Kprime;

  PreCodeParams p;
  p.Kprime = Kprime;
  p.J = kTable2_J[row];
  p.S = kTable2_S[row];
  p.H = kTable2_H[row];
  p.W = kTable2_W[row];
  p.L = Kprime + p.S + p.H;
  p.P = p.L - p.W;
  p.P1 = smallest_prime_at_least(p.P);
  p.U = p.P - p.H;
  p.B = p.W - p.S;
  return p;
}

/// RFC 6330 Table 1 / Section 5.3.5.2: degree generator. v is drawn from
/// [0, 2^20); W bounds the maximum usable degree (min(d, W-2)).
inline uint32_t Deg(uint32_t v, uint32_t W) {
  uint32_t d = 0;
  for (uint32_t j = 1; j < 31; j++) {
    if (v < kDegreeTableF[j]) { d = j; break; }
  }
  return std::min(d, W - 2);
}

/// One source/repair tuple, as produced by the Tuple[] generator.
struct Tuple {
  uint32_t d, a, b;    // LT part: degree, step, start column (in [0, W))
  uint32_t d1, a1, b1; // PI part: degree, step, start column (in [0, P))
};

/// RFC 6330 Section 5.3.5.4. Tuple[K', X] -- note the RFC's own text in
/// Section 5.3.3.2 writes this as "Tuple[K, X]" (unprimed K), which is
/// inconsistent with the generator's own formal signature "Tuple[K', X]"
/// (Section 5.3.5.4's own heading and parameter list) and with how Section
/// 5.3.3.4.1 invokes it. This implementation follows the generator's formal
/// definition and always passes K' (the extended/padded source symbol
/// count), matching every other use of Tuple[] in the RFC.
inline Tuple tuple_generator(uint32_t X, const PreCodeParams& p) {
  uint64_t A = 53591ULL + (uint64_t)p.J * 997ULL;
  if (A % 2 == 0) A += 1;
  uint64_t B = 10267ULL * (p.J + 1ULL);
  uint64_t y = (B + (uint64_t)X * A) % (1ULL << 32);
  uint32_t v = Rand((uint32_t)y, 0, 1u << 20);

  Tuple t;
  t.d = Deg(v, p.W);
  t.a = 1 + Rand((uint32_t)y, 1, p.W - 1);
  t.b = Rand((uint32_t)y, 2, p.W);
  t.d1 = (t.d < 4) ? (2 + Rand(X, 3, 2)) : 2;
  t.a1 = 1 + Rand(X, 4, p.P1 - 1);
  t.b1 = Rand(X, 5, p.P1);
  return t;
}

/// The column-index walk implicit in the Encoding Symbol Generator Enc[]
/// (RFC 6330 Section 5.3.5.3): which of the L intermediate symbols a given
/// tuple XORs together. The LT part touches columns in [0, W); the PI part
/// touches columns [W, W+P) (skipping the "b1 >= P" gap exactly as the RFC's
/// pseudocode does). Kept separate from actually reading/summing symbol data
/// so it can be reused for both encoding and for building the matrix rows
/// used by the linear system.
inline std::vector<uint32_t> encoding_indices(uint32_t X, const PreCodeParams& p) {
  Tuple t = tuple_generator(X, p);
  std::vector<uint32_t> indices;
  indices.reserve(t.d + t.d1);

  uint32_t b = t.b;
  indices.push_back(b);
  for (uint32_t j = 1; j < t.d; j++) {
    b = (b + t.a) % p.W;
    indices.push_back(b);
  }

  uint32_t b1 = t.b1;
  while (b1 >= p.P) b1 = (b1 + t.a1) % p.P1;
  indices.push_back(p.W + b1);
  for (uint32_t j = 1; j < t.d1; j++) {
    b1 = (b1 + t.a1) % p.P1;
    while (b1 >= p.P) b1 = (b1 + t.a1) % p.P1;
    indices.push_back(p.W + b1);
  }

  return indices;
}

} // namespace RaptorQ
} // namespace LibFlute
