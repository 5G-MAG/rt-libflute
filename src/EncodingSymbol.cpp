// libflute - FLUTE/ALC library
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
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
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <cmath>
#include <arpa/inet.h>
#include "EncodingSymbol.h"
#include "spdlog/spdlog.h"

auto LibFlute::EncodingSymbol::from_payload(char* encoded_data, size_t data_len, const FecOti& fec_oti, ContentEncoding encoding) -> std::vector<EncodingSymbol>
{
  auto source_block_number = 0;
  auto encoding_symbol_id = 0;
  std::vector<EncodingSymbol> symbols;

  if (encoding != ContentEncoding::NONE && encoding != ContentEncoding::GZIP) {
    throw std::runtime_error("Only unencoded or gzipped content is supported");
  }

  // The FEC Payload ID wire format (16-bit SBN + 16-bit ESI, network byte
  // order) is the same for Compact No-Code (RFC 5052 §5.1) and Raptor
  // (RFC 5053 §3.1): only the *meaning* of an ESI >= the source block's
  // symbol count differs (repair symbol vs. undefined).
  if (fec_oti.encoding_id == FecScheme::CompactNoCode ||
      fec_oti.encoding_id == FecScheme::Raptor) {
    source_block_number = ntohs(*(uint16_t*)encoded_data);
    encoded_data += 2;
    encoding_symbol_id = ntohs(*(uint16_t*)encoded_data);
    encoded_data += 2;
    data_len -= 4;
  } else {
    throw std::runtime_error("Unsupported FEC scheme");
  }

  int nof_symbols = std::ceil((float)data_len / (float)fec_oti.encoding_symbol_length);
  for (int i = 0; i < nof_symbols; i++) {
    symbols.emplace_back(encoding_symbol_id, source_block_number, encoded_data, std::min(data_len, (size_t)fec_oti.encoding_symbol_length), fec_oti.encoding_id);
    encoded_data += fec_oti.encoding_symbol_length;
    encoding_symbol_id++;
  }

  return symbols;
}

auto LibFlute::EncodingSymbol::to_payload(const std::vector<EncodingSymbol>& symbols, char* encoded_data, size_t data_len, const FecOti& fec_oti, ContentEncoding encoding) -> size_t
{
  size_t len = 0;
  auto ptr = encoded_data;

  /* The FEC Payload ID written below is taken from the first symbol, so an empty list dereferences
     end(). Reachable from the public API: AlcPacket's send-side constructor passes whatever vector
     it was given straight through, and nothing checked. An ALC packet with no encoding symbols has
     no payload to identify, so this is refused rather than silently emitting a header-only packet.
     RULES.md rule 12 prefers failing loudly over quietly producing something meaningless.
     `code-derived, no spec claim`. */
  if (symbols.empty()) {
    throw std::invalid_argument("Cannot build an ALC packet payload from an empty symbol list");
  }

  auto first_symbol = symbols.begin();
  bool scheme_supported = fec_oti.encoding_id == FecScheme::CompactNoCode ||
                           fec_oti.encoding_id == FecScheme::Raptor;
  if (scheme_supported && data_len >= 4) {
    *((uint16_t*)ptr) = htons(first_symbol->source_block_number());
    ptr += 2;
    *((uint16_t*)ptr) = htons(first_symbol->id());
    ptr += 2;
    len += 4;
    data_len -= 4;
  } else {
    throw std::runtime_error("Unsupported FEC scheme");
  }

  for (const auto& symbol : symbols) {
    if (symbol.len() <= data_len) {
      auto symbol_len = symbol.encode_to(ptr, data_len);
      data_len -= symbol_len;
      len += symbol_len;
      ptr += symbol_len;
    } else {
      spdlog::warn("Not enough space in payload buffer for encoding symbol");
      break;
    }
  }
  return len;
}

auto LibFlute::EncodingSymbol::decode_to(char* buffer, size_t max_length) const -> void {
  // An encoding symbol's on-the-wire bytes are already its final content --
  // for Raptor that's true just as much as for Compact No-Code: a
  // repair symbol's payload is the fully-computed LT combination by the time
  // it reaches this class. What to *do* with a repair symbol (recognising
  // its ESI is >= the source block's symbol count, feeding it to the block's
  // decoder, and reconstructing any missing source symbols) is File's job,
  // not this class's -- see File::put_symbol.
  if (_data_len <= max_length) {
    memcpy(buffer, _encoded_data, _data_len);
  }
}

auto LibFlute::EncodingSymbol::encode_to(char* buffer, size_t max_length) const -> size_t {
  if (_data_len <= max_length) {
    memcpy(buffer, _encoded_data, _data_len);
    return _data_len;
  }
  return 0;
}
