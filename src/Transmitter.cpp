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
#include <errno.h>
#include <fcntl.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#if HAVE_MMAP
#include <sys/mman.h>
#endif
#include <unistd.h>

// Suppress warnings about MD5 being deprecated in later versions of OpenSSL
#define OPENSSL_SUPPRESS_DEPRECATED 1
#include <openssl/md5.h>
#include "../utils/base64.h"

#include <zlib.h>

#include <ctime>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <list>
#include <string>
#include <system_error>

#include "spdlog/spdlog.h"
#include "File.h"
#include "IpSec.h"

#include "Transmitter.h"

namespace LibFlute {

static void create_udp_pkt( char *udp_buffer, const boost::asio::ip::udp::endpoint &endpoint, const char *data, size_t data_len,
                            const boost::asio::ip::address &local_address );
static void create_ip_hdr( char *ip_buffer, const boost::asio::ip::udp::endpoint &endpoint, size_t pkt_size,
                           const boost::asio::ip::address &local_address );
static uint16_t calculate_sum( const uint8_t *buffer, size_t len );

/** Fixed IP header length for the family in use: 20 bytes for an IPv4 header without options,
 *  40 for an IPv6 header, which is fixed length and needs no extension header here.
 *
 *  RFC 8200 clause 8.3: "an upper-layer protocol must take into account the larger size of the
 *  IPv6 header relative to the IPv4 header." */
static size_t ip_header_length(bool is_v6) { return is_v6 ? 40 : 20; }
static void write_uint16_be( uint8_t *buffer, uint16_t value );
static void write_uint32_be( uint8_t *buffer, uint32_t value );

/*****************************************************************************
 * Transmitter::FileDescription class
 *****************************************************************************/

Transmitter::FileDescription::FileDescription ( const std::string &content_location, const std::string &filename )
    : _tsi()
    , _file_entry({ .toi=0, .content_location=content_location})
    , _compression_type(Transmitter::FileDescription::COMPRESSION_NONE)
    , _filename()
    , _file_handle(-1)
    , _data(nullptr)
    , _data_length(0)
{
  _attach_file(filename);
  _calculate_file_entry();
}

Transmitter::FileDescription::FileDescription(const std::string &content_location, const std::vector<char> &data)
    : _tsi()
    , _file_entry({ .toi=0, .content_location=content_location})
    , _compression_type(Transmitter::FileDescription::COMPRESSION_NONE)
    , _filename()
    , _file_handle(-1)
    , _data(data.data())
    , _data_length(data.size())
{
  _calculate_file_entry();
}

Transmitter::FileDescription::FileDescription(const std::string &content_location, const std::vector<unsigned char> &data)
    : _tsi()
    , _file_entry({ .toi=0, .content_location=content_location})
    , _compression_type(Transmitter::FileDescription::COMPRESSION_NONE)
    , _filename()
    , _file_handle(-1)
    , _data(reinterpret_cast<const char*>(data.data()))
    , _data_length(data.size())
{
  _calculate_file_entry();
}

Transmitter::FileDescription::FileDescription(const std::string &content_location, const char *data, size_t length)
    : _tsi()
    , _file_entry({ .toi=0, .content_location=content_location})
    , _compression_type(Transmitter::FileDescription::COMPRESSION_NONE)
    , _filename()
    , _file_handle(-1)
    , _data(data)
    , _data_length(data?length:0)
{
  _calculate_file_entry();
}

Transmitter::FileDescription::FileDescription(const std::string &content_location)
    : _tsi()
    , _file_entry({ .toi=0, .content_location=content_location})
    , _compression_type(Transmitter::FileDescription::COMPRESSION_NONE)
    , _filename()
    , _file_handle(-1)
    , _data(nullptr)
    , _data_length(0)
{
  _calculate_file_entry();
}

Transmitter::FileDescription::FileDescription(const Transmitter::FileDescription &other)
    : _tsi(other._tsi)
    , _file_entry(other._file_entry)
    , _compression_type(other._compression_type)
    , _filename(other._filename)
    , _file_handle(-1)
    , _data(other._data)
    , _data_length(other._data_length)
{
  if (!_filename.empty()) {
    if (other._file_handle >= 0) {
      _file_handle = dup(other._file_handle);
    }
#if HAVE_MMAP
    // Map the file contents into memory
    _data = reinterpret_cast<char*>(mmap(nullptr, _data_length, PROT_READ, MAP_SHARED, _file_handle, 0));
#else
    // copy the file contents into a new memory block
    char *data = new char[_data_length];
    _data = data;
    memcpy(data, other._data, _data_length);
#endif
  }
}

Transmitter::FileDescription::FileDescription(Transmitter::FileDescription &&other)
    : _tsi(std::move(other._tsi))
    , _file_entry(other._file_entry)
    , _compression_type(other._compression_type)
    , _filename(std::move(other._filename))
    , _file_handle(other._file_handle)
    , _data(other._data)
    , _data_length(other._data_length)
{
  other._data = nullptr;
  other._data_length = 0;
  other._file_handle = -1;
}

Transmitter::FileDescription::~FileDescription ()
{
  _free_file_data();
}

Transmitter::FileDescription &Transmitter::FileDescription::operator=(const Transmitter::FileDescription &other)
{
  _tsi = other._tsi;
  _file_entry = other._file_entry;
  _compression_type = other._compression_type;
  _filename = other._filename;
  _file_handle = -1;
  _data = other._data;
  _data_length = other._data_length;

  if (!_filename.empty()) {
    if (other._file_handle >= 0) {
      _file_handle = dup(other._file_handle);
    }
#if HAVE_MMAP
    // Map the file contents into memory
    _data = reinterpret_cast<char*>(mmap(nullptr, _data_length, PROT_READ, MAP_SHARED, _file_handle, 0));
#else
    // copy the file contents into a new memory block
    char *data = new char[_data_length];
    _data = data;
    memcpy(data, other._data, _data_length);
#endif
  }

  return *this;
}

Transmitter::FileDescription &Transmitter::FileDescription::operator=(Transmitter::FileDescription &&other)
{
  _tsi = std::move(other._tsi);
  _file_entry = other._file_entry;
  _compression_type = other._compression_type;
  _filename = std::move(other._filename);
  _file_handle = other._file_handle;
  other._file_handle = -1;

  _data = other._data;
  other._data = nullptr;
  _data_length = other._data_length;
  other._data_length = 0;

  return *this;
}

bool Transmitter::FileDescription::operator==(const Transmitter::FileDescription &other) const
{
  if (_tsi != other._tsi) return false;
  if (_compression_type != other._compression_type) return false;

  // _file_entry
  if (_file_entry != other._file_entry) return false;

  //if (_filename != other._filename) return false;

  if (_data_length != other._data_length) return false;

  if (_data == other._data) return true;
  return memcmp(_data, other._data, _data_length) == 0;
}

const char *Transmitter::FileDescription::data()
{
  return _data;
}

size_t Transmitter::FileDescription::data_length()
{
  return _data_length;
}

void Transmitter::FileDescription::_reset_toi()
{
  // Remembers the TOI being vacated so Transmitter::send() can remove its now-stale FDT entry
  // when it assigns a new one -- without this, a FileDescription whose content genuinely
  // changes (as opposed to being resent unchanged) leaves the FDT entry for its previous TOI
  // orphaned forever, since nothing else ever points at that old TOI again to clean it up.
  // The FDT grows without bound as a result, one orphaned entry per content change.
  if (_file_entry.toi != 0) _previous_toi = _file_entry.toi;
  _file_entry.toi = 0;
}

Transmitter::FileDescription &Transmitter::FileDescription::set_compression(
								Transmitter::FileDescription::CompressionAlgorithm compression)
{
  if (compression != _compression_type) {
    _compression_type = compression;
    switch (_compression_type) {
    case COMPRESSION_GZIP:
      _file_entry.content_encoding = "gzip";
      break;
    case COMPRESSION_DEFLATE:
      _file_entry.content_encoding = "deflate";
      break;
    default:
      _file_entry.content_encoding.clear();
      break;
    }
    /* change in compression will change transmitted data, reset the TOI */
    _reset_toi();
    _calculate_file_entry();
  }

  return *this;
}

Transmitter::FileDescription &Transmitter::FileDescription::set_content_location(const std::string &location)
{
  _file_entry.content_location = location;

  return *this;
}

Transmitter::FileDescription &Transmitter::FileDescription::set_content(const std::string &filename)
{
  if (filename != _filename) {
    _free_file_data();
    _attach_file(filename);
    /* Assume a change of filename changes the contents too and zero the TOI */
    _reset_toi();
    _calculate_file_entry();
  }

  return *this;
}

Transmitter::FileDescription &Transmitter::FileDescription::set_content(const char *data, size_t data_length)
{
  if (!data) data_length=0;
  if (data != _data || _data_length != data_length) {
    /* data area has changed in some way, do we need to reset the TOI? */
    if (_data_length != data_length) {
      /* data length has changed, reset the TOI */
      _reset_toi();
    } else if (data) {
      if (!_data) {
	if (data_length) {
          /* data being added, reset the TOI */
          _reset_toi();
        }
      } else if (data_length) {
        /* had data before and have new data now, but are they the same? */
        unsigned char md5[MD5_DIGEST_LENGTH];
        MD5(reinterpret_cast<const unsigned char*>(data), data_length, md5);
        if (_file_entry.content_md5 != base64_encode(md5, sizeof(md5))) {
          /* data contents are different, reset TOI */
          _reset_toi();
        }
      }
    } else if (_data) {
      /* data being removed, reset the TOI */
      _reset_toi();
    }

    _free_file_data();
    _data = data;
    _data_length = data_length;
    _calculate_file_entry();
  }

  return *this;
}

Transmitter::FileDescription &Transmitter::FileDescription::set_content(const std::vector<char> &data)
{
  return set_content(data.data(), data.size());
}

Transmitter::FileDescription &Transmitter::FileDescription::set_content(const std::vector<unsigned char> &data)
{
  return set_content(reinterpret_cast<const char*>(data.data()), data.size());
}

Transmitter::FileDescription &Transmitter::FileDescription::set_content_type(const std::string &content_type)
{
  _file_entry.content_type = content_type;
  return *this;
}

static const Transmitter::FileDescription::date_time_type &_get_ntp_epoch()
{
  static bool is_set = false;
  static Transmitter::FileDescription::date_time_type ntp_epoch;
  if (!is_set) {
    std::tm ntp_epoch_tm = {.tm_mday=1, .tm_mon=0, .tm_year=0};
    ntp_epoch = std::chrono::system_clock::from_time_t(std::mktime(&ntp_epoch_tm));
    is_set = true;
  }
  return ntp_epoch;
}

Transmitter::FileDescription &Transmitter::FileDescription::set_expiry_time(
								const Transmitter::FileDescription::date_time_type &expiry_time)
{
  auto diff = std::chrono::duration_cast<std::chrono::seconds>(expiry_time - _get_ntp_epoch());
  _file_entry.expires = diff.count();

  return *this;
}

Transmitter::FileDescription &Transmitter::FileDescription::set_cache_expiry_time(
								const Transmitter::FileDescription::date_time_type &expiry_time)
{
  auto diff = std::chrono::duration_cast<std::chrono::seconds>(expiry_time - _get_ntp_epoch());
  _file_entry.cache_control.cache_expires = diff.count();

  return *this;
}

Transmitter::FileDescription::date_time_type Transmitter::FileDescription::get_expiry_time() const
{
  auto durn = std::chrono::duration_cast<date_time_type::duration>(std::chrono::seconds(_file_entry.expires));
  return _get_ntp_epoch() + durn;
}

Transmitter::FileDescription &Transmitter::FileDescription::set_etag(const std::string &etag)
{
  _file_entry.etag = etag;
  return *this;
}

const std::string &Transmitter::FileDescription::get_etag() const
{
  return _file_entry.etag;
}

Transmitter::FileDescription &Transmitter::FileDescription::merge_fec_oti(const FecOti &fec_oti)
{
  if (static_cast<unsigned>(_file_entry.fec_oti.encoding_id) == 0) {
    _file_entry.fec_oti.encoding_id = fec_oti.encoding_id;
  }
  if (!_file_entry.fec_oti.instance_id) {
    _file_entry.fec_oti.instance_id = fec_oti.instance_id;
  }
  if (!_file_entry.fec_oti.transfer_length) {
    _file_entry.fec_oti.transfer_length = fec_oti.transfer_length;
  }
  if (!_file_entry.fec_oti.encoding_symbol_length) {
    _file_entry.fec_oti.encoding_symbol_length = fec_oti.encoding_symbol_length;
  }
  if (!_file_entry.fec_oti.max_source_block_length) {
    _file_entry.fec_oti.max_source_block_length = fec_oti.max_source_block_length;
  }
  if (!_file_entry.fec_oti.max_number_of_encoding_symbols) {
    _file_entry.fec_oti.max_number_of_encoding_symbols = fec_oti.max_number_of_encoding_symbols;
  }
  return *this;
}

void Transmitter::FileDescription::_attach_file(const std::string &filename)
{
  _filename = filename;
  _file_handle = open(_filename.c_str(), O_RDONLY);
  if (_file_handle < 0) {
    throw std::system_error(errno, std::generic_category(), "Could not open the file");
  }
  // Get the size
  off_t pos = lseek(_file_handle, 0, SEEK_END);
  if (pos < 0) {
    throw std::system_error(errno, std::generic_category(), "Could not find the file length");
  }
  _data_length = static_cast<size_t>(pos);
  lseek(_file_handle, 0, SEEK_SET);

#if HAVE_MMAP
  // Map the file contents into memory
  _data = reinterpret_cast<char*>(mmap(nullptr, _data_length, PROT_READ, MAP_SHARED, _file_handle, 0));
#else
  // Load the file contents into memory
  char *data = new char[_data_length];
  _data = data;
  ssize_t nread = read(_file_handle, data, _data_length);
  if (nread < 0 || static_cast<size_t>(nread) != _data_length) {
    throw std::system_error(errno, std::generic_category(), "Could not read the file contents");
  }
  close(_file_handle);
  _file_handle = -1;
#endif
}

void Transmitter::FileDescription::_free_file_data()
{
  if (!_filename.empty()) {
#if HAVE_MMAP
    if (_data) munmap(const_cast<char*>(_data), _data_length);
    if (_file_handle >= 0) close(_file_handle);
#else
    delete[] const_cast<char*>(_data);
#endif
    _filename.clear();
  }
}

void Transmitter::FileDescription::_calculate_file_entry()
{
  // Content length
  _file_entry.content_length = _data_length;

  // Initial transfer length assumes no encoding, this may be changed on transmission
  _file_entry.fec_oti.transfer_length = _data_length;

  // MD5 checksum
  if (_data && _data_length) {
    unsigned char md5[MD5_DIGEST_LENGTH];
    MD5(reinterpret_cast<const unsigned char*>(_data), _data_length, md5);
    _file_entry.content_md5 = base64_encode(md5, sizeof(md5));
  } else {
    _file_entry.content_md5.clear();
  }
}

/*****************************************************************************
 * Transmitter class
 *****************************************************************************/

Transmitter::Transmitter ( const std::string& destination_address, short port,
                           uint64_t tsi, unsigned short mtu, uint32_t rate_limit,
                           boost::asio::io_context& io_context,
                           const std::optional<boost::asio::ip::udp::endpoint> &tunnel_endpoint,
                           Transmitter::FdtNamespace fdt_namespace, bool active,
                           const std::optional<std::string> &source_address,
                           const std::optional<FecOti> &content_fec_oti,
                           Profile profile,
                           uint32_t fec_redundancy_level )
    : _endpoint(boost::asio::ip::make_address(destination_address), port)
    , _source_address()
    , _socket(io_context, _endpoint.protocol())
    , _io_context(io_context)
    , _send_timer(io_context)
    , _fdt_timer(io_context)
    , _tsi(tsi)
    , _mtu(mtu)
    , _files()
    , _files_mutex()
    , _mcast_address(destination_address)
    , _rate_limit(rate_limit)
    , _tunnel_endpoint(tunnel_endpoint)
    , _tunnel_local_address()
    , _active(active)
    , _profile(profile)
    , _fec_redundancy_level(fec_redundancy_level)
{
  /* The 3GPP profiles name the FEC schemes they admit, and the set is closed.

     TS 26.346 V18.2.0 clause L.4.7: "the two FEC schemes referenced in this specification, the
     Compact No-Code FEC scheme as specified in RFC 3695 [13], and the Raptor FEC scheme as
     specified in RFC 5053 [91] are optional to implement by the BM-SC and mandatory to support by
     the UE."

     TS 26.517 does not widen it: clause 6.2.4.5 gives the repair byte-range determination for FEC
     Encoding ID 0 and FEC Encoding ID 1 and for no other value.

     RaptorQ is RFC 6330, which neither document references, so a receiver operating either profile
     has no obligation to decode it and in general will not. Refused here rather than sent: a
     session no receiver can decode is worse than a refusal at setup. Available under
     Profile::Unprofiled, which is what this branch adds it for.

     Tested against the admissible set rather than against RaptorQ by name, so that a scheme added
     later is refused under a profile until someone decides otherwise, rather than admitted by
     default. */
  if (is_3gpp(profile) && content_fec_oti.has_value() &&
      !is_3gpp_admissible_fec_scheme(content_fec_oti->encoding_id)) {
    throw std::runtime_error(
        "the 3GPP profiles admit only the Compact No-Code and Raptor FEC schemes; use one of those "
        "or Profile::Unprofiled");
  }

  /* The 3GPP profiles fix the TSI field at its narrowest width, so a value that would need the
     wider encoding cannot be signalled under either of them. This is a clause 7.2 rule, binding on
     MBMS download generally, not one of annex L.4's profile restrictions.

     TS 26.346 V18.2.0 clause 7.2.7: "The Transmission Session Identifier (TSI) field shall be of
     length 16 bits (S=0, H=1, 16 bits)."

     Outside the profile RFC 3451 permits 16, 32 or 48 bits and the wider encoding is used, which is
     what the TSI widening on this branch is for. Refused rather than truncated, since truncation
     puts the session on an identifier nobody configured, and rather than widened, since that emits
     a header the profile forbids. */
  if (is_3gpp(_profile) && tsi > 0xFFFF) {
    throw std::runtime_error(
        "TSI does not fit the 16-bit field TS 26.346 clause 7.2.7 fixes for it; use a TSI of 65535 "
        "or less, or Profile::Unprofiled where RFC 3451 permits the wider encoding");
  }

  if (source_address) {
    _source_address = boost::asio::ip::make_address(source_address.value());
  }
  _max_payload = mtu -
    ip_header_length(_endpoint.address().is_v6()) - // IP header, v4 or v6
     8 - // UDP header
    32 - // ALC Header with EXT_FDT and EXT_FTI
     4;  // SBN and ESI for compact no-code FEC
  if (_tunnel_endpoint.has_value()) {
    // Remove extra overhead for UDP tunnelling, if set
    _max_payload -= ip_header_length(_endpoint.address().is_v6()) + // IP header, v4 or v6
                    8; // UDP header
    boost::asio::ip::udp::socket local_socket(_io_context, _tunnel_endpoint.value().protocol());
    local_socket.connect(_tunnel_endpoint.value());
    _tunnel_local_address = local_socket.local_endpoint().address();
  }
  uint32_t max_source_block_length = 64;

  _socket.set_option(boost::asio::ip::multicast::enable_loopback(true));
  _socket.set_option(boost::asio::ip::udp::socket::reuse_address(true));

  /* A tunnelled session still sends an untunnelled copy to the real multicast destination, and
     that copy has to originate from the configured source address too, or a receiver filtering
     on the announced source (an SDP a=source-filter, say) never matches it. Binding was skipped
     whenever a tunnel was configured, on the assumption the socket only ever reached the tunnel
     endpoint. */
  if (_source_address) {
    _socket.bind(boost::asio::ip::udp::endpoint(_source_address.value(),0));
    // bind() only sets the packet's claimed source address; it does not choose which interface a
    // multicast send actually goes out on. Without IP_MULTICAST_IF (boost's outbound_interface),
    // the kernel picks the default route's interface for every multicast send regardless of the
    // bound source (confirmed live: `ip route get <group>` with no source hint resolves to the
    // machine's default-route interface, not the one implied by _source_address; the same query
    // with `from <_source_address>` resolves correctly, which is what a bound send does NOT
    // consult for multicast). code-derived, no spec claim: this is host networking, not FLUTE's
    // own behaviour, so only applied when the source is IPv4 (the only case this project's own
    // deployments use).
    if (_source_address->is_v4()) {
      _socket.set_option(boost::asio::ip::multicast::outbound_interface(_source_address->to_v4()));
    }
  }

  // Caller-supplied FEC OTI selects the scheme; otherwise Compact No-Code.
  if (content_fec_oti.has_value() && content_fec_oti->encoding_id != FecScheme::CompactNoCode) {
    _fec_oti = FecOti{
      .encoding_id = content_fec_oti->encoding_id,
      .encoding_symbol_length = _max_payload,
      .max_source_block_length = content_fec_oti->max_source_block_length,
      .max_number_of_encoding_symbols = content_fec_oti->max_number_of_encoding_symbols};
  } else {
    _fec_oti = FecOti{
      .encoding_id = FecScheme::CompactNoCode,
      .encoding_symbol_length = _max_payload,
      .max_source_block_length = max_source_block_length};
  }
  /* A sub-block must stay under 256 KB under either 3GPP profile.

     TS 26.346 V18.2.0 clause 7.2.3: "The values of N, Z, T and A shall be set such that the
     sub-block size is less than 256 KB."

     N, Z, T and A are the RFC 5053 scheme-specific parameters, so this binds the Raptor path. This
     implementation fixes N at 1 (see FecOti::nof_sub_blocks), so a sub-block is a source block and
     the constraint reduces to K * T < 256 KB, where T is the encoding symbol length settled above.

     The obligation is on how the sender chooses the parameters, so the ceiling is applied as the
     default when the caller named no maximum, rather than refusing a session that asked for nothing
     wrong. A caller who does name a maximum above the ceiling is refused, since honouring it would
     breach the clause and silently shrinking it would discard a stated configuration. */
  if (is_3gpp(profile) && _fec_oti.encoding_id == FecScheme::Raptor) {
    constexpr uint32_t kMaxSubBlockSize = 256 * 1024;
    const uint32_t ceiling_k = (kMaxSubBlockSize - 1) / _fec_oti.encoding_symbol_length;
    if (ceiling_k == 0) {
      throw std::runtime_error(
          "encoding symbol length leaves no room for a source block under the 256 KB sub-block "
          "ceiling TS 26.346 clause 7.2.3 sets");
    }
    if (_fec_oti.max_source_block_length == 0) {
      _fec_oti.max_source_block_length = ceiling_k;
    } else if (_fec_oti.max_source_block_length > ceiling_k) {
      throw std::runtime_error(
          "max_source_block_length would put a sub-block over the 256 KB ceiling TS 26.346 clause "
          "7.2.3 sets for the 3GPP profiles; lower it, shorten the symbol, or use Profile::Unprofiled");
    }
  }

  _fdt = std::make_unique<FileDeliveryTable>(1, _fec_oti, fdt_namespace, profile);

  if (_active) {
    start_fdt_repeat_timer();
    send_next_packet();
  }
}

Transmitter::~Transmitter() = default;

auto Transmitter::udp_tunnel_address(const boost::asio::ip::udp::endpoint &new_tunnel_endpoint) -> Transmitter&
{
  return udp_tunnel_address(std::optional<boost::asio::ip::udp::endpoint>(new_tunnel_endpoint));
}

auto Transmitter::udp_tunnel_address(boost::asio::ip::udp::endpoint &&new_tunnel_endpoint) -> Transmitter&
{
  return udp_tunnel_address(std::optional<boost::asio::ip::udp::endpoint>(std::move(new_tunnel_endpoint)));
}

auto Transmitter::udp_tunnel_address(const std::optional<boost::asio::ip::udp::endpoint> &new_tunnel_endpoint) -> Transmitter&
{
  return udp_tunnel_address(std::move(std::optional<boost::asio::ip::udp::endpoint>(new_tunnel_endpoint)));
}

auto Transmitter::udp_tunnel_address(std::optional<boost::asio::ip::udp::endpoint> &&new_tunnel_endpoint) -> Transmitter&
{
  if (!!_tunnel_endpoint == !!new_tunnel_endpoint) {
    /* change existing tunnel */
    if (_tunnel_endpoint) _tunnel_endpoint = new_tunnel_endpoint;
  } else if (_tunnel_endpoint) {
    /* removing tunnel */
    _max_payload += ip_header_length(_endpoint.address().is_v6()) + // IP header, v4 or v6
                    8; // UDP header
    _tunnel_endpoint = std::nullopt;
  } else {
    /* new tunnel */
    _tunnel_endpoint = std::move(new_tunnel_endpoint);
    _max_payload -= ip_header_length(_endpoint.address().is_v6()) + // IP header, v4 or v6
                    8; // UDP header
  }

  if (_tunnel_endpoint) {
    boost::asio::ip::udp::socket local_socket(_io_context, _tunnel_endpoint.value().protocol());
    local_socket.connect(_tunnel_endpoint.value());
    _tunnel_local_address = local_socket.local_endpoint().address();
  }
  return *this;
}

auto Transmitter::udp_tunnel_address(const std::nullopt_t&) -> Transmitter&
{
  return udp_tunnel_address(std::optional<boost::asio::ip::udp::endpoint>(std::nullopt));
}

auto Transmitter::endpoint(const std::string &address, uint32_t port) -> Transmitter&
{
  return endpoint(boost::asio::ip::udp::endpoint(boost::asio::ip::make_address(address), port));
}

auto Transmitter::endpoint(const boost::asio::ip::udp::endpoint &destination) -> Transmitter&
{
  _endpoint = destination;
  return *this;
}

auto Transmitter::endpoint(boost::asio::ip::udp::endpoint &&destination) -> Transmitter&
{
  _endpoint = std::move(destination);
  return *this;
}

auto Transmitter::source_address(const std::optional<boost::asio::ip::address> &source_address) -> Transmitter&
{
  _source_address = source_address;
  /* A tunnelled session still sends an untunnelled copy to the real multicast destination, and
     that copy has to originate from the configured source address too, or a receiver filtering
     on the announced source (an SDP a=source-filter, say) never matches it. Binding was skipped
     whenever a tunnel was configured, on the assumption the socket only ever reached the tunnel
     endpoint. */
  if (_source_address) {
    _socket.bind(boost::asio::ip::udp::endpoint(_source_address.value(),0));
    // See the same bind()'s own comment in start(): bind() alone does not steer a multicast
    // send onto this address's interface, only IP_MULTICAST_IF does.
    if (_source_address->is_v4()) {
      _socket.set_option(boost::asio::ip::multicast::outbound_interface(_source_address->to_v4()));
    }
  }
  return *this;
}

auto Transmitter::source_address(std::optional<boost::asio::ip::address> &&source_address) -> Transmitter&
{
  _source_address = std::move(source_address);
  /* A tunnelled session still sends an untunnelled copy to the real multicast destination, and
     that copy has to originate from the configured source address too, or a receiver filtering
     on the announced source (an SDP a=source-filter, say) never matches it. Binding was skipped
     whenever a tunnel was configured, on the assumption the socket only ever reached the tunnel
     endpoint. */
  if (_source_address) {
    _socket.bind(boost::asio::ip::udp::endpoint(_source_address.value(),0));
    // See the same bind()'s own comment in start(): bind() alone does not steer a multicast
    // send onto this address's interface, only IP_MULTICAST_IF does.
    if (_source_address->is_v4()) {
      _socket.set_option(boost::asio::ip::multicast::outbound_interface(_source_address->to_v4()));
    }
  }
  return *this;
}

auto Transmitter::enable_ipsec(uint32_t spi, const std::string& key, const std::string& auth_key) -> void
{
  IpSec::enable_esp(spi, _mcast_address, _endpoint.port(), IpSec::Direction::Out, key,
                    auth_key);
}

auto Transmitter::handle_send_to(const boost::system::error_code& error) -> void
{
  if (!error) {
  }
}

auto Transmitter::seconds_since_epoch() -> uint64_t
{
  return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count() +
      2'208'988'800; /* add the difference in seconds between the Unix epoch (1 January 1970, 00:00:00 UTC)
                        and the NTP epoch (1 January 1900, 00:00:00 UTC) */
}

auto Transmitter::send_fdt() -> void {
  if (_fdt->file_entries().empty()) return;

  const uint32_t instance_id = _fdt->instance_id();
  const bool already_serialised = !_fdt_string_storage.empty() && _fdt_serialised_instance_id == instance_id;

  if (already_serialised) {
    /* This instance has already been serialised, so re-send those exact bytes rather than
       building new ones. An FDT Instance is identified by its ID, and a receiver reassembles
       its symbols under that identity.
       RFC 3926 clause 3.4.1: "Each FDT Instance is uniquely identified within the file delivery
       session by its FDT Instance ID."
       Re-serialising on every repeat gave the same ID a different byte string each time (the
       FDT-Instance Expires attribute moves with the clock), so a receiver combining symbols it
       believed belonged to one object was mixing two, and reassembly could never validate.
       Repeating an instance is expected -- the same clause's own model allows it: "A certain FDT
       Instance may be repeated several times during a session" -- but a repeat has to be the same
       instance, not a new document under an old number. */
    std::lock_guard<std::mutex> guard(_files_mutex);
    auto in_flight = _files.find(0);
    if (in_flight != _files.end() && in_flight->second && !in_flight->second->complete()) {
      /* Still going out. Replacing it here would restart it from its first symbol, and under a
         rate limit an FDT spanning several symbols would never reach its last one. */
      return;
    }
  } else {
    _fdt->set_expires(seconds_since_epoch() + _fdt_repeat_interval * 2);
    // Store into the member, not a local - see _fdt_string_storage's own doc
    // comment: the File below only keeps a raw pointer into this buffer, and
    // that pointer is read later, asynchronously, by send_next_packet().
    _fdt_string_storage = _fdt->to_string();
    _fdt_serialised_instance_id = instance_id;
  }
  // The FDT itself always goes out as Compact No-Code, regardless of what
  // FEC scheme protects this Transmitter's content: it's re-sent on its own
  // repeat timer already (real redundancy without needing FEC), and
  // Raptor/RaptorQ both have a minimum source-block size (K >= 4 for
  // Raptor) that a small FDT's single source block can fall below --
  // caught by testing a real Transmitter -> Receiver transfer, not by any
  // in-process File/codec test, all of which constructed content Files
  // directly and never exercised send_fdt()'s own File construction.
  FecOti fdt_fec_oti{
    .encoding_id = FecScheme::CompactNoCode,
    .encoding_symbol_length = _fec_oti.encoding_symbol_length,
    .max_source_block_length = 64};
  auto file = std::make_shared<File>(
        0,
        fdt_fec_oti,
        "",
        "",
        seconds_since_epoch() + _fdt_repeat_interval * 2,
        (char*)_fdt_string_storage.c_str(),
        _fdt_string_storage.length(),
        true);
  if (file) {
    file->set_fdt_instance_id( _fdt->instance_id() );
    spdlog::debug("Sending FDT instance {}:\n{}", _fdt->instance_id(), _fdt->to_string());
    {
      std::lock_guard<std::mutex> guard(_files_mutex);
      _files.insert_or_assign(0, file);
    }
    _fdt->sent();
  }
}

auto Transmitter::send(
    const std::string& content_location,
    const std::string& content_type,
    uint32_t expires,
    char* data,
    size_t length) -> uint16_t
{
  auto toi = _toi;
  _toi++;
  if (_toi == 0) _toi = 1; // clamp to >= 1 in case it wraps

  auto file = std::make_shared<File>(
        toi,
        _fec_oti,
        content_location,
        content_type,
        expires,
        data,
        length);
  file->set_fec_redundancy_level(_fec_redundancy_level);

  _fdt->add(file->meta());
  send_fdt();
  {
    std::lock_guard<std::mutex> guard(_files_mutex);
    _files.insert({toi, file});
  }
  return toi;
}

auto Transmitter::send(const std::shared_ptr<Transmitter::FileDescription> &file_description) -> uint16_t
{
  if (file_description->has_tsi() && file_description->tsi() != _tsi) {
    // Reset TOI if the file_description is being used on a new TSI
    file_description->toi(0);
    spdlog::debug("Reset TOI for FileDescription");
  }

  // Set the TSI and TOI for the FileDescription
  file_description->tsi(_tsi);
  if (file_description->toi() == 0) {
    if (file_description->previous_toi() != 0) {
      // This FileDescription's content changed since its last send (set_content()/
      // set_compression() zeroed its TOI for exactly this reason) -- the FDT entry for its
      // old TOI is otherwise never revisited once a new TOI is assigned below, and would sit
      // in the FDT forever, growing it by one stale entry per content change.
      _fdt->remove(file_description->previous_toi());
      file_description->reset_previous_toi();
    }
    file_description->toi(_toi);
    _toi++;
    if (_toi == 0) _toi = 1; // clamp to >= 1 in case it wraps
    spdlog::debug("Assigned new TOI {}", file_description->toi());
  }

  // Copy in default FEC parameters if not already set
  file_description->merge_fec_oti(_fec_oti);

  auto file = std::make_shared<File>(file_description);
  file->set_fec_redundancy_level(_fec_redundancy_level);
  {
    std::lock_guard<std::mutex> guard(_files_mutex);
    // A reused (carousel-repeat) FileDescription keeps the same TOI, but map::insert() is a
    // no-op if that key is already present -- silently keeping the stale File and discarding
    // the just-built one with the updated content. Use assignment so a resend actually
    // replaces it.
    _files[file_description->toi()] = file;
  }
  // add() replaces the entry for a TOI it already holds, so a carousel repetition no longer needs
  // to remove the previous entry first to stop the FDT growing without bound. It also reports
  // whether the table actually changed: a repetition that describes the object exactly as the
  // current instance already does leaves the table alone, and reissuing the FDT there would be
  // harmful rather than merely wasteful. send_fdt() replaces the in-flight FDT object wholesale
  // (insert_or_assign on TOI 0), restarting its transmission from the first symbol, so a sender
  // repeating many objects would restart a multi-symbol FDT faster than it can finish sending,
  // and receivers would never assemble a complete instance. The FDT's own repeat timer
  // (fdt_send_tick) re-sends it regardless.
  if (_fdt->add(file->meta())) {
    send_fdt();
  }
  return file_description->toi();
}

auto Transmitter::fdt_send_tick(const boost::system::error_code& error) -> void
{
  if (error == boost::asio::error::operation_aborted) return;
  if (_active) {
    send_fdt();
    start_fdt_repeat_timer();
  }
}

auto Transmitter::withdraw_file(uint32_t toi) -> void
{
  if (toi == 0) return; // TOI 0 is the FDT itself, never a described file
  {
    std::lock_guard<std::mutex> guard(_files_mutex);
    _files.erase(toi);
  }
  _fdt->remove(toi);
  send_fdt();
}

auto Transmitter::file_transmitted(uint32_t toi) -> void
{
  {
    std::lock_guard<std::mutex> guard(_files_mutex);
    _files.erase(toi);
  }
  if (toi != 0) {
    /* A repeating sender re-sends an object under the TOI it already has (see send(), which treats
       a non-zero TOI as a resend), so dropping the description here would leave that object
       transmitted but undescribed for the rest of the session. A receiver cannot recover an object
       whose TOI no entry describes, however cleanly its symbols arrive, and receivers join at
       arbitrary times, so the entry is what makes a repeating object acquirable at all. Retain the
       description while the sender still intends to repeat it; one-shot senders keep removing it,
       which is what bounds FDT growth for them.

       Note this deliberately does NOT advertise the result as a full snapshot: the MBMS Download
       Profile prohibits the sender doing so. TS 26.346 V18.2.0 clause L.4.3: "The following
       parameters, defined at the FDT-Instance level, shall not be used by the FLUTE sender:",
       whose list carries the mbms2008:FullFDT attribute. Retaining entries needs no such
       signalling; it only stops the table forgetting objects that are still being sent. */
    if (!_retain_transmitted_in_fdt) {
      _fdt->remove(toi);
      /* The table just changed, so publish the new one. Under retention it did not change: the
         entry is still there and still correct, and re-publishing would be actively harmful.
         send_fdt() replaces the in-flight FDT object wholesale (insert_or_assign on TOI 0), which
         restarts its transmission from the first symbol. A retained table describes every object
         the sender repeats, so it spans several symbols, and a sender completing one object per
         few hundred milliseconds would restart it far more often than it can finish, leaving
         receivers with a permanent stream of first symbols and no complete instance. Leave
         re-transmission to the FDT's own repeat timer. */
      send_fdt();
    }

    if (_completion_cb) {
      _completion_cb(toi);
    }
  }

  bool drained = false;
  {
    std::lock_guard<std::mutex> guard(_files_mutex);
    drained = _files.empty();
    if (_deactivate_when_all_files_sent && drained) {
      _complete_deactivation();
    }
  }

  /* The last packet with the flag set has now gone out, but a receiver that lost it has no other
     way to learn the session ended: once the file set empties, send_fdt() has nothing to repeat.
     One data-less packet, once. */
  if (_session_closing && drained) {
    send_close_session_packet();
  }
}

auto Transmitter::send_next_packet() -> void
{
  uint32_t bytes_queued = 0;

  if (!_active) return;
  std::shared_ptr<File> file;
  {
    std::lock_guard<std::mutex> guard(_files_mutex);
    for (auto& file_m : _files) {
      auto &next_file = file_m.second;

      if (next_file && !next_file->complete()) {
        file = next_file;
        break;
      }
    }
  }
  if (file) {
    auto symbols = file->get_next_symbols(_max_payload);

    if (symbols.size()) {
      for(const auto& symbol : symbols) {
        spdlog::debug("sending TOI {} SBN {} ID {}", file->meta().toi, symbol.source_block_number(), symbol.id() );
      }
      auto packet = std::make_shared<AlcPacket>(_tsi, file->meta().toi, file->meta().fec_oti, symbols, _max_payload, file->fdt_instance_id(),
                                                 _session_closing, _closing_objects.count(file->meta().toi) > 0);
      bytes_queued += packet->size();

      /* A tunnel is an additional path, not a replacement for the announced one. Sending only the
         encapsulated copy leaves a receiver that joins the announced destination directly, rather
         than sitting behind the tunnel's decapsulation, with no packets at all. Both copies go out
         when a tunnel is configured, and completion is tracked from the tunnelled send, which is
         the primary path in that configuration; the plain copy is fire-and-forget. Without a
         tunnel the plain send is the only one, and it carries the completion. */
      if (_tunnel_endpoint) {
        _socket.async_send_to(
            boost::asio::buffer(packet->data(), packet->size()), _endpoint,
            [packet](const boost::system::error_code& error, std::size_t /*bytes_transferred*/)
            {
              if (error) {
                spdlog::debug("sent_to (plain) error: {}", error.message());
              }
            });

        /* The completion handler owns the encapsulated buffer through this shared_ptr, so it
           outlives the asynchronous send. A raw new[] freed straight after issuing the send would
           be read after free. */
        const size_t ip_hdr_len = ip_header_length(_endpoint.address().is_v6());
        const size_t data_size = packet->size() + ip_hdr_len + 8 /* UDP header */;
        auto tunnel_data = std::make_shared<std::vector<char>>(data_size);
        auto local_address = _source_address ? _source_address.value() : _tunnel_local_address;
        create_udp_pkt(tunnel_data->data() + ip_hdr_len, _endpoint, packet->data(), packet->size(), local_address);
        create_ip_hdr(tunnel_data->data(), _endpoint, data_size, local_address);

        _socket.async_send_to(
            boost::asio::buffer(*tunnel_data), _tunnel_endpoint.value(),
            [file, symbols, packet, tunnel_data, this](
                const boost::system::error_code& error, std::size_t /*bytes_transferred*/)
            {
              if (error) {
                spdlog::debug("sent_to (tunnel) error: {}", error.message());
              } else {
                file->mark_completed(symbols, !error);
                if (file->complete()) {
                  file_transmitted(file->meta().toi);
                }
              }
            });
      } else {
        _socket.async_send_to(
            boost::asio::buffer(packet->data(), packet->size()), _endpoint,
            [file, symbols, packet, this](
                const boost::system::error_code& error, std::size_t /*bytes_transferred*/)
            {
              if (error) {
                spdlog::debug("sent_to error: {}", error.message());
              } else {
                file->mark_completed(symbols, !error);
                if (file->complete()) {
                  file_transmitted(file->meta().toi);
                }
              }
            });
      }
    }
  }
  if (_active) {
    if (!bytes_queued) {
      _send_timer.expires_after(std::chrono::milliseconds(10));
      _send_timer.async_wait( boost::bind(&Transmitter::send_next_packet, this));
    } else {
      if (_rate_limit == 0) {
        boost::asio::post(_io_context, boost::bind(&Transmitter::send_next_packet, this));
      } else {
        auto send_duration = ((bytes_queued * 8.0) / (double)_rate_limit/1000.0) * 1000.0 * 1000.0;
        spdlog::trace("Rate limiter: queued {} bytes, limit {} kbps, next send in {} us",
            bytes_queued, _rate_limit, send_duration);
        _send_timer.expires_after(std::chrono::microseconds(
              static_cast<int>(ceil(send_duration))));
        _send_timer.async_wait( boost::bind(&Transmitter::send_next_packet, this));
      }
    }
  }
}

auto Transmitter::activate() -> void
{
  if (!_active) {
    _deactivate_when_all_files_sent = false;
    _active = true;
    start_fdt_repeat_timer();
    send_next_packet();
  }
}

auto Transmitter::deactivate(bool finish_file_transmissions) -> void
{
  if (_active) {
    if (finish_file_transmissions) {
      std::lock_guard<std::mutex> guard(_files_mutex);
      if (!_files.empty()) {
        _deactivate_when_all_files_sent = true;
        return;
      }

      _complete_deactivation();
      return;
    }

    _complete_deactivation();
  }
}

auto Transmitter::_complete_deactivation() -> void
{
  _deactivate_when_all_files_sent = false;
  _active = false;
  _fdt_timer.cancel();
  _send_timer.cancel();
}

auto Transmitter::close_session() -> void
{
  _session_closing = true;
  _fdt->set_complete(true);

  /* Every packet from here on carries the flag, which is the whole signal while there is still
     something to send. With an empty queue there is nothing to attach it to, so the flag would
     reach no receiver at all; RFC 3926 clause 3.1 provides a packet for exactly that case and this
     is where it is due. */
  bool queue_empty = false;
  {
    std::lock_guard<std::mutex> guard(_files_mutex);
    queue_empty = _files.empty();
  }
  if (queue_empty) {
    send_close_session_packet();
  }
}

/* RFC 3450 clause 4.1: "In some special cases an ALC sender may need to produce ALC packets that do
   not contain any payload.  This may be required, for example, to signal the end of a session or to
   convey congestion control information."

   Fire and forget on both paths. There is no file to mark complete and nothing to retransmit: the
   flag is advisory on the receiver's side, which RFC 5651 clause 5.1 puts as "the receiver SHOULD
   assume that no more packets will be sent to the session", so a lost one costs a receiver a
   timeout rather than data. */
auto Transmitter::send_close_session_packet() -> void
{
  std::shared_ptr<AlcPacket> packet;
  try {
    packet = std::make_shared<AlcPacket>(_tsi, AlcPacket::CloseSession{});
  } catch (const std::exception& ex) {
    spdlog::warn("Not signalling end of session: {}", ex.what());
    return;
  }

  _socket.async_send_to(
      boost::asio::buffer(packet->data(), packet->size()), _endpoint,
      [packet](const boost::system::error_code& error, std::size_t /*bytes_transferred*/)
      {
        if (error) {
          spdlog::debug("close session send error: {}", error.message());
        }
      });

  if (_tunnel_endpoint) {
    const size_t ip_hdr_len = ip_header_length(_endpoint.address().is_v6());
    const size_t data_size = packet->size() + ip_hdr_len + 8 /* UDP header */;
    auto tunnel_data = std::make_shared<std::vector<char>>(data_size);
    auto local_address = _source_address ? _source_address.value() : _tunnel_local_address;
    create_udp_pkt(tunnel_data->data() + ip_hdr_len, _endpoint, packet->data(), packet->size(),
                   local_address);
    create_ip_hdr(tunnel_data->data(), _endpoint, data_size, local_address);

    _socket.async_send_to(
        boost::asio::buffer(*tunnel_data), _tunnel_endpoint.value(),
        [packet, tunnel_data](const boost::system::error_code& error,
                              std::size_t /*bytes_transferred*/)
        {
          if (error) {
            spdlog::debug("close session tunnel send error: {}", error.message());
          }
        });
  }
}

auto Transmitter::close_object(uint32_t toi) -> void
{
  _closing_objects.insert(toi);
}

auto Transmitter::start_fdt_repeat_timer() -> void
{
    _fdt_timer.expires_after(std::chrono::seconds(_fdt_repeat_interval));
    _fdt_timer.async_wait( boost::bind(&Transmitter::fdt_send_tick, this, boost::placeholders::_1));
}

static void create_udp_pkt(char *udp_buffer, const boost::asio::ip::udp::endpoint &endpoint, const char *data, size_t data_len, const boost::asio::ip::address &local_address)
{
  auto *udp_bytes = reinterpret_cast<uint8_t*>(udp_buffer);
  const auto udp_length = static_cast<uint16_t>(data_len + 8);
  const bool is_v6 = endpoint.address().is_v6();

  write_uint16_be(udp_bytes, endpoint.port());
  write_uint16_be(udp_bytes + 2, endpoint.port());
  write_uint16_be(udp_bytes + 4, udp_length);
  write_uint16_be(udp_bytes + 6, 0);
  memcpy(udp_buffer + 8, data, data_len);

  std::vector<uint8_t> checksum_bytes;
  if (is_v6) {
    /* RFC 8200 clause 8.1's pseudo-header: source (16), destination (16), upper-layer packet
       length (32), three zero octets, next header (8). */
    checksum_bytes.resize(40 + udp_length);
    auto src_bytes = local_address.to_v6().to_bytes();
    auto dst_bytes = endpoint.address().to_v6().to_bytes();
    memcpy(checksum_bytes.data(), src_bytes.data(), src_bytes.size());
    memcpy(checksum_bytes.data() + 16, dst_bytes.data(), dst_bytes.size());
    write_uint32_be(checksum_bytes.data() + 32, udp_length);
    checksum_bytes[36] = 0;
    checksum_bytes[37] = 0;
    checksum_bytes[38] = 0;
    checksum_bytes[39] = endpoint.protocol().protocol();
    memcpy(checksum_bytes.data() + 40, udp_bytes, udp_length);
  } else {
    checksum_bytes.resize(12 + udp_length);
    write_uint32_be(checksum_bytes.data(), local_address.to_v4().to_uint());
    write_uint32_be(checksum_bytes.data() + 4, endpoint.address().to_v4().to_uint());
    checksum_bytes[8] = 0;
    checksum_bytes[9] = endpoint.protocol().protocol();
    write_uint16_be(checksum_bytes.data() + 10, udp_length);
    memcpy(checksum_bytes.data() + 12, udp_bytes, udp_length);
  }

  uint16_t checksum = calculate_sum(checksum_bytes.data(), checksum_bytes.size());
  if (is_v6 && checksum == 0) {
    /* RFC 8200 clause 8.1: "whenever originating a UDP packet, an IPv6 node must compute a UDP
       checksum over the packet and the pseudo-header, and, if that computation yields a result
       of zero, it must be changed to hex FFFF for placement in the UDP header." */
    checksum = 0xFFFF;
  }
  write_uint16_be(udp_bytes + 6, checksum);
}

static void create_ip_hdr(char *ip_buffer, const boost::asio::ip::udp::endpoint &endpoint, size_t pkt_size, const boost::asio::ip::address &local_address)
{
  auto *ip_bytes = reinterpret_cast<uint8_t*>(ip_buffer);

  if (endpoint.address().is_v6()) {
    /* RFC 8200 clause 3 gives the header format. It is fixed at 40 bytes and carries no
       checksum field of its own, which is why the UDP checksum above is mandatory. */
    memset(ip_bytes, 0, 40);
    ip_bytes[0] = 0x60; // version 6, traffic class high nibble zero
    write_uint16_be(ip_bytes + 4, static_cast<uint16_t>(pkt_size - 40)); // payload, excluding this header
    ip_bytes[6] = endpoint.protocol().protocol(); // next header
    ip_bytes[7] = 63; // hop limit, matching the IPv4 path's TTL below
    auto src_bytes = local_address.to_v6().to_bytes();
    auto dst_bytes = endpoint.address().to_v6().to_bytes();
    memcpy(ip_bytes + 8, src_bytes.data(), src_bytes.size());
    memcpy(ip_bytes + 24, dst_bytes.data(), dst_bytes.size());
    return;
  }

  memset(ip_bytes, 0, 20);
  ip_bytes[0] = 0x45; // IPv4, 20-byte header
  ip_bytes[1] = 0;
  write_uint16_be(ip_bytes + 2, static_cast<uint16_t>(pkt_size));
  write_uint16_be(ip_bytes + 4, 0);
  write_uint16_be(ip_bytes + 6, 0);
  ip_bytes[8] = 63;
  ip_bytes[9] = endpoint.protocol().protocol();
  write_uint32_be(ip_bytes + 12, local_address.to_v4().to_uint());
  write_uint32_be(ip_bytes + 16, endpoint.address().to_v4().to_uint());
  write_uint16_be(ip_bytes + 10, calculate_sum(ip_bytes, 20));
}

static uint16_t calculate_sum(const uint8_t *buffer, size_t len)
{
  uint32_t cksum = 0;

  while (len > 1) {
    cksum += (static_cast<uint32_t>(buffer[0]) << 8) | static_cast<uint32_t>(buffer[1]);
    len -= 2;
    buffer += 2;
  }
  if (len > 0) {
    cksum += static_cast<uint32_t>(buffer[0]) << 8;
  }

  while (cksum >> 16) {
    cksum = (cksum & 0xFFFF) + (cksum >> 16);
  }

  return static_cast<uint16_t>(~cksum);
}

static void write_uint16_be(uint8_t *buffer, uint16_t value)
{
  buffer[0] = static_cast<uint8_t>((value >> 8) & 0xFF);
  buffer[1] = static_cast<uint8_t>(value & 0xFF);
}

static void write_uint32_be(uint8_t *buffer, uint32_t value)
{
  buffer[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
  buffer[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
  buffer[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
  buffer[3] = static_cast<uint8_t>(value & 0xFF);
}

} // End namespace LibFlute
