#include <cstdio>
#include <cstring>
#include <iostream>
#include <arpa/inet.h>
#include "AlcPacket.h"

LibFlute::AlcPacket::AlcPacket(char* data, size_t len)
{
  if (len < 4) {
    throw "Packet too short";
  }

  std::memcpy(&_lct_header, data, 4);
  if (_lct_header.version != 1) {
    throw "Unsupported LCT version";
  }

  char* hdr_ptr = data + 4;
  if (_lct_header.congestion_control_flag != 0) {
    throw "Unsupported CCI field length";
  }
  // [TODO] read CCI
  hdr_ptr += 4;

  if (_lct_header.half_word_flag == 0 && _lct_header.tsi_flag == 0) {
    throw "TSI field not present";
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
    throw "TOI field not present";
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
          throw "TOI fields over 64 bits in length are not supported";
        } else {
          _toi = ntohl(*(uint32_t*)hdr_ptr);
          hdr_ptr += 4;
          _toi |= (uint64_t)(ntohl(*(uint32_t*)hdr_ptr)) << 32;
          hdr_ptr += 4;
        }
        break;
      default:
        throw "TOI fields over 64 bits in length are not supported";
        break;
  } 

  if (_lct_header.codepoint == 0) {
    _fec_oti.encoding_id = FecScheme::CompactNoCode;
  } else {
    throw "Only Compact No-Code FEC is supported";
  }

  auto expected_header_len = 2 +
   _lct_header.congestion_control_flag +
   _lct_header.half_word_flag +
   _lct_header.tsi_flag +
   _lct_header.toi_flag;

  auto ext_header_len = (_lct_header.lct_header_len - expected_header_len) * 4;

  while (ext_header_len > 0) {
    uint8_t het = *hdr_ptr;
    hdr_ptr += 1;
    uint8_t hel = 0;
    if (het < 128) {
      hel = *hdr_ptr;
      hdr_ptr += 1;
    }

    switch ((AlcPacket::HeaderExtension)het) {
      case EXT_NOP: 
      case EXT_AUTH: 
      case EXT_TIME:  {
                        hdr_ptr += 3;
                        break; // ignored
                      }
      case EXT_FTI: {
                      if (_fec_oti.encoding_id == FecScheme::CompactNoCode) {
                        if (hel != 4) {
                          throw "Invalid length for EXT_FTI header extension";
                        }
                        _fec_oti.transfer_length = (uint64_t)(ntohs(*(uint16_t*)hdr_ptr)) << 32;
                        hdr_ptr += 2;
                        _fec_oti.transfer_length |= (uint64_t)(ntohl(*(uint32_t*)hdr_ptr));
                        hdr_ptr += 4;
                        hdr_ptr += 2; // reserved
                        _fec_oti.encoding_symbol_length = ntohs(*(uint16_t*)hdr_ptr);
                        hdr_ptr += 2;
                        _fec_oti.max_source_block_length = ntohl(*(uint32_t*)hdr_ptr);
                        hdr_ptr += 4;
                      }
                      break; 
                    }
      case EXT_FDT: {
                      uint8_t flute_version = (*hdr_ptr & 0xF0) >> 4;
                      if (flute_version > 2) {
                        throw "Unsupported FLUTE version";
                      }
                      _fdt_instance_id =  (*hdr_ptr & 0x0F) << 16;
                      hdr_ptr++;
                      _fdt_instance_id |= ntohs(*(uint16_t*)hdr_ptr);
                      hdr_ptr += 2;
                      break; 
                    }
      case EXT_CENC: {
                       uint8_t encoding = *hdr_ptr;
                       switch (encoding) {
                         case 0: _content_encoding = ContentEncoding::NONE; break;
                         case 1: _content_encoding = ContentEncoding::ZLIB; break;
                         case 2: _content_encoding = ContentEncoding::DEFLATE; break;
                         case 3: _content_encoding = ContentEncoding::GZIP; break;
                       }
                       hdr_ptr += 3;
                       break; 
                     }
    }

    ext_header_len -= 4;
    ext_header_len -= hel * 4;
  }
}

LibFlute::AlcPacket::AlcPacket(uint16_t tsi, uint16_t toi, LibFlute::FecOti fec_oti, const std::vector<LibFlute::EncodingSymbol>& symbols, size_t max_size, uint32_t fdt_instance_id)
  : _fec_oti(fec_oti)
{
  auto lct_header_len = 3;
  if (toi == 0) { // Add extensions for FDT
    lct_header_len += 5;
  }

  auto max_packet_length = max_size +
    lct_header_len * 4
    + 4 ;

  _buffer = (char*)calloc(max_packet_length, sizeof(char));

  auto lct_header = (lct_header_t*)_buffer;

  lct_header->version = 1;
  lct_header->half_word_flag = 1;
  lct_header->lct_header_len = lct_header_len;
  auto hdr_ptr = _buffer + 4;
  auto payload_ptr = _buffer + 4 * lct_header_len;

  auto payload_size = EncodingSymbol::to_payload(symbols, payload_ptr, max_size, _fec_oti, ContentEncoding::NONE);
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
    *((uint16_t*)hdr_ptr) = htons((_fec_oti.transfer_length & 0x00FF0000) >> 32);
    hdr_ptr += 2;
    *((uint32_t*)hdr_ptr) = htonl(_fec_oti.transfer_length & 0x0000FFFF);
    hdr_ptr += 4;
    hdr_ptr += 2; // reserved
    *((uint16_t*)hdr_ptr) = htons(_fec_oti.encoding_symbol_length);
    hdr_ptr += 2;
    *((uint32_t*)hdr_ptr) = htonl(_fec_oti.max_source_block_length);
    hdr_ptr += 4;
  }



  /*
  memset(&_lct_header, 0, sizeof(_lct_header));
  _lct_header.version = 1;
  _lct_header.half_word_flag = 1;
  if (fec_scheme == LibFlute::FecScheme::CompactNoCode) {
    _lct_header.codepoint = 0;
  } else {
    throw "Only Compact No-Code FEC is supported";
  }
  */

}

LibFlute::AlcPacket::~AlcPacket()
{
  if (_buffer) free(_buffer);
}
