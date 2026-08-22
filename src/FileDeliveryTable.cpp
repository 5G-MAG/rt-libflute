// libflute - FLUTE/ALC library
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
//               2025 British Broadcasting Corporation (David Waring <david.waring2@bbc.co.uk>)
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
#include "FileDeliveryTable.h"
#include "tinyxml2.h"
#include <chrono>
#include <iostream>
#include <string>
#include <map>
#include "spdlog/spdlog.h"
#include "base64.h"

namespace {

/* The Raptor scheme-specific FEC Object Transmission Information, carried in the FDT as one
   base64 attribute rather than as separate fields.

   RFC 5053 clause 3.2.3: "a 4-octet field consisting of the parameters Z (2 octets), N (1 octet),
   and Al (1 octet)"

   The FDT attribute that carries it is FEC-OTI-Scheme-Specific-Info, typed xs:base64Binary by the
   TS 26.346 annex L.6.1 profiled schema at both the FDT-Instance and File levels.

   Compact No-Code has no scheme-specific OTI to carry: RFC 3695 clause 3 specifies a FEC Payload
   ID for it and defines no scheme-specific element, and the schema makes the attribute
   use="optional", so it is simply omitted for that scheme. */
std::string encode_raptor_scheme_specific(uint16_t z, uint8_t n, uint8_t al)
{
  std::string raw;
  raw.push_back(static_cast<char>((z >> 8) & 0xFF));
  raw.push_back(static_cast<char>(z & 0xFF));
  raw.push_back(static_cast<char>(n));
  raw.push_back(static_cast<char>(al));
  return base64_encode(raw);
}

// Returns false when the attribute is not a well-formed 4-octet field.
bool decode_raptor_scheme_specific(const std::string &b64, uint16_t &z, uint8_t &n, uint8_t &al)
{
  const auto raw = base64_decode(b64);
  if (raw.size() != 4) return false;
  z  = static_cast<uint16_t>((static_cast<uint8_t>(raw[0]) << 8) | static_cast<uint8_t>(raw[1]));
  n  = static_cast<uint8_t>(raw[2]);
  al = static_cast<uint8_t>(raw[3]);
  return true;
}

}  // namespace


namespace {
  class XMLNamespaces {
  public:
    XMLNamespaces() :_default_ns() ,_prefix_to_ns_map() {};
    XMLNamespaces(const XMLNamespaces &to_copy) :_default_ns(to_copy._default_ns) ,_prefix_to_ns_map(to_copy._prefix_to_ns_map) {};
    XMLNamespaces(XMLNamespaces &&to_move) :_default_ns(std::move(to_move._default_ns)) ,_prefix_to_ns_map(std::move(to_move._prefix_to_ns_map)) {};
    XMLNamespaces(const tinyxml2::XMLElement *element, const XMLNamespaces &parent_ns = XMLNamespaces());

    const tinyxml2::XMLElement *findChildElement(const tinyxml2::XMLElement *element, const std::string &name, const std::string &ns = std::string()) const;
    const tinyxml2::XMLElement *findSiblingElement(const tinyxml2::XMLElement *child_elem, const std::string &name, const std::string &ns = std::string()) const;
    const tinyxml2::XMLAttribute *findAttribute(const tinyxml2::XMLElement *element, const std::string &name, const std::string &ns = std::string()) const;
    bool matches(const std::string &prefixed_name, const std::string &name, const std::string &ns = std::string()) const;
    const std::string &elementNamespace(const tinyxml2::XMLElement *element) const;

  private:
    std::string _default_ns;   // namespace given for "xmlns=..."
    std::map<std::string, std::string> _prefix_to_ns_map; // namespaces given for "xmlns:prefix=..."
  };

  XMLNamespaces::XMLNamespaces(const tinyxml2::XMLElement *element, const XMLNamespaces &parent_ns)
    :_default_ns(parent_ns._default_ns)
    ,_prefix_to_ns_map(parent_ns._prefix_to_ns_map)
  {
    if (!element) return;
    for (auto attr_ptr = element->FirstAttribute(); attr_ptr; attr_ptr = attr_ptr->Next()) {
      std::string attr_name = attr_ptr->Name();
      if (attr_name == "xmlns") {
        _default_ns = std::string(attr_ptr->Value());
      } else if (attr_name.substr(0,6) == "xmlns:") {
        _prefix_to_ns_map.insert(std::make_pair(attr_name.substr(6), std::string(attr_ptr->Value())));
      }
    }
  }

  const tinyxml2::XMLElement *XMLNamespaces::findChildElement(const tinyxml2::XMLElement *element, const std::string &name, const std::string &ns) const
  {
    if (!element) return nullptr;
    auto elem_ptr = element->FirstChildElement();
    if (!elem_ptr) return nullptr;
    XMLNamespaces child_ns(elem_ptr, *this);
    if (child_ns.matches(elem_ptr->Name(), name, ns)) return elem_ptr;
    return findSiblingElement(elem_ptr, name, ns);
  }

