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
// Unit tests for the from-scratch RFC 5053 Raptor codec (fec/RaptorCodec.h)
// and its integration into LibFlute::File. Two levels, matching how this
// was actually developed and verified:
//  - RaptorCodecTest: the codec in isolation, round-tripping through
//    simulated symbol loss with realistic repair overhead.
//  - FileRaptorTest: the real File/EncodingSymbol wire-format API,
//    end-to-end -- this is what caught the one real bug found during
//    development (a missing zero-pad for a file's final short symbol; the
//    standalone codec test alone never exercised a non-uniform symbol
//    length, so it passed even with that bug present).
#include <gtest/gtest.h>
#include <random>
#include <algorithm>
#include "fec/RaptorCodec.h"
#include "File.h"

using namespace LibFlute;
using namespace LibFlute::Raptor;

namespace {

std::vector<std::vector<uint8_t>> make_source(uint32_t K, size_t T, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<std::vector<uint8_t>> src(K, std::vector<uint8_t>(T));
  for (auto& sym : src) for (auto& b : sym) b = (uint8_t)rng();
  return src;
}

// Encodes K source symbols, then delivers `received_overhead` more than K
// out of a pool of `pool_overhead` extra repair symbols (a random subset,
// so which ones survive is independent of source vs. repair), and checks
// the decoder recovers the original data exactly.
void expect_round_trip(uint32_t K, size_t T, uint32_t seed, uint32_t received_overhead, uint32_t pool_overhead) {
  RaptorCodec encoder(K);
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

  RaptorCodec decoder(K);
  for (uint32_t i = 0; i < received_count; i++) {
    decoder.add_received_symbol(pool[i], encoder.generate_encoding_symbol(pool[i], intermediate));
  }

  ASSERT_TRUE(decoder.can_decode()) << "needs " << decoder.symbols_needed() << " more independent symbols";
  EXPECT_EQ(decoder.decode_source_symbols(), source);
}

} // namespace

TEST(FileRaptorTest, EndToEndThroughRealFileApiSingleBlock) {
  const size_t data_len = 20000;
  std::vector<char> data(data_len);
  std::mt19937 rng(123);
  for (auto& b : data) b = (char)rng();

  FecOti oti{
    .encoding_id = FecScheme::Raptor,
    .encoding_symbol_length = 256,
    .max_source_block_length = 200, // single block: Kt=79 <= 200
    .max_number_of_encoding_symbols = 150,
  };

  File encoder(1, oti, "test.bin", "application/octet-stream", 0, data.data(), data_len, true);
  auto symbols = encoder.get_next_symbols(1024 * 1024);
  ASSERT_FALSE(symbols.empty());
  EXPECT_EQ(encoder.fec_oti().nof_source_blocks, 1u);

  File decoder(encoder.meta());

  std::vector<size_t> order(symbols.size());
  for (size_t i = 0; i < order.size(); i++) order[i] = i;
  std::shuffle(order.begin(), order.end(), rng);
  size_t to_deliver = std::min(symbols.size(), (size_t)100); // K(~79) + ~20 overhead
  std::vector<bool> deliver(symbols.size(), false);
  for (size_t i = 0; i < to_deliver; i++) deliver[order[i]] = true;

  for (size_t i = 0; i < symbols.size(); i++) {
    if (deliver[i]) decoder.put_symbol(symbols[i]);
  }

  ASSERT_TRUE(decoder.complete());
  ASSERT_EQ(decoder.length(), data_len);
  EXPECT_EQ(0, memcmp(decoder.buffer(), data.data(), data_len));
}

