// libflute - FLUTE/ALC library
//
// Copyright (C) 2026 5G-MAG
//
// Licensed under the License terms and conditions for use, reproduction, and
// distribution of 5G-MAG software (the "License").  You may not use this file
// except in compliance with the License.  You may obtain a copy of the License at
// https://www.5g-mag.com/reference-tools.  Unless required by applicable law or
// agreed to in writing, software distributed under the License is distributed on
// an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.
//
// See the License for the specific language governing permissions and limitations
// under the License.
//
// Targeted regression tests for the RFC 6726/5651 compliance fixes: FLUTE
// version nibble, wide-TSI transmit encoding, FDT Instance ID wraparound,
// FDT Complete attribute, LCT Close Session/Object flags, and EXT_FTI
// bootstrapping of an unknown TOI.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <boost/asio.hpp>

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "AlcPacket.h"
#include "FileDeliveryTable.h"
#include "Receiver.h"

using namespace LibFlute;
using namespace std::chrono_literals;

namespace {
  std::vector<EncodingSymbol> make_symbols(const char* data, size_t len) {
    return { EncodingSymbol(0, 0, const_cast<char*>(data), len, FecScheme::CompactNoCode) };
  }
}

// ---------------------------------------------------------------------------
// Fix 1: FLUTE version nibble in EXT_FDT must be 2, not 1 (RFC 6726 SS3.1/3.4.1)
// ---------------------------------------------------------------------------
TEST(ProtocolFixes, FdtPacketCarriesFluteVersion2InExtFdtNibble) {
  const char payload[] = "<FDT-Instance/>";
  auto symbols = make_symbols(payload, sizeof(payload) - 1);
  FecOti fec_oti{FecScheme::CompactNoCode, 0, sizeof(payload) - 1, 1400, 64, 0};

  AlcPacket packet(/*tsi*/1234, /*toi*/0, fec_oti, symbols, 1400, /*fdt_instance_id*/5);

  // Decode it back and confirm the receive-side parser (which tolerates
  // 0/1/2) is happy, then inspect the raw nibble directly.
  AlcPacket decoded(packet.data(), packet.size());
  EXPECT_EQ(decoded.fdt_instance_id(), 5u);

  // Locate the EXT_FDT extension byte directly: header_length() tells us
  // where the LCT header (base + extensions) ends; EXT_FDT is the first
  // extension for a TOI==0 packet, so its HET byte is right after the base
  // header (LCT word + CCI word + TSI/TOI half-word = 3 words = 12 bytes).
  const auto* bytes = reinterpret_cast<const unsigned char*>(packet.data());
  ASSERT_GE(packet.size(), 14u);
  EXPECT_EQ(bytes[12], 192); // EXT_FDT HET
  // EXT_FDT is a fixed 4-byte ("immediate", HET > 127) extension: HET, then a
  // byte combining the 4-bit FLUTE version nibble with the top 4 bits of the
  // FDT Instance ID, then the low 16 bits of the Instance ID.
  uint8_t flute_version_nibble = (bytes[13] & 0xF0) >> 4;
  EXPECT_EQ(flute_version_nibble, 2u);
}

// ---------------------------------------------------------------------------
// Fix 2: TSI is no longer truncated to 16 bits on transmit
// ---------------------------------------------------------------------------
TEST(ProtocolFixes, TsiRoundTripsAt16BitWidth) {
  const char payload[] = "hello";
  auto symbols = make_symbols(payload, sizeof(payload) - 1);
  FecOti fec_oti{FecScheme::CompactNoCode, 0, sizeof(payload) - 1, 1400, 64, 0};

  AlcPacket packet(/*tsi*/0xBEEFULL, /*toi*/7, fec_oti, symbols, 1400, 0);
  AlcPacket decoded(packet.data(), packet.size());
  EXPECT_EQ(decoded.tsi(), 0xBEEFULL);
  EXPECT_EQ(decoded.toi(), 7u);
}