  const tinyxml2::XMLElement *XMLNamespaces::findSiblingElement(const tinyxml2::XMLElement *child_elem, const std::string &name, const std::string &ns) const
  {
    if (!child_elem) return nullptr;
    for (auto elem_ptr = child_elem->NextSiblingElement(); elem_ptr; elem_ptr = elem_ptr->NextSiblingElement()) {
      XMLNamespaces child_ns(elem_ptr, *this);
      if (child_ns.matches(elem_ptr->Name(), name, ns)) return elem_ptr;
    }
    return nullptr;
  }

  const tinyxml2::XMLAttribute *XMLNamespaces::findAttribute(const tinyxml2::XMLElement *element, const std::string &name, const std::string &ns) const
  {
    const std::string &elem_ns = elementNamespace(element);
    for (auto attr_ptr = element->FirstAttribute(); attr_ptr; attr_ptr = attr_ptr->Next()) {
      XMLNamespaces attr_ns(*this);
      attr_ns._default_ns = elem_ns;
      if (attr_ns.matches(attr_ptr->Name(), name, ns)) return attr_ptr;
    }
    return nullptr;
  }

  bool XMLNamespaces::matches(const std::string &prefixed_name, const std::string &name, const std::string &ns) const
  {
    std::string match_ns(_default_ns);
    std::string match_name(prefixed_name);
    auto pos = match_name.find_first_of(':');
    if (pos != std::string::npos) {
      auto prefix = match_name.substr(0,pos);
      match_name.erase(0,pos+1);
      auto it = _prefix_to_ns_map.find(prefix);
      if (it != _prefix_to_ns_map.end()) {
        match_ns = _prefix_to_ns_map.at(prefix);
      } else {
        match_ns = prefix;
      }
    }
    return name == match_name && ns == match_ns;
  }

  const std::string &XMLNamespaces::elementNamespace(const tinyxml2::XMLElement *element) const
  {
    std::string elem_name(element->Name());
    auto pos = elem_name.find_first_of(':');
    if (pos != std::string::npos) {
      auto elem_prefix = elem_name.substr(0,pos);
      auto it = _prefix_to_ns_map.find(elem_prefix);
      if (it != _prefix_to_ns_map.end()) {
        return _prefix_to_ns_map.at(elem_prefix);
      } else {
        static const std::string unknown("<Unknown>");
        return unknown;
      }
    }
    return _default_ns;
  }
}

bool LibFlute::FileDeliveryTable::FileEntry::operator==(const LibFlute::FileDeliveryTable::FileEntry &other) const
{
  return toi == other.toi && content_length == other.content_length && expires == other.expires &&
         cache_control.no_cache == other.cache_control.no_cache && fec_oti == other.fec_oti &&
         cache_control.cache_expires == other.cache_control.cache_expires && content_location == other.content_location &&
         content_md5 == other.content_md5 && content_type == other.content_type && content_encoding == other.content_encoding &&
         etag == other.etag;
}

LibFlute::FileDeliveryTable::FileDeliveryTable(uint32_t instance_id, FecOti fec_oti, FdtNamespace fdt_namespace,
                                               Profile profile)
  : _instance_id( instance_id )
  , _instance_id_sent( instance_id - 1 )
  , _global_fec_oti( fec_oti )
  , _fdt_namespace( fdt_namespace )
  , _profile( profile )
{
  /* Each 3GPP profile fixes the FDT schema, so the namespace is taken from the profile rather than
     from a separate argument that could disagree with it.

     TS 26.517 V18.6.0 clause 6.2.1, for Ts26517: "The MBSTF shall use the Profiled FDT Schema
     according to clause L.6 of TS 26.346 [7] to describe the object list currently being
     transmitted in the MBS Distribution Session."

     TS 26.346 V18.2.0 clause 7.2.9, for Ts26346: "The extended FLUTE FDT instance schema
     defined in clause 7.2.10.1 (based on the one in RFC 3926 [9]) shall be used."

     General FLUTE keeps whatever the caller asked for, RFC 3926 fixing no namespace. */
  /* TS 26.346 V18.2.0 clause 7.2.9: "When the FEC Encoding ID indicates the "Compact No-Code FEC
     scheme", the value of this data element shall not exceed 65535, consistent with the 16-bit
     constraint on the Encoding Symbol ID". Refused at construction rather than clamped: clamping
     would silently repartition the object and leave the operator's configuration unexplained. */
  if (is_3gpp(_profile) && _global_fec_oti.encoding_id == FecScheme::CompactNoCode &&
      _global_fec_oti.max_source_block_length > 65535) {
    throw std::runtime_error(
        "FEC-OTI-Maximum-Source-Block-Length exceeds the 65535 the 3GPP profiles allow for the "
        "Compact No-Code FEC scheme");
  }

  switch (_profile) {
    case Profile::Ts26517:        _fdt_namespace = FDT_NS_3GPP_CONSOLIDATED_V2; break;
    case Profile::Ts26346: _fdt_namespace = FDT_NS_DRAFT_2005; break;
    case Profile::Unprofiled: break;
  }
}

