// libflute - FLUTE/ALC library
//
// Copyright (C) 2026 5G-MAG Association (Jordi J. Gimenez <gimenez@5g-mag.com>)
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
// Unit tests for the from-scratch RFC 6330 RaptorQ codec (fec/RaptorQCodec.h)
// and its integration into LibFlute::File. Mirrors test_raptor_fec.cpp's
// structure; see that file's header comment for the rationale behind
// testing at both the codec level and the real File API level.
#include <gtest/gtest.h>

#include "Transmitter.h"

#include <boost/asio.hpp>
#include <random>
#include <algorithm>
#include "fec/RaptorQCodec.h"
#include "File.h"

using namespace LibFlute;
using namespace LibFlute::RaptorQ;

namespace {

std::vector<std::vector<uint8_t>> make_source(uint32_t K, size_t T, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<std::vector<uint8_t>> src(K, std::vector<uint8_t>(T));
  for (auto& sym : src) for (auto& b : sym) b = (uint8_t)rng();
  return src;
}

void expect_round_trip(uint32_t K, size_t T, uint32_t seed, uint32_t received_overhead, uint32_t pool_overhead) {
  RaptorQCodec encoder(K);
  auto source = make_source(K, T, seed);
  auto intermediate = encoder.compute_intermediate_symbols(source);
  ASSERT_EQ(intermediate.size(), encoder.L());

  for (uint32_t i = 0; i < K; i++) {
    EXPECT_EQ(encoder.generate_encoding_symbol(i, intermediate), source[i]) << "systematic property failed at ESI " << i;
  }

  uint32_t pool_size = K + pool_overhead;
  uint32_t received_count = K + received_overhead;
  ASSERT_LE(received_count, pool_size);

  std::mt19937 rng(seed * 7919u + 1);
  std::vector<uint32_t> pool(pool_size);
  for (uint32_t i = 0; i < pool_size; i++) pool[i] = i;
  std::shuffle(pool.begin(), pool.end(), rng);

  RaptorQCodec decoder(K);
  for (uint32_t i = 0; i < received_count; i++) {
    decoder.add_received_symbol(pool[i], encoder.generate_encoding_symbol(pool[i], intermediate));
  }

  ASSERT_TRUE(decoder.can_decode()) << "needs " << decoder.symbols_needed() << " more independent symbols";
  EXPECT_EQ(decoder.decode_source_symbols(), source);
}

} // namespace

TEST(RaptorQCodecTest, CanonicalExactlyKInOrderAlwaysDecodes) {
  RaptorQCodec encoder(10);
  auto source = make_source(10, 8, 1);
  auto intermediate = encoder.compute_intermediate_symbols(source);
  RaptorQCodec decoder(10);
  for (uint32_t i = 0; i < 10; i++) {
    decoder.add_received_symbol(i, encoder.generate_encoding_symbol(i, intermediate));
  }
  ASSERT_TRUE(decoder.can_decode());
  EXPECT_EQ(decoder.decode_source_symbols(), source);
}

TEST(RaptorQCodecTest, SmallBlockIncludingBelowMinimumSupportedKprime) {
  expect_round_trip(10, 8, 15, /*received_overhead=*/2, /*pool_overhead=*/25);
  // K=4 is below Table 2's smallest K' (10) -- exercises the K -> K' padding step.
  expect_round_trip(4, 4, 8, /*received_overhead=*/12, /*pool_overhead=*/20);
}

TEST(RaptorQCodecTest, MediumBlock) {
  expect_round_trip(100, 16, 3, 15, 40);
  expect_round_trip(100, 16, 4, 25, 60);
}

TEST(RaptorQCodecTest, LargerBlockModestOverheadIsAlreadySafe) {
  // K=300, not Raptor's test's K=1000: GF256LinearSystem is dense
  // (GF256LinearSystem.h explains why -- real, non-unity HDPC
  // coefficients), so it costs meaningfully more per equation than
  // Raptor's bit-packed GF(2) system does at the same K. 300 is still a
  // real, non-trivial block size; K=1000 was verified separately during
  // development (see this codec's commit message for measured timing) and
  // isn't worth this suite's runtime budget on every run.
  expect_round_trip(300, 32, 5, 10, 40);
  expect_round_trip(300, 32, 6, 20, 60);
}

