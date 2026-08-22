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
#include "fec/RaptorCodec.h"
#include <stdexcept>

namespace LibFlute {
namespace Raptor {

namespace {

/// RFC 5053 §5.4.2.3 LDPC symbols: for each source symbol index i in
/// [0, K), it contributes to three of the S LDPC rows (identified by `b`,
/// possibly with repeats if S is small -- the caller's toggle-XOR semantics
/// handle that correctly). Returns, for each LDPC row, the list of source
/// symbol columns that XOR into it (the row's own I_S identity column is
/// added separately by the caller).
std::vector<std::vector<uint32_t>> ldpc_row_compositions(uint32_t K, uint32_t S) {
  std::vector<std::vector<uint32_t>> rows(S);
  for (uint32_t i = 0; i < K; i++) {
    uint32_t a = 1 + (i / S) % (S - 1);
    uint32_t b = i % S;
    rows[b].push_back(i);
    b = (b + a) % S;
    rows[b].push_back(i);
    b = (b + a) % S;
    rows[b].push_back(i);
  }
  return rows;
}

/// RFC 5053 §5.4.2.3 Half (HDPC) symbols: row h collects every column
/// j in [0, K+S) whose Gray-sequence entry m[j] has bit h set.
std::vector<std::vector<uint32_t>> half_row_compositions(uint32_t K, uint32_t S, uint32_t H, uint32_t Hprime) {
  auto m = gray_sequence_with_popcount(K + S, Hprime);
  std::vector<std::vector<uint32_t>> rows(H);
  for (uint32_t h = 0; h < H; h++) {
    for (uint32_t j = 0; j < K + S; j++) {
      if (bit_set(m[j], h)) rows[h].push_back(j);
    }
  }
  return rows;
}

} // namespace

RaptorCodec::RaptorCodec(uint32_t K)
  : _params(derive_precode_params(K))
  , _system(_params.L)
{
  const auto& p = _params;

  auto ldpc = ldpc_row_compositions(p.K, p.S);
  for (uint32_t row = 0; row < p.S; row++) {
    auto cols = ldpc[row];
    cols.push_back(p.K + row); // I_S identity column for this LDPC row
    _system.add_equation(cols, {});
  }

  auto half = half_row_compositions(p.K, p.S, p.H, p.Hprime);
  for (uint32_t row = 0; row < p.H; row++) {
    auto cols = half[row];
    cols.push_back(p.K + p.S + row); // I_H identity column for this Half row
    _system.add_equation(cols, {});
  }
}

std::vector<std::vector<uint8_t>> RaptorCodec::compute_intermediate_symbols(
    const std::vector<std::vector<uint8_t>>& source_symbols) {
  if (source_symbols.size() != _params.K) {
    throw std::invalid_argument("RaptorCodec: expected exactly K source symbols");
  }
  for (uint32_t esi = 0; esi < _params.K; esi++) {
    _system.add_equation(lt_indices(esi, _params), source_symbols[esi]);
  }
  if (!_system.fully_determined()) {
    // Should not happen for any K the RFC's systematic index table supports;
    // see the class-level comment. Surfacing this loudly rather than
    // returning a partial/garbage result.
    throw std::logic_error("RaptorCodec: pre-coding + source symbols did not "
                            "fully determine the intermediate symbols for K=" +
                            std::to_string(_params.K) + " (rank " +
                            std::to_string(_system.rank()) + "/" + std::to_string(_params.L) + ")");
  }
  std::vector<std::vector<uint8_t>> intermediate;
  intermediate.reserve(_params.L);
  for (uint32_t i = 0; i < _params.L; i++) {
    intermediate.push_back(_system.solved_value(i));
  }
  return intermediate;
}

std::vector<uint8_t> RaptorCodec::generate_encoding_symbol(
    uint32_t esi, const std::vector<std::vector<uint8_t>>& intermediate_symbols) const {
  auto indices = lt_indices(esi, _params);
  std::vector<uint8_t> result = intermediate_symbols[indices[0]]; // copy
  for (size_t j = 1; j < indices.size(); j++) {
    const auto& sym = intermediate_symbols[indices[j]];
    for (size_t b = 0; b < result.size(); b++) result[b] ^= sym[b];
  }
  return result;
}

bool RaptorCodec::add_received_symbol(uint32_t esi, const std::vector<uint8_t>& data) {
  return _system.add_equation(lt_indices(esi, _params), data);
}

std::vector<std::vector<uint8_t>> RaptorCodec::decode_source_symbols() {
  if (!can_decode()) {
    throw std::logic_error("RaptorCodec: not enough independent symbols received yet");
  }
  std::vector<std::vector<uint8_t>> source;
  source.reserve(_params.K);
  for (uint32_t esi = 0; esi < _params.K; esi++) {
    auto indices = lt_indices(esi, _params);
    std::vector<uint8_t> sym = _system.solved_value(indices[0]);
    for (size_t j = 1; j < indices.size(); j++) {
      const auto& v = _system.solved_value(indices[j]);
      for (size_t b = 0; b < sym.size(); b++) sym[b] ^= v[b];
    }
    source.push_back(std::move(sym));
  }
  return source;
}

} // namespace Raptor
} // namespace LibFlute
