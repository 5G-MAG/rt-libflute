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
#include <cstring>
#include <iostream>
#include <arpa/inet.h>
#include "AlcPacket.h"

LibFlute::AlcPacket::AlcPacket(char* data, size_t len)
{
  if (len < 4) {
    throw std::runtime_error("Packet too short");
  }

  std::memcpy(&_lct_header, data, 4);
  if (_lct_header.version != 1) {
    throw std::runtime_error("Unsupported LCT version");
  }

  char* hdr_ptr = data + 4;
  if (_lct_header.congestion_control_flag != 0) {
    throw std::runtime_error("Unsupported CCI field length");
  }
  // [TODO] read CCI
  hdr_ptr += 4;

  if (_lct_header.half_word_flag == 0 && _lct_header.tsi_flag == 0) {
    throw std::runtime_error("TSI field not present");
  }
  auto tsi_shift = 0;
  if(_lct_header.half_word_flag == 1) {
    _tsi = ntohs(*(uint16_t*)hdr_ptr);
    tsi_shift = 16;
    hdr_ptr += 2;
  } 
  if(_lct_header.tsi_flag == 1) {
    _tsi |= ntohl(*(uint32_t*)hdr_ptr) << tsi_shift;
    hdr_ptr += 4;
  } 

  if ( _lct_header.close_session_flag == 0 && _lct_header.half_word_flag == 0 && _lct_header.toi_flag == 0) {
    throw std::runtime_error("TOI field not present");
  }
  auto toi_shift = 0;
  if(_lct_header.half_word_flag == 1) {
    _toi = ntohs(*(uint16_t*)hdr_ptr);
    toi_shift = 16;
    hdr_ptr += 2;
  } 
  switch(_lct_header.toi_flag) {
      case 0: break;
      case 1: 
        _toi |= ntohl(*(uint32_t*)hdr_ptr) << toi_shift;
        hdr_ptr += 4;
        break;
      case 2:
        if (toi_shift > 0) {
          throw std::runtime_error("TOI fields over 64 bits in length are not supported");
        } else {
          _toi = ntohl(*(uint32_t*)hdr_ptr);
          hdr_ptr += 4;
          _toi |= (uint64_t)(ntohl(*(uint32_t*)hdr_ptr)) << 32;
          hdr_ptr += 4;
        }
        break;
      default:
        throw std::runtime_error("TOI fields over 64 bits in length are not supported");
  } 

  if (_lct_header.codepoint == 0) {
    _fec_oti.encoding_id = FecScheme::CompactNoCode;
  } else {
    throw std::runtime_error("Only Compact No-Code FEC is supported");
  }

  auto expected_header_len = 2 +
   _lct_header.congestion_control_flag +
   _lct_header.half_word_flag +
   _lct_header.tsi_flag +
   _lct_header.toi_flag;

  size_t ext_header_len = (_lct_header.lct_header_len - expected_header_len) * 4;
  while (ext_header_len > 0) {
    auto ext_ptr = hdr_ptr;
    uint8_t het = *ext_ptr;
    ext_ptr += 1; // Skip HET
    uint8_t hel = 0;
    size_t ext_len = 4;
    if (het <= 127) {
      hel = *ext_ptr;
      ext_len = hel * 4;
      ext_ptr += 1; // Skip HEL
    }

    if (ext_len > ext_header_len) {
      throw std::runtime_error("Header extension length exceeds remaining header length");
    }

    switch ((AlcPacket::HeaderExtension)het) {
      case EXT_NOP:
      case EXT_AUTH:
      case EXT_TIME:  {
                        break; // ignored
                      }
      case EXT_FTI: {
                      if (_fec_oti.encoding_id == FecScheme::CompactNoCode) {
                        if (hel != 4) {
                          throw std::runtime_error("Invalid length for EXT_FTI header extension");
                        }
                        _fec_oti.transfer_length = (uint64_t)(ntohs(*(uint16_t*)ext_ptr)) << 32;
                        ext_ptr += 2;
                        _fec_oti.transfer_length |= (uint64_t)(ntohl(*(uint32_t*)ext_ptr));
                        ext_ptr += 4;
                        ext_ptr += 2; // reserved
                        _fec_oti.encoding_symbol_length = ntohs(*(uint16_t*)ext_ptr);
                        ext_ptr += 2;
                        _fec_oti.max_source_block_length = ntohl(*(uint32_t*)ext_ptr);
                      }
                      break;
                    }
      case EXT_FDT: {
                      uint8_t flute_version = (*ext_ptr & 0xF0) >> 4;
                      if (flute_version > 2) {
                        throw std::runtime_error("Unsupported FLUTE version");
                      }
                      _fdt_instance_id =  (*ext_ptr & 0x0F) << 16;
                      ext_ptr++;
                      _fdt_instance_id |= ntohs(*(uint16_t*)ext_ptr);
                      break;
                    }
      case EXT_CENC: {
                       uint8_t encoding = *ext_ptr;
                       switch (encoding) {
                         case 0: _content_encoding = ContentEncoding::NONE; break;
                         case 1: _content_encoding = ContentEncoding::ZLIB; break;
                         case 2: _content_encoding = ContentEncoding::DEFLATE; break;
                         case 3: _content_encoding = ContentEncoding::GZIP; break;
                       }
                       break;
                     }
    }

    ext_header_len -= ext_len;
    hdr_ptr += ext_len;
  }
}