TEST(RaptorQCodecTest, DecodesReliablyAtLowerOverheadThanRaptor) {
  // RaptorQ's well-documented design improvement over the original Raptor:
  // near-zero decoding failure probability even at 0-1 overhead for K >= 30
  // (see the from-scratch implementation's commit message for measured
  // failure rates). This isn't a statistical test (too slow/flaky for a
  // unit test); it just confirms a single trial at K+1 succeeds where
  // Raptor would need much more overhead to be reliable.
  expect_round_trip(30, 8, 99, /*received_overhead=*/1, /*pool_overhead=*/10);
  expect_round_trip(100, 16, 100, /*received_overhead=*/1, /*pool_overhead=*/10);
}

TEST(FileRaptorQTest, EndToEndThroughRealFileApiSingleBlock) {
  const size_t data_len = 20000;
  std::vector<char> data(data_len);
  std::mt19937 rng(123);
  for (auto& b : data) b = (char)rng();

  FecOti oti{
    .encoding_id = FecScheme::RaptorQ,
    .encoding_symbol_length = 256,
    .max_source_block_length = 200,
    .max_number_of_encoding_symbols = 90, // RaptorQ needs far less overhead than Raptor
  };

  File encoder(1, oti, "test.bin", "application/octet-stream", 0, data.data(), data_len, true);
  auto symbols = encoder.get_next_symbols(1024 * 1024);
  ASSERT_FALSE(symbols.empty());
  EXPECT_EQ(encoder.fec_oti().nof_source_blocks, 1u);

  File decoder(encoder.meta());

  std::vector<size_t> order(symbols.size());
  for (size_t i = 0; i < order.size(); i++) order[i] = i;
  std::shuffle(order.begin(), order.end(), rng);
  size_t to_deliver = std::min(symbols.size(), (size_t)85); // K(~79) + modest overhead
  std::vector<bool> deliver(symbols.size(), false);
  for (size_t i = 0; i < to_deliver; i++) deliver[order[i]] = true;

  for (size_t i = 0; i < symbols.size(); i++) {
    if (deliver[i]) decoder.put_symbol(symbols[i]);
  }

  ASSERT_TRUE(decoder.complete());
  ASSERT_EQ(decoder.length(), data_len);
  EXPECT_EQ(0, memcmp(decoder.buffer(), data.data(), data_len));
}

TEST(FileRaptorQTest, EndToEndThroughRealFileApiMultiBlock) {
  const size_t data_len = 20000;
  std::vector<char> data(data_len);
  std::mt19937 rng(456);
  for (auto& b : data) b = (char)rng();

  FecOti oti{
    .encoding_id = FecScheme::RaptorQ,
    .encoding_symbol_length = 256,
    .max_source_block_length = 20,
    .max_number_of_encoding_symbols = 32,
  };

  File encoder(2, oti, "test2.bin", "application/octet-stream", 0, data.data(), data_len, true);
  auto symbols = encoder.get_next_symbols(1024 * 1024);
  ASSERT_GT(encoder.fec_oti().nof_source_blocks, 1u);

  File decoder(encoder.meta());

  std::vector<size_t> order(symbols.size());
  for (size_t i = 0; i < order.size(); i++) order[i] = i;
  std::shuffle(order.begin(), order.end(), rng);
  size_t to_deliver = (size_t)(symbols.size() * 0.92);
  std::vector<bool> deliver(symbols.size(), false);
  for (size_t i = 0; i < to_deliver; i++) deliver[order[i]] = true;

  for (size_t i = 0; i < symbols.size(); i++) {
    if (deliver[i]) decoder.put_symbol(symbols[i]);
  }

  ASSERT_TRUE(decoder.complete());
  ASSERT_EQ(decoder.length(), data_len);
  EXPECT_EQ(0, memcmp(decoder.buffer(), data.data(), data_len));
}

// ----------------------------------------------------------------------------------------------
// The scheme-specific FEC OTI layout differs between the two schemes, in the same four octets:
// Z and N are the opposite way round. Encoding one with the other's layout silently corrupts both
// values, so this asserts the two encodings differ for identical inputs.
//
// RFC 5053 clause 3.2.3, Raptor: "a 4-octet field consisting of the parameters Z (2 octets),
// N (1 octet), and Al (1 octet)"
//
// RFC 6330 clause 3.3.3, RaptorQ: "The number of source blocks (Z): 8-bit unsigned integer." and
// "The number of sub-blocks (N): 16-bit unsigned integer."
// ----------------------------------------------------------------------------------------------

#include "FileDeliveryTable.h"

