#include <gtest/gtest.h>

#include <stdexcept>
#include <optional>
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

TEST(GeneralFluteTest, FecInstanceIdStillCarriedOutsideTheProfile) {
  auto oti = make_fec_oti();
  oti.instance_id = 7;
  FileDeliveryTable fdt(1, oti, FileDeliveryTable::FDT_NS_RFC3926, Profile::GeneralFlute);
  fdt.add(make_entry(oti));
  EXPECT_NE(fdt.to_string().find("FEC-OTI-FEC-Instance-ID"), std::string::npos);
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

/* RFC 3451 clause 5.1 puts SCT (T=1) and ERT (R=1) inside the header, after the TOI and before
   any extension, and both count toward HDR_LEN. TS 26.346 clause L.4.7 says an MBMS network does
   not use them and the UE "should ignore them" -- and ignoring a field still means stepping over
   it, so this applies in both profiles. Byte 1 bits, MSB first: S O O H T R A B. */

TEST(LctHeaderParseTest, SenderCurrentTimeFieldIsAccountedForInTheHeaderLength) {
  // T=1 is bit 4 from the MSB of byte 1, i.e. 0x08, alongside H=1 (0x10).
  std::vector<char> p{static_cast<char>(0x10), static_cast<char>(0x10 | 0x08),
                      static_cast<char>(4), static_cast<char>(0x00)};
  p.resize(16, 0);  // CCI(4) + TSI/TOI half-words(4) + SCT(4) = 4 words after the base word
  EXPECT_NO_THROW(AlcPacket(p.data(), p.size()));
}

TEST(LctHeaderParseTest, ExpectedResidualTimeFieldIsAccountedForInTheHeaderLength) {
  // R=1 is bit 5 from the MSB of byte 1, i.e. 0x04.
  std::vector<char> p{static_cast<char>(0x10), static_cast<char>(0x10 | 0x04),
                      static_cast<char>(4), static_cast<char>(0x00)};
  p.resize(16, 0);
  EXPECT_NO_THROW(AlcPacket(p.data(), p.size()));
}

TEST(LctHeaderParseTest, BothTimingFieldsPresentIsAccountedFor) {
  std::vector<char> p{static_cast<char>(0x10), static_cast<char>(0x10 | 0x08 | 0x04),
                      static_cast<char>(5), static_cast<char>(0x00)};
  p.resize(20, 0);  // ... + SCT(4) + ERT(4)
  EXPECT_NO_THROW(AlcPacket(p.data(), p.size()));
}

/* RFC 3926 clause 3.4.1 requires the EXT_FDT version field to be 1 in a version 1 session, and
   RFC 6726 clause 11.1 records that version 1 and version 2 are not interchangeable. General
   FLUTE, applying in both profiles. EXT_FDT is HET 192, a fixed-length 4-byte extension, so its
   first byte holds V in the top nibble and the FDT Instance ID's top 4 bits in the low nibble. */

namespace {

std::vector<char> packet_with_ext_fdt(uint8_t flute_version) {
  std::vector<char> p{static_cast<char>(0x10), static_cast<char>(0x10),
                      static_cast<char>(4), static_cast<char>(0x00)};
  p.resize(12, 0);
  p.push_back(static_cast<char>(192));                          // HET = EXT_FDT
  p.push_back(static_cast<char>((flute_version & 0x0F) << 4));  // V, then ID bits 19..16
  p.push_back(0);                                               // FDT Instance ID low 16 bits
  p.push_back(0);
  return p;
}

}  // namespace

TEST(FluteVersionTest, VersionOneIsAccepted) {
  auto p = packet_with_ext_fdt(1);
  EXPECT_NO_THROW(AlcPacket(p.data(), p.size()));
}

TEST(FluteVersionTest, VersionTwoIsRejected) {
  // Previously accepted, which meant decoding a session this build does not implement.
  auto p = packet_with_ext_fdt(2);
  EXPECT_THROW(AlcPacket(p.data(), p.size()), std::runtime_error);
}

TEST(FluteVersionTest, VersionZeroIsRejected) {
  auto p = packet_with_ext_fdt(0);
  EXPECT_THROW(AlcPacket(p.data(), p.size()), std::runtime_error);
}

/* TS 26.346 V18.2.0 clause L.6.3 fixes schemaVersion at 2 for the profiled FDT schema, whose
   L.6.1 definition makes it a mandatory child element after the File elements. Keyed on the FDT
   namespace, not the profile: it is required by the schema being emitted. */

TEST(ProfiledFdtSchemaTest, SchemaVersionTwoIsEmittedForTheProfiledSchema) {
  auto out = emit(FileDeliveryTable::FDT_NS_3GPP_CONSOLIDATED_V2);
  EXPECT_NE(out.find("<schemaVersion>2</schemaVersion>"), std::string::npos);
}

TEST(ProfiledFdtSchemaTest, SchemaVersionFollowsTheFileElements) {
  // The schema's sequence is File then schemaVersion, so order is part of validity.
  auto out = emit(FileDeliveryTable::FDT_NS_3GPP_CONSOLIDATED_V2);
  auto file_pos = out.find("<File ");
  auto sv_pos = out.find("<schemaVersion>");
  ASSERT_NE(file_pos, std::string::npos);
  ASSERT_NE(sv_pos, std::string::npos);
  EXPECT_LT(file_pos, sv_pos);
}

TEST(ProfiledFdtSchemaTest, SchemaVersionNotEmittedForOtherSchemas) {
  // Meaningless in a document that does not declare the profiled schema.
  EXPECT_EQ(emit(FileDeliveryTable::FDT_NS_RFC3926).find("schemaVersion"), std::string::npos);
  EXPECT_EQ(emit(FileDeliveryTable::FDT_NS_DRAFT_2005).find("schemaVersion"), std::string::npos);
}

TEST(ProfiledFdtSchemaTest, DelimiterIsNotEmitted) {
  // Clause L.6.3A calls for it only when a future optional element is added; the base sequence
  // has none, so emitting one would not match the schema.
  EXPECT_EQ(emit(FileDeliveryTable::FDT_NS_3GPP_CONSOLIDATED_V2).find("delimiter"),
            std::string::npos);
}

/* The File element's Expires attribute and the mbms2007:Cache-Control Expires element are
   different things: FileType's Expires says when the file stops being valid, while
   CacheControlType is an xs:choice whose Expires is a caching directive to intermediates. They
   were previously set together, emitted from the same member and parsed into the same member, so
   nothing could tell them apart. These give them different values on purpose. */

namespace {

std::string emit_with_expiries(uint64_t file_expires, std::optional<uint64_t> cache_expires) {
  auto oti = make_fec_oti();
  FileDeliveryTable fdt(1, oti, FileDeliveryTable::FDT_NS_3GPP_CONSOLIDATED_V2);
  auto e = make_entry(oti);
  e.expires = file_expires;
  e.cache_control.cache_expires = cache_expires;
  fdt.add(e);
  return fdt.to_string();
}

}  // namespace

TEST(ExpiryAttributesTest, FileAndCacheExpiriesAreEmittedIndependently) {
  // Distinct values, so a single shared member cannot satisfy both assertions.
  auto out = emit_with_expiries(1111, 2222);
  EXPECT_NE(out.find("Expires=\"1111\""), std::string::npos);      // FileType attribute
  EXPECT_NE(out.find(">2222<"), std::string::npos);                // Cache-Control element text
  EXPECT_EQ(out.find("Expires=\"2222\""), std::string::npos);      // not swapped
  EXPECT_EQ(out.find(">1111<"), std::string::npos);
}

TEST(ExpiryAttributesTest, FileExpiresOmittedWhenUnset) {
  // use="optional" in the profiled schema, so absent is valid and preferable to a bogus 0.
  // Scoped to the File element: FDT-Instance carries its own required Expires attribute, which
  // is a different attribute on a different element and must not be confused with this one.
  auto out = emit_with_expiries(0, std::nullopt);
  auto file_start = out.find("<File ");
  ASSERT_NE(file_start, std::string::npos);
  auto file_end = out.find(">", file_start);
  ASSERT_NE(file_end, std::string::npos);
  const auto file_tag = out.substr(file_start, file_end - file_start);
  EXPECT_EQ(file_tag.find("Expires="), std::string::npos);
}

TEST(ExpiryAttributesTest, NoCacheControlElementWhenNoDirectiveWasSet) {
  // The element is minOccurs="0"; emitting an empty one would be worse than omitting it.
  auto out = emit_with_expiries(1111, std::nullopt);
  EXPECT_EQ(out.find("Cache-Control"), std::string::npos);
}

TEST(ExpiryAttributesTest, CacheControlStillCarriesOnlyOneChoiceMember) {
  // CacheControlType is an xs:choice, so no-cache and Expires must not both appear.
  auto oti = make_fec_oti();
  FileDeliveryTable fdt(1, oti, FileDeliveryTable::FDT_NS_3GPP_CONSOLIDATED_V2);
  auto e = make_entry(oti);
  e.cache_control.no_cache = true;
  e.cache_control.cache_expires = 2222;
  fdt.add(e);
  auto out = fdt.to_string();
  EXPECT_NE(out.find("no-cache"), std::string::npos);
  EXPECT_EQ(out.find(">2222<"), std::string::npos);
}
