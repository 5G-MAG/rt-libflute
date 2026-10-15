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
#include "fec/GF2LinearSystem.h"
#include <stdexcept>
#include <cstring>

namespace LibFlute {
namespace Raptor {

GF2LinearSystem::GF2LinearSystem(uint32_t num_unknowns)
  : _num_unknowns(num_unknowns)
  , _words_per_row((num_unknowns + 63) / 64)
  , _row_of_pivot(num_unknowns, -1)
{
  _rows.reserve(num_unknowns);
}

void GF2LinearSystem::ensure_symbol_length(size_t len) {
  if (!_symbol_length_known) {
    _symbol_length = len;
    _symbol_length_known = true;
  } else if (len != _symbol_length) {
    throw std::invalid_argument("GF2LinearSystem: mismatched symbol length in rhs");
  }
}

const std::vector<uint8_t>& GF2LinearSystem::materialize(const std::optional<std::vector<uint8_t>>& rhs) const {
  if (rhs.has_value()) return *rhs;
  if (_zero_scratch.size() != _symbol_length) {
    _zero_scratch.assign(_symbol_length, 0);
  }
  return _zero_scratch;
}

void GF2LinearSystem::xor_bits(std::vector<uint64_t>& dst, const std::vector<uint64_t>& src) {
  for (size_t i = 0; i < dst.size(); i++) dst[i] ^= src[i];
}

void GF2LinearSystem::xor_bytes(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src) {
  // src may be shorter only if it's the shared zero-scratch buffer sized to
  // the current symbol length; dst is always sized to symbol_length once set.
  size_t n = std::min(dst.size(), src.size());
  for (size_t i = 0; i < n; i++) dst[i] ^= src[i];
}

bool GF2LinearSystem::test_bit(const std::vector<uint64_t>& bits, uint32_t col) {
  return (bits[col / 64] >> (col % 64)) & 1ULL;
}

void GF2LinearSystem::toggle_bit(std::vector<uint64_t>& bits, uint32_t col) {
  bits[col / 64] ^= (1ULL << (col % 64));
}

int GF2LinearSystem::lowest_set_bit(const std::vector<uint64_t>& bits) const {
  for (size_t w = 0; w < bits.size(); w++) {
    if (bits[w] != 0) {
      for (int b = 0; b < 64; b++) {
        if ((bits[w] >> b) & 1ULL) return (int)(w * 64 + b);
      }
    }
  }
  return -1;
}

bool GF2LinearSystem::add_equation(const std::vector<uint32_t>& columns, std::vector<uint8_t> rhs) {
  std::optional<std::vector<uint8_t>> rhs_opt;
  if (!rhs.empty()) {
    ensure_symbol_length(rhs.size());
    rhs_opt = std::move(rhs);
  }

  std::vector<uint64_t> bits(_words_per_row, 0);
  for (uint32_t col : columns) {
    if (col >= _num_unknowns) {
      throw std::out_of_range("GF2LinearSystem: equation references unknown index out of range");
    }
    toggle_bit(bits, col); // repeated indices toggle (XOR), matching RFC "^=" construction
  }

  // Reduce the new row against every existing pivot row. A single forward
  // pass over columns suffices because the existing rows already form a full
  // reduced row-echelon system (each pivot row has a zero bit at every other
  // row's pivot column), so clearing bit c can't reintroduce a bit at any
  // column < c we've already cleared.
  for (uint32_t c = 0; c < _num_unknowns; c++) {
    int r = _row_of_pivot[c];
    if (r < 0) continue;
    if (test_bit(bits, c)) {
      xor_bits(bits, _rows[r].bits);
      if (rhs_opt.has_value() || _rows[r].rhs.has_value()) {
        auto& dst = rhs_opt.has_value() ? *rhs_opt : (rhs_opt.emplace(_symbol_length, 0), *rhs_opt);
        xor_bytes(dst, materialize(_rows[r].rhs));
      }
    }
  }

  int pivot = lowest_set_bit(bits);
  if (pivot < 0) {
    // Redundant equation: it's a linear combination of ones we already have.
    // (If rhs_opt is non-zero here it would mean the input data was
    // inconsistent with equations already accepted -- that indicates
    // corrupted/mismatched symbols upstream, not a solvable state, so we
    // just drop the equation rather than accepting a contradiction.)
    return false;
  }

  // Back-substitute the new row into every existing row that still has a bit
  // set at the new pivot column, to keep the whole system in full RREF.
  for (auto& row : _rows) {
    if (test_bit(row.bits, (uint32_t)pivot)) {
      xor_bits(row.bits, bits);
      if (rhs_opt.has_value() || row.rhs.has_value()) {
        if (!row.rhs.has_value()) row.rhs.emplace(_symbol_length, 0);
        xor_bytes(*row.rhs, materialize(rhs_opt));
      }
    }
  }

  Row new_row;
  new_row.bits = std::move(bits);
  new_row.rhs = std::move(rhs_opt);
  new_row.pivot = (uint32_t)pivot;
  _rows.push_back(std::move(new_row));
  _row_of_pivot[pivot] = (int)_rows.size() - 1;
  _rank++;
  return true;
}

const std::vector<uint8_t>& GF2LinearSystem::solved_value(uint32_t index) const {
  if (index >= _num_unknowns) throw std::out_of_range("GF2LinearSystem: unknown index out of range");
  if (!fully_determined()) throw std::logic_error("GF2LinearSystem: system is not fully determined yet");
  int r = _row_of_pivot[index];
  // fully_determined() guarantees every column has a pivot row.
  return materialize(_rows[r].rhs);
}

} // namespace Raptor
} // namespace LibFlute
