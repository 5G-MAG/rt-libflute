#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "AlcPacket.h"
#include "FileDeliveryTable.h"

using namespace LibFlute;

namespace {

FecOti make_fec_oti() {
  FecOti oti{};
  oti.encoding_id = FecScheme::CompactNoCode;
  oti.instance_id = 0;
  oti.transfer_length = 4096;
  oti.encoding_symbol_length = 1400;
  oti.max_source_block_length = 64;
  oti.max_number_of_encoding_symbols = 0;
  return oti;
}

FileDeliveryTable::FileEntry make_entry(const FecOti &oti) {
  FileDeliveryTable::FileEntry e{};
  e.toi = 1;
  e.content_location = "http://example.invalid/seg1.m4s";
  e.content_length = 4096;
  e.expires = 0;
  e.fec_oti = oti;
  e.cache_control.no_cache = false;
  return e;
}

// Serialise a one-file FDT. The profile argument is left at its library default unless a
// test is specifically about the non-3GPP behaviour, so the default itself stays under test.
std::string emit(FileDeliveryTable::FdtNamespace ns) {
  auto oti = make_fec_oti();
  FileDeliveryTable fdt(1, oti, ns);
  fdt.add(make_entry(oti));
  return fdt.to_string();
}

std::string emit(FileDeliveryTable::FdtNamespace ns, Profile profile) {
  auto oti = make_fec_oti();
  FileDeliveryTable fdt(1, oti, ns, profile);
  fdt.add(make_entry(oti));
  return fdt.to_string();
}

}  // namespace

/* TS 26.346 V18.2.0 clause L.4.4 lists Transfer-Length among the attributes that
   "shall not be carried in the FDT sent by the FLUTE sender". These assert on the
   emitted XML rather than on the FileEntry, because the object carrying a value the
   serialiser then withholds is exactly the case that has to pass. */

TEST(MbmsDownloadProfileTest, TransferLengthNotCarriedInConsolidatedV2Schema) {
  EXPECT_EQ(emit(FileDeliveryTable::FDT_NS_3GPP_CONSOLIDATED_V2).find("Transfer-Length"),
            std::string::npos);
}

TEST(MbmsDownloadProfileTest, TransferLengthNotCarriedInDraft2005Schema) {
  EXPECT_EQ(emit(FileDeliveryTable::FDT_NS_DRAFT_2005).find("Transfer-Length"),
            std::string::npos);
}

TEST(GeneralFluteTest, TransferLengthStillCarriedOutsideTheProfile) {
  // Plain RFC 3926, where clause 3.4.2 permits the attribute, so it is kept. This has to be
  // asked for explicitly: the library default is the 3GPP profile.
  EXPECT_NE(emit(FileDeliveryTable::FDT_NS_RFC3926, Profile::GeneralFlute).find("Transfer-Length"),
            std::string::npos);
}

/* The delimitation itself. A session is bound by the general FLUTE documents always, and by
   TS 26.346 annex L.4 only under the 3GPP profile, which is the default. */

TEST(ProfileDefaultTest, DefaultIsTheMbms3gppProfile) {
  auto oti = make_fec_oti();
  FileDeliveryTable fdt(1, oti);
  EXPECT_EQ(fdt.profile(), Profile::Mbms3gpp);
}

TEST(ProfileDefaultTest, ProfileNotFdtNamespaceDecidesTheRestriction) {
  // The namespace says which schema is emitted; the profile says which obligations apply.
  // Same namespace, opposite outcomes, driven only by the profile.
  const auto ns = FileDeliveryTable::FDT_NS_RFC3926;
  EXPECT_EQ(emit(ns, Profile::Mbms3gpp).find("Transfer-Length"), std::string::npos);
  EXPECT_NE(emit(ns, Profile::GeneralFlute).find("Transfer-Length"), std::string::npos);
}