LibFlute::FileDeliveryTable::FileDeliveryTable(uint32_t instance_id, char* buffer, size_t len) 
  : _instance_id( instance_id )
  , _instance_id_sent( instance_id - 1 )
  , _global_fec_oti()
{
  static const std::string mbms2007_ns("urn:3GPP:metadata:2007:MBMS:FLUTE:FDT"); // 3GPP TS 26.346 Clause 7.2.10.2
  static const std::string mbms2012_ns("urn:3GPP:metadata:2012:MBMS:FLUTE:FDT"); // 3GPP TS 26.346 Clause 7.2.10.2
  tinyxml2::XMLDocument doc(true, tinyxml2::COLLAPSE_WHITESPACE);
  doc.Parse(buffer, len);
  auto fdt_instance = doc.RootElement();
  // A malformed / truncated FDT buffer leaves no root element; every access
  // below (elementNamespace, Name(), ...) would then dereference null and crash
  // the receiver. Throw instead so Receiver::handle_receive_from's
  // catch(std::exception&) drops the bad FDT and keeps the client alive.
  if (doc.Error() || fdt_instance == nullptr) {
    spdlog::warn("FDT parse failed ({}) at line {}, len={}",
                 doc.ErrorName() ? doc.ErrorName() : "?", doc.ErrorLineNum(), len);
    throw std::runtime_error(std::string("FDT XML parse failed: ") +
                             (doc.ErrorName() ? doc.ErrorName() : "no root element"));
  }
  XMLNamespaces root_ns(fdt_instance);
  auto fdt_ns = root_ns.elementNamespace(fdt_instance);
  if (!root_ns.matches(fdt_instance->Name(), "FDT-Instance", fdt_ns)) {
    throw std::runtime_error("Root element is not FDT-Instance");
  }

  if (fdt_ns == "") {
    _fdt_namespace = FDT_NS_NONE;
  } else if (fdt_ns == "http://www.example.com/flute") { // RFC 3926 Section 3.4.2
    _fdt_namespace = FDT_NS_RFC3926;
  } else if (fdt_ns == "urn:IETF:metadata:2005:FLUTE:FDT") { // 3GPP TS 26.346 Clause 7.2.10.1
    _fdt_namespace = FDT_NS_DRAFT_2005;
//  } else if (fdt_ns == "urn:ietf:params:xml:ns:fdt") { // RFC 6726 - FLUTEv2 - needs more work
//    _fdt_namespace = FDT_NS_RFC6726;
  } else if (fdt_ns == "urn:3GPP:metadata:2022:FLUTE:FDT") { // 3GPP TS 26.346 Clause L.6.1
    _fdt_namespace = FDT_NS_3GPP_CONSOLIDATED_V2;
  } else {
    throw std::runtime_error("FDT namespace not recognised");
  }

  _expires = std::stoull(root_ns.findAttribute(fdt_instance, "Expires", fdt_ns)->Value());

  auto complete_attr = root_ns.findAttribute(fdt_instance, "Complete", fdt_ns);
  if (complete_attr != nullptr) {
    std::string val(complete_attr->Value());
    _complete = (val == "true" || val == "1");
  }

  spdlog::debug("Received new FDT with instance ID {}: {}", instance_id, buffer);

  auto val = root_ns.findAttribute(fdt_instance, "FEC-OTI-FEC-Encoding-ID", fdt_ns);
  if (val != nullptr) {
    /* Refuse an identifier naming no scheme this library implements, rather than casting it into
       the enumeration and discovering it several layers down. A sender under either 3GPP profile
       may not use one (TS 26.346 V18.2.0 clause L.4.7 names the admissible schemes), and an
       unprofiled sender using one this library cannot decode is equally undeliverable. */
    auto scheme = fec_scheme_from_encoding_id(strtoul(val->Value(), nullptr, 0));
    if (!scheme) {
      throw std::runtime_error(
          std::string("FDT-Instance FEC-OTI-FEC-Encoding-ID names no FEC scheme this library "
                      "implements: ") + val->Value());
    }
    _global_fec_oti.encoding_id = *scheme;
  }

  val = root_ns.findAttribute(fdt_instance, "FEC-OTI-FEC-Instance-ID", fdt_ns);
  if (val != nullptr) {
    _global_fec_oti.instance_id = strtoul(val->Value(), nullptr, 0);
  }

  val = root_ns.findAttribute(fdt_instance, "FEC-OTI-Maximum-Source-Block-Length", fdt_ns);
  if (val != nullptr) {
    _global_fec_oti.max_source_block_length = strtoul(val->Value(), nullptr, 0);
  }

  val = root_ns.findAttribute(fdt_instance, "FEC-OTI-Encoding-Symbol-Length", fdt_ns);
  if (val != nullptr) {
    _global_fec_oti.encoding_symbol_length = strtoul(val->Value(), nullptr, 0);
  }

  val = root_ns.findAttribute(fdt_instance, "FEC-OTI-Max-Number-of-Encoding-Symbols", fdt_ns);
  if (val != nullptr) {
    _global_fec_oti.max_number_of_encoding_symbols = strtoul(val->Value(), nullptr, 0);
  }

  // Raptor scheme-specific OTI, read from the one attribute the schema defines for it. A
  // malformed value is ignored rather than fatal: the Common FEC OTI alone is enough to receive
  // a Compact No-Code session, and refusing the whole FDT would be worse than losing one scheme's
  // parameters.
  val = root_ns.findAttribute(fdt_instance, "FEC-OTI-Scheme-Specific-Info", fdt_ns);
  if (val != nullptr) {
    uint16_t z = 0; uint8_t n = 0, al = 0;
    if (decode_raptor_scheme_specific(val->Value(), z, n, al)) {
      _global_fec_oti.nof_source_blocks = z;
      _global_fec_oti.nof_sub_blocks = n;
      _global_fec_oti.symbol_alignment = al;
    } else {
      spdlog::warn("Ignoring malformed FEC-OTI-Scheme-Specific-Info on the FDT-Instance");
    }
  }

  for (auto file = root_ns.findChildElement(fdt_instance, "File", fdt_ns);
      file != nullptr; file = root_ns.findSiblingElement(file, "File", fdt_ns)) {

    XMLNamespaces file_ns(file, root_ns);

    // File required attributes
    auto toi_str = file_ns.findAttribute(file, "TOI", fdt_ns);
    if (toi_str == nullptr) {
      throw std::runtime_error("Missing TOI attribute on File element");
    }
    uint32_t toi = strtoull(toi_str->Value(), nullptr, 0);

    auto content_location = file_ns.findAttribute(file, "Content-Location", fdt_ns);
    if (content_location == nullptr) {
      throw std::runtime_error("Missing Content-Location attribute on File element");
    }

    // File optional attributes
    uint32_t content_length = 0;
    val = file_ns.findAttribute(file, "Content-Length", fdt_ns);
    if (val != nullptr) {
      content_length = strtoull(val->Value(), nullptr, 0);
    }

    uint32_t transfer_length = 0;
    val = file_ns.findAttribute(file, "Transfer-Length", fdt_ns);
    if (val != nullptr) {
      transfer_length = strtoull(val->Value(), nullptr, 0);
    } else {
      transfer_length = content_length;
    }

    auto content_md5 = std::string();
    val = file_ns.findAttribute(file, "Content-MD5", fdt_ns);
    if (val != nullptr) {
      content_md5 = val->Value();
    }

    auto content_encoding = std::string();
    val = file_ns.findAttribute(file, "Content-Encoding", fdt_ns);
    if (val != nullptr) {
      content_encoding = val->Value();
    }

    auto content_type = std::string();
    val = file_ns.findAttribute(file, "Content-Type", fdt_ns);
    if (val != nullptr) {
      content_type = val->Value();
    }

    auto encoding_id = _global_fec_oti.encoding_id;
    val = file_ns.findAttribute(file, "FEC-OTI-FEC-Encoding-ID", fdt_ns);
    if (val != nullptr) {
      // Same check as at the FDT-Instance level above; the File level may override it.
      auto scheme = fec_scheme_from_encoding_id(strtoul(val->Value(), nullptr, 0));
      if (!scheme) {
        throw std::runtime_error(
            std::string("File FEC-OTI-FEC-Encoding-ID names no FEC scheme this library "
                        "implements: ") + val->Value());
      }
      encoding_id = *scheme;
    }

    auto fec_instance_id = _global_fec_oti.instance_id;
    val = file_ns.findAttribute(file, "FEC-OTI-FEC-Instance-ID", fdt_ns);
    if (val != nullptr) {
      fec_instance_id = strtoul(val->Value(), nullptr, 0);
    }

    auto max_source_block_length = _global_fec_oti.max_source_block_length;
    val = file_ns.findAttribute(file, "FEC-OTI-Maximum-Source-Block-Length", fdt_ns);
    if (val != nullptr) {
      max_source_block_length = strtoul(val->Value(), nullptr, 0);
    }

    auto encoding_symbol_length = _global_fec_oti.encoding_symbol_length;
    val = file_ns.findAttribute(file, "FEC-OTI-Encoding-Symbol-Length", fdt_ns);
    if (val != nullptr) {
      encoding_symbol_length = strtoul(val->Value(), nullptr, 0);
    }

    auto max_number_of_encoding_symbols = _global_fec_oti.max_number_of_encoding_symbols;
    val = file_ns.findAttribute(file, "FEC-OTI-Max-Number-of-Encoding-Symbols", fdt_ns);
    if (val != nullptr) {
      max_number_of_encoding_symbols = strtoul(val->Value(), nullptr, 0);
    }

    // Same single attribute at the File level, overriding the FDT-Instance value when present.
    auto nof_source_blocks = _global_fec_oti.nof_source_blocks;
    auto nof_sub_blocks = _global_fec_oti.nof_sub_blocks;
    auto symbol_alignment = _global_fec_oti.symbol_alignment;
    val = file_ns.findAttribute(file, "FEC-OTI-Scheme-Specific-Info", fdt_ns);
    if (val != nullptr) {
      uint16_t z = 0; uint8_t n = 0, al = 0;
      if (decode_raptor_scheme_specific(val->Value(), z, n, al)) {
        nof_source_blocks = z;
        nof_sub_blocks = n;
        symbol_alignment = al;
      } else {
        spdlog::warn("Ignoring malformed FEC-OTI-Scheme-Specific-Info on File TOI {}", toi);
      }
    }

    auto mbms2012_file_etag = "";
    val = file_ns.findAttribute(file, "File-ETag", mbms2012_ns);
    if (val != nullptr) {
      mbms2012_file_etag = val->Value();
    }

    // File optional elements

    bool no_cache = false;
    //bool max_stale = false;
    /* The File element's own Expires attribute, read into its own member. Previously this was
       left at whatever the cache directive said, which made the two indistinguishable on a round
       trip and hid the emit-side defect. */
    uint64_t file_expires = 0;
    auto file_expires_attr = file_ns.findAttribute(file, "Expires", fdt_ns);
    if (file_expires_attr != nullptr) {
      /* TS 26.346 V18.2.0 annex L: "When the optional File@Expires attribute is provided, its value
         shall take precedence over that of the FDT@Expires attribute." Recorded on the entry; the
         effective expiry accessor below applies the precedence so every caller gets it. */
      file_expires = strtoull(file_expires_attr->Value(), nullptr, 0);
    }

    std::optional<uint64_t> cache_expires = std::nullopt;
    auto cc = file_ns.findChildElement(file, "Cache-Control", mbms2007_ns);
    if (cc) {
      XMLNamespaces cc_ns(cc, file_ns);

      // mbms2007:Cache-Control optional elements

      auto no_cache_elem = cc_ns.findChildElement(cc, "no-cache", mbms2007_ns);
      if (no_cache_elem) {
        no_cache = true;
      }

      //auto max_stale_elem = cc_ns.findChildElement(cc, "max-stale", mbms2007_ns);
      //if (max_stale_elem) {
      //  max_stale = true;
      //}

      auto expires_elem = cc_ns.findChildElement(cc, "Expires", mbms2007_ns);
      if (expires_elem) {
        cache_expires = strtoul(expires_elem->GetText(), nullptr, 0);
      }
    }

    FecOti fec_oti{
      .encoding_id = (FecScheme)encoding_id,
      .instance_id = fec_instance_id,
      .transfer_length = transfer_length,
      .encoding_symbol_length = encoding_symbol_length,
      .max_source_block_length = max_source_block_length,
      .max_number_of_encoding_symbols = max_number_of_encoding_symbols,
      .nof_source_blocks = nof_source_blocks,
      .nof_sub_blocks = nof_sub_blocks,
      .symbol_alignment = symbol_alignment
    };

    FileEntry fe{
      toi,
      std::string(content_location->Value()),
      content_length,
      content_md5,
      content_type,
      file_expires,
      fec_oti,
      {
        no_cache,
        cache_expires
      },
      content_encoding,
      mbms2012_file_etag
    };
    _file_entries.push_back(fe);
  }
}