TEST(ProtocolFixes, TsiRoundTripsAt32BitWidth) {
  const char payload[] = "hello";
  auto symbols = make_symbols(payload, sizeof(payload) - 1);
  FecOti fec_oti{FecScheme::CompactNoCode, 0, sizeof(payload) - 1, 1400, 64, 0};

  // Larger than 16 bits: previously this would have been silently truncated
  // to 0xBEEF on the wire.
  const uint64_t wide_tsi = 0x12345678ULL;
  AlcPacket packet(wide_tsi, /*toi*/7, fec_oti, symbols, 1400, 0);
  AlcPacket decoded(packet.data(), packet.size());
  EXPECT_EQ(decoded.tsi(), wide_tsi);
  EXPECT_EQ(decoded.toi(), 7u);
}

TEST(ProtocolFixes, TsiRoundTripsAt48BitWidth) {
  const char payload[] = "hello";
  auto symbols = make_symbols(payload, sizeof(payload) - 1);
  FecOti fec_oti{FecScheme::CompactNoCode, 0, sizeof(payload) - 1, 1400, 64, 0};

  const uint64_t wide_tsi = 0xAABBCCDDEEFFULL; // 48-bit max representable
  AlcPacket packet(wide_tsi, /*toi*/42, fec_oti, symbols, 1400, 0);
  AlcPacket decoded(packet.data(), packet.size());
  EXPECT_EQ(decoded.tsi(), wide_tsi);
  EXPECT_EQ(decoded.toi(), 42u);
}

TEST(ProtocolFixes, TsiBeyond48BitsThrows) {
  const char payload[] = "hello";
  auto symbols = make_symbols(payload, sizeof(payload) - 1);
  FecOti fec_oti{FecScheme::CompactNoCode, 0, sizeof(payload) - 1, 1400, 64, 0};

  EXPECT_THROW(
      AlcPacket(0x1000000000000ULL, /*toi*/1, fec_oti, symbols, 1400, 0),
      std::runtime_error);
}

// ---------------------------------------------------------------------------
// Fix 5: LCT Close Session / Close Object flags round-trip
// ---------------------------------------------------------------------------
TEST(ProtocolFixes, CloseSessionAndCloseObjectFlagsRoundTrip) {
  const char payload[] = "hello";
  auto symbols = make_symbols(payload, sizeof(payload) - 1);
  FecOti fec_oti{FecScheme::CompactNoCode, 0, sizeof(payload) - 1, 1400, 64, 0};

  AlcPacket packet(/*tsi*/1, /*toi*/1, fec_oti, symbols, 1400, 0,
                   /*close_session*/true, /*close_object*/true);
  AlcPacket decoded(packet.data(), packet.size());
  EXPECT_TRUE(decoded.close_session());
  EXPECT_TRUE(decoded.close_object());
}

TEST(ProtocolFixes, CloseFlagsDefaultToFalse) {
  const char payload[] = "hello";
  auto symbols = make_symbols(payload, sizeof(payload) - 1);
  FecOti fec_oti{FecScheme::CompactNoCode, 0, sizeof(payload) - 1, 1400, 64, 0};

  AlcPacket packet(/*tsi*/1, /*toi*/1, fec_oti, symbols, 1400, 0);
  AlcPacket decoded(packet.data(), packet.size());
  EXPECT_FALSE(decoded.close_session());
  EXPECT_FALSE(decoded.close_object());
}

// ---------------------------------------------------------------------------
// Fix 3: FDT Instance ID wraparound (RFC 6726 SS3.4.1)
// ---------------------------------------------------------------------------
TEST(FdtInstanceIdWraparound, NormalIncrementBelowCeiling) {
  std::set<uint32_t> expired;
  EXPECT_EQ(FileDeliveryTable::next_instance_id(5, expired), 6u);
  EXPECT_EQ(expired.count(5u), 1u);
}

TEST(FdtInstanceIdWraparound, WrapsToSmallestExpiredIdAtCeiling) {
  std::set<uint32_t> expired = {1u, 5u, 100u};
  auto next = FileDeliveryTable::next_instance_id(FileDeliveryTable::kMaxFdtInstanceId, expired);
  EXPECT_EQ(next, 1u); // smallest previously-expired id reused
  EXPECT_EQ(expired.count(1u), 0u); // consumed, now live again
  EXPECT_EQ(expired.count(FileDeliveryTable::kMaxFdtInstanceId), 1u); // old ceiling id now recorded as expired
}