TEST(MbmsDownloadProfileTest, ContentLengthIsCarriedInEveryMode) {
  // Guards the fallback the prohibition relies on: with Transfer-Length withheld, a
  // receiver derives the transfer length from Content-Length.
  for (auto ns : {FileDeliveryTable::FDT_NS_3GPP_CONSOLIDATED_V2,
                  FileDeliveryTable::FDT_NS_DRAFT_2005,
                  FileDeliveryTable::FDT_NS_RFC3926}) {
    EXPECT_NE(emit(ns).find("Content-Length"), std::string::npos);
  }
}

/* TS 26.346 V18.2.0 clause L.4.3 forbids the sender using the Complete attribute, while its
   NOTE keeps receiver support mandatory. These cover the sender half; the parser is unchanged. */

TEST(MbmsDownloadProfileTest, CompleteNotCarriedUnderThe3gppProfile) {
  auto oti = make_fec_oti();
  FileDeliveryTable fdt(1, oti, FileDeliveryTable::FDT_NS_3GPP_CONSOLIDATED_V2);
  fdt.add(make_entry(oti));
  fdt.set_complete(true);
  EXPECT_EQ(fdt.to_string().find("Complete"), std::string::npos);
}

TEST(GeneralFluteTest, CompleteStillCarriedOutsideTheProfile) {
  // RFC 3926 clause 3.4.2 permits it, so plain FLUTE keeps it.
  auto oti = make_fec_oti();
  FileDeliveryTable fdt(1, oti, FileDeliveryTable::FDT_NS_RFC3926, Profile::GeneralFlute);
  fdt.add(make_entry(oti));
  fdt.set_complete(true);
  EXPECT_NE(fdt.to_string().find("Complete"), std::string::npos);
}

/* TS 26.346 V18.2.0 clause L.4.2 forbids FEC-OTI-FEC-Instance-ID at both levels. The pre-existing
   guard was on the value, so a non-zero instance ID leaked the attribute into a 3GPP session. */

TEST(MbmsDownloadProfileTest, FecInstanceIdNotCarriedUnderThe3gppProfile) {
  auto oti = make_fec_oti();
  oti.instance_id = 7;  // non-zero, so the old value-only guard would have emitted it
  FileDeliveryTable fdt(1, oti, FileDeliveryTable::FDT_NS_3GPP_CONSOLIDATED_V2);
  auto e = make_entry(oti);
  e.fec_oti.instance_id = 9;  // differs from the global, so the File-level guard would fire too
  fdt.add(e);
  EXPECT_EQ(fdt.to_string().find("FEC-OTI-FEC-Instance-ID"), std::string::npos);
}

TEST(GeneralFluteTest, FecInstanceIdNotCarriedOutsideTheProfileEither) {
  /* Corrected from an earlier version of this test, which asserted the attribute WAS carried
     under general FLUTE. RFC 5052 clause 6.2.4 forbids it for a Fully-Specified FEC scheme
     regardless of profile, and both schemes this library implements are Fully-Specified, so the
     3GPP profile was never the binding constraint. */
  auto oti = make_fec_oti();
  oti.instance_id = 7;
  FileDeliveryTable fdt(1, oti, FileDeliveryTable::FDT_NS_RFC3926, Profile::GeneralFlute);
  fdt.add(make_entry(oti));
  EXPECT_EQ(fdt.to_string().find("FEC-OTI-FEC-Instance-ID"), std::string::npos);
}

/* TS 26.346 V18.2.0 clause L.4.2 permits Content-Encoding only when set to 'gzip', and
   prohibits any other value. Refused rather than silently dropped: dropping it would leave
   encoded payload with nothing on the wire saying so. */

TEST(MbmsDownloadProfileTest, GzipContentEncodingIsAccepted) {
  auto oti = make_fec_oti();
  FileDeliveryTable fdt(1, oti, FileDeliveryTable::FDT_NS_3GPP_CONSOLIDATED_V2);
  auto e = make_entry(oti);
  e.content_encoding = "gzip";
  EXPECT_NO_THROW(fdt.add(e));
  EXPECT_NE(fdt.to_string().find("gzip"), std::string::npos);
}