namespace {
  auto ntp_seconds_since_epoch() -> uint64_t
  {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() +
        2'208'988'800; /* Unix epoch -> NTP epoch offset, matching Transmitter::seconds_since_epoch() */
  }
}

void LibFlute::FileDeliveryTable::set_expires(uint64_t exp)
{
  /* RFC 3926 clause 3.3: "A sender MUST use an expiry time in the future upon creation of an FDT
     Instance relative to its Sender Current Time (SCT)." An instance created already expired can
     never be used to interpret anything, and a receiver following clause 7.2.9 discards it on
     arrival, so this is refused rather than sent. */
  const auto now = ntp_seconds_since_epoch();
  if (exp <= now) {
    throw std::runtime_error(
        "FDT Instance expiry must be in the future. A receiver discards an instance that has "
        "already expired, so such a session delivers nothing. See the citation at this check.");
  }
  _expires = exp;
}

uint32_t LibFlute::FileDeliveryTable::next_instance_id(uint32_t current, uint64_t current_expires,
                                                        uint64_t now,
                                                        std::map<uint32_t, uint64_t>& expired_instance_ids)
{
  /* RFC 3926 clause 3.4.1: "After reaching the maximum value (2^20-1), the numbering starts again
     from '0'."

     That is the whole sequence, and it has no failure case. The same clause recommends, but does
     not require, that a sender wait for the previous instance carrying a wraparound ID to expire,
     so a reuse that is still live is warned about rather than replaced with a different ID or
     turned into an error. RFC 6726 clause 3.4.1 does make that a prohibition, but this library
     implements RFC 3926, which TS 26.346 clause L.4.1 references as its FLUTE specification. */
  expired_instance_ids[current] = current_expires;

  if (current < kMaxFdtInstanceId) {
    return current + 1;
  }

  const auto previous = expired_instance_ids.find(0);
  if (previous != expired_instance_ids.cend() && previous->second >= now) {
    spdlog::warn("FDT Instance ID wrapping to 0 while the previous instance using it has not yet "
                 "expired (Expires {}, now {})", previous->second, now);
  }
  expired_instance_ids.erase(0);
  return 0;
}

