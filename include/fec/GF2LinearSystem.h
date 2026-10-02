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

namespace LibFlute {
namespace Raptor {

/// An incrementally-solved linear system over GF(2) whose unknowns are byte
/// buffers (encoding symbols) rather than scalars: each equation is a set of
/// unknown indices that XOR together to a right-hand-side byte buffer.
///
/// This is the shared engine behind both the Raptor encoder (which solves the
/// K + S + H equations formed by the source symbols plus the LDPC and Half
/// pre-coding constraints -- RFC 5053 §5.4.2.3/§5.6 -- to recover the L
/// intermediate symbols) and the decoder (same equations, but with received
/// encoding symbols of possibly mixed source/repair origin standing in for
/// some of the source-symbol rows, and possibly more of them than K).
///
/// Equations are kept in reduced row-echelon (Gauss-Jordan) form as they are
/// added, so once the rank reaches num_unknowns() every row already *is* the
/// solution for one unknown -- no separate back-substitution pass is needed.
/// This is plain Gaussian elimination, not the two-phase inactivation
/// decoding RFC 5053 §5.5 outlines as a performance optimisation: it is
/// O(rank) work per equation and O(rank^2) bit-operations overall, which is
/// the correct, spec-compliant result at every source block size this code
/// supports, just not the asymptotically fastest way to get there. Revisit
/// with inactivation decoding if profiling shows this is a bottleneck for the
/// largest (K close to 8192) source blocks.
class GF2LinearSystem {
  public:
    /// @param num_unknowns Number of intermediate symbols (L)
    explicit GF2LinearSystem(uint32_t num_unknowns);

    /// Number of unknowns in the system (L)
    uint32_t num_unknowns() const { return _num_unknowns; }

    /// Number of independent equations accepted so far
    uint32_t rank() const { return _rank; }

    /// True once enough independent equations have been seen to solve for
    /// every unknown
    bool fully_determined() const { return _rank == _num_unknowns; }

    /// The byte length of the unknowns/right-hand-sides, once known (it
    /// becomes known the first time a non-empty rhs is added; LDPC/Half
    /// constraint rows carry an implicit all-zero rhs of this length, filled
    /// in lazily since those rows are usually added before any real symbol
    /// data is available)
    size_t symbol_length() const { return _symbol_length; }

    /// Add one equation: the XOR of the unknowns at `columns` (repeated
    /// indices toggle, matching the RFC's "^=" construction -- this is NOT
    /// set membership) equals `rhs`. Pass an empty `rhs` for an implicit
    /// all-zero right-hand side (used by the LDPC/Half constraint rows).
    ///
    /// Returns true if this equation increased the rank (i.e. it wasn't a
    /// linear combination of equations already known); the caller doesn't
    /// need the return value for correctness, only for progress tracking
    /// (e.g. "do we have enough equations yet").
    bool add_equation(const std::vector<uint32_t>& columns, std::vector<uint8_t> rhs);

    /// Once fully_determined(), returns the solved value for unknown `index`.
    /// Throws std::logic_error if called before the system is fully
    /// determined, or std::out_of_range for an invalid index.
    const std::vector<uint8_t>& solved_value(uint32_t index) const;

  private:
    struct Row {
      std::vector<uint64_t> bits;             // packed bit-columns, length words_per_row
      std::optional<std::vector<uint8_t>> rhs; // nullopt == implicit all-zero
      uint32_t pivot = 0;                      // column this row is the pivot for
    };

    void ensure_symbol_length(size_t len);
    const std::vector<uint8_t>& materialize(const std::optional<std::vector<uint8_t>>& rhs) const;
    static void xor_bits(std::vector<uint64_t>& dst, const std::vector<uint64_t>& src);
    static void xor_bytes(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src);
    static bool test_bit(const std::vector<uint64_t>& bits, uint32_t col);
    static void toggle_bit(std::vector<uint64_t>& bits, uint32_t col);
    int lowest_set_bit(const std::vector<uint64_t>& bits) const;

    uint32_t _num_unknowns;
    uint32_t _words_per_row;
    uint32_t _rank = 0;
    size_t _symbol_length = 0;
    bool _symbol_length_known = false;

    // _row_of_pivot[c] = index into _rows of the row whose pivot is column c,
    // or -1 if that column has no pivot row yet.
    std::vector<int> _row_of_pivot;
    std::vector<Row> _rows;

    mutable std::vector<uint8_t> _zero_scratch; // reused buffer for materializing implicit-zero rhs
};

} // namespace Raptor
} // namespace LibFlute