namespace {

std::string ssi_for(LibFlute::FecScheme scheme, uint16_t z, uint16_t n, uint8_t al) {
  LibFlute::FecOti oti{};
  oti.encoding_id = scheme;
  oti.transfer_length = 4096;
  oti.encoding_symbol_length = 512;
  oti.max_source_block_length = 64;
  oti.nof_source_blocks = z;
  oti.nof_sub_blocks = n;
  oti.symbol_alignment = al;
  LibFlute::FileDeliveryTable fdt(1, oti, LibFlute::FileDeliveryTable::FDT_NS_3GPP_CONSOLIDATED_V2);
  LibFlute::FileDeliveryTable::FileEntry e{};
  e.toi = 1;
  e.content_location = "http://example.invalid/a";
  e.content_length = 4096;
  e.fec_oti = oti;
  fdt.add(e);
  return fdt.to_string();
}

}  // namespace

TEST(RaptorQSchemeSpecificOtiTest, LayoutDiffersFromRaptorForTheSameValues) {
  // Z = 1, N = 0x0203, Al = 4.
  // RaptorQ packs Z into one octet and N into two: 01 02 03 04 -> AQIDBA==
  // Raptor packs Z into two and N into one:       00 01 03 04 -> AAEDBA==
  const auto rq = ssi_for(LibFlute::FecScheme::RaptorQ, 1, 0x0203, 4);
  const auto ra = ssi_for(LibFlute::FecScheme::Raptor, 1, 0x0203, 4);
  EXPECT_NE(rq.find("AQIDBA=="), std::string::npos);
  EXPECT_NE(ra.find("AAEDBA=="), std::string::npos);
  EXPECT_EQ(rq.find("AAEDBA=="), std::string::npos);
  EXPECT_EQ(ra.find("AQIDBA=="), std::string::npos);
}

TEST(RaptorQSchemeSpecificOtiTest, RoundTripsWithTheRaptorQWidths) {
  // N above 255 is the case a shared layout would truncate.
  const auto out = ssi_for(LibFlute::FecScheme::RaptorQ, 7, 0x0140, 4);
  std::vector<char> buf(out.begin(), out.end());
  buf.push_back('\0');
  LibFlute::FileDeliveryTable parsed(1, buf.data(), buf.size());
  ASSERT_FALSE(parsed.file_entries().empty());
  const auto &oti = parsed.file_entries().front().fec_oti;
  EXPECT_EQ(oti.nof_source_blocks, 7);
  EXPECT_EQ(oti.nof_sub_blocks, 0x0140);
  EXPECT_EQ(oti.symbol_alignment, 4);
}

// The transmit loop as Transmitter actually drives it: one datagram-sized
// request at a time, each batch acknowledged before the next is asked for. The
// File-level tests above instead pull every symbol in a single 1 MB call and
// acknowledge nothing, so they cannot see whether a repair symbol would ever
// reach the wire in a real session.
namespace {
struct RqTransmitRun {
  size_t total = 0;
  size_t repair = 0;
  uint32_t max_esi = 0;
  bool complete = false;
  std::vector<LibFlute::EncodingSymbol> symbols;
};

RqTransmitRun rq_drive_transmit_loop(LibFlute::File& f, size_t datagram_payload, uint32_t k) {
  RqTransmitRun r;
  for (int guard = 0; guard < 10000 && !f.complete(); guard++) {
    auto batch = f.get_next_symbols(datagram_payload);
    if (batch.empty()) break;
    for (auto& s : batch) {
      r.total++;
      if (s.id() >= k) r.repair++;
      r.max_esi = std::max(r.max_esi, s.id());
      r.symbols.push_back(s);
    }
    f.mark_completed(batch, true);
  }
  r.complete = f.complete();
  return r;
}
} // namespace

// Kt = ceil(20000/256) = 79 source symbols in one source block, and a FEC OTI
// permitting 90 encoding symbols for it, so the repair budget is 90 - 79 = 11
// and a fully transmitted block is exactly 90 symbols.
TEST(RaptorQRepairTransmissionTest, RepairSymbolsAreTransmittedAfterTheSourceSymbols) {
  const size_t data_len = 20000;
  const uint32_t K = 79;
  std::vector<char> data(data_len);
  std::mt19937 rng(31337);
  for (auto& b : data) b = (char)rng();

  FecOti oti{
    .encoding_id = FecScheme::RaptorQ,
    .encoding_symbol_length = 256,
    .max_source_block_length = 200,
    .max_number_of_encoding_symbols = 90,
  };
  File encoder(1, oti, "test.bin", "application/octet-stream", 0, data.data(), data_len, true);
  ASSERT_EQ(encoder.fec_oti().nof_source_blocks, 1u);

  auto run = rq_drive_transmit_loop(encoder, 256 + 4, K);

  EXPECT_EQ(run.total, 90u) << "the block's whole encoding symbol budget must go out";
  EXPECT_EQ(run.repair, 11u) << "repair symbols must reach the wire, not just be budgeted";
  EXPECT_EQ(run.max_esi, 89u);
  EXPECT_TRUE(run.complete) << "the file must still finish once its repair budget is out";
}

