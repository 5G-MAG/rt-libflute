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
#include <stdexcept>
#include <vector>
#include <algorithm>
#include "fec/RaptorTables.h"

// The scalar building blocks of RFC 5053 ("Raptor Forward Error Correction
// Scheme for Object Delivery"): the Rand() PRNG, the degree distribution
// (Table 1), and the LT triple generator. Every formula here is transcribed
// directly from the RFC text (section numbers cited per function); only the
// three big constant tables it depends on live in RaptorTables.h.
//
// This is deliberately kept free of any I/O, symbol buffers, or matrix code
// so it can be unit-tested against known-good (X, K) -> (d, a, b) triples in
// isolation before it's trusted to drive the encoder/decoder.

namespace LibFlute {
namespace Raptor {

/// RFC 5053 §5.4.4.1: Rand[X, i, m] = (V0[(X+i) % 256] ^ V1[(floor(X/256)+i) % 256]) % m
inline uint32_t Rand(uint32_t X, uint32_t i, uint32_t m) {
  uint32_t v0 = kRandTableV0[(X + i) % 256];
  uint32_t v1 = kRandTableV1[(X / 256 + i) % 256];
  return (v0 ^ v1) % m;
}

/// RFC 5053 §5.4.2.3: derive (L, S, H, Hprime) from K, the number of source
/// symbols in a source block. L is the number of intermediate symbols; the
/// last S+H of them are the LDPC and Half (HDPC) redundancy symbols.
struct PreCodeParams {
  uint32_t K = 0;
  uint32_t S = 0;
  uint32_t H = 0;
  uint32_t Hprime = 0;
  uint32_t L = 0;
  uint32_t Lprime = 0; // smallest prime >= L, used as the LT working modulus
};

/// Smallest prime >= n. L never exceeds a few thousand for the K range this
/// implementation supports (K <= 8192), so plain trial division is fine.
inline uint32_t smallest_prime_at_least(uint32_t n) {
  if (n <= 2) return 2;
  auto is_prime = [](uint32_t v) {
    if (v < 2) return false;
    if (v % 2 == 0) return v == 2;
    for (uint32_t d = 3; d * d <= v; d += 2) {
      if (v % d == 0) return false;
    }
    return true;
  };
  uint32_t v = (n % 2 == 0) ? n + 1 : n;
  while (!is_prime(v)) v += 2;
  return v;
}

inline PreCodeParams derive_precode_params(uint32_t K) {
  if (K < 4 || K > 8192) {
    throw std::invalid_argument("Raptor: K must be in [4, 8192] (RFC 5053 §5.7 systematic index table range)");
  }
  PreCodeParams p;
  p.K = K;

  // RFC 5053 §5.4.2.3: "X be the smallest positive integer such that X*(X-1) >= 2*K"
  uint32_t X = 1;
  while ((uint64_t)X * (X - 1) < 2ULL * K) X++;

  // "S be the smallest prime integer such that S >= ceil(0.01*K) + X"
  uint32_t ceil_001K = (uint32_t)((K + 99) / 100); // ceil(K/100) == ceil(0.01*K)
  p.S = smallest_prime_at_least(ceil_001K + X);

  // "H be the smallest integer such that choose(H, ceil(H/2)) >= K + S"
  auto choose = [](uint64_t n, uint64_t k) -> uint64_t {
    if (k > n) return 0;
    if (k > n - k) k = n - k;
    uint64_t result = 1;
    for (uint64_t i = 0; i < k; i++) {
      result = result * (n - i) / (i + 1);
      if (result > (1ULL << 40)) return result; // saturate, we only need >= comparison
    }
    return result;
  };
  uint32_t H = 0;
  while (choose(H, (H + 1) / 2) < (uint64_t)K + p.S) H++;
  p.H = H;
  p.Hprime = (H + 1) / 2; // ceil(H/2)

  p.L = K + p.S + p.H;
  p.Lprime = smallest_prime_at_least(p.L);
  return p;
}

/// RFC 5053 §5.4.4.2, Table 1: degree distribution. find j such that
/// f[j-1] <= v < f[j], return d[j]. f[] is the cumulative threshold out of
/// 2^^20, d[] the resulting LT degree.
inline uint32_t Deg(uint32_t v) {
  static const uint32_t f[8] = {0, 10241, 491582, 712794, 831695, 948446, 1032189, 1048576};
  static const uint32_t d[8] = {0, 1, 2, 3, 4, 10, 11, 40};
  for (int j = 1; j < 8; j++) {
    if (v < f[j]) return d[j];
  }
  // v == 2^20 - 1 at most by construction (Rand()'s modulus), so we should
  // always return inside the loop; this is only reached if v is out of range.
  throw std::out_of_range("Raptor: Deg() called with v outside [0, 2^20)");
}

/// One LT triple, as produced by the Triple Generator for a given (K, X).
struct Triple {
  uint32_t d; // degree
  uint32_t a; // in [1, L'-1]
  uint32_t b; // in [0, L'-1]
};

/// RFC 5053 §5.4.4.4 Triple Generator. X is the encoding symbol ID (ESI).
/// J(K) is looked up from the systematic index table (RFC 5053 §5.7).
inline Triple triple_generator(uint32_t X, const PreCodeParams& p) {
  const uint32_t Q = 65521;
  uint32_t JK = kSystematicIndexJofK[p.K];
  uint32_t A = (53591 + JK * 997) % Q;
  uint32_t B = 10267 * (JK + 1) % Q;
  uint32_t Y = (B + (uint64_t)X * A) % Q;
  uint32_t v = Rand(Y, 0, 1u << 20);
  Triple t;
  t.d = Deg(v);
  t.a = 1 + Rand(Y, 1, p.Lprime - 1);
  t.b = Rand(Y, 2, p.Lprime);
  return t;
}

/// True if bit `b` (0 == least significant) of `x` is set.
inline bool bit_set(uint32_t x, uint32_t b) {
  return (x >> b) & 1u;
}

/// Half symbols. Walks integers in order, keeping every Gray-coded value
/// (x ^ (x>>1)) that has exactly `ones` bits set, until `length` of them have
/// been collected. That is the m[k] subsequence the clause defines, not the
/// Gray sequence itself.
///
/// The previous version of this comment quoted a sentence that does not appear in
/// the document, with an ellipsis standing in for text that was never there, and
/// it conflated g[] with m[k]. The two definitions, verbatim:
///
/// RFC 5053 clause 5.4.2.3, NOTE on g[i]: "g[i] is the Gray sequence, in which
/// each element differs from the previous one in a single bit position"
///
/// RFC 5053 clause 5.4.2.3: "m[k] denote the subsequence of g[.] whose elements
/// have exactly k non-zero bits in their binary representation."
inline std::vector<uint32_t> gray_sequence_with_popcount(size_t length, uint32_t ones) {
  std::vector<uint32_t> result;
  result.reserve(length);
  for (uint64_t x = 0; result.size() < length; x++) {
    uint32_t g = (uint32_t)((x >> 1) ^ x);
    if ((uint32_t)__builtin_popcount(g) == ones) {
      result.push_back(g);
    }
  }
  return result;
}

/// RFC 5053 §5.4.4.3 "LT Encoding Symbol Generator": walks (b, a) starting
/// from the triple for ESI X, skipping any b >= L (those columns belong to
/// the LDPC/Half symbols' identity padding within L', not to a real
/// intermediate symbol used by this walk), collecting min(d-1, L-1) + 1
/// column indices. Repeated indices are possible and are intentionally not
/// deduplicated -- the caller (GF2LinearSystem::add_equation) treats repeats
/// as XOR-toggle, exactly like the RFC's "result = result ^ C[b]" construction.
inline std::vector<uint32_t> lt_indices(uint32_t X, const PreCodeParams& p) {
  Triple t = triple_generator(X, p);
  uint32_t b = t.b;
  uint32_t a = t.a;
  auto skip_to_valid = [&]() {
    while (b >= p.L) b = (b + a) % p.Lprime;
  };
  skip_to_valid();
  std::vector<uint32_t> indices;
  uint32_t terms = 1 + std::min(t.d - 1, p.L - 1);
  indices.push_back(b);
  for (uint32_t j = 1; j < terms; j++) {
    b = (b + a) % p.Lprime;
    skip_to_valid();
    indices.push_back(b);
  }
  return indices;
}

} // namespace Raptor
} // namespace LibFlute
