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

LibFlute::AlcPacket::AlcPacket(char* data, size_t len, uint8_t expected_flute_version)
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
  /* Version 2 sits on RFC 5651, which deleted both fields and reassigned their flag bits.
     RFC 5651 clause 11: "Removal of the Sender Current Time and Expected Residual Time LCT
     header fields." RFC 6726 clause 11.1 on what that means for the two bits: "In [RFC5651],
     these fields MUST be set to zero and MUST be ignored by receivers (instead, the EXT_TIME
     Header Extensions can convey this information if needed)." So under version 2 they contribute
     no words to the header and nothing is stepped over; under version 1 the RFC 3451 reading
     above still applies, unchanged. */
  const bool lct_carries_sct_ert = (expected_flute_version == 1);

  const size_t standard_header_words = 2 +
    _lct_header.congestion_control_flag +
    _lct_header.half_word_flag +
    _lct_header.tsi_flag +
    _lct_header.toi_flag +
    (lct_carries_sct_ert ? (_lct_header.sct_flag + _lct_header.ert_flag) : 0);

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
  if (lct_carries_sct_ert) {
    if (_lct_header.sct_flag) hdr_ptr += 4;
    if (_lct_header.ert_flag) hdr_ptr += 4;
  }

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
                        /* The FEC Instance ID, not a reserved field. RFC 3926 clause 5.1.1:
                           "It is only present if the value of FEC Encoding ID is in the range of
                           128-255. When the value of FEC Encoding ID is in the range of 0-127, this
                           field is set to 0." Compact No-Code is 0, so it is stepped over rather
                           than read; the width is the same either way. */
                        ext_ptr += 2;
                        _fec_oti.encoding_symbol_length = ntohs(*(uint16_t*)ext_ptr);
                        ext_ptr += 2;
                        _fec_oti.max_source_block_length = ntohl(*(uint32_t*)ext_ptr);
                        _has_fti = true;
                      }
                      break;
                    }
      case EXT_FDT: {
                      uint8_t flute_version = (*ext_ptr & 0xF0) >> 4;
                      /* The version field identifies which protocol the packet belongs to, and
                         the two are not interchangeable, so a packet is accepted only if it
                         carries the version this session was configured for. The default is 1,
                         which is what TS 26.346 selects, so a 3GPP session behaves exactly as it
                         does on the version 1 branch.

                         RFC 3926 clause 3.4.1: "This document specifies FLUTE version 1. Hence
                         in any ALC packet that carries FDT Instance and that belongs to the file
                         delivery session as specified in this specification MUST set this field
                         to '1'."

                         RFC 6726 clause 11.1: "Therefore, an implementation that relies on
                         [RFC3926] and RFC 3451 will not be backwards compatible with FLUTE as
                         specified in this document."

                         RFC 6726 clause 3.1 requires the receiver to tell sessions apart by
                         version: "If multiple FLUTE sessions are sent to a channel, then
                         receivers MUST determine the FLUTE protocol version, based on version
                         fields and the (source IP address, TSI) pair carried in the ALC/LCT
                         header of the packet." */
                      if (flute_version != expected_flute_version) {
                        throw std::runtime_error("FLUTE version " + std::to_string(flute_version) +
                                                 " in EXT_FDT, but this session is configured for "
                                                 "version " + std::to_string(expected_flute_version));
                      }
                      _flute_version = flute_version;
                      _fdt_instance_id =  (*ext_ptr & 0x0F) << 16;
                      ext_ptr++;
                      _fdt_instance_id |= ntohs(*(uint16_t*)ext_ptr);
                      break;
                    }
      case EXT_CENC: {
                       /* The set of algorithms is open: RFC 3926 clause 3.4.3 says of the CENC
                          field that "The definition of this field is outside the scope of this
                          specification." A value this library does not know therefore has to be
                          refused rather than ignored. Falling through to NONE would hand the
                          undecoded bytes to the FDT parser as though they were XML, which fails
                          somewhere further on with an error naming the wrong thing. */
                       uint8_t encoding = *ext_ptr;
                       switch (encoding) {
                         case 0: _content_encoding = ContentEncoding::NONE; break;
                         case 1: _content_encoding = ContentEncoding::ZLIB; break;
                         case 2: _content_encoding = ContentEncoding::DEFLATE; break;
                         case 3: _content_encoding = ContentEncoding::GZIP; break;
                         default:
                           throw std::runtime_error(
                               "EXT_CENC names content encoding " + std::to_string(encoding) +
                               ", which this library cannot decode");
                       }
                       break;
                     }
    }

    ext_header_len -= ext_len;
    hdr_ptr += ext_len;
  }
}