TEST(MbmsDownloadProfileTest, NonGzipContentEncodingIsRefused) {
  auto oti = make_fec_oti();
  FileDeliveryTable fdt(1, oti, FileDeliveryTable::FDT_NS_3GPP_CONSOLIDATED_V2);
  auto e = make_entry(oti);
  e.content_encoding = "deflate";
  EXPECT_THROW(fdt.add(e), std::invalid_argument);
}

TEST(MbmsDownloadProfileTest, AbsentContentEncodingIsAccepted) {
  // The attribute is a "may", so carrying nothing is conformant.
  auto oti = make_fec_oti();
  FileDeliveryTable fdt(1, oti, FileDeliveryTable::FDT_NS_3GPP_CONSOLIDATED_V2);
  EXPECT_NO_THROW(fdt.add(make_entry(oti)));
}

TEST(GeneralFluteTest, NonGzipContentEncodingIsAllowedOutsideTheProfile) {
  // RFC 3926 places no such restriction, so plain FLUTE accepts it.
  auto oti = make_fec_oti();
  FileDeliveryTable fdt(1, oti, FileDeliveryTable::FDT_NS_RFC3926, Profile::GeneralFlute);
  auto e = make_entry(oti);
  e.content_encoding = "deflate";
  EXPECT_NO_THROW(fdt.add(e));
  EXPECT_NE(fdt.to_string().find("deflate"), std::string::npos);
}

/* ---------------------------------------------------------------------------------------------
   LCT header parse robustness. General FLUTE, not 3GPP: these are properties of the RFC 3451
   header itself and apply in both profiles. The inputs are built as raw bytes because the
   transmitter cannot produce them, which is why nothing previously covered this path.

   Layout, RFC 3451 clause 5.1: byte 0 is V(4) C(2) r(2); byte 1 is S(1) O(2) H(1) T(1) R(1)
   A(1) B(1); byte 2 is HDR_LEN in 32-bit words; byte 3 is the Codepoint.
   --------------------------------------------------------------------------------------------- */

namespace {

// V=1, H=1 (so a 16-bit TSI and TOI half-word are present), everything else clear.
// Standard header is then 2 + H = 3 words = 12 bytes.
std::vector<char> lct_packet(uint8_t hdr_len_words, const std::vector<uint8_t> &extension = {}) {
  std::vector<char> p{static_cast<char>(0x10),   // V=1, C=0, r=0
                      static_cast<char>(0x10),   // H=1
                      static_cast<char>(hdr_len_words),
                      static_cast<char>(0x00)};  // Codepoint 0 = Compact No-Code
  p.resize(12, 0);                               // CCI (4) + TSI half-word (2) + TOI half-word (2)
  for (auto b : extension) p.push_back(static_cast<char>(b));
  return p;
}

}  // namespace

TEST(LctHeaderParseTest, WellFormedMinimalHeaderStillParses) {
  auto p = lct_packet(3);
  EXPECT_NO_THROW(AlcPacket(p.data(), p.size()));
}

TEST(LctHeaderParseTest, HeaderLongerThanTheDatagramIsRejected) {
  // HDR_LEN claims 10 words (40 bytes) but only 12 bytes were received. Without the check every
  // subsequent read runs past the end of the buffer.
  auto p = lct_packet(10);
  EXPECT_THROW(AlcPacket(p.data(), p.size()), std::runtime_error);
}

TEST(LctHeaderParseTest, HeaderShorterThanItsOwnFlagsIsRejected) {
  // Flags require 3 words; the header claims 2. Without the check the extension-space
  // calculation goes negative and becomes a very large size_t.
  auto p = lct_packet(2);
  EXPECT_THROW(AlcPacket(p.data(), p.size()), std::runtime_error);
}

TEST(LctHeaderParseTest, ZeroLengthHeaderExtensionIsRejectedRatherThanLooping) {
  // HET below 128 is a variable-length extension, so HEL is read and gives the length. HEL 0
  // means a zero-length extension: the walk would consume nothing and never terminate.
  auto p = lct_packet(4, {100 /* HET: variable-length, and not one this library handles */,
                          0 /* HEL = 0 */, 0, 0});
  EXPECT_THROW(AlcPacket(p.data(), p.size()), std::runtime_error);
}
