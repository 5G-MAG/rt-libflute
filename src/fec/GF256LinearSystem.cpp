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
#include "fec/GF256LinearSystem.h"
#include "fec/GF256.h"
#include <stdexcept>

namespace LibFlute {
namespace RaptorQ {

GF256LinearSystem::GF256LinearSystem(uint32_t num_unknowns)
  : _num_unknowns(num_unknowns)
  , _row_of_pivot(num_unknowns, -1)
{
  _rows.reserve(num_unknowns);
}

void GF256LinearSystem::ensure_symbol_length(size_t len) {
  if (!_symbol_length_known) {
    _symbol_length = len;
    _symbol_length_known = true;
  } else if (len != _symbol_length) {
    throw std::invalid_argument("GF256LinearSystem: mismatched symbol length in rhs");
  }
}

const std::vector<uint8_t>& GF256LinearSystem::materialize(const std::optional<std::vector<uint8_t>>& rhs) const {
  if (rhs.has_value()) return *rhs;
  if (_zero_scratch.size() != _symbol_length) {
    _zero_scratch.assign(_symbol_length, 0);
  }
  return _zero_scratch;
}

void GF256LinearSystem::scale_and_add_row(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src, uint8_t factor) {
  if (factor == 0) return;
  for (size_t j = 0; j < dst.size(); j++) {
    dst[j] ^= gf_mul(factor, src[j]);
  }
}

void GF256LinearSystem::scale_and_add_rhs(std::optional<std::vector<uint8_t>>& dst, const std::optional<std::vector<uint8_t>>& src, uint8_t factor) {
  if (factor == 0) return;
  if (!src.has_value() && !_symbol_length_known) {
    // Both operands are implicitly all-zero and the symbol length isn't
    // known yet (this can happen while only zero-rhs equations -- LDPC/HDPC/
    // padding rows -- have been added so far, before any real symbol data
    // has arrived). There's nothing to materialize correctly yet, and
    // nothing useful to do: factor * 0 is still 0, so leave dst untouched
    // rather than manifesting a bogus zero-length placeholder that would
    // silently stay the wrong length forever once the real length *is*
    // established by a later equation.
    return;
  }
  const auto& s = materialize(src);
  if (!dst.has_value()) dst.emplace(_symbol_length, 0);
  for (size_t j = 0; j < dst->size(); j++) {
    (*dst)[j] ^= gf_mul(factor, s[j]);
  }
}

void GF256LinearSystem::scale_rhs_in_place(std::optional<std::vector<uint8_t>>& rhs, uint8_t factor) {
  if (!rhs.has_value()) return; // scaling the implicit zero vector is still zero
  if (factor == 1) return;
  for (auto& b : *rhs) b = gf_mul(b, factor);
}

int GF256LinearSystem::first_nonzero(const std::vector<uint8_t>& coeffs) const {
  for (size_t j = 0; j < coeffs.size(); j++) {
    if (coeffs[j] != 0) return (int)j;
  }
  return -1;
}

bool GF256LinearSystem::add_equation(const std::vector<std::pair<uint32_t, uint8_t>>& terms, std::vector<uint8_t> rhs) {
  std::optional<std::vector<uint8_t>> rhs_opt;
  if (!rhs.empty()) {
    ensure_symbol_length(rhs.size());
    rhs_opt = std::move(rhs);
  }

  std::vector<uint8_t> row(_num_unknowns, 0);
  for (auto& [col, coeff] : terms) {
    if (col >= _num_unknowns) {
      throw std::out_of_range("GF256LinearSystem: equation references unknown index out of range");
    }
    row[col] ^= coeff; // repeated columns accumulate (GF(256) addition), matching the RFC's "+="-style construction
  }

  // Reduce against every existing pivot row -- a single forward pass
  // suffices for the same reason it does in GF2LinearSystem: the existing
  // rows are already in full reduced row-echelon form.
  for (uint32_t c = 0; c < _num_unknowns; c++) {
    int r = _row_of_pivot[c];
    if (r < 0) continue;
    uint8_t factor = row[c];
    if (factor == 0) continue;
    scale_and_add_row(row, _rows[r].coeffs, factor);
    scale_and_add_rhs(rhs_opt, _rows[r].rhs, factor);
  }

  int pivot = first_nonzero(row);
  if (pivot < 0) {
    return false; // redundant equation (or, if rhs_opt ended up non-zero, inconsistent input data -- dropped either way)
  }

  // Normalize so the pivot column's coefficient is exactly 1.
  uint8_t pivot_val = row[pivot];
  if (pivot_val != 1) {
    uint8_t inv = gf_inv(pivot_val);
    for (auto& c : row) c = gf_mul(c, inv);
    scale_rhs_in_place(rhs_opt, inv);
  }

  // Back-substitute into every existing row with a non-zero entry at the
  // new pivot column, to keep the whole system in full RREF.
  for (auto& existing : _rows) {
    uint8_t factor = existing.coeffs[pivot];
    if (factor == 0) continue;
    scale_and_add_row(existing.coeffs, row, factor);
    scale_and_add_rhs(existing.rhs, rhs_opt, factor);
  }

  Row new_row;
  new_row.coeffs = std::move(row);
  new_row.rhs = std::move(rhs_opt);
  new_row.pivot = (uint32_t)pivot;
  _rows.push_back(std::move(new_row));
  _row_of_pivot[pivot] = (int)_rows.size() - 1;
  _rank++;
  return true;
}

const std::vector<uint8_t>& GF256LinearSystem::solved_value(uint32_t index) const {
  if (index >= _num_unknowns) throw std::out_of_range("GF256LinearSystem: unknown index out of range");
  if (!fully_determined()) throw std::logic_error("GF256LinearSystem: system is not fully determined yet");
  int r = _row_of_pivot[index];
  return materialize(_rows[r].rhs);
}

} // namespace RaptorQ
} // namespace LibFlute
