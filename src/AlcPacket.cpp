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

  /* Everything below walks the header using lengths taken from the packet itself, so the packet's
     own claim about its header size is validated first, against both the flags that imply a
     minimum and the number of bytes actually received.

     RFC 3451 clause 5.1 on the field being trusted here:
     "Total length of the LCT header in units of 32-bit words."

     Two ways this goes wrong without the checks. A header claiming fewer words than its own
     flags require makes the extension-space calculation below negative, which becomes a very
     large size_t. A header claiming more words than were received sends every read past the end
     of the buffer. Both are reachable from one datagram, so neither is a theoretical concern.
     `code-derived, no spec claim` beyond the field's own definition. */
  /* RFC 3451 clause 5.1 places two optional 32-bit fields inside the header, after the TOI and
     before any header extension, each present only when its flag is set:
     "Sender Current Time (SCT, if T = 1)" and "Expected Residual Time (ERT, if R = 1)".
     Both count toward HDR_LEN. Omitting them made a conformant peer's SCT and ERT words get
     walked as HET/HEL extension pairs, corrupting extension parsing.

     TS 26.346 V18.2.0 clause L.4.7 on the MBMS side: "The network should set these flags/fields
     to zero, and the UE should ignore them." Ignoring a field still means stepping over it, so
     this is needed in both profiles: robustness against a non-conformant sender under the 3GPP
     profile, plain correctness under general FLUTE. */
  const size_t standard_header_words = 2 +
    _lct_header.congestion_control_flag +
    _lct_header.half_word_flag +
    _lct_header.tsi_flag +
    _lct_header.toi_flag +
    _lct_header.sct_flag +
    _lct_header.ert_flag;

  if (_lct_header.lct_header_len < standard_header_words) {
    throw std::runtime_error("LCT header length is shorter than its own flags require");
  }
  if ((size_t)_lct_header.lct_header_len * 4 > len) {
    throw std::runtime_error("LCT header length exceeds the received packet length");
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

  // Step over the SCT and ERT words when present, so the extension walk below starts where the
  // extensions actually begin. RFC 3451 clause 5.1 field order is CCI, TSI, TOI, SCT, ERT.
  if (_lct_header.sct_flag) hdr_ptr += 4;
  if (_lct_header.ert_flag) hdr_ptr += 4;

  if (_lct_header.codepoint == 0) {
    _fec_oti.encoding_id = FecScheme::CompactNoCode;
  } else {
    throw std::runtime_error("Only Compact No-Code FEC is supported");
  }

  /* RFC 3451 clause 5.1: "if HDR_LEN is larger than the length of the standard header then the
     remaining header space is taken by Header Extension fields." Both terms were validated
     above, so this subtraction cannot wrap. */
  size_t ext_header_len = ((size_t)_lct_header.lct_header_len - standard_header_words) * 4;
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

    /* A variable-length extension declaring HEL 0 gives a zero-length extension, which the
       bound below does not catch: the loop would then consume nothing and never terminate on a
       single malformed packet. HEL counts the whole extension including its own HET and HEL
       bytes, so zero is never legitimate. */
    if (ext_len == 0) {
      throw std::runtime_error("Header extension declares a zero length");
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
                        _has_fti = true;
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

LibFlute::AlcPacket::AlcPacket(uint64_t tsi, uint16_t toi, LibFlute::FecOti fec_oti, const std::vector<LibFlute::EncodingSymbol>& symbols, size_t max_encoding_symbol_size, uint32_t fdt_instance_id,
                                bool close_session_flag, bool close_object_flag)
  : _fec_oti(fec_oti)
{
  // TSI width: this wire scheme always carries a 16-bit half-word component (half_word_flag=1,
  // shared with TOI's own 16-bit half-word below) plus, when tsi_flag=1, an extra 32-bit word
  // holding the high-order bits -- giving a 48-bit ceiling, matching the decoder in this same
  // file. Values that fit in 16 bits keep the original on-wire size; anything larger sets
  // tsi_flag and adds the extra word, instead of silently truncating to the low 16 bits.
  if (tsi > 0xFFFFFFFFFFFFULL) {
    throw std::runtime_error("TSI exceeds the 48-bit field width supported by this LCT encoding");
  }
  const bool wide_tsi = tsi > 0xFFFF;

  const size_t max_alc_header_size = 4;
  auto lct_header_len = 3;
  if (wide_tsi) {
    lct_header_len += 1;
  }
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
  lct_header->tsi_flag = wide_tsi ? 1 : 0;
  lct_header->close_session_flag = close_session_flag ? 1 : 0;
  lct_header->close_object_flag = close_object_flag ? 1 : 0;
  lct_header->lct_header_len = lct_header_len;
  auto hdr_ptr = _buffer + 4;
  auto payload_ptr = _buffer + 4 * lct_header_len;

  auto payload_size = EncodingSymbol::to_payload(symbols, payload_ptr, max_encoding_symbol_size + max_alc_header_size, _fec_oti, ContentEncoding::NONE);
  _len = 4 * lct_header_len + payload_size;

  hdr_ptr += 4; // CCI = 0

  *((uint16_t*)hdr_ptr) = htons(static_cast<uint16_t>(tsi & 0xFFFF));
  hdr_ptr += 2;

  if (wide_tsi) {
    *((uint32_t*)hdr_ptr) = htonl(static_cast<uint32_t>(tsi >> 16));
    hdr_ptr += 4;
  }

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