TEST(FdtInstanceIdWraparound, FallsBackToZeroWhenNothingExpiredYet) {
  std::set<uint32_t> expired; // synthetic edge case: ceiling reached with no history at all
  auto next = FileDeliveryTable::next_instance_id(FileDeliveryTable::kMaxFdtInstanceId, expired);
  EXPECT_EQ(next, 0u);
}

TEST(FdtInstanceIdWraparound, NeverExceeds20BitsAcrossManyAdds) {
  // Simulate a long run of adds/sends starting right at the ceiling to
  // confirm the ID never walks outside the 20-bit space it must fit in on
  // the wire (previously a plain ++ would overflow uint32_t long before that
  // could be observed, and silently bit-mask on the wire instead).
  FileDeliveryTable fdt(FileDeliveryTable::kMaxFdtInstanceId, FecOti{FecScheme::CompactNoCode, 0, 0, 1400, 64, 0});
  FileDeliveryTable::FileEntry fe{};
  fe.toi = 1;
  fe.content_location = "a";
  for (int i = 0; i < 10; ++i) {
    fdt.sent();
    fe.toi = static_cast<uint32_t>(i + 1);
    fdt.add(fe);
    EXPECT_LE(fdt.instance_id(), FileDeliveryTable::kMaxFdtInstanceId);
  }
}

// ---------------------------------------------------------------------------
// Fix 4: FDT `Complete` attribute read/write
// ---------------------------------------------------------------------------
TEST(FdtCompleteAttribute, DefaultsToFalseAndRoundTripsWhenSet) {
  FecOti fec_oti{FecScheme::CompactNoCode, 0, 0, 1400, 64, 0};
  FileDeliveryTable fdt(1, fec_oti);
  EXPECT_FALSE(fdt.complete());

  fdt.set_complete(true);
  EXPECT_TRUE(fdt.complete());

  auto xml = fdt.to_string();
  EXPECT_NE(xml.find("Complete=\"true\""), std::string::npos);

  FileDeliveryTable parsed(1, const_cast<char*>(xml.c_str()), xml.length());
  EXPECT_TRUE(parsed.complete());
}

TEST(FdtCompleteAttribute, AbsentWhenNotSet) {
  FecOti fec_oti{FecScheme::CompactNoCode, 0, 0, 1400, 64, 0};
  FileDeliveryTable fdt(1, fec_oti);
  auto xml = fdt.to_string();
  EXPECT_EQ(xml.find("Complete="), std::string::npos);

  FileDeliveryTable parsed(1, const_cast<char*>(xml.c_str()), xml.length());
  EXPECT_FALSE(parsed.complete());
}

