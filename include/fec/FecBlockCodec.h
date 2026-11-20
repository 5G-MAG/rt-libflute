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

namespace LibFlute {

/// Shared shape of a per-source-block FEC codec, implemented by both
/// Raptor::RaptorCodec (RFC 5053) and RaptorQ::RaptorQCodec (RFC 6330).
/// Lets File hold one polymorphic codec per source block instead of
/// duplicating its Raptor-family handling once per scheme -- the two
/// schemes' internal maths are quite different (GF(2) vs GF(256), three-
/// vs six-element tuples, no K->K' padding step vs one), but from File's
/// point of view they're both "feed it symbols, ask if it can decode yet".
class FecBlockCodec {
  public:
    virtual ~FecBlockCodec() = default;

    /// Encoder: solve for the intermediate symbols given all K source
    /// symbols (each the same fixed length).
    virtual std::vector<std::vector<uint8_t>> compute_intermediate_symbols(
        const std::vector<std::vector<uint8_t>>& source_symbols) = 0;

    /// Produce the encoding symbol for ESI `esi` (source or repair) from
    /// already-computed intermediate symbols.
    virtual std::vector<uint8_t> generate_encoding_symbol(
        uint32_t esi, const std::vector<std::vector<uint8_t>>& intermediate_symbols) const = 0;

    /// Decoder: register one received encoding symbol. Returns true if it
    /// was independent of everything received so far.
    virtual bool add_received_symbol(uint32_t esi, const std::vector<uint8_t>& data) = 0;

    virtual bool can_decode() const = 0;
    virtual uint32_t symbols_needed() const = 0;

    /// Decoder: once can_decode(), recover all K source symbols.
    virtual std::vector<std::vector<uint8_t>> decode_source_symbols() = 0;
};

} // namespace LibFlute
