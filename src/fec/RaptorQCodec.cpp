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
#include "fec/RaptorQCodec.h"
#include "fec/GF256.h"
#include <stdexcept>
#include <vector>

namespace LibFlute {
namespace RaptorQ {

namespace {

using Terms = std::vector<std::pair<uint32_t, uint8_t>>;

/// Which of the S LDPC rows each of the first B intermediate symbols XORs into. This is a
/// paraphrase of the construction in RFC 6330 clause 5.3.3.3, not a quotation of it; the two
/// definitions it relies on, verbatim, are:
///
/// RFC 6330 clause 5.3.3.3: "C[0], ..., C[B-1] denote the intermediate symbols that are LT
/// symbols but not LDPC symbols."
///
/// RFC 6330 clause 5.3.3.3: "The pre-coding relationships amongst the L intermediate symbols are
/// defined by requiring that a set of S+H linear combinations of the intermediate symbols
/// evaluate to zero."
///
/// B is W - S, per the same clause. Coefficients are all 1 -- the
/// LDPC relations are plain XOR, no GF(256) scaling.
std::vector<std::vector<uint32_t>> ldpc_first_loop(uint32_t B, uint32_t S) {
  std::vector<std::vector<uint32_t>> rows(S);
  for (uint32_t i = 0; i < B; i++) {
    uint32_t a = 1 + i / S;
    uint32_t b = i % S;
    rows[b].push_back(i);
    b = (b + a) % S;
    rows[b].push_back(i);
    b = (b + a) % S;
    rows[b].push_back(i);
  }
  return rows;
}

/// Builds the H HDPC equations' coefficients over columns [0, K'+S), via
/// the MT*GAMMA construction (RFC 6330 Section 5.3.3.3). Built as dense
/// H x (K'+S) and (K'+S) x (K'+S) matrices and multiplied directly -- see
/// GF256LinearSystem.h's class comment for why dense is an acceptable
/// trade-off here (bounded block sizes, correctness-first).
std::vector<std::vector<uint8_t>> hdpc_coefficient_rows(uint32_t Kprime, uint32_t S, uint32_t H) {
  uint32_t n = Kprime + S; // MT is H x n, GAMMA is n x n

  std::vector<std::vector<uint8_t>> MT(H, std::vector<uint8_t>(n, 0));
  for (uint32_t j = 0; j < n - 1; j++) {
    uint32_t i0 = Rand(j + 1, 6, H);
    // Rand(., ., H-1) returns a value in [0, H-2], so this is always in
    // [1, H-1] -- never a multiple of H -- which guarantees i1 != i0. MT's
    // definition ("is 1 if i==i0 or i==i1") is a plain boolean OR, not an
    // accumulation, but since the two indices can never coincide there's
    // no ambiguity to resolve either way.
    uint32_t i1 = (i0 + Rand(j + 1, 7, H - 1) + 1) % H;
    MT[i0][j] = 1;
    MT[i1][j] = 1;
  }
  for (uint32_t i = 0; i < H; i++) {
    MT[i][n - 1] = gf_alpha_pow(i);
  }

  // GAMMA[i,j] = alpha^(i-j) for i >= j, else 0. Built row by row using the
  // recurrence GAMMA[i,j] = alpha * GAMMA[i,j+1] for j < i (so each row is
  // one gf_mul away from the previous entry) rather than an alpha_pow call
  // per cell.
  std::vector<std::vector<uint8_t>> GAMMA(n, std::vector<uint8_t>(n, 0));
  for (uint32_t i = 0; i < n; i++) {
    GAMMA[i][i] = 1; // alpha^0
    for (uint32_t j = i; j-- > 0;) {
      GAMMA[i][j] = gf_mul(GAMMA[i][j + 1], 2 /* alpha */);
    }
  }

  std::vector<std::vector<uint8_t>> result(H, std::vector<uint8_t>(n, 0));
  for (uint32_t h = 0; h < H; h++) {
    for (uint32_t k = 0; k < n; k++) {
      uint8_t mt_hk = MT[h][k];
      if (mt_hk == 0) continue;
      const auto& gamma_row = GAMMA[k];
      auto& out = result[h];
      for (uint32_t j = 0; j <= k; j++) { // GAMMA[k][j] == 0 for j > k
        out[j] ^= gf_mul(mt_hk, gamma_row[j]);
      }
    }
  }
  return result;
}

} // namespace

RaptorQCodec::RaptorQCodec(uint32_t K)
  : _K(K)
  , _params(derive_precode_params(smallest_supported_Kprime(K)))
  , _system(_params.L)
{
  const auto& p = _params;

  // LDPC relations: S rows. Column B+row is the row's own LDPC symbol
  // (coefficient 1); the first loop contributes B-domain columns; the
  // second loop contributes two PI-domain columns (W+a, W+b).
  auto ldpc1 = ldpc_first_loop(p.B, p.S);
  for (uint32_t row = 0; row < p.S; row++) {
    Terms terms;
    for (uint32_t col : ldpc1[row]) terms.push_back({col, 1});
    terms.push_back({p.B + row, 1});
    uint32_t a = row % p.P;
    uint32_t b = (row + 1) % p.P;
    terms.push_back({p.W + a, 1});
    terms.push_back({p.W + b, 1});
    _system.add_equation(terms, {});
  }

  // HDPC relations: H rows over columns [0, K'+S), plus the row's own HDPC
  // symbol at column K'+S+row (coefficient 1).
  auto hdpc = hdpc_coefficient_rows(p.Kprime, p.S, p.H);
  for (uint32_t row = 0; row < p.H; row++) {
    Terms terms;
    for (uint32_t col = 0; col < p.Kprime + p.S; col++) {
      if (hdpc[row][col] != 0) terms.push_back({col, hdpc[row][col]});
    }
    terms.push_back({p.Kprime + p.S + row, 1});
    _system.add_equation(terms, {});
  }

  // Padding symbols (ISI in [K, K')) are always zero, and are known without
  // needing to receive anything -- feed them in now, for free, same as the
  // pre-coding rows above.
  for (uint32_t isi = K; isi < p.Kprime; isi++) {
    Terms terms;
    for (uint32_t col : encoding_indices(isi, p)) terms.push_back({col, 1});
    _system.add_equation(terms, {});
  }
}

std::vector<std::vector<uint8_t>> RaptorQCodec::compute_intermediate_symbols(
    const std::vector<std::vector<uint8_t>>& source_symbols) {
  if (source_symbols.size() != _K) {
    throw std::invalid_argument("RaptorQCodec: expected exactly K source symbols");
  }
  for (uint32_t esi = 0; esi < _K; esi++) {
    Terms terms;
    for (uint32_t col : encoding_indices(esi_to_isi(esi), _params)) terms.push_back({col, 1});
    _system.add_equation(terms, source_symbols[esi]);
  }
  if (!_system.fully_determined()) {
    throw std::logic_error("RaptorQCodec: pre-coding + source symbols did not fully "
                            "determine the intermediate symbols for K=" + std::to_string(_K) +
                            " (K'=" + std::to_string(_params.Kprime) + ", rank " +
                            std::to_string(_system.rank()) + "/" + std::to_string(_params.L) + ")");
  }
  std::vector<std::vector<uint8_t>> intermediate;
  intermediate.reserve(_params.L);
  for (uint32_t i = 0; i < _params.L; i++) intermediate.push_back(_system.solved_value(i));
  return intermediate;
}

std::vector<uint8_t> RaptorQCodec::generate_encoding_symbol(
    uint32_t esi, const std::vector<std::vector<uint8_t>>& intermediate_symbols) const {
  auto indices = encoding_indices(esi_to_isi(esi), _params);
  std::vector<uint8_t> result = intermediate_symbols[indices[0]];
  for (size_t j = 1; j < indices.size(); j++) {
    const auto& sym = intermediate_symbols[indices[j]];
    for (size_t b = 0; b < result.size(); b++) result[b] ^= sym[b];
  }
  return result;
}

bool RaptorQCodec::add_received_symbol(uint32_t esi, const std::vector<uint8_t>& data) {
  Terms terms;
  for (uint32_t col : encoding_indices(esi_to_isi(esi), _params)) terms.push_back({col, 1});
  return _system.add_equation(terms, data);
}

std::vector<std::vector<uint8_t>> RaptorQCodec::decode_source_symbols() {
  if (!can_decode()) {
    throw std::logic_error("RaptorQCodec: not enough independent symbols received yet");
  }
  std::vector<std::vector<uint8_t>> source;
  source.reserve(_K);
  for (uint32_t esi = 0; esi < _K; esi++) {
    auto indices = encoding_indices(esi_to_isi(esi), _params);
    std::vector<uint8_t> sym = _system.solved_value(indices[0]);
    for (size_t j = 1; j < indices.size(); j++) {
      const auto& v = _system.solved_value(indices[j]);
      for (size_t b = 0; b < sym.size(); b++) sym[b] ^= v[b];
    }
    source.push_back(std::move(sym));
  }
  return source;
}

} // namespace RaptorQ
} // namespace LibFlute
