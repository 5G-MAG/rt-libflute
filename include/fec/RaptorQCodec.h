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
#include "fec/RaptorQMath.h"
#include "fec/GF256LinearSystem.h"
#include "fec/FecBlockCodec.h"

namespace LibFlute {
namespace RaptorQ {

/// A complete RFC 6330 RaptorQ codec for a single source block of K source
/// symbols. Structured to mirror LibFlute::Raptor::RaptorCodec's public
/// shape (compute_intermediate_symbols / generate_encoding_symbol on the
/// encoder side, add_received_symbol / can_decode / decode_source_symbols
/// on the decoder side) even though the underlying maths are quite
/// different -- GF(256) throughout rather than GF(2), a six-element tuple
/// (LT part + PI part) instead of Raptor's three, and a K -> K' padding step
/// this scheme has and Raptor doesn't.
///
/// Encoding Symbol IDs (ESI, this class's public numbering, 0-based, same
/// convention as Raptor) map to the RFC's Internal Symbol IDs (ISI, what
/// the tuple/encoding generators actually take) as: ISI == ESI for source
/// ESIs (< K), and ISI == ESI + (K' - K) for repair ESIs (>= K) -- see RFC
/// 6330 Section 5.3.2's closing paragraph. The K' - K padding symbols
/// (ISI in [K, K')) are always zero and are seeded into the linear system
/// up front, for free, in the constructor -- exactly like the S+H
/// pre-coding rows -- so decoding only ever needs K genuinely-received
/// symbols' worth of new information, not K'.
class RaptorQCodec : public FecBlockCodec {
  public:
    /// @param K number of source symbols in the block (RFC 6330 supports K
    /// up to K'_max = 56403; this K may be smaller, it's padded up to the
    /// nearest supported K' internally).
    explicit RaptorQCodec(uint32_t K);

    uint32_t K() const { return _K; }
    uint32_t Kprime() const { return _params.Kprime; }
    uint32_t L() const { return _params.L; }

    std::vector<std::vector<uint8_t>> compute_intermediate_symbols(
        const std::vector<std::vector<uint8_t>>& source_symbols) override;

    std::vector<uint8_t> generate_encoding_symbol(
        uint32_t esi, const std::vector<std::vector<uint8_t>>& intermediate_symbols) const override;

    bool add_received_symbol(uint32_t esi, const std::vector<uint8_t>& data) override;

    bool can_decode() const override { return _system.fully_determined(); }
    uint32_t symbols_needed() const override { return _system.num_unknowns() - _system.rank(); }

    std::vector<std::vector<uint8_t>> decode_source_symbols() override;

  private:
    uint32_t esi_to_isi(uint32_t esi) const {
      return (esi < _K) ? esi : esi + (_params.Kprime - _K);
    }

    uint32_t _K;
    PreCodeParams _params;
    GF256LinearSystem _system;
};

} // namespace RaptorQ
} // namespace LibFlute