LibFlute::AlcPacket::AlcPacket(uint16_t tsi, uint16_t toi, LibFlute::FecOti fec_oti, const std::vector<LibFlute::EncodingSymbol>& symbols, size_t max_encoding_symbol_size, uint32_t fdt_instance_id)
  : _fec_oti(fec_oti)
{
  const size_t max_alc_header_size = 4;
  auto lct_header_len = 3;
  if (toi == 0) { // Add extensions for FDT
    lct_header_len += 5;
  }

  auto max_packet_length = max_encoding_symbol_size +
    lct_header_len * 4
    + max_alc_header_size ;

  _buffer = (char*)calloc(max_packet_length, sizeof(char));

  auto lct_header = (lct_header_t*)_buffer;

  lct_header->version = 1;
  lct_header->half_word_flag = 1;
  lct_header->lct_header_len = lct_header_len;
  auto hdr_ptr = _buffer + 4;
  auto payload_ptr = _buffer + 4 * lct_header_len;

  auto payload_size = EncodingSymbol::to_payload(symbols, payload_ptr, max_encoding_symbol_size + max_alc_header_size, _fec_oti, ContentEncoding::NONE);
  _len = 4 * lct_header_len + payload_size;
  
  hdr_ptr += 4; // CCI = 0
  
  *((uint16_t*)hdr_ptr) = htons(tsi);
  hdr_ptr += 2;
  
  *((uint16_t*)hdr_ptr) = htons(toi);
  hdr_ptr += 2;

  if (toi == 0) { // Add extensions for FDT
    *((uint8_t*)hdr_ptr) = EXT_FDT;
    hdr_ptr += 1;
    *((uint8_t*)hdr_ptr) = 1 << 4 | (fdt_instance_id & 0x000F0000) >> 16;
    hdr_ptr += 1;
    *((uint16_t*)hdr_ptr) = htons(fdt_instance_id & 0x0000FFFF);
    hdr_ptr += 2;

    *((uint8_t*)hdr_ptr) = EXT_FTI;
    hdr_ptr += 1;
    *((uint8_t*)hdr_ptr) = 4; // HEL
    hdr_ptr += 1;
    // EXT_FTI Transfer Length is a 48-bit field (RFC 5052 Compact No-Code FTI):
    // high 16 bits then low 32 bits, matching the decoder above
    // ((hi16 << 32) | lo32). The previous masks (& 0x00FF0000) >> 32 (always 0)
    // and & 0x0000FFFF (low 16 bits only) truncated the length to 16 bits, so any
    // object > 64 KB was signalled with a bogus tiny length -> the receiver
    // under-allocated source blocks and rejected the real symbols.
    *((uint16_t*)hdr_ptr) = htons(static_cast<uint16_t>((_fec_oti.transfer_length >> 32) & 0xFFFF));
    hdr_ptr += 2;
    *((uint32_t*)hdr_ptr) = htonl(static_cast<uint32_t>(_fec_oti.transfer_length & 0xFFFFFFFF));
    hdr_ptr += 4;
    hdr_ptr += 2; // reserved
    *((uint16_t*)hdr_ptr) = htons(_fec_oti.encoding_symbol_length);
    hdr_ptr += 2;
    *((uint32_t*)hdr_ptr) = htonl(_fec_oti.max_source_block_length);
  }
}

LibFlute::AlcPacket::~AlcPacket()
{
  if (_buffer) free(_buffer);
}