TEST(RaptorQRepairTransmissionTest, FileIsNotCompleteWhileRepairSymbolsAreStillOwed) {
  const size_t data_len = 20000;
  const uint32_t K = 79;
  std::vector<char> data(data_len, 'q');
  FecOti oti{
    .encoding_id = FecScheme::RaptorQ,
    .encoding_symbol_length = 256,
    .max_source_block_length = 200,
    .max_number_of_encoding_symbols = 90,
  };
  File encoder(1, oti, "test.bin", "application/octet-stream", 0, data.data(), data_len, true);

  size_t sent = 0;
  bool complete_at_last_source_symbol = false;
  while (!encoder.complete()) {
    auto batch = encoder.get_next_symbols(256 + 4);
    if (batch.empty()) break;
    sent += batch.size();
    encoder.mark_completed(batch, true);
    if (sent == K) complete_at_last_source_symbol = encoder.complete();
  }
  EXPECT_FALSE(complete_at_last_source_symbol)
      << "completing at the last source symbol strands the repair budget";
  EXPECT_EQ(sent, 90u);
}

// A RaptorQ receiver that holds every source symbol is finished, and knows
// nothing about any repair budget: the transmit-side condition must not leak in.
TEST(RaptorQRepairTransmissionTest, ReceiverCompletesOnSourceSymbolsAlone) {
  const size_t data_len = 20000;
  const uint32_t K = 79;
  std::vector<char> data(data_len);
  std::mt19937 rng(777);
  for (auto& b : data) b = (char)rng();

  FecOti oti{
    .encoding_id = FecScheme::RaptorQ,
    .encoding_symbol_length = 256,
    .max_source_block_length = 200,
    .max_number_of_encoding_symbols = 90,
  };
  File encoder(1, oti, "test.bin", "application/octet-stream", 0, data.data(), data_len, true);
  auto run = rq_drive_transmit_loop(encoder, 256 + 4, K);

  File decoder(encoder.meta());
  for (auto& s : run.symbols) {
    if (s.id() < K) decoder.put_symbol(s);
  }
  EXPECT_TRUE(decoder.complete()) << "no repair symbol delivered, yet nothing is missing";
  EXPECT_EQ(memcmp(decoder.buffer(), data.data(), data_len), 0);
}

// The point of the repair symbols: source symbols alone are not enough once
// some are lost, and the budget that now reaches the wire must close the gap.
TEST(RaptorQRepairTransmissionTest, RepairSymbolsRecoverLostSourceSymbols) {
  const size_t data_len = 20000;
  const uint32_t K = 79;
  std::vector<char> data(data_len);
  std::mt19937 rng(2024);
  for (auto& b : data) b = (char)rng();

  FecOti oti{
    .encoding_id = FecScheme::RaptorQ,
    .encoding_symbol_length = 256,
    .max_source_block_length = 200,
    .max_number_of_encoding_symbols = 90,
  };
  File encoder(1, oti, "test.bin", "application/octet-stream", 0, data.data(), data_len, true);
  auto run = rq_drive_transmit_loop(encoder, 256 + 4, K);
  ASSERT_EQ(run.repair, 11u);

  // Drop one source symbol in every twelve, deliver every repair symbol.
  File decoder(encoder.meta());
  size_t dropped = 0;
  for (auto& s : run.symbols) {
    if (s.id() < K && (s.id() % 12) == 11) { dropped++; continue; }
    decoder.put_symbol(s);
  }
  ASSERT_GT(dropped, 0u);
  EXPECT_TRUE(decoder.complete()) << dropped << " source symbols lost and not recovered";
  EXPECT_EQ(memcmp(decoder.buffer(), data.data(), data_len), 0);
}


/* RaptorQ is offered for general FLUTE only. TS 26.346 V18.2.0 clause L.4.7 names the schemes the
   MBMS Download Profile admits: "Regarding Application Layer FEC support, the two FEC schemes
   referenced in this specification, the Compact No-Code FEC scheme as specified in RFC 3695 [13],
   and the Raptor FEC scheme as specified in RFC 5053 [91] are optional to implement by the BM-SC
   and mandatory to support by the UE." RaptorQ is RFC 6330 and is not referenced by TS 26.346. */
