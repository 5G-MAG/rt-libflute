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
#include <optional>
#include <utility>

namespace LibFlute {
namespace RaptorQ {

/// An incrementally-solved linear system over GF(256), the RaptorQ
/// counterpart to Raptor's GF2LinearSystem. The difference that actually
/// matters here (not just the field size) is that RaptorQ's HDPC relations
/// (RFC 6330 Section 5.3.3.3, the MT*GAMMA construction) have real,
/// non-unity octet coefficients -- unlike Raptor, where every pre-coding/LT
/// relation is a plain XOR (coefficient always 1) -- so this can't reuse
/// GF2LinearSystem's packed-bit rows; each row genuinely needs one octet
/// coefficient per column.
///
/// Rows are stored densely (one octet per unknown). That's the right
/// trade-off for correctness-first: RaptorQ blocks are bounded by a
/// caller-supplied K cap in practice (same as this library's Raptor
/// implementation -- see RaptorCodec.h), and a dense L*L octet matrix is
/// entirely reasonable at the block sizes that cap implies. It would not be
/// reasonable at RFC 6330's full K'_max = 56403 (L^2 bytes would be
/// gigabytes); that's a scaling concern for a future sparse/inactivation
/// implementation, not a correctness one.
class GF256LinearSystem {
  public:
    explicit GF256LinearSystem(uint32_t num_unknowns);

    uint32_t num_unknowns() const { return _num_unknowns; }
    uint32_t rank() const { return _rank; }
    bool fully_determined() const { return _rank == _num_unknowns; }
    size_t symbol_length() const { return _symbol_length; }

    /// Add one equation: sum over (column, coefficient) pairs in `terms` of
    /// coefficient * unknown[column] == rhs. Repeated columns accumulate
    /// (their coefficients add, i.e. XOR) rather than overwriting, matching
    /// the RFC's repeated "D[b] = D[b] + C[i]" construction. Pass an empty
    /// `rhs` for an implicit all-zero right-hand side.
    ///
    /// Returns true if this equation increased the rank.
    bool add_equation(const std::vector<std::pair<uint32_t, uint8_t>>& terms, std::vector<uint8_t> rhs);

    /// Once fully_determined(), returns the solved value for unknown `index`.
    const std::vector<uint8_t>& solved_value(uint32_t index) const;

  private:
    struct Row {
      std::vector<uint8_t> coeffs; // length num_unknowns; pivot column's entry is exactly 1
      std::optional<std::vector<uint8_t>> rhs; // nullopt == implicit all-zero
      uint32_t pivot = 0;
    };

    void ensure_symbol_length(size_t len);
    const std::vector<uint8_t>& materialize(const std::optional<std::vector<uint8_t>>& rhs) const;
    static void scale_and_add_row(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src, uint8_t factor);
    void scale_and_add_rhs(std::optional<std::vector<uint8_t>>& dst, const std::optional<std::vector<uint8_t>>& src, uint8_t factor);
    void scale_rhs_in_place(std::optional<std::vector<uint8_t>>& rhs, uint8_t factor);
    int first_nonzero(const std::vector<uint8_t>& coeffs) const;

    uint32_t _num_unknowns;
    uint32_t _rank = 0;
    size_t _symbol_length = 0;
    bool _symbol_length_known = false;

    std::vector<int> _row_of_pivot; // size num_unknowns, -1 if none
    std::vector<Row> _rows;

    mutable std::vector<uint8_t> _zero_scratch;
};

} // namespace RaptorQ
} // namespace LibFlute