auto LibFlute::FileDeliveryTable::advance_instance_id() -> void
{
  _instance_id = next_instance_id(_instance_id, _expires, ntp_seconds_since_epoch(),
                                  _expired_instance_ids);
}

auto LibFlute::FileDeliveryTable::add(const FileEntry& fe) -> void
{
  /* The MBMS Download Profile permits exactly one content encoding, and forbids every other.
     TS 26.346 V18.2.0 clause L.4.2, second list: "The following FDT attribute, defined at both
     the FDT-Instance and File levels, may be carried in the FDT sent by the FLUTE sender, under
     either the File-Instance or File element, and shall be supported by the FLUTE receiver:"
     the single item there is Content-Encoding set to 'gzip'. The third list of the same clause
     then prohibits the attribute "set to a value other than 'gzip'".

     Refused here rather than silently dropped from the emitted FDT. Dropping the attribute would
     leave the payload encoded and the receiver with nothing saying so, which is undecodable
     content rather than a conformant session; RULES.md rule 12 prefers failing loudly over
     quietly adjusting away a caller's misconfiguration. Absent is fine: the attribute is a may. */
  if (is_3gpp(_profile) && !fe.content_encoding.empty() && fe.content_encoding != "gzip") {
    throw std::invalid_argument(
        "Content-Encoding must be absent or gzip in the MBMS Download Profile, got: " +
        fe.content_encoding + ". Use Profile::Unprofiled for a non-3GPP session.");
  }
  if (_instance_id == _instance_id_sent) advance_instance_id();
  _file_entries.push_back(fe);
}

