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
// Tests for FLUTE version 2 (RFC 6726) session selection.
//
// Version 1 (RFC 3926) and version 2 (RFC 6726) are separate protocols. RFC 6726 clause 11.1:
// "an implementation that relies on [RFC3926] and RFC 3451 will not be backwards compatible with
// FLUTE as specified in this document." These tests cover the version selector that keeps the two
// apart, not a complete version 2 implementation; see README-FLUTE-V2.md for what is missing.

#include <gtest/gtest.h>

#include <arpa/inet.h>

#include <cstring>
#include <map>
#include <vector>

#include "AlcPacket.h"
#include "EncodingSymbol.h"
#include "FileDeliveryTable.h"

using namespace LibFlute;

namespace {

std::vector<EncodingSymbol> make_symbols(const char* payload, size_t len) {
  std::vector<EncodingSymbol> symbols;
  symbols.emplace_back(0, 0, const_cast<char*>(payload), len, FecScheme::CompactNoCode);
  return symbols;
}

uint8_t version_nibble(const AlcPacket& packet) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(packet.data());
  EXPECT_EQ(bytes[12], 192); // EXT_FDT HET
  return (bytes[13] & 0xF0) >> 4;
}

}  // namespace

// RFC 3926 clause 3.4.1: "This document specifies FLUTE version 1. Hence in any ALC packet that
// carries FDT Instance and that belongs to the file delivery session as specified in this
// specification MUST set this field to '1'."
TEST(FluteV2, TransmitDefaultsToVersion1) {
  const char payload[] = "<FDT-Instance/>";
  auto symbols = make_symbols(payload, sizeof(payload) - 1);
  FecOti fec_oti{FecScheme::CompactNoCode, 0, sizeof(payload) - 1, 1400, 64, 0};

  AlcPacket packet(/*tsi*/1234, /*toi*/0, fec_oti, symbols, 1400, /*fdt_instance_id*/5);

  EXPECT_EQ(version_nibble(packet), 1u);
}

// RFC 6726 clause 3.4.1: "This document specifies FLUTE version 2. Hence, in any ALC packet that
// carries an FDT Instance and that belongs to the file delivery session as specified in this
// specification MUST set this field to '2'."
TEST(FluteV2, TransmitSignalsVersion2WhenSelected) {
  const char payload[] = "<FDT-Instance/>";
  auto symbols = make_symbols(payload, sizeof(payload) - 1);
  FecOti fec_oti{FecScheme::CompactNoCode, 0, sizeof(payload) - 1, 1400, 64, 0};

  AlcPacket packet(/*tsi*/1234, /*toi*/0, fec_oti, symbols, 1400, /*fdt_instance_id*/5,
                   /*close_session*/false, /*close_object*/false, /*flute_version*/2);

  EXPECT_EQ(version_nibble(packet), 2u);
  // The FDT Instance ID shares the octet with the version nibble, so confirm the wider field
  // survived the change of version.
  AlcPacket decoded(packet.data(), packet.size(), /*expected_flute_version*/2);
  EXPECT_EQ(decoded.fdt_instance_id(), 5u);
}

// RFC 6726 clause 3.1: "If multiple FLUTE sessions are sent to a channel, then receivers MUST
// determine the FLUTE protocol version, based on version fields and the (source IP address, TSI)
// pair carried in the ALC/LCT header of the packet."
TEST(FluteV2, ReceiverAcceptsOnlyTheVersionItWasConfiguredFor) {
  const char payload[] = "<FDT-Instance/>";
  auto symbols = make_symbols(payload, sizeof(payload) - 1);
  FecOti fec_oti{FecScheme::CompactNoCode, 0, sizeof(payload) - 1, 1400, 64, 0};

  AlcPacket v1(/*tsi*/1234, /*toi*/0, fec_oti, symbols, 1400, /*fdt_instance_id*/5);
  AlcPacket v2(/*tsi*/1234, /*toi*/0, fec_oti, symbols, 1400, /*fdt_instance_id*/5,
               false, false, /*flute_version*/2);

  // Matching version accepted.
  EXPECT_NO_THROW(AlcPacket(v1.data(), v1.size(), 1));
  EXPECT_NO_THROW(AlcPacket(v2.data(), v2.size(), 2));

  // Mismatched version refused in both directions: a version 2 session must not decode version 1
  // packets any more than a version 1 session may decode version 2 packets.
  EXPECT_THROW(AlcPacket(v2.data(), v2.size(), 1), std::runtime_error);
  EXPECT_THROW(AlcPacket(v1.data(), v1.size(), 2), std::runtime_error);
}

