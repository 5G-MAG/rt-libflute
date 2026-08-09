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
    // Cast to uint64_t *before* shifting: ntohl() returns uint32_t, and
    // shifting that left by tsi_shift==16 in 32-bit arithmetic silently
    // discards its own top 16 bits (the high bits of a 48-bit TSI) before
    // the |= ever promotes it to 64 bits. Latent until AlcPacket's send-side
    // constructor actually started emitting tsi_flag==1 (see AlcPacket's
    // encode constructor) -- previously unreachable in practice.
    _tsi |= (uint64_t)ntohl(*(uint32_t*)hdr_ptr) << tsi_shift;
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
        // Same uint64_t-before-shift fix as the TSI decode above (this
        // case's own case 2, below, already got it right).
        _toi |= (uint64_t)ntohl(*(uint32_t*)hdr_ptr) << toi_shift;
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

  // The LCT Codepoint (CP) field carries the FEC Encoding ID of the scheme in
  // use for this packet's content (RFC 5651 §4.2); this library sets it to
  // the same numeric values as FecScheme's IANA-registered ones (0, 1, 6),
  // so the mapping back is direct.
  switch (_lct_header.codepoint) {
    case 0: _fec_oti.encoding_id = FecScheme::CompactNoCode; break;
    case 1: _fec_oti.encoding_id = FecScheme::Raptor; break;
    case 6: _fec_oti.encoding_id = FecScheme::RaptorQ; break;
    default: throw std::runtime_error("Unsupported FEC scheme (codepoint " + std::to_string(_lct_header.codepoint) + ")");
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
                      // Common FEC OTI (RFC 5052 §5.2): 10 octets of Transfer
                      // Length (48 bits, in the high bits of a 6-octet field),
                      // 16 reserved bits, and Encoding Symbol Length (16
                      // bits) -- the same for every scheme. hel covers this
                      // plus 4 octets of scheme-specific OTI (hel==4 total).
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

                      if (_fec_oti.encoding_id == FecScheme::CompactNoCode) {
                        // Scheme-specific OTI (RFC 5052 §5.1): 32-bit Maximum
                        // Source Block Length.
                        _fec_oti.max_source_block_length = ntohl(*(uint32_t*)ext_ptr);
                      } else if (_fec_oti.encoding_id == FecScheme::Raptor) {
                        // Scheme-specific OTI (RFC 5053 §3.2.3): 16-bit Z
                        // (number of source blocks), 8-bit N (sub-blocks,
                        // always 1 in this implementation), 8-bit Al (symbol
                        // alignment).
                        _fec_oti.nof_source_blocks = ntohs(*(uint16_t*)ext_ptr);
                        ext_ptr += 2;
                        _fec_oti.nof_sub_blocks = *(uint8_t*)ext_ptr;
                        ext_ptr += 1;
                        _fec_oti.symbol_alignment = *(uint8_t*)ext_ptr;
                      } else if (_fec_oti.encoding_id == FecScheme::RaptorQ) {
                        // Scheme-specific OTI (RFC 6330 §3.3.3): note the
                        // field widths differ from Raptor's despite the
                        // same 4-octet total -- 8-bit Z, 16-bit N (always 1
                        // here), 8-bit Al.
                        _fec_oti.nof_source_blocks = *(uint8_t*)ext_ptr;
                        ext_ptr += 1;
                        _fec_oti.nof_sub_blocks = ntohs(*(uint16_t*)ext_ptr);
                        ext_ptr += 2;
                        _fec_oti.symbol_alignment = *(uint8_t*)ext_ptr;
                      } else {
                        throw std::runtime_error("EXT_FTI parsing not implemented for this FEC scheme yet");
                      }
                      _has_fti = true;
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
                               bool close_session, bool close_object)
  : _fec_oti(fec_oti)
{
  const size_t max_alc_header_size = 4;

  // Choose the narrowest half_word_flag(H)/tsi_flag(S) combination able to carry the
  // actual TSI value on the wire (RFC 5651 SS4.2): H=1 emits a 16-bit half-word for
  // *both* TSI and TOI; S adds a further 32-bit TSI field on top of that half-word.
  // With H=0 there is no half-word at all, so a TOI field is only present if its own
  // flag (O) is set -- this library's TOI never exceeds 16 bits, so O=1 here is only
  // ever a side effect of clearing H to make room for a wider TSI, not a widening of
  // TOI's own range.
  uint8_t half_word_flag;
  uint8_t tsi_flag;
  uint8_t toi_flag;
  if (tsi <= 0xFFFFULL) {
    half_word_flag = 1; tsi_flag = 0; toi_flag = 0; // TSI+TOI: 16 bits each (half-word only)
  } else if (tsi <= 0xFFFFFFFFULL) {
    half_word_flag = 0; tsi_flag = 1; toi_flag = 1;  // TSI: 32 bits; TOI: 32 bits (H=0 requires O=1 for a TOI field to be present at all)
  } else if (tsi <= 0xFFFFFFFFFFFFULL) {
    half_word_flag = 1; tsi_flag = 1; toi_flag = 0; // TSI: 48 bits (16-bit half-word + 32-bit field); TOI: 16 bits (half-word only)
  } else {
    throw std::runtime_error("TSI exceeds the 48-bit maximum representable in an LCT header");
  }

  auto lct_header_len = 2 // LCT header word + CCI word
    + half_word_flag       // 1 word: 16-bit TSI half-word + 16-bit TOI half-word
    + tsi_flag             // 1 word: 32-bit TSI field
    + toi_flag;            // 1 word: 32-bit TOI field
  if (toi == 0) { // Add extensions for FDT
    lct_header_len += 5;
  }

  auto max_packet_length = max_encoding_symbol_size +
    lct_header_len * 4
    + max_alc_header_size ;

  _buffer = (char*)calloc(max_packet_length, sizeof(char));

  auto lct_header = (lct_header_t*)_buffer;

  lct_header->version = 1;
  lct_header->half_word_flag = half_word_flag;
  lct_header->tsi_flag = tsi_flag;
  lct_header->toi_flag = toi_flag;
  lct_header->close_session_flag = close_session ? 1 : 0;
  lct_header->close_object_flag = close_object ? 1 : 0;
  lct_header->lct_header_len = lct_header_len;
  auto hdr_ptr = _buffer + 4;
  auto payload_ptr = _buffer + 4 * lct_header_len;

  auto payload_size = EncodingSymbol::to_payload(symbols, payload_ptr, max_encoding_symbol_size + max_alc_header_size, _fec_oti, ContentEncoding::NONE);
  _len = 4 * lct_header_len + payload_size;

  hdr_ptr += 4; // CCI = 0

  // TSI: half-word (low 16 bits) if H=1, then a 32-bit field if S=1 -- carrying
  // either the full value (H=0) or the remaining upper bits (H=1, shifted by 16),
  // mirroring the receive-side decoder's tsi_shift logic exactly.
  if (half_word_flag) {
    *((uint16_t*)hdr_ptr) = htons(static_cast<uint16_t>(tsi & 0xFFFFULL));
    hdr_ptr += 2;
  }
  if (tsi_flag) {
    uint32_t tsi_field = half_word_flag
      ? static_cast<uint32_t>((tsi >> 16) & 0xFFFFFFFFULL)
      : static_cast<uint32_t>(tsi & 0xFFFFFFFFULL);
    *((uint32_t*)hdr_ptr) = htonl(tsi_field);
    hdr_ptr += 4;
  }

  // TOI: same half-word/field structure as TSI, but this library's TOI value
  // itself is always <= 16 bits, so the 32-bit field (when present, O=1) just
  // carries the same value zero-extended, not extra range.
  if (half_word_flag) {
    *((uint16_t*)hdr_ptr) = htons(toi);
    hdr_ptr += 2;
  }
  if (toi_flag) {
    *((uint32_t*)hdr_ptr) = htonl(static_cast<uint32_t>(toi));
    hdr_ptr += 4;
  }

  if (toi == 0) { // Add extensions for FDT
    *((uint8_t*)hdr_ptr) = EXT_FDT;
    hdr_ptr += 1;
    // FLUTE version nibble (RFC 6726 SS3.1/SS3.4.1): this library implements
    // FLUTE v2, so this MUST be 2, not 1 -- the receive-side EXT_FDT parser
    // already tolerates 0/1/2 here (see the flute_version > 2 check above).
    *((uint8_t*)hdr_ptr) = 2 << 4 | (fdt_instance_id & 0x000F0000) >> 16;
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
