// libflute - FLUTE/ALC library
//
// Tests for source block partitioning's divisors.
//
// File::encode(), the Content-Encoding path, is deliberately NOT covered here. It runs inside the
// File constructor and the only way to reach it with a usable FEC OTI is through
// Transmitter::FileDescription, whose merge_fec_oti() is protected, so a unit test cannot
// configure one without standing up a Transmitter with its sockets and io_context. That gap is
// recorded rather than worked around; see the commit that fixed the compress loop.

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include <boost/asio.hpp>

#include <memory>
#include <string>

#include "File.h"
#include "Transmitter.h"

using namespace LibFlute;

namespace {

FecOti oti_with(uint32_t encoding_symbol_length, uint32_t max_source_block_length) {
  FecOti oti{};
  oti.encoding_id = FecScheme::CompactNoCode;
  oti.transfer_length = 4096;
  oti.encoding_symbol_length = encoding_symbol_length;
  oti.max_source_block_length = max_source_block_length;
  return oti;
}

std::shared_ptr<File> build(const FecOti &oti, std::vector<char> &data) {
  return std::make_shared<File>(/*toi*/1, oti, "http://example.invalid/o",
                                "application/octet-stream", /*expires*/0,
                                data.data(), data.size(), /*copy_data*/true);
}

}  // namespace

/* Both values are used as denominators in RFC 5052 clause 9.1 partitioning. A default-constructed
   FecOti leaves them 0, which made the first division produce inf, the block count inf, and block
   creation effectively unbounded, so the constructor hung instead of reporting anything. The
   public constructors accept a FecOti without inspecting it, so this is reachable by a caller. */

TEST(PartitioningDivisorTest, UsableFecOtiIsAccepted) {
  std::vector<char> data(4096, 'x');
  EXPECT_NO_THROW(build(oti_with(1400, 64), data));
}

TEST(PartitioningDivisorTest, ZeroEncodingSymbolLengthIsRefusedNotHung) {
  std::vector<char> data(4096, 'x');
  EXPECT_THROW(build(oti_with(0, 64), data), std::runtime_error);
}

TEST(PartitioningDivisorTest, ZeroMaxSourceBlockLengthIsRefusedNotHung) {
  std::vector<char> data(4096, 'x');
  EXPECT_THROW(build(oti_with(1400, 0), data), std::runtime_error);
}

TEST(PartitioningDivisorTest, DefaultConstructedFecOtiIsRefused) {
  // The exact shape that hung: nothing configured at all.
  std::vector<char> data(4096, 'x');
  FecOti bare{};
  bare.encoding_id = FecScheme::CompactNoCode;
  bare.transfer_length = 4096;
  EXPECT_THROW(build(bare, data), std::runtime_error);
}


/* The MBMS Download Profile permits content encoding and provides no carrier for the resulting
   transfer length, so this sender declines it there. TS 26.346 V18.2.0 clause L.4.4 forbids
   Transfer-Length in the FDT, clause 7.2.8 forbids EXT_FTI on a content packet, and RFC 3926 clause
   3.4.2 lets Content-Length stand in only where no encoding was applied. Raised as
   5G-MAG/Standards#212. Declining to encode is conformant, the attribute being optional for a
   sender; the verbatim clauses are quoted at the check itself in Transmitter.cpp. */
namespace {

std::shared_ptr<Transmitter::FileDescription> gzipped_file() {
  const std::vector<char> payload(4096, 'x');
  auto fd = std::make_shared<Transmitter::FileDescription>("test/compressible.bin", payload);
  fd->set_compression(Transmitter::FileDescription::COMPRESSION_GZIP);
  return fd;
}

}  // namespace

TEST(ProfileContentEncodingTest, RefusedUnderThe3gppProfiles) {
  boost::asio::io_context io;
  Transmitter tx("239.1.2.30", 5000, /*tsi*/ 1, /*mtu*/ 1400, /*rate_limit*/ 0, io,
                 /*tunnel_endpoint*/ std::nullopt, FileDeliveryTable::FDT_NS_NONE,
                 /*active*/ false, /*source_address*/ std::nullopt, Profile::Ts26517);
  EXPECT_THROW(tx.send(gzipped_file()), std::runtime_error)
      << "a gzip-encoded object was accepted under a profile that cannot carry its transfer length";
}

TEST(ProfileContentEncodingTest, AllowedOutsideTheProfile) {
  boost::asio::io_context io;
  Transmitter tx("239.1.2.31", 5000, /*tsi*/ 1, /*mtu*/ 1400, /*rate_limit*/ 0, io,
                 /*tunnel_endpoint*/ std::nullopt, FileDeliveryTable::FDT_NS_NONE,
                 /*active*/ false, /*source_address*/ std::nullopt, Profile::Unprofiled);
  EXPECT_NO_THROW(tx.send(gzipped_file()))
      << "plain RFC 3926 permits Content-Encoding, and Transfer-Length with it";
}

TEST(ProfileContentEncodingTest, AnUnencodedObjectIsUnaffected) {
  boost::asio::io_context io;
  Transmitter tx("239.1.2.32", 5000, /*tsi*/ 1, /*mtu*/ 1400, /*rate_limit*/ 0, io,
                 /*tunnel_endpoint*/ std::nullopt, FileDeliveryTable::FDT_NS_NONE,
                 /*active*/ false, /*source_address*/ std::nullopt, Profile::Ts26517);
  const std::vector<char> payload(4096, 'y');
  auto fd = std::make_shared<Transmitter::FileDescription>("test/plain.bin", payload);
  EXPECT_NO_THROW(tx.send(fd));
}
