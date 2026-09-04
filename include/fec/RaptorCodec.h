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
#include <vector>
#include "fec/RaptorMath.h"
#include "fec/GF2LinearSystem.h"
#include "fec/FecBlockCodec.h"

namespace LibFlute {
namespace Raptor {

/// A complete RFC 5053 Raptor codec for a single source block of K source
/// symbols. Systematic: the source symbols are encoding symbols ESI 0..K-1;
/// any ESI (source or repair, unbounded above) can be produced once the L
/// intermediate symbols are known.
///
/// One instance is set up per source block (the LDPC/Half pre-coding
/// constraint rows -- RFC 5053 §5.4.2.3 -- are built once, in the
/// constructor, since they depend only on K); it is then driven from either
/// side:
///  - encoder: compute_intermediate_symbols() with all K source symbols,
///    then generate_encoding_symbol() for any ESI to transmit;
///  - decoder: add_received_symbol() as symbols arrive (source or repair, any
///    order, duplicates tolerated), decode_source_symbols() once can_decode().
class RaptorCodec : public FecBlockCodec {
  public:
    /// @param K number of source symbols in the block (RFC 5053 §5.7 range: [4, 8192])
    explicit RaptorCodec(uint32_t K);

    uint32_t K() const { return _params.K; }
    uint32_t L() const { return _params.L; }

    /// Encoder: solve for the L intermediate symbols given all K source
    /// symbols (each exactly `symbol_length` bytes). Throws std::logic_error
    /// if the resulting system isn't fully determined -- this should not
    /// happen for any K in the RFC's supported range, since the systematic
    /// index table (§5.7) is specifically chosen to make the combined
    /// LDPC+Half+source-LT system non-singular; a failure here points at a
    /// bug (e.g. a corrupted table) rather than an expected runtime outcome.
    std::vector<std::vector<uint8_t>> compute_intermediate_symbols(
        const std::vector<std::vector<uint8_t>>& source_symbols) override;

    /// Produce the encoding symbol for ESI `esi` (source, if < K, or repair
    /// otherwise) from already-computed intermediate symbols.
    std::vector<uint8_t> generate_encoding_symbol(
        uint32_t esi, const std::vector<std::vector<uint8_t>>& intermediate_symbols) const override;

    /// Decoder: register one received encoding symbol (source or repair).
    /// Returns true if it was independent of everything received so far
    /// (i.e. it moved the decoder closer to being able to decode).
    bool add_received_symbol(uint32_t esi, const std::vector<uint8_t>& data) override;

    /// True once enough independent symbols have been received to recover
    /// every intermediate symbol (and therefore every source symbol).
    bool can_decode() const override { return _system.fully_determined(); }

    /// How many more independent symbols are needed before can_decode()
    /// would become true, purely for progress reporting.
    uint32_t symbols_needed() const override { return _system.num_unknowns() - _system.rank(); }

    /// Decoder: once can_decode(), recover all K source symbols.
    std::vector<std::vector<uint8_t>> decode_source_symbols() override;

  private:
    PreCodeParams _params;
    GF2LinearSystem _system;
};

} // namespace Raptor
} // namespace LibFlute
