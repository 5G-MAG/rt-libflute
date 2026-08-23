#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <boost/asio.hpp>

#include <chrono>
#include <cstring>
#include <map>
#include <stdexcept>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "AlcPacket.h"
#include "File.h"
#include "FileDeliveryTable.h"
#include "Receiver.h"
#include "Transmitter.h"

using namespace LibFlute;
using namespace std::chrono_literals;

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
/* General FLUTE, so the namespace under test is the one actually emitted. The 3GPP profiles derive
   their own namespace from the profile, which is the point of those, so they cannot be used to
   exercise an arbitrary namespace. */
std::string emit(FileDeliveryTable::FdtNamespace ns) {
  auto oti = make_fec_oti();
  FileDeliveryTable fdt(1, oti, ns, Profile::Unprofiled);
  fdt.add(make_entry(oti));
  return fdt.to_string();
}

std::string emit(FileDeliveryTable::FdtNamespace ns, Profile profile) {
  auto oti = make_fec_oti();
  FileDeliveryTable fdt(1, oti, ns, profile);
  fdt.add(make_entry(oti));
  return fdt.to_string();
}


/* NTP-epoch seconds a little way ahead of now. RFC 3926 clause 3.3 requires an FDT Instance's expiry
   to be in the future, so a fixed small constant is no longer a usable test value. */
uint64_t future_ntp(uint64_t seconds_ahead) {
  return (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch()).count()
         + 2208988800ULL + seconds_ahead;
}
}  // namespace

/* TS 26.346 V18.2.0 clause L.4.4 lists Transfer-Length among the attributes that
   "shall not be carried in the FDT sent by the FLUTE sender". These assert on the
   emitted XML rather than on the FileEntry, because the object carrying a value the
   serialiser then withholds is exactly the case that has to pass. */

TEST(MbmsDownloadProfileTest, TransferLengthNotCarriedUnderTs26517) {
  EXPECT_EQ(emit(FileDeliveryTable::FDT_NS_NONE, Profile::Ts26517).find("Transfer-Length"),
            std::string::npos);
}

TEST(MbmsDownloadProfileTest, TransferLengthNotCarriedUnderTs26346) {
  EXPECT_EQ(emit(FileDeliveryTable::FDT_NS_NONE, Profile::Ts26346).find("Transfer-Length"),
            std::string::npos);
}

/* The namespace argument is ignored under a 3GPP profile, which derives its own. TS 26.517 V18.6.0
   clause 6.2.1: "The MBSTF shall use the Profiled FDT Schema according to clause L.6 of TS 26.346
   [7] to describe the object list currently being transmitted in the MBS Distribution Session." */
TEST(MbmsDownloadProfileTest, ProfileDecidesTheSchemaNotTheNamespaceArgument) {
  auto mbs = emit(FileDeliveryTable::FDT_NS_RFC3926, Profile::Ts26517);
  EXPECT_NE(mbs.find("urn:3GPP:metadata:2022:FLUTE:FDT"), std::string::npos) << mbs;
  EXPECT_NE(mbs.find("<schemaVersion>2</schemaVersion>"), std::string::npos);

  auto mbms = emit(FileDeliveryTable::FDT_NS_RFC3926, Profile::Ts26346);
  EXPECT_NE(mbms.find("urn:IETF:metadata:2005:FLUTE:FDT"), std::string::npos) << mbms;
  EXPECT_NE(mbms.find("<schemaVersion>4</schemaVersion>"), std::string::npos);
}

TEST(GeneralFluteTest, TransferLengthStillCarriedOutsideTheProfile) {
  // Plain RFC 3926, where clause 3.4.2 permits the attribute, so it is kept. This has to be
  // asked for explicitly: the library default is the 3GPP profile.
  EXPECT_NE(emit(FileDeliveryTable::FDT_NS_RFC3926, Profile::Unprofiled).find("Transfer-Length"),
            std::string::npos);
}

/* The delimitation itself. A session is bound by the general FLUTE documents always, and by
   TS 26.346 annex L.4 only under the 3GPP profile, which is the default. */

TEST(ProfileDefaultTest, DefaultIsTheMbms3gppProfile) {
  auto oti = make_fec_oti();
  FileDeliveryTable fdt(1, oti);
  EXPECT_EQ(fdt.profile(), Profile::Ts26517);
}