namespace {

LibFlute::FecOti raptorq_oti() {
  LibFlute::FecOti oti{};
  oti.encoding_id = LibFlute::FecScheme::RaptorQ;
  oti.encoding_symbol_length = 1200;
  oti.max_source_block_length = 64;
  return oti;
}

}  // namespace

TEST(ProfileFecSchemeTest, RaptorQRefusedUnderThe3gppProfiles) {
  boost::asio::io_context io;
  EXPECT_THROW(
      LibFlute::Transmitter("239.1.4.10", 5000, /*tsi*/ 1, /*mtu*/ 1400, /*rate_limit*/ 0, io,
                            std::nullopt, LibFlute::FileDeliveryTable::FDT_NS_NONE,
                            /*active*/ false, std::nullopt, raptorq_oti(),
                            LibFlute::Profile::Ts26517),
      std::runtime_error);
}

TEST(ProfileFecSchemeTest, RaptorQAllowedOutsideThe3gppProfiles) {
  boost::asio::io_context io;
  EXPECT_NO_THROW(
      LibFlute::Transmitter("239.1.4.11", 5000, /*tsi*/ 1, /*mtu*/ 1400, /*rate_limit*/ 0, io,
                            std::nullopt, LibFlute::FileDeliveryTable::FDT_NS_NONE,
                            /*active*/ false, std::nullopt, raptorq_oti(),
                            LibFlute::Profile::Unprofiled));
}

TEST(ProfileFecSchemeTest, RaptorRemainsAvailableUnderThe3gppProfiles) {
  auto oti = raptorq_oti();
  oti.encoding_id = LibFlute::FecScheme::Raptor;
  boost::asio::io_context io;
  EXPECT_NO_THROW(
      LibFlute::Transmitter("239.1.4.12", 5000, /*tsi*/ 1, /*mtu*/ 1400, /*rate_limit*/ 0, io,
                            std::nullopt, LibFlute::FileDeliveryTable::FDT_NS_NONE,
                            /*active*/ false, std::nullopt, oti,
                            LibFlute::Profile::Ts26517));
}

/* The admissible set is closed, so the check has to be a match against the two schemes clause
   L.4.7 names and not a rejection of RaptorQ by name. A scheme this library does not implement is
   the case that separates the two readings: the denylist admitted it, the allowlist refuses it.
   Reached by casting, because no enumerator names such a scheme; that is exactly the state a
   future addition to the enumeration would create before anyone revisited this check. */
TEST(ProfileFecSchemeTest, ASchemeOutsideTheAdmissibleSetIsRefusedUnderThe3gppProfiles) {
  EXPECT_FALSE(LibFlute::is_3gpp_admissible_fec_scheme(static_cast<LibFlute::FecScheme>(7)));
  EXPECT_TRUE(LibFlute::is_3gpp_admissible_fec_scheme(LibFlute::FecScheme::CompactNoCode));
  EXPECT_TRUE(LibFlute::is_3gpp_admissible_fec_scheme(LibFlute::FecScheme::Raptor));
  EXPECT_FALSE(LibFlute::is_3gpp_admissible_fec_scheme(LibFlute::FecScheme::RaptorQ));

  auto oti = raptorq_oti();
  oti.encoding_id = static_cast<LibFlute::FecScheme>(7);
  boost::asio::io_context io;
  EXPECT_THROW(
      LibFlute::Transmitter("239.1.4.13", 5000, /*tsi*/ 1, /*mtu*/ 1400, /*rate_limit*/ 0, io,
                            std::nullopt, LibFlute::FileDeliveryTable::FDT_NS_NONE,
                            /*active*/ false, std::nullopt, oti,
                            LibFlute::Profile::Ts26346),
      std::runtime_error);
}

/* This branch adds the enumerator, so it has to add the wire identifier alongside it, or a
   received FDT declaring RFC 6330's FEC Encoding ID 6 would be refused by the parser. */
TEST(ProfileFecSchemeTest, TheRaptorQEncodingIdMapsToTheScheme) {
  EXPECT_EQ(LibFlute::fec_scheme_from_encoding_id(6), LibFlute::FecScheme::RaptorQ);
  EXPECT_EQ(LibFlute::fec_scheme_from_encoding_id(0), LibFlute::FecScheme::CompactNoCode);
  EXPECT_EQ(LibFlute::fec_scheme_from_encoding_id(1), LibFlute::FecScheme::Raptor);
  EXPECT_FALSE(LibFlute::fec_scheme_from_encoding_id(7).has_value());
}