// RFC 6726 clause 3.4.2 defines the FDT schema with
// targetNamespace="urn:ietf:params:xml:ns:fdt".
TEST(FluteV2, FdtCarriesTheRfc6726Namespace) {
  /* General FLUTE: the 3GPP profiles derive their own schema and neither is RFC 6726's, so a
     version 2 session is by definition not one of them. */
  FileDeliveryTable fdt(1, FecOti{FecScheme::CompactNoCode, 0, 0, 1400, 64, 0},
                        FileDeliveryTable::FDT_NS_RFC6726, Profile::Unprofiled);
  auto xml = fdt.to_string();
  EXPECT_NE(xml.find("urn:ietf:params:xml:ns:fdt"), std::string::npos) << xml;
}


/* RFC 5651 clause 11 lists among its changes from RFC 3451: "Removal of the Sender Current Time
   and Expected Residual Time LCT header fields." RFC 6726 clause 11.1 says what a version 2 peer
   does with the two flag bits that carried them: "In [RFC5651], these fields MUST be set to zero
   and MUST be ignored by receivers (instead, the EXT_TIME Header Extensions can convey this
   information if needed)."

   The packets below are hand-built with T and R set and two words following the TOI, which a
   version 1 receiver must step over as SCT and ERT and a version 2 receiver must not. */
namespace {

std::vector<char> packet_with_t_and_r_set(const std::string& payload) {
  // 2 words (LCT header + CCI) + 1 word (TSI/TOI half-words) + 2 words (SCT, ERT) = 5
  const size_t header_words = 5;
  const size_t header_bytes = header_words * 4;
  std::vector<char> buf(header_bytes + 4 /* SBN+ESI */ + payload.size(), 0);
  auto* b = reinterpret_cast<unsigned char*>(buf.data());

  b[0] = (1 << 4);  // LCT version 1, which both RFC 3451 and RFC 5651 use
  /* Byte 1, low bit first: close_object, close_session, ert, sct, half_word, toi(2), tsi. */
  b[1] = 0x10 /* half_word */ | 0x08 /* sct */ | 0x04 /* ert */;
  b[2] = static_cast<unsigned char>(header_words);
  b[3] = 0;  // Compact No-Code

  uint16_t tsi_be = htons(1), toi_be = htons(7);
  std::memcpy(b + 8, &tsi_be, 2);
  std::memcpy(b + 10, &toi_be, 2);
  // Bytes 12-19 are the SCT and ERT words, left zero.
  std::memcpy(buf.data() + header_bytes + 4, payload.data(), payload.size());
  return buf;
}

}  // namespace

TEST(FluteV2, Version1StepsOverTheSctAndErtHeaderFields) {
  auto buf = packet_with_t_and_r_set("payload-v1");
  AlcPacket alc(buf.data(), buf.size(), /*expected_flute_version*/ 1);
  EXPECT_EQ(alc.toi(), 7u);
  // 5 declared words, all of them standard for version 1, so no extension space is left over.
  EXPECT_EQ(alc.header_length(), 20u);
}

