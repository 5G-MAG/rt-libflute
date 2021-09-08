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
#include <cstdio>
#include <cstring>
#include <iostream>
#include <cmath>
#include <arpa/inet.h>
#include "EncodingSymbol.h"

auto LibFlute::EncodingSymbol::from_payload(char* encoded_data, size_t data_len, const FecOti& fec_oti, ContentEncoding encoding) -> std::vector<EncodingSymbol> 
{
  auto source_block_number = 0;
  auto encoding_symbol_id = 0;
  std::vector<EncodingSymbol> symbols;

  if (encoding != ContentEncoding::NONE) {
    throw "Only unencoded content is supported";
  }
  
  if (fec_oti.encoding_id == FecScheme::CompactNoCode) {
    source_block_number = ntohs(*(uint16_t*)encoded_data);
    encoded_data += 2;
    encoding_symbol_id = ntohs(*(uint16_t*)encoded_data);
    encoded_data += 2;
    data_len -= 4;
  } else {
    throw "Only compact no-code FEC is supported";
  }

  int nof_symbols = std::ceil((float)data_len / (float)fec_oti.encoding_symbol_length);
  for (int i = 0; i < nof_symbols; i++) {
    if (fec_oti.encoding_id == FecScheme::CompactNoCode) {
      symbols.emplace_back(encoding_symbol_id, source_block_number, encoded_data, std::min(data_len, (size_t)fec_oti.encoding_symbol_length), fec_oti.encoding_id);
    }
    encoded_data += fec_oti.encoding_symbol_length;
    encoding_symbol_id++;
  }

  return symbols;
}

auto LibFlute::EncodingSymbol::to_payload(const std::vector<EncodingSymbol>& symbols, char* encoded_data, size_t data_len, const FecOti& fec_oti, ContentEncoding encoding) -> size_t
{
  size_t len = 0;
  auto ptr = encoded_data;
  auto first_symbol = symbols.begin();
  if (fec_oti.encoding_id == FecScheme::CompactNoCode) {
    *((uint16_t*)ptr) = htons(first_symbol->source_block_number());
    ptr += 2;
    *((uint16_t*)ptr) = htons(first_symbol->id());
    ptr += 2;
    len += 4;
  } else {
    throw "Only compact no-code FEC is supported";
  }

  for (const auto& symbol : symbols) {
    if (symbol.len() <= data_len) {
      auto symbol_len = symbol.encode_to(ptr, data_len);
      data_len -= symbol_len;
      encoded_data += symbol_len;
      len += symbol_len;
    }
  }
  return len;
}

auto LibFlute::EncodingSymbol::decode_to(char* buffer, size_t max_length) const -> void {
  if (_fec_scheme == FecScheme::CompactNoCode) {
    if (_data_len <= max_length) {
      memcpy(buffer, _encoded_data, _data_len);
    }
  }
}

auto LibFlute::EncodingSymbol::encode_to(char* buffer, size_t max_length) const -> size_t {
  if (_fec_scheme == FecScheme::CompactNoCode) {
    if (_data_len <= max_length) {
      memcpy(buffer, _encoded_data, _data_len);
      return _data_len;
    }
  }
  return 0;
}