LibFlute::AlcPacket::AlcPacket(uint64_t tsi, uint16_t toi, LibFlute::FecOti fec_oti, const std::vector<LibFlute::EncodingSymbol>& symbols, size_t max_encoding_symbol_size, uint32_t fdt_instance_id,
                                bool close_session_flag, bool close_object_flag,
                                uint8_t flute_version)
  : _fec_oti(fec_oti), _flute_version(flute_version)
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
    /* FLUTE version nibble, from the version this session was configured for.

       RFC 3926 clause 3.4.1: "This document specifies FLUTE version 1. Hence in any ALC packet
       that carries FDT Instance and that belongs to the file delivery session as specified in
       this specification MUST set this field to '1'."

       RFC 6726 clause 3.4.1: "This document specifies FLUTE version 2. Hence, in any ALC packet
       that carries an FDT Instance and that belongs to the file delivery session as specified in
       this specification MUST set this field to '2'."

       The default is 1, which is what TS 26.346 V18.2.0 clause L.2 selects by referencing RFC
       3926. Version 2 is opt-in and is not for 3GPP MBMS use. */
    *((uint8_t*)hdr_ptr) = (flute_version & 0x0F) << 4 | (fdt_instance_id & 0x000F0000) >> 16;
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
    /* The FEC Instance ID, left at the zero the buffer already holds, which is what a
       Fully-Specified scheme requires.

       RFC 3926 clause 5.1.1: "When the value of FEC Encoding ID is in the range of 0-127, this
       field is set to 0." */
    hdr_ptr += 2;
    *((uint16_t*)hdr_ptr) = htons(_fec_oti.encoding_symbol_length);
    hdr_ptr += 2;
    *((uint32_t*)hdr_ptr) = htonl(_fec_oti.max_source_block_length);
  }
}

LibFlute::AlcPacket::AlcPacket(uint64_t tsi, CloseSession)
{
  /* RFC 3926 clause 3.1: "the exception that ALC packets sent in a FLUTE session with the Close
     Session (A) flag set to 1 (signaling the end of the session) and that contain no payload
     (carrying no information for any file or FDT) SHALL NOT carry the TOI"

     The TSI and TOI share the half-word flag, so dropping the TOI drops the half-word too and the
     TSI becomes a whole number of 32-bit words.
     RFC 5651 clause 5.1: "The TSI field is 32*S + 16*H
     bits in length"
     One word is what this builds, which caps the TSI at 32 bits; a session with a
     wider TSI cannot express this packet at all and says so rather than emitting a TOI the clause
     forbids. */
  if (tsi > 0xFFFFFFFFULL) {
    throw std::runtime_error(
        "a data-less Close Session packet carries no TOI, so its TSI must fit in 32 bits");
  }

  /* Base word, CCI, TSI. No TOI, no extensions, no FEC Payload ID, no payload. */
  const uint8_t lct_header_len = 3;
  _len = 4 * lct_header_len;
  _buffer = (char*)calloc(_len, sizeof(char));

  /* Written through the member so that this object's own accessors describe the packet it built,
     not just the bytes on the wire. */
  std::memset(&_lct_header, 0, sizeof(_lct_header));
  _lct_header.version = 1;
  _lct_header.half_word_flag = 0;
  _lct_header.tsi_flag = 1;
  _lct_header.toi_flag = 0;
  _lct_header.close_session_flag = 1;
  _lct_header.lct_header_len = lct_header_len;
  std::memcpy(_buffer, &_lct_header, sizeof(_lct_header));

  /* The CCI word stays at the zero calloc left. A session running a congestion control building
     block has nothing to say in a packet that carries no data. */
  *((uint32_t*)(_buffer + 8)) = htonl(static_cast<uint32_t>(tsi));
}

LibFlute::AlcPacket::~AlcPacket()
{
  if (_buffer) free(_buffer);
}
