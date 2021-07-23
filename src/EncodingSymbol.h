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
