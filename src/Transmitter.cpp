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
    , _fec_redundancy_level(fec_redundancy_level)
{
  if (source_address) {
    _source_address = boost::asio::ip::make_address(source_address.value());
  }
  _max_payload = mtu -
    20 - // IPv4 header
     8 - // UDP header
    32 - // ALC Header with EXT_FDT and EXT_FTI
     4;  // SBN and ESI for compact no-code FEC
  if (_tunnel_endpoint.has_value()) {
    // Remove extra overhead for UDP tunnelling, if set
    _max_payload -= 20 + // IPv4 header
                    8; // UDP header
    boost::asio::ip::udp::socket local_socket(_io_context, _tunnel_endpoint.value().protocol());
    local_socket.connect(_tunnel_endpoint.value());
    _tunnel_local_address = local_socket.local_endpoint().address();
  }
  uint32_t max_source_block_length = 64;

  _socket.set_option(boost::asio::ip::multicast::enable_loopback(true));
  _socket.set_option(boost::asio::ip::udp::socket::reuse_address(true));

  // BUG FIX: this used to skip binding whenever a tunnel was configured, on the assumption the
  // socket was only ever used to reach the tunnel endpoint in that case. Now that a plain,
  // untunnelled copy is also sent directly to the real multicast destination (see send_next_packet()
  // below), that copy needs to originate from _source_address too, or an SSM-joined receiver
  // filtering on the announced source address (e.g. the SDP's "a=source-filter") never sees it --
  // confirmed live: without this, the plain copy left from the host's default outbound interface
  // address instead of the configured source, and never matched the receiver's SSM filter.
  if (_source_address && !_tunnel_endpoint) {
    _socket.bind(boost::asio::ip::udp::endpoint(_source_address.value(),0));
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
    _max_payload += 20 + // IPv4 header
                    8; // UDP header
    _tunnel_endpoint = std::nullopt;
  } else {
    /* new tunnel */
    _tunnel_endpoint = std::move(new_tunnel_endpoint);
    _max_payload -= 20 + // IPv4 header
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
  if (_source_address && !_tunnel_endpoint) {
    _socket.bind(boost::asio::ip::udp::endpoint(_source_address.value(),0));
  }
  return *this;
}

auto Transmitter::source_address(std::optional<boost::asio::ip::address> &&source_address) -> Transmitter&
{
  _source_address = std::move(source_address);
  if (_source_address && !_tunnel_endpoint) {
    _socket.bind(boost::asio::ip::udp::endpoint(_source_address.value(),0));
  }
  return *this;
}

auto Transmitter::enable_ipsec(uint32_t spi, const std::string& key) -> void
{
  IpSec::enable_esp(spi, _mcast_address, IpSec::Direction::Out, key);
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
  _fdt->set_expires(seconds_since_epoch() + _fdt_repeat_interval * 2);
  // Store into the member, not a local - see _fdt_string_storage's own doc
  // comment: the File below only keeps a raw pointer into this buffer, and
  // that pointer is read later, asynchronously, by send_next_packet().
  _fdt_string_storage = _fdt->to_string();
  // The FDT itself always goes out as Compact No-Code, regardless of what
  // FEC scheme protects this Transmitter's content: it's re-sent on its own
  // repeat timer already (real redundancy without needing FEC), and
  // Raptor has a minimum source-block size (K >= 4) that a small FDT's
  // single source block can fall below --
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
  bool is_resend = (file_description->toi() != 0);
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
  if (is_resend) {
    // Without this, add() below unconditionally appends another <File> entry for the same
    // TOI on every single carousel repetition, without ever removing the previous one (the
    // FDT has no other dedup by TOI) -- the FDT grows without bound the longer the object
    // stays in the carousel, and eventually becomes too large to serialise/parse correctly.
    _fdt->remove(file_description->toi());
  }
  _fdt->add(file->meta());
  send_fdt();
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

auto Transmitter::file_transmitted(uint32_t toi) -> void
{
  {
    std::lock_guard<std::mutex> guard(_files_mutex);
    _files.erase(toi);
  }
  if (toi != 0) {
    _fdt->remove(toi);
    send_fdt();

    if (_completion_cb) {
      _completion_cb(toi);
    }
  }

  {
    std::lock_guard<std::mutex> guard(_files_mutex);
    if (_deactivate_when_all_files_sent && _files.empty()) {
      _complete_deactivation();
    }
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

      // BUG FIX: this used to be if/else -- tunnel the packet to the N3mb GTP-U peer *instead
      // of* sending a plain copy to the real multicast destination, whenever a tunnel_endpoint
      // was configured. That meant any receiver doing a direct FLUTE-layer SSM join on the
      // announced address (rather than sitting behind the RAN's GTP-U decapsulation) never saw
      // a single packet -- confirmed live: continuous "Transmitted" log lines and real GTP-U
      // traffic reaching the UPF, but zero packets ever reaching the plain destination address.
      // Send both: the plain copy is what a direct SSM subscriber needs; the tunnelled copy is
      // the real, spec-compliant N3mb path (TS 23.247) that the RAN's GTP-U decapsulation
      // consumes. Completion (mark_completed/file_transmitted) is tracked from whichever send
      // is the "primary" one for this configuration -- the tunnel send when tunnelling is
      // active (preserving the exact completion timing tunnel-mode already had), otherwise the
      // plain send (the only copy in that case). The other send, if any, is fire-and-forget.
      if (_tunnel_endpoint) {
        // Fire-and-forget plain copy, in addition to the tunnelled one below.
        _socket.async_send_to(
            boost::asio::buffer(packet->data(), packet->size()), _endpoint,
            [packet](const boost::system::error_code& error, std::size_t /*bytes_transferred*/)
            {
              if (error) {
                spdlog::debug("sent_to (plain) error: {}", error.message());
              }
            });

        // Own the encapsulated buffer via a shared_ptr captured by the completion handler --
        // async_send_to is asynchronous, so the buffer must outlive the call. (The previous
        // code called `delete[] data` immediately after issuing async_send_to, before the
        // operation could have completed -- a use-after-free/dangling-buffer bug in its own
        // right, fixed here as a side effect of restructuring this block.)
        size_t data_size = packet->size() + 20 /* IP header */ + 8 /* UDP header */;
        auto data = std::make_shared<std::vector<char>>(data_size);
        create_udp_pkt(data->data() + 20, _endpoint, packet->data(), packet->size(), _source_address ? _source_address.value() : _tunnel_local_address);
        create_ip_hdr(data->data(), _endpoint, data_size, _source_address ? _source_address.value() : _tunnel_local_address);
        _socket.async_send_to(
            boost::asio::buffer(*data), _tunnel_endpoint.value(),
            [file, symbols, packet, data, this](
                const boost::system::error_code& error,
                std::size_t /*bytes_transferred*/)
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
                const boost::system::error_code& error,
                std::size_t /*bytes_transferred*/)
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
  const auto source_address = local_address.to_v4().to_uint();
  const auto destination_address = endpoint.address().to_v4().to_uint();

  write_uint16_be(udp_bytes, endpoint.port());
  write_uint16_be(udp_bytes + 2, endpoint.port());
  write_uint16_be(udp_bytes + 4, udp_length);
  write_uint16_be(udp_bytes + 6, 0);
  memcpy(udp_buffer + 8, data, data_len);

  std::vector<uint8_t> checksum_bytes(12 + udp_length);
  write_uint32_be(checksum_bytes.data(), source_address);
  write_uint32_be(checksum_bytes.data() + 4, destination_address);
  checksum_bytes[8] = 0;
  checksum_bytes[9] = endpoint.protocol().protocol();
  write_uint16_be(checksum_bytes.data() + 10, udp_length);
  memcpy(checksum_bytes.data() + 12, udp_bytes, udp_length);

  write_uint16_be(udp_bytes + 6, calculate_sum(checksum_bytes.data(), checksum_bytes.size()));
}

static void create_ip_hdr(char *ip_buffer, const boost::asio::ip::udp::endpoint &endpoint, size_t pkt_size, const boost::asio::ip::address &local_address)
{
  auto *ip_bytes = reinterpret_cast<uint8_t*>(ip_buffer);

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