TEST(ProfileDefaultTest, ProfileNotFdtNamespaceDecidesTheRestriction) {
  // The namespace says which schema is emitted; the profile says which obligations apply.
  // Same namespace, opposite outcomes, driven only by the profile.
  const auto ns = FileDeliveryTable::FDT_NS_RFC3926;
  EXPECT_EQ(emit(ns, Profile::Ts26517).find("Transfer-Length"), std::string::npos);
  EXPECT_NE(emit(ns, Profile::Unprofiled).find("Transfer-Length"), std::string::npos);
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
  FileDeliveryTable fdt(1, oti, FileDeliveryTable::FDT_NS_RFC3926, Profile::Unprofiled);
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
  FileDeliveryTable fdt(1, oti, FileDeliveryTable::FDT_NS_RFC3926, Profile::Unprofiled);
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
  FileDeliveryTable fdt(1, oti, FileDeliveryTable::FDT_NS_RFC3926, Profile::Unprofiled);
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

TEST(ProfiledFdtSchemaTest, SchemaVersionNotEmittedForSchemasThatDoNotDefineIt) {
  // Meaningless in a document declaring a schema whose sequence has no such element.
  EXPECT_EQ(emit(FileDeliveryTable::FDT_NS_RFC3926).find("schemaVersion"), std::string::npos);
  EXPECT_EQ(emit(FileDeliveryTable::FDT_NS_NONE).find("schemaVersion"), std::string::npos);
}

/* TS 26.346 V18.2.0 clause 7.2.10.1: "In this version of the present document the network shall set
   the content of the schemaVersion element, defined as a child of the FDT-Instance element, to the
   value 4." That is the extended schema of that clause, which is the one MbmsDownload emits, and it
   takes a different value from the annex L.6.1 profiled schema's 2. */
TEST(ProfiledFdtSchemaTest, SchemaVersionFourForTheExtendedSchema) {
  auto xml = emit(FileDeliveryTable::FDT_NS_DRAFT_2005);
  EXPECT_NE(xml.find("<schemaVersion>4</schemaVersion>"), std::string::npos) << xml;
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


/* RFC 3926 clause 3.4.1 defines the FDT Instance ID sequence and its wraparound. next_instance_id()
   is the sequence as a pure function, so these exercise it without a live session. */

TEST(FdtInstanceIdWraparoundTest, IdIncrementsBelowTheCeiling) {
  std::map<uint32_t, uint64_t> expired;
  EXPECT_EQ(FileDeliveryTable::next_instance_id(5, /*current_expires*/ 1000, /*now*/ 2000, expired), 6u);
  EXPECT_EQ(expired.at(5u), 1000u);
}

/* RFC 3926 clause 3.4.1: "After reaching the maximum value (2^20-1), the numbering starts again
   from '0'." Not to the smallest expired identifier, and not to any other value. */
TEST(FdtInstanceIdWraparoundTest, IdWrapsToZeroAtTheCeiling) {
  std::map<uint32_t, uint64_t> expired = {{1u, 500u}, {5u, 500u}, {100u, 500u}};
  auto next = FileDeliveryTable::next_instance_id(FileDeliveryTable::kMaxFdtInstanceId,
                                                  /*current_expires*/ 1500, /*now*/ 1000, expired);
  EXPECT_EQ(next, 0u);
  EXPECT_EQ(expired.at(FileDeliveryTable::kMaxFdtInstanceId), 1500u);
}

/* The clause states the sequence unconditionally and gives it no failure case. Waiting for the
   previous holder to expire is a recommendation in the same clause, so it is warned about rather
   than met by choosing a different identifier or by refusing to continue. RFC 6726 clause 3.4.1
   does make it a prohibition, but that is the v2 rule and this library implements RFC 3926. */
TEST(FdtInstanceIdWraparoundTest, IdWrapsToZeroEvenWhenZeroHasNotExpired) {
  std::map<uint32_t, uint64_t> expired = {{0u, 5000u}, {1u, 5000u}};
  uint32_t next = 0xFFFFFFFFu;
  EXPECT_NO_THROW(next = FileDeliveryTable::next_instance_id(FileDeliveryTable::kMaxFdtInstanceId,
                                                             /*current_expires*/ 5000, /*now*/ 1000,
                                                             expired));
  EXPECT_EQ(next, 0u);
}

/* A plain increment would leave the 20-bit field long before a caller could observe it, and the
   value would be silently masked on the wire. */
TEST(FdtInstanceIdWraparoundTest, IdStaysInsideTheFieldAcrossManySends) {
  auto oti = make_fec_oti();
  FileDeliveryTable fdt(FileDeliveryTable::kMaxFdtInstanceId, oti);
  auto entry = make_entry(oti);
  for (int i = 0; i < 10; ++i) {
    fdt.sent();
    entry.toi = static_cast<uint32_t>(i + 1);
    fdt.add(entry);
    EXPECT_LE(fdt.instance_id(), FileDeliveryTable::kMaxFdtInstanceId);
  }
}

/* Reception bootstrapped from a content packet's own EXT_FTI, for a TOI the FDT has not yet
   described. This library's own Transmitter carries EXT_FTI only on the FDT itself, so the packets
   below are hand-built to stand in for a different, spec-general sender. */
namespace {

/* A minimal ALC/LCT packet for one TOI, Compact No-Code, carrying EXT_FTI and no EXT_FDT.
   transfer_length defaults to the payload size, giving a single-packet object that completes on
   arrival; a caller passes a larger value to keep the bootstrapped file incomplete. */
std::vector<char> build_content_packet_with_fti(uint16_t tsi, uint16_t toi,
                                                uint16_t encoding_symbol_length,
                                                uint32_t max_source_block_length,
                                                const std::string& symbol_data,
                                                uint32_t declared_transfer_length = 0) {
  const size_t lct_header_len_words = 7;  // LCT header + CCI, TSI/TOI half-words, EXT_FTI
  const size_t header_bytes = lct_header_len_words * 4;
  const size_t sbn_esi_bytes = 4;
  std::vector<char> buf(header_bytes + sbn_esi_bytes + symbol_data.size(), 0);
  auto* b = reinterpret_cast<unsigned char*>(buf.data());

  b[0] = (1 << 4);  // FLUTE version 1, no congestion control, no PSI
  b[1] = 0x10;      // half-word flag set, TSI/TOI flags and both Close flags clear
  b[2] = static_cast<unsigned char>(lct_header_len_words);
  b[3] = 0;         // codepoint: Compact No-Code

  uint16_t tsi_be = htons(tsi);
  uint16_t toi_be = htons(toi);
  std::memcpy(b + 8, &tsi_be, 2);
  std::memcpy(b + 10, &toi_be, 2);

  size_t off = 12;
  b[off] = 64;      // EXT_FTI
  b[off + 1] = 4;   // HEL: 4 words
  uint32_t transfer_length =
      declared_transfer_length ? declared_transfer_length : static_cast<uint32_t>(symbol_data.size());
  uint16_t transfer_len_hi_be = htons(0);
  uint32_t transfer_len_lo_be = htonl(transfer_length);
  std::memcpy(b + off + 2, &transfer_len_hi_be, 2);
  std::memcpy(b + off + 4, &transfer_len_lo_be, 4);
  uint16_t esl_be = htons(encoding_symbol_length);
  std::memcpy(b + off + 10, &esl_be, 2);
  uint32_t msbl_be = htonl(max_source_block_length);
  std::memcpy(b + off + 12, &msbl_be, 4);

  std::memcpy(buf.data() + header_bytes + sbn_esi_bytes, symbol_data.data(), symbol_data.size());
  return buf;
}

std::vector<EncodingSymbol> make_symbols(const char* data, size_t len) {
  return {EncodingSymbol(0, 0, const_cast<char*>(data), len, FecScheme::CompactNoCode)};
}

}  // namespace

/* Checks the hand-built packet parses the way the two tests below assume, so a failure there is
   read as a Receiver failure rather than a malformed fixture. */
TEST(ExtFtiBootstrapTest, HandBuiltPacketCarriesItsOwnFti) {
  auto buf = build_content_packet_with_fti(777, 5, 1000, 64, "0123456789");
  AlcPacket alc(buf.data(), buf.size());
  EXPECT_EQ(alc.tsi(), 777u);
  EXPECT_EQ(alc.toi(), 5u);
  EXPECT_TRUE(alc.has_fec_oti());
  EXPECT_EQ(alc.fec_oti().encoding_symbol_length, 1000u);
  EXPECT_EQ(alc.fec_oti().max_source_block_length, 64u);
}

TEST(ExtFtiBootstrapTest, ReceptionStartsFromThePacketsOwnFti) {
  boost::asio::io_context io;
  auto work_guard = boost::asio::make_work_guard(io);
  LibFlute::Receiver receiver("0.0.0.0", "239.255.9.9", 19191, /*tsi*/ 777, io);
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
  EXPECT_TRUE(found) << "no file was started for TOI 5 from the packet's own EXT_FTI";

  receiver.stop();
  work_guard.reset();
  io.stop();
  io_thread.join();
}

/* The head start is only worth taking if the FDT, once it arrives, fills in the metadata on the
   file already being reassembled. Replacing it would discard every symbol received so far. */
TEST(ExtFtiBootstrapTest, ArrivingFdtMetadataIsAdoptedInPlace) {
  boost::asio::io_context io;
  auto work_guard = boost::asio::make_work_guard(io);
  LibFlute::Receiver receiver("0.0.0.0", "239.255.9.10", 19192, /*tsi*/ 778, io);
  std::thread io_thread([&io]() { io.run(); });

  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(sock, 0);
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(19192);
  inet_pton(AF_INET, "239.255.9.10", &dst.sin_addr);

  auto send_buf = [&](const std::vector<char>& buf) {
    auto sent = sendto(sock, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    EXPECT_EQ(sent, static_cast<ssize_t>(buf.size()));
  };

  /* Two symbols declared, one delivered, so the file is still incomplete when the FDT arrives.
     A completed file is replaced by the same-content-location handling before this can be seen. */
  send_buf(build_content_packet_with_fti(778, 5, 1000, 64, "0123456789",
                                         /*declared_transfer_length*/ 2000));

  LibFlute::File* bootstrapped = nullptr;
  for (int i = 0; i < 50 && !bootstrapped; ++i) {
    std::this_thread::sleep_for(20ms);
    for (const auto& f : receiver.file_list()) {
      if (f->meta().toi == 5) bootstrapped = f.get();
    }
  }
  ASSERT_NE(bootstrapped, nullptr) << "no file was started for TOI 5 from the packet's own EXT_FTI";
  EXPECT_TRUE(bootstrapped->meta().content_location.empty());
  EXPECT_FALSE(bootstrapped->complete());

  auto oti = make_fec_oti();
  FileDeliveryTable fdt(1, oti);
  FileDeliveryTable::FileEntry entry = make_entry(oti);
  entry.toi = 5;
  entry.content_location = "bootstrapped.bin";
  entry.content_length = 2000;
  fdt.add(entry);
  auto xml = fdt.to_string();
  FecOti fdt_oti{FecScheme::CompactNoCode, 0, xml.length(), 1400, 64, 0};
  AlcPacket fdt_packet(/*tsi*/ 778, /*toi*/ 0, fdt_oti, make_symbols(xml.c_str(), xml.length()), 1400,
                       fdt.instance_id());
  send_buf(std::vector<char>(fdt_packet.data(), fdt_packet.data() + fdt_packet.size()));

  bool adopted = false;
  for (int i = 0; i < 50 && !adopted; ++i) {
    std::this_thread::sleep_for(20ms);
    for (const auto& f : receiver.file_list()) {
      if (f->meta().toi == 5 && f->meta().content_location == "bootstrapped.bin") {
        adopted = true;
        EXPECT_EQ(f.get(), bootstrapped)
            << "TOI 5 was replaced with a new file instead of adopting the FDT metadata in place";
      }
    }
  }
  EXPECT_TRUE(adopted) << "TOI 5 never took the content location the FDT gave it";

  ::close(sock);
  receiver.stop();
  work_guard.reset();
  io.stop();
  io_thread.join();
}


/* The MBMS Download Profile fixes the TSI field at 16 bits, so a wider value cannot be signalled
   under it. TS 26.346 V18.2.0 clause 7.2.7: "The Transmission Session Identifier (TSI) field shall
   be of length 16 bits (S=0, H=1, 16 bits)." Outside the profile RFC 3451 permits the wider
   encoding and the widening on this branch applies. */
TEST(ProfileTsiWidthTest, WideTsiRefusedUnderTheMbmsDownloadProfile) {
  boost::asio::io_context io;
  EXPECT_THROW(
      LibFlute::Transmitter("239.1.3.10", 5000, /*tsi*/ 0x10000, /*mtu*/ 1400, /*rate_limit*/ 0, io,
                            std::nullopt, FileDeliveryTable::FDT_NS_NONE, /*active*/ false,
                            std::nullopt, Profile::Ts26517),
      std::runtime_error);
}

TEST(ProfileTsiWidthTest, SixteenBitTsiAcceptedUnderTheProfile) {
  boost::asio::io_context io;
  EXPECT_NO_THROW(
      LibFlute::Transmitter("239.1.3.11", 5000, /*tsi*/ 0xFFFF, /*mtu*/ 1400, /*rate_limit*/ 0, io,
                            std::nullopt, FileDeliveryTable::FDT_NS_NONE, /*active*/ false,
                            std::nullopt, Profile::Ts26517));
}

TEST(ProfileTsiWidthTest, WideTsiAcceptedOutsideTheProfile) {
  boost::asio::io_context io;
  EXPECT_NO_THROW(
      LibFlute::Transmitter("239.1.3.12", 5000, /*tsi*/ 0x10000, /*mtu*/ 1400, /*rate_limit*/ 0, io,
                            std::nullopt, FileDeliveryTable::FDT_NS_NONE, /*active*/ false,
                            std::nullopt, Profile::Unprofiled));
}


/* TS 26.346 V18.2.0 clause 7.2.9: "When the FEC Encoding ID indicates the "Compact No-Code FEC
   scheme", the value of this data element shall not exceed 65535, consistent with the 16-bit
   constraint on the Encoding Symbol ID". Refused rather than clamped, since clamping would
   repartition the object without telling the operator. */
TEST(ProfileSourceBlockLengthTest, AboveTheCompactNoCodeCeilingIsRefused) {
  auto oti = make_fec_oti();
  oti.max_source_block_length = 65536;
  EXPECT_THROW(FileDeliveryTable(1, oti, FileDeliveryTable::FDT_NS_NONE, Profile::Ts26517),
               std::runtime_error);
  EXPECT_THROW(FileDeliveryTable(1, oti, FileDeliveryTable::FDT_NS_NONE, Profile::Ts26346),
               std::runtime_error);
}

TEST(ProfileSourceBlockLengthTest, AtTheCeilingIsAccepted) {
  auto oti = make_fec_oti();
  oti.max_source_block_length = 65535;
  EXPECT_NO_THROW(FileDeliveryTable(1, oti, FileDeliveryTable::FDT_NS_NONE, Profile::Ts26517));
}

TEST(ProfileSourceBlockLengthTest, NotAppliedOutsideThe3gppProfiles) {
  auto oti = make_fec_oti();
  oti.max_source_block_length = 65536;
  EXPECT_NO_THROW(FileDeliveryTable(1, oti, FileDeliveryTable::FDT_NS_NONE, Profile::Unprofiled));
}

/* TS 26.346 V18.2.0 annex L: "When the optional File@Expires attribute is provided, its value shall
   take precedence over that of the FDT@Expires attribute." */
TEST(EffectiveExpiryTest, FileExpiresTakesPrecedenceOverTheInstanceValue) {
  auto oti = make_fec_oti();
  FileDeliveryTable fdt(1, oti, FileDeliveryTable::FDT_NS_NONE, Profile::Ts26517);
  const auto instance_expiry = future_ntp(600);
  fdt.set_expires(instance_expiry);

  auto entry = make_entry(oti);
  entry.expires = instance_expiry + 1000;
  EXPECT_EQ(fdt.effective_expiry(entry), instance_expiry + 1000);

  entry.expires = 0;  // attribute absent
  EXPECT_EQ(fdt.effective_expiry(entry), instance_expiry);
}

/* TS 26.346 V18.2.0 clause 7.2.9: "For MBMS operation, the UE shall not use a received FDT Instance
   to interpret packets received beyond the expiration time of the FDT Instance." */
TEST(FdtExpiryTest, AnInstanceIsExpiredOnceItsExpiresHasPassed) {
  auto oti = make_fec_oti();
  FileDeliveryTable fdt(1, oti, FileDeliveryTable::FDT_NS_NONE, Profile::Ts26517);
  const auto expiry = future_ntp(600);
  fdt.set_expires(expiry);
  EXPECT_FALSE(fdt.expired(expiry - 1));
  EXPECT_FALSE(fdt.expired(expiry));
  EXPECT_TRUE(fdt.expired(expiry + 1));
}

TEST(FdtExpiryTest, AnInstanceWithNoExpiresNeverExpires) {
  auto oti = make_fec_oti();
  FileDeliveryTable fdt(1, oti, FileDeliveryTable::FDT_NS_NONE, Profile::Ts26517);
  EXPECT_FALSE(fdt.expired(0xFFFFFFFFULL));
}


/* RFC 3926 clause 3.3: "A sender MUST use an expiry time in the future upon creation of an FDT
   Instance relative to its Sender Current Time (SCT)." Binding under every profile. */
TEST(FdtExpiryTest, APastExpiryIsRefused) {
  auto oti = make_fec_oti();
  FileDeliveryTable fdt(1, oti, FileDeliveryTable::FDT_NS_NONE, Profile::Ts26517);
  EXPECT_THROW(fdt.set_expires(1000), std::runtime_error);
  EXPECT_THROW(fdt.set_expires(0), std::runtime_error);
}

TEST(FdtExpiryTest, APastExpiryIsRefusedWhenUnprofiledToo) {
  auto oti = make_fec_oti();
  FileDeliveryTable fdt(1, oti, FileDeliveryTable::FDT_NS_NONE, Profile::Unprofiled);
  EXPECT_THROW(fdt.set_expires(1000), std::runtime_error);
  EXPECT_NO_THROW(fdt.set_expires(future_ntp(60)));
}

/* ------------------------------------------------------------------------------------------- */
/* A packet that carries no payload, which RFC 3450 clause 4.1 provides for and RFC 3926 clause
   3.1 gives a shape to in a FLUTE session.                                                     */

/* RFC 3926 clause 3.1: "the exception that ALC packets sent in a FLUTE session with the Close
   Session (A) flag set to 1 (signaling the end of the session) and that contain no payload
   (carrying no information for any file or FDT) SHALL NOT carry the TOI" */
TEST(DataLessClosePacket, CarriesTheCloseFlagAndNoToi) {
  AlcPacket packet(/*tsi*/ 0x1234, AlcPacket::CloseSession{});

  EXPECT_EQ(packet.size(), 12u) << "base word, CCI, TSI, and nothing else";
  EXPECT_EQ(packet.size(), packet.header_length()) << "no payload means no FEC Payload ID either";

  const auto* bytes = reinterpret_cast<const unsigned char*>(packet.data());
  EXPECT_EQ(bytes[0] >> 4, 1) << "LCT version 1";
  EXPECT_EQ((bytes[0] >> 2) & 0x03, 0) << "C=0, a 32-bit CCI";
  EXPECT_EQ((bytes[1] >> 7) & 0x01, 1) << "S=1, a 32-bit TSI";
  EXPECT_EQ((bytes[1] >> 5) & 0x03, 0) << "O=0, no TOI word";
  EXPECT_EQ((bytes[1] >> 4) & 0x01, 0) << "H=0, no half-word for either field";
  EXPECT_EQ((bytes[1] >> 1) & 0x01, 1) << "A=1, Close Session";
  EXPECT_EQ(bytes[1] & 0x01, 0) << "B=0";
  EXPECT_EQ(bytes[2], 3) << "three 32-bit words of header";

  /* The CCI word is zero: there is no data to pace. */
  EXPECT_EQ(bytes[4], 0); EXPECT_EQ(bytes[5], 0);
  EXPECT_EQ(bytes[6], 0); EXPECT_EQ(bytes[7], 0);

  /* The TSI, in the single word the encoding leaves for it. */
  const uint32_t tsi = (uint32_t(bytes[8]) << 24) | (uint32_t(bytes[9]) << 16) |
                       (uint32_t(bytes[10]) << 8) | uint32_t(bytes[11]);
  EXPECT_EQ(tsi, 0x1234u);
}

/* Dropping the TOI drops the half-word the two fields share, which leaves the TSI one whole word.
   RFC 5651 clause 5.1: "The TSI field is 32*S + 16*H bits in length" */
TEST(DataLessClosePacket, RefusedRatherThanTruncatedForAWiderTsi) {
  EXPECT_NO_THROW(AlcPacket(0xFFFFFFFFULL, AlcPacket::CloseSession{}));
  EXPECT_THROW(AlcPacket(0x100000000ULL, AlcPacket::CloseSession{}), std::runtime_error);
}

/* The packet this library now sends must be one it can also read back, and reading it must not
   invent a TOI.
   RFC 3450 clause 4.1: "The total datagram length, conveyed by outer protocol headers
   (e.g., the IP or UDP header), enables receivers to detect the absence of the ALC payload and FEC
   Payload ID." */
TEST(DataLessClosePacket, RoundTripsThroughTheParser) {
  AlcPacket sent(/*tsi*/ 0xABCD, AlcPacket::CloseSession{});
  std::vector<char> wire(sent.data(), sent.data() + sent.size());

  AlcPacket received(wire.data(), wire.size());
  EXPECT_EQ(received.tsi(), 0xABCDu);
  EXPECT_TRUE(received.close_session_flag());
  EXPECT_FALSE(received.close_object_flag());
  EXPECT_EQ(received.header_length(), wire.size())
      << "the whole datagram is header, so a receiver sees a zero-length payload";
}