namespace {

/* The same two bit positions, but with a real EXT_FTI occupying the four words that follow the
   TOI. A version 2 receiver, for which those bits size nothing, finds the extension there. A
   version 1 receiver would consume the first two of those words as SCT and ERT and misread the
   rest, which is the incompatibility RFC 6726 clause 11.1 describes. */
std::vector<char> packet_with_t_and_r_set_and_ext_fti(uint16_t encoding_symbol_length) {
  const size_t header_words = 3 /* LCT header, CCI, TSI/TOI */ + 4 /* EXT_FTI */;
  const size_t header_bytes = header_words * 4;
  std::vector<char> buf(header_bytes + 4 /* SBN+ESI */ + 8, 0);
  auto* b = reinterpret_cast<unsigned char*>(buf.data());

  b[0] = (1 << 4);
  b[1] = 0x10 /* half_word */ | 0x08 /* sct */ | 0x04 /* ert */;
  b[2] = static_cast<unsigned char>(header_words);
  b[3] = 0;

  uint16_t tsi_be = htons(1), toi_be = htons(7);
  std::memcpy(b + 8, &tsi_be, 2);
  std::memcpy(b + 10, &toi_be, 2);

  size_t off = 12;
  b[off] = 64;      // EXT_FTI
  b[off + 1] = 4;   // HEL, in words
  uint32_t transfer_len_be = htonl(8);
  std::memcpy(b + off + 4, &transfer_len_be, 4);
  uint16_t esl_be = htons(encoding_symbol_length);
  std::memcpy(b + off + 10, &esl_be, 2);
  uint32_t msbl_be = htonl(64);
  std::memcpy(b + off + 12, &msbl_be, 4);
  return buf;
}

}  // namespace

TEST(FluteV2, Version2TreatsTheSctAndErtBitsAsReservedAndUnsized) {
  auto buf = packet_with_t_and_r_set_and_ext_fti(1200);
  AlcPacket alc(buf.data(), buf.size(), /*expected_flute_version*/ 2);
  EXPECT_EQ(alc.toi(), 7u);
  ASSERT_TRUE(alc.has_fec_oti())
      << "the extension after the TOI was not reached, so the two bits still sized the header";
  EXPECT_EQ(alc.fec_oti().encoding_symbol_length, 1200u);
}

TEST(FluteV2, Version1WouldNotReachThatExtension) {
  /* The counterpart, kept so the difference between the two readings is visible rather than
     asserted only on one side. Under version 1 the first two words of the extension are taken as
     SCT and ERT, and what remains does not parse as a header extension. */
  auto buf = packet_with_t_and_r_set_and_ext_fti(1200);
  EXPECT_THROW(AlcPacket(buf.data(), buf.size(), /*expected_flute_version*/ 1), std::runtime_error);
}

/* RFC 6726 clause 3.4.1: "After reaching the maximum value (2^20-1), the numbering starts from the
   smallest FDT Instance ID value assigned to an expired FDT Instance." Version 1 wraps to 0
   instead, which the version 1 suite covers. */
TEST(FluteV2, InstanceIdWrapsToTheSmallestExpiredIdentifier) {
  std::map<uint32_t, uint64_t> expired = {{9u, 500u}, {3u, 500u}, {40u, 500u}};
  auto next = FileDeliveryTable::next_instance_id(FileDeliveryTable::kMaxFdtInstanceId,
                                                  /*current_expires*/ 1500, /*now*/ 1000, expired,
                                                  /*flute_version*/ 2);
  EXPECT_EQ(next, 3u);
  EXPECT_EQ(expired.count(3u), 0u) << "the reused identifier is no longer available";
  EXPECT_EQ(expired.at(FileDeliveryTable::kMaxFdtInstanceId), 1500u);
}

TEST(FluteV2, InstanceIdSkipsAnIdentifierThatHasNotExpired) {
  // 3 is still live at now=1000, so the smallest *expired* identifier is 9.
  std::map<uint32_t, uint64_t> expired = {{3u, 5000u}, {9u, 500u}};
  auto next = FileDeliveryTable::next_instance_id(FileDeliveryTable::kMaxFdtInstanceId,
                                                  /*current_expires*/ 1500, /*now*/ 1000, expired,
                                                  /*flute_version*/ 2);
  EXPECT_EQ(next, 9u);
}