TEST(FileRaptorTest, EndToEndThroughRealFileApiMultiBlock) {
  // Forces multiple source blocks (K capped well below Kt), exercising
  // per-block codec setup/repair generation independently.
  const size_t data_len = 20000;
  std::vector<char> data(data_len);
  std::mt19937 rng(456);
  for (auto& b : data) b = (char)rng();

  FecOti oti{
    .encoding_id = FecScheme::Raptor,
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
  size_t to_deliver = (size_t)(symbols.size() * 0.92); // small per-block K needs generous overhead
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
// The LCT Codepoint field, which is where the receive path gets the FEC scheme from and nowhere
// else. The sender previously never wrote it, so every packet claimed Compact No-Code whatever
// scheme was in use, and a Raptor session's EXT_FTI was then parsed with the wrong field layout.
//
// RFC 3450 clause 2.2 makes using the field for this a MAY and offers the identity mapping as its
// example; both halves of this library use that mapping. General FLUTE, not 3GPP: TS 26.346 does
// not mention the Codepoint field.
// ----------------------------------------------------------------------------------------------

#include "AlcPacket.h"

namespace {

LibFlute::FecOti codepoint_test_oti(LibFlute::FecScheme scheme) {
  LibFlute::FecOti oti{};
  oti.encoding_id = scheme;
  oti.transfer_length = 1024;
  oti.encoding_symbol_length = 512;
  oti.max_source_block_length = 8;
  return oti;
}

// A packet needs at least one encoding symbol to be meaningful, so tests build a real one.
char codepoint_symbol_data[512] = {0};

std::vector<LibFlute::EncodingSymbol> one_symbol(LibFlute::FecScheme scheme) {
  std::vector<LibFlute::EncodingSymbol> v;
  v.emplace_back(/*id*/0, /*source_block_number*/0, codepoint_symbol_data,
                 sizeof(codepoint_symbol_data), scheme);
  return v;
}

// Byte 3 of the LCT header is the Codepoint (RFC 3451 clause 5.1).
uint8_t emitted_codepoint(LibFlute::FecScheme scheme) {
  auto symbols = one_symbol(scheme);
  LibFlute::AlcPacket p(/*tsi*/1, /*toi*/1, codepoint_test_oti(scheme), symbols, 512,
                        /*fdt_instance_id*/0);
  return static_cast<uint8_t>(p.data()[3]);
}

}  // namespace

// An empty symbol list previously dereferenced end() and crashed; it is now refused.
TEST(LctCodepointTest, EmptySymbolListIsRefusedRatherThanCrashing) {
  std::vector<LibFlute::EncodingSymbol> none;
  EXPECT_THROW(LibFlute::AlcPacket(1, 1, codepoint_test_oti(LibFlute::FecScheme::Raptor),
                                   none, 512, 0),
               std::invalid_argument);
}

TEST(LctCodepointTest, RaptorSessionEmitsFecEncodingIdOne) {
  EXPECT_EQ(emitted_codepoint(LibFlute::FecScheme::Raptor), 1);
}

TEST(LctCodepointTest, CompactNoCodeSessionEmitsFecEncodingIdZero) {
  EXPECT_EQ(emitted_codepoint(LibFlute::FecScheme::CompactNoCode), 0);
}

TEST(LctCodepointTest, SchemeSurvivesARoundTripThroughTheWire) {
  // The defect this guards: the two halves of the library disagreeing about the scheme.
  auto symbols = one_symbol(LibFlute::FecScheme::Raptor);
  LibFlute::AlcPacket sent(1, 1, codepoint_test_oti(LibFlute::FecScheme::Raptor), symbols, 512, 0);
  LibFlute::AlcPacket received(sent.data(), sent.size());
  EXPECT_EQ(received.fec_scheme(), LibFlute::FecScheme::Raptor);
}

// ----------------------------------------------------------------------------------------------
// The scheme-specific FEC OTI in the FDT. This previously emitted three attribute names that
// appear in no specification (zero occurrences of any of them in TS 26.346) instead of the one
// the schema defines, FEC-OTI-Scheme-Specific-Info, typed xs:base64Binary in TS 26.346 annex
// L.6.1 at both the FDT-Instance and File levels.
//
// RFC 5053 clause 3.2.3 fixes the payload: "a 4-octet field consisting of the parameters
// Z (2 octets), N (1 octet), and Al (1 octet)".
//
// Compact No-Code carries nothing here: RFC 3695 clause 3 defines a FEC Payload ID for it and no
// scheme-specific element, and the schema makes the attribute use="optional".
// ----------------------------------------------------------------------------------------------

#include "FileDeliveryTable.h"

namespace {

LibFlute::FecOti ssi_oti(LibFlute::FecScheme scheme) {
  LibFlute::FecOti oti{};
  oti.encoding_id = scheme;
  oti.transfer_length = 4096;
  oti.encoding_symbol_length = 512;
  oti.max_source_block_length = 64;
  oti.nof_source_blocks = 0x0102;   // Z, two octets, so byte order is observable
  oti.nof_sub_blocks = 3;           // N
  oti.symbol_alignment = 4;         // Al
  return oti;
}

std::string ssi_emit(LibFlute::FecScheme scheme) {
  auto oti = ssi_oti(scheme);
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

TEST(SchemeSpecificOtiTest, RaptorEmitsTheSchemaDefinedAttribute) {
  auto out = ssi_emit(LibFlute::FecScheme::Raptor);
  EXPECT_NE(out.find("FEC-OTI-Scheme-Specific-Info"), std::string::npos);
  // 0x01 0x02 0x03 0x04 base64-encodes to AQIDBA==
  EXPECT_NE(out.find("AQIDBA=="), std::string::npos);
}

TEST(SchemeSpecificOtiTest, NoneOfTheInventedAttributeNamesAreEmitted) {
  auto out = ssi_emit(LibFlute::FecScheme::Raptor);
  EXPECT_EQ(out.find("FEC-OTI-Number-Of-Source-Blocks"), std::string::npos);
  EXPECT_EQ(out.find("FEC-OTI-Number-Of-Sub-Blocks"), std::string::npos);
  EXPECT_EQ(out.find("FEC-OTI-Symbol-Alignment-Parameter"), std::string::npos);
}

TEST(SchemeSpecificOtiTest, CompactNoCodeCarriesNoSchemeSpecificInfo) {
  // L11: the scheme defines no scheme-specific OTI, so there is nothing to carry.
  auto out = ssi_emit(LibFlute::FecScheme::CompactNoCode);
  EXPECT_EQ(out.find("FEC-OTI-Scheme-Specific-Info"), std::string::npos);
}

TEST(SchemeSpecificOtiTest, RoundTripsThroughTheFdt) {
  auto out = ssi_emit(LibFlute::FecScheme::Raptor);
  std::vector<char> buf(out.begin(), out.end());
  buf.push_back('\0');
  LibFlute::FileDeliveryTable parsed(1, buf.data(), buf.size());
  ASSERT_FALSE(parsed.file_entries().empty());
  const auto &oti = parsed.file_entries().front().fec_oti;
  EXPECT_EQ(oti.nof_source_blocks, 0x0102);
  EXPECT_EQ(oti.nof_sub_blocks, 3);
  EXPECT_EQ(oti.symbol_alignment, 4);
}

// The transmit loop as Transmitter actually drives it: one datagram-sized
// request at a time, each batch acknowledged before the next is asked for.
// The tests above instead pull every symbol in a single 1 MB call and never
// acknowledge anything, which is why they passed while no repair symbol had
// ever reached the wire.
namespace {
struct TransmitRun {
  size_t total = 0;
  size_t repair = 0;
  uint32_t max_esi = 0;
  bool complete = false;
  std::vector<LibFlute::EncodingSymbol> symbols;
};

TransmitRun drive_transmit_loop(LibFlute::File& f, size_t datagram_payload, uint32_t k) {
  TransmitRun r;
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

// Kt = ceil(20000/256) = 79 source symbols, one source block (79 <= 200), and a
// FEC OTI permitting 100 encoding symbols for it, so the repair budget is
// 100 - 79 = 21 and a fully transmitted block is exactly 100 symbols.
TEST(RaptorRepairTransmissionTest, RepairSymbolsAreTransmittedAfterTheSourceSymbols) {
  const size_t data_len = 20000;
  const uint32_t K = 79;
  std::vector<char> data(data_len);
  std::mt19937 rng(4242);
  for (auto& b : data) b = (char)rng();

  FecOti oti{
    .encoding_id = FecScheme::Raptor,
    .encoding_symbol_length = 256,
    .max_source_block_length = 200,
    .max_number_of_encoding_symbols = 100,
  };
  File encoder(1, oti, "test.bin", "application/octet-stream", 0, data.data(), data_len, true);
  ASSERT_EQ(encoder.fec_oti().nof_source_blocks, 1u);

  auto run = drive_transmit_loop(encoder, 256 + 4, K);

  EXPECT_EQ(run.total, 100u) << "the block's whole encoding symbol budget must go out";
  EXPECT_EQ(run.repair, 21u) << "repair symbols must reach the wire, not just be budgeted";
  EXPECT_EQ(run.max_esi, 99u);
  EXPECT_TRUE(run.complete) << "the file must still finish once its repair budget is out";
}

// A block whose source symbols are all acknowledged is not yet finished: the
// repair budget still has to go out, and complete() must not report otherwise.
TEST(RaptorRepairTransmissionTest, FileIsNotCompleteWhileRepairSymbolsAreStillOwed) {
  const size_t data_len = 20000;
  const uint32_t K = 79;
  std::vector<char> data(data_len, 'x');
  FecOti oti{
    .encoding_id = FecScheme::Raptor,
    .encoding_symbol_length = 256,
    .max_source_block_length = 200,
    .max_number_of_encoding_symbols = 100,
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
  EXPECT_EQ(sent, 100u);
}

// The condition above is encoder-side only. A receiver that has recovered every
// source symbol is finished, and knows nothing about any repair budget.
TEST(RaptorRepairTransmissionTest, ReceiverCompletesOnSourceSymbolsAlone) {
  const size_t data_len = 20000;
  const uint32_t K = 79;
  std::vector<char> data(data_len);
  std::mt19937 rng(99);
  for (auto& b : data) b = (char)rng();

  FecOti oti{
    .encoding_id = FecScheme::Raptor,
    .encoding_symbol_length = 256,
    .max_source_block_length = 200,
    .max_number_of_encoding_symbols = 100,
  };
  File encoder(1, oti, "test.bin", "application/octet-stream", 0, data.data(), data_len, true);
  auto run = drive_transmit_loop(encoder, 256 + 4, K);

  File decoder(encoder.meta());
  for (auto& s : run.symbols) {
    if (s.id() < K) decoder.put_symbol(s);
  }
  EXPECT_TRUE(decoder.complete()) << "no repair symbol delivered, yet nothing is missing";
  EXPECT_EQ(memcmp(decoder.buffer(), data.data(), data_len), 0);
}

// Compact No-Code has no repair symbols at all, and must still finish on its
// last source symbol.
TEST(RaptorRepairTransmissionTest, CompactNoCodeIsUnaffected) {
  const size_t data_len = 20000;
  const uint32_t K = 79;
  std::vector<char> data(data_len, 'y');
  FecOti oti{
    .encoding_id = FecScheme::CompactNoCode,
    .encoding_symbol_length = 256,
    .max_source_block_length = 200,
    .max_number_of_encoding_symbols = 100,
  };
  File encoder(1, oti, "test.bin", "application/octet-stream", 0, data.data(), data_len, true);
  auto run = drive_transmit_loop(encoder, 256 + 4, K);

  EXPECT_EQ(run.repair, 0u);
  EXPECT_EQ(run.total, (size_t)K);
  EXPECT_TRUE(run.complete);
}

// The repair budget comes from one of two operator-set sources and nothing else:
// the FEC OTI's encoding-symbol maximum, or failing that the session's FEC
// redundancy level as a percentage of k.
// TS 26.346 V18.2.0 clause 7.3.2.11 fixes the unit: "For example, a FEC
// redundancy level of 40% means that for an FEC-encoded block of K symbols,
// 1.4*K symbols are broadcast over the air."
namespace {
// Kt = ceil(data_len/256) source symbols in one block, with no encoding-symbol
// maximum set, so the redundancy level alone decides the repair count.
size_t repair_symbols_at_level(size_t data_len, uint32_t level, uint32_t k,
                               uint32_t max_encoding_symbols = 0) {
  std::vector<char> data(data_len, 'z');
  FecOti oti{
    .encoding_id = FecScheme::Raptor,
    .encoding_symbol_length = 256,
    .max_source_block_length = 200,
    .max_number_of_encoding_symbols = max_encoding_symbols,
  };
  File encoder(1, oti, "test.bin", "application/octet-stream", 0, data.data(), data_len, true);
  encoder.set_fec_redundancy_level(level);
  return drive_transmit_loop(encoder, 256 + 4, k).repair;
}
} // namespace

TEST(FecRedundancyLevelTest, LevelIsAPercentageOfTheSourceBlock) {
  // K = ceil(20000/256) = 79
  EXPECT_EQ(repair_symbols_at_level(20000, 10, 79), 8u);   // ceil(7.9)
  EXPECT_EQ(repair_symbols_at_level(20000, 20, 79), 16u);  // ceil(15.8)
  EXPECT_EQ(repair_symbols_at_level(20000, 40, 79), 32u);  // ceil(31.6), the clause's own example
}

TEST(FecRedundancyLevelTest, TheDefaultIsTenPercentAndIsNotAFloor) {
  // Unset level: the documented default applies, and applies uniformly. The
  // superseded fallback had a floor of 4 symbols resting on no source, which
  // gave a small block proportionally more protection than a large one.
  std::vector<char> data(2000, 'z');
  FecOti oti{
    .encoding_id = FecScheme::Raptor,
    .encoding_symbol_length = 256,
    .max_source_block_length = 200,
  };
  File encoder(1, oti, "test.bin", "application/octet-stream", 0, data.data(), data.size(), true);
  auto run = drive_transmit_loop(encoder, 256 + 4, 8);  // K = ceil(2000/256) = 8
  EXPECT_EQ(run.repair, 1u) << "10% of 8 symbols, with no floor applied";
  EXPECT_EQ(run.total, 9u);
  EXPECT_TRUE(run.complete);
}

TEST(FecRedundancyLevelTest, AnEncodingSymbolMaximumOverridesTheLevel) {
  // The OTI budget is the more specific operator statement, so it wins: 90 - 79,
  // not 50% of 79.
  EXPECT_EQ(repair_symbols_at_level(20000, 50, 79, /*max_encoding_symbols=*/90), 11u);
}

TEST(FecRedundancyLevelTest, LevelZeroSendsNoRepairAndStillCompletes) {
  std::vector<char> data(20000, 'z');
  FecOti oti{
    .encoding_id = FecScheme::Raptor,
    .encoding_symbol_length = 256,
    .max_source_block_length = 200,
  };
  File encoder(1, oti, "test.bin", "application/octet-stream", 0, data.data(), data.size(), true);
  encoder.set_fec_redundancy_level(0);
  auto run = drive_transmit_loop(encoder, 256 + 4, 79);
  EXPECT_EQ(run.repair, 0u) << "an operator asking for no redundancy gets none";
  EXPECT_EQ(run.total, 79u);
  EXPECT_TRUE(run.complete) << "a block owing no repair symbol must not stall the transfer";
}