// ---------------------------------------------------------------------------
// Fix 6: EXT_FTI bootstraps a File for a TOI with no prior FDT entry
// ---------------------------------------------------------------------------
namespace {
  // Hand-builds a minimal ALC/LCT packet for TOI 5, Compact No-Code, carrying
  // only EXT_FTI (no EXT_FDT) -- exactly the "content packet arrives before
  // its FDT entry" scenario RFC 6726 SS3.4.1 allows a receiver to bootstrap
  // from. This library's own Transmitter never emits EXT_FTI on content
  // packets (only on the TOI==0 FDT itself), so this mirrors what a
  // different, spec-general FLUTE sender could produce.
  std::vector<char> build_content_packet_with_fti(uint16_t tsi, uint16_t toi, uint16_t encoding_symbol_length,
                                                    uint32_t max_source_block_length, const std::string& symbol_data) {
    // 2 words (LCT header + CCI) + 1 word (TSI/TOI half-word) + 4 words (EXT_FTI) = 7 words
    const size_t lct_header_len_words = 7;
    const size_t header_bytes = lct_header_len_words * 4;
    const size_t sbn_esi_bytes = 4;
    std::vector<char> buf(header_bytes + sbn_esi_bytes + symbol_data.size(), 0);
    auto* b = reinterpret_cast<unsigned char*>(buf.data());

    // LCT header word: version=1 (nibble), half_word_flag=1, lct_header_len, codepoint=0
    b[0] = (1 << 4); // version=1, congestion_control=0, PSI=0, res1=0
    // Second header byte's bitfield layout (little-endian: lowest-declared member
    // in the lowest bits): close_object_flag(bit0), close_session_flag(bit1),
    // res(bits2-3), half_word_flag(bit4), toi_flag(bits5-6), tsi_flag(bit7).
    b[1] = 0x10; // half_word_flag=1, everything else 0

    b[2] = static_cast<unsigned char>(lct_header_len_words);
    b[3] = 0; // codepoint = CompactNoCode

    // CCI (4 bytes) already zeroed
    // TSI half-word (2 bytes) + TOI half-word (2 bytes)
    uint16_t tsi_be = htons(tsi);
    uint16_t toi_be = htons(toi);
    std::memcpy(b + 8, &tsi_be, 2);
    std::memcpy(b + 10, &toi_be, 2);

    // EXT_FTI extension (16 bytes): HET, HEL, transfer_length(48 bits as hi16+lo32), reserved(16), esl(16), max_source_block_length(32)
    size_t off = 12;
    b[off] = 64; // EXT_FTI
    b[off + 1] = 4; // HEL (16 bytes total)
    uint16_t transfer_len_hi = 0;
    uint32_t transfer_len_lo = htonl(static_cast<uint32_t>(symbol_data.size()));
    uint16_t transfer_len_hi_be = htons(transfer_len_hi);
    std::memcpy(b + off + 2, &transfer_len_hi_be, 2);
    std::memcpy(b + off + 4, &transfer_len_lo, 4);
    // 2 bytes reserved at off+8..+9
    uint16_t esl_be = htons(encoding_symbol_length);
    std::memcpy(b + off + 10, &esl_be, 2);
    uint32_t msbl_be = htonl(max_source_block_length);
    std::memcpy(b + off + 12, &msbl_be, 4);

    // Payload: SBN=0, ESI=0, then the symbol bytes
    size_t payload_off = header_bytes;
    // SBN/ESI already zeroed
    std::memcpy(buf.data() + payload_off + sbn_esi_bytes, symbol_data.data(), symbol_data.size());

    return buf;
  }
}

TEST(ExtFtiBootstrap, PacketWithOwnFtiParsesAsExpected) {
  // Sanity-check the hand-built packet decodes the way this test assumes,
  // independent of the Receiver, before relying on it in the live test below.
  auto buf = build_content_packet_with_fti(777, 5, 1000, 64, "0123456789");
  AlcPacket alc(buf.data(), buf.size());
  EXPECT_EQ(alc.tsi(), 777u);
  EXPECT_EQ(alc.toi(), 5u);
  EXPECT_TRUE(alc.has_fti());
  EXPECT_EQ(alc.fec_oti().encoding_symbol_length, 1000u);
  EXPECT_EQ(alc.fec_oti().max_source_block_length, 64u);
  EXPECT_FALSE(alc.close_session());
  EXPECT_FALSE(alc.close_object());
}

TEST(ExtFtiBootstrap, ReceiverBootstrapsFileFromOwnFtiWhenFdtEntryMissing) {
  boost::asio::io_context io;
  auto work_guard = boost::asio::make_work_guard(io);
  LibFlute::Receiver receiver("0.0.0.0", "239.255.9.9", 19191, /*tsi*/777, io);
  std::thread io_thread([&io]() { io.run(); });

  auto buf = build_content_packet_with_fti(777, 5, 1000, 64, "0123456789");

  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(sock, 0);
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(19191);
  inet_pton(AF_INET, "239.255.9.9", &dst.sin_addr);
  auto sent = sendto(sock, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
  EXPECT_EQ(sent, static_cast<ssize_t>(buf.size()));
  ::close(sock);

  // Poll briefly for the async receive to be processed.
  bool found = false;
  for (int i = 0; i < 50 && !found; ++i) {
    std::this_thread::sleep_for(20ms);
    for (const auto& f : receiver.file_list()) {
      if (f->meta().toi == 5) {
        found = true;
        EXPECT_EQ(f->fec_oti().encoding_symbol_length, 1000u);
        EXPECT_EQ(f->fec_oti().max_source_block_length, 64u);
      }
    }
  }
  EXPECT_TRUE(found) << "Receiver did not bootstrap a File for TOI 5 from its own EXT_FTI";

  receiver.stop();
  work_guard.reset();
  io.stop();
  io_thread.join();
}