/* "Senders MUST NOT reuse an FDT Instance ID value that is already in use for a non-expired FDT
   Instance." With none expired there is no identifier the sender is permitted to take, and the
   same clause leaves that case to the implementation. Version 1 wraps to 0 and warns instead. */
TEST(FluteV2, InstanceIdRefusesToReuseALiveIdentifier) {
  std::map<uint32_t, uint64_t> expired = {{0u, 5000u}, {1u, 5000u}};
  EXPECT_THROW(FileDeliveryTable::next_instance_id(FileDeliveryTable::kMaxFdtInstanceId,
                                                   /*current_expires*/ 5000, /*now*/ 1000, expired,
                                                   /*flute_version*/ 2),
               std::runtime_error);
}

TEST(FluteV2, InstanceIdIncrementsBelowTheCeilingAsInVersion1) {
  std::map<uint32_t, uint64_t> expired;
  EXPECT_EQ(FileDeliveryTable::next_instance_id(11, 1000, 2000, expired, /*flute_version*/ 2), 12u);
}

/* RFC 6726 clause 3.3: "both a sender and a receiver easily determine to which (136-year) epoch
   the FDT Instance expiration time value pertains by choosing the epoch for which the expiration
   time is closest in time to the current time." The clause's own worked example is used here. */
TEST(FluteV2, ExpiryIsReadInTheEraNearestTheCurrentTime) {
  static constexpr uint64_t era = 1ULL << 32;

  // The clause's example: a session started at NTP 4,294,944,000, a few hours before era 0 ends,
  // declaring an expiry of 149,504, which belongs to the next era.
  EXPECT_EQ(FileDeliveryTable::expiry_in_nearest_era(149504u, 4294944000ULL), era + 149504ULL);

  // A receiver joining at NTP 63,104 in era 1 reads the same value as era 1 too.
  EXPECT_EQ(FileDeliveryTable::expiry_in_nearest_era(149504u, era + 63104ULL), era + 149504ULL);

  // Well inside an era, the value is taken at face value.
  EXPECT_EQ(FileDeliveryTable::expiry_in_nearest_era(2000000000u, 1999999000ULL), 2000000000ULL);
}


/* Version 2 is not available under either 3GPP profile, both being built on RFC 3926.

   TS 26.346 V18.2.0 clause 7.2.0: "MBMS
   Clients and servers supporting MBMS download shall implement the FLUTE specification (RFC 3926
   [9]), as well as ALC (RFC 3450 [10]) and LCT (RFC 3451 [11]) features that FLUTE inherits."
   RFC 3926 is version 1, and RFC 6726 clause 11.1 records that the two are not interchangeable. */
TEST(FluteV2, RefusedUnderThe3gppProfiles) {
  auto oti = FecOti{FecScheme::CompactNoCode, 0, 0, 1400, 64, 0};
  FileDeliveryTable mbs(1, oti, FileDeliveryTable::FDT_NS_NONE, Profile::Ts26517);
  EXPECT_THROW(mbs.set_flute_version(2), std::runtime_error);

  FileDeliveryTable mbms(1, oti, FileDeliveryTable::FDT_NS_NONE, Profile::Ts26346);
  EXPECT_THROW(mbms.set_flute_version(2), std::runtime_error);
}

TEST(FluteV2, AcceptedUnderGeneralFlute) {
  auto oti = FecOti{FecScheme::CompactNoCode, 0, 0, 1400, 64, 0};
  FileDeliveryTable fdt(1, oti, FileDeliveryTable::FDT_NS_RFC6726, Profile::Unprofiled);
  EXPECT_NO_THROW(fdt.set_flute_version(2));
  EXPECT_EQ(fdt.flute_version(), 2);
}