auto LibFlute::FileDeliveryTable::remove(uint32_t toi) -> void
{
  for (auto it = _file_entries.cbegin(); it != _file_entries.cend();) {
    if (it->toi == toi) {
      it = _file_entries.erase(it);
    } else {
      ++it;
    }
  }
  if (_instance_id == _instance_id_sent) advance_instance_id();
}

auto LibFlute::FileDeliveryTable::to_string() const -> std::string {
  tinyxml2::XMLDocument doc;
  doc.InsertFirstChild( doc.NewDeclaration() );
  auto root = doc.NewElement("FDT-Instance");
  switch (_fdt_namespace) {
    case FDT_NS_RFC3926:
      // RFC 3926 Section 3.4.2
      root->SetAttribute("xmlns", "http://www.example.com/flute");
      break;
    case FDT_NS_DRAFT_2005:
      // 3GPP TS 26.346 Clause 7.2.10.1
      root->SetAttribute("xmlns", "urn:IETF:metadata:2005:FLUTE:FDT");
      break;
//    case FDT_NS_RFC6726:  // FLUTE v2 - Will need other things implementing to use this
//      // RFC 6726
//      root->SetAttribute("xmlns", "urn:ietf:params:xml:ns:fdt");
//      break;
    case FDT_NS_3GPP_CONSOLIDATED_V2:
      // 3GPP TS 26.346 Clause L.6.1
      root->SetAttribute("xmlns", "urn:3GPP:metadata:2022:FLUTE:FDT");
      break;
    default:
      break;
  }
  root->SetAttribute("Expires", std::to_string(_expires).c_str());
  /* The Complete attribute is permitted by RFC 3926 clause 3.4.2, which makes it optional on the
     FDT-Instance element, but the MBMS Download Profile forbids a sender using it.
     TS 26.346 V18.2.0 clause L.4.3: "The following parameters, defined at the FDT-Instance level,
     shall not be used by the FLUTE sender:"
     Complete is the first item of that list.

     Sender-only. The parser above is deliberately untouched, because the same clause's NOTE makes
     receiver support for this one mandatory: "With the exception of Complete, which is mandatory,
     these parameters are optional to support by the FLUTE receiver." Reading the prohibition as
     binding both directions would break reception from a conformant peer. */
  if (_complete && !is_3gpp(_profile)) root->SetAttribute("Complete", "true");
  root->SetAttribute("FEC-OTI-FEC-Encoding-ID", (unsigned)_global_fec_oti.encoding_id);
  /* The existing guard is on the value, not on the profile: it withholds the attribute only when
     the instance ID happens to be 0. The MBMS Download Profile forbids it outright, at both
     levels, whatever the value.
     TS 26.346 V18.2.0 clause L.4.2, third list: "The following FDT parameters, defined at both
     the FDT-Instance and File levels, shall not be used by the FLUTE sender, in either the
     File-Instance or File element:"
     FEC-OTI-FEC-Instance-ID is the second item, annotated there as not applicable to the
     Release 9 FEC schemes. Sender-only: that clause's NOTE 2 leaves these "optional to support
     by the FLUTE receiver", so both parsers stay. */
  /* Stricter than the profile: the FEC building block forbids this element outright for the
     schemes this library implements, so it is withheld in both profiles rather than only under
     the 3GPP one.
     RFC 5052 clause 6.2.4: "The FEC Instance ID MUST be used by all Under-Specified FEC schemes
     and MUST NOT be used by Fully-Specified FEC Schemes."

     Both schemes here are Fully-Specified, stated by their own defining documents.
     RFC 3695: "This document also describes the Fully-Specified FEC scheme corresponding to FEC
     Encoding ID 0."
     RFC 5053: "The Raptor FEC Scheme is a Fully-Specified FEC Scheme corresponding to FEC
     Encoding ID 1."

     The 3GPP profile forbids it too, so this satisfies that as well.
     TS 26.346 V18.2.0 clause L.4.2, third list: "The following FDT parameters, defined at both
     the FDT-Instance and File levels, shall not be used by the FLUTE sender, in either the
     File-Instance or File element:"
     FEC-OTI-FEC-Instance-ID is the second item, annotated there as not applicable to the
     Release 9 FEC schemes.

     Sender-only, so both parsers stay. The member is kept rather than removed because an
     Under-Specified scheme would need it, and removing it would erase the reason it exists
     (rule 14).
     TS 26.346 V18.2.0 clause L.4.2, NOTE 2: "These parameters are optional to support by the
     FLUTE receiver." */
  (void)_global_fec_oti.instance_id;  // never emitted: see above
  root->SetAttribute("FEC-OTI-Maximum-Source-Block-Length", (unsigned)_global_fec_oti.max_source_block_length);
  root->SetAttribute("FEC-OTI-Encoding-Symbol-Length", (unsigned)_global_fec_oti.encoding_symbol_length);
  if (_global_fec_oti.encoding_id == FecScheme::Raptor) {
    // Raptor scheme-specific OTI (RFC 5053 §3.2.3)
    /* One base64 attribute, not three invented ones. The names previously emitted here,
       FEC-OTI-Number-Of-Source-Blocks, FEC-OTI-Number-Of-Sub-Blocks and
       FEC-OTI-Symbol-Alignment-Parameter, appear in no specification: zero occurrences in
       TS 26.346, and RFC 5053 defines no FDT mapping of its own. See the helper above. */
    root->SetAttribute("FEC-OTI-Scheme-Specific-Info",
                       encode_raptor_scheme_specific(_global_fec_oti.nof_source_blocks,
                                                     _global_fec_oti.nof_sub_blocks,
                                                     _global_fec_oti.symbol_alignment).c_str());
  }
  root->SetAttribute("xmlns:mbms2007", "urn:3GPP:metadata:2007:MBMS:FLUTE:FDT"); // 3GPP TS 26.346 Clause 7.2.10.2
  root->SetAttribute("xmlns:mbms2012", "urn:3GPP:metadata:2012:MBMS:FLUTE:FDT"); // 3GPP TS 26.346 Clause 7.2.10.2
  doc.InsertEndChild(root);

  for (const auto& file : _file_entries) {
    auto f = doc.NewElement("File");
    f->SetAttribute("TOI", file.toi);
    f->SetAttribute("Content-Location", file.content_location.c_str());
    f->SetAttribute("Content-Length", file.content_length);
    /* TS 26.346 V18.2.0 clause L.4.4, on the File-level attributes, fourth list:
       "The following attributes shall not be carried in the FDT sent by the FLUTE sender:"
       Transfer-Length is the first item of that list.

       The prohibition binds a sender operating the MBMS Download Profile, which is what
       Profile::Ts26517 selects. Under Profile::Unprofiled the session is plain RFC 3926, where
       the attribute is permitted, so it is kept. Keyed on the profile rather than on the FDT
       namespace because the namespace says which schema is emitted, not which obligations apply.

       The parser below is deliberately unchanged, because the same clause's NOTE keeps this one
       mandatory for receivers: "With the exception of Transfer-Length, which is mandatory, these
       parameters are optional to support by the FLUTE receiver." Nothing is lost on the wire either:
       the receive path falls back to Content-Length when the attribute is absent. */
    if (!is_3gpp(_profile) && file.fec_oti.transfer_length)
      f->SetAttribute("Transfer-Length", file.fec_oti.transfer_length);
    if (!file.content_md5.empty()) f->SetAttribute("Content-MD5", file.content_md5.c_str());
    if (!file.content_encoding.empty()) f->SetAttribute("Content-Encoding", file.content_encoding.c_str());
    if (!file.content_type.empty()) f->SetAttribute("Content-Type", file.content_type.c_str());
    if (file.fec_oti.encoding_id != _global_fec_oti.encoding_id)
      f->SetAttribute("FEC-OTI-FEC-Encoding-ID", (unsigned)file.fec_oti.encoding_id);
    // Same RFC 5052 clause 6.2.4 prohibition as at the FDT-Instance level above, and the same
    // clause L.4.2 one, applied at the File level. Never emitted for a Fully-Specified scheme.
    if (file.fec_oti.max_source_block_length != 0 &&
        file.fec_oti.max_source_block_length != _global_fec_oti.max_source_block_length)
      f->SetAttribute("FEC-OTI-Maximum-Source-Block-Length", (unsigned)file.fec_oti.max_source_block_length);
    if (file.fec_oti.encoding_symbol_length != 0 &&
        file.fec_oti.encoding_symbol_length != _global_fec_oti.encoding_symbol_length)
      f->SetAttribute("FEC-OTI-Encoding-Symbol-Length", (unsigned)file.fec_oti.encoding_symbol_length);
    if (file.fec_oti.encoding_id == FecScheme::Raptor) {
      if (file.fec_oti.nof_source_blocks != _global_fec_oti.nof_source_blocks)
        f->SetAttribute("FEC-OTI-Scheme-Specific-Info",
                        encode_raptor_scheme_specific(file.fec_oti.nof_source_blocks,
                                                      file.fec_oti.nof_sub_blocks,
                                                      file.fec_oti.symbol_alignment).c_str());
    }
    if (!file.etag.empty()) f->SetAttribute("mbms2012:File-ETag", file.etag.c_str());
    /* FileType's own Expires attribute, use="optional" in the annex L.6.1 profiled schema, so it
       is emitted only when the caller set one and omitted otherwise. It was never emitted before,
       which meant a File-level expiry could be set through the API and silently not reach the
       wire. Distinct from the cache directive below. */
    if (file.expires) f->SetAttribute("Expires", std::to_string(file.expires).c_str());
    if (file.cache_control.no_cache || file.cache_control.cache_expires) {
      auto cc = doc.NewElement("mbms2007:Cache-Control");
      if (file.cache_control.no_cache) {
        auto noc = doc.NewElement("mbms2007:no-cache");
        noc->SetText("true");
        cc->InsertEndChild(noc);
      } else {
        /* From the cache-control member, not from the File element's own Expires. The two are
           different things: CacheControlType in the annex L.6.1 profiled schema is an xs:choice
           of no-cache, max-stale and Expires, and its Expires is a caching directive, while
           FileType's Expires attribute says when the file itself stops being valid. Taking this
           from file.expires made the emitted directive whatever the file expiry happened to be. */
        auto exp = doc.NewElement("mbms2007:Expires");
        exp->SetText(std::to_string(file.cache_control.cache_expires.value()).c_str());
        cc->InsertEndChild(exp);
      }
      f->InsertEndChild(cc);
    }
    root->InsertEndChild(f);
  }


  /* Both 3GPP schemas make schemaVersion a mandatory child element of FDT-Instance, placed after
     the File elements, and each fixes its own value. Omitting it, or emitting the other schema's
     value, produces a document that does not validate against the schema it declares.

     Keyed on the FDT namespace, because this belongs to the schema being emitted and is meaningless
     in a document that declares neither.

     TS 26.346 V18.2.0 clause L.6.3, for the annex L.6.1 profiled schema: "The BM-SC shall set the
     schemaVersion element to 2 in all instance documents"

     TS 26.346 V18.2.0 clause 7.2.10.1, for the extended schema of that clause: "In this version of
     the present document the network shall set the content of the schemaVersion element, defined as
     a child of the FDT-Instance element, to the value 4."

     The delimiter element is deliberately not emitted: neither schema's sequence contains one. */
  if (_fdt_namespace == FDT_NS_3GPP_CONSOLIDATED_V2 || _fdt_namespace == FDT_NS_DRAFT_2005) {
    auto sv = doc.NewElement("schemaVersion");
    sv->SetText(_fdt_namespace == FDT_NS_3GPP_CONSOLIDATED_V2 ? 2 : 4);
    root->InsertEndChild(sv);
  }

  tinyxml2::XMLPrinter printer;
  doc.Print(&printer);
  return std::string(printer.CStr());
}
