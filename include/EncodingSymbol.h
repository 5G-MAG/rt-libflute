// libflute - FLUTE/ALC library
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vector>
#include "flute_types.h"

namespace LibFlute {
  class EncodingSymbol {
    public:
      EncodingSymbol(uint32_t id, uint32_t source_block_number, char* encoded_data, size_t data_len, FecScheme fec_scheme)
        : _id(id)
        , _source_block_number(source_block_number)
        , _encoded_data(encoded_data)
        , _data_len(data_len)
        , _fec_scheme(fec_scheme) {}
      virtual ~EncodingSymbol() {};

      static std::vector<EncodingSymbol> from_payload(char* encoded_data, size_t data_len, const FecOti& fec_oti, ContentEncoding encoding);
      static size_t to_payload(const std::vector<EncodingSymbol>&, char* encoded_data, size_t data_len, const FecOti& fec_oti, ContentEncoding encoding);

      uint32_t id() const { return _id; };
      uint32_t source_block_number() const { return _source_block_number; };

      void decode_to(char* buffer, size_t max_length) const;
      size_t encode_to(char* buffer, size_t max_length) const;
      size_t len() const { return _data_len; };

    private:
      uint32_t _id = 0;
      uint32_t _source_block_number = 0;
      FecScheme _fec_scheme;
      char* _encoded_data;
      size_t _data_len;
  };
};
