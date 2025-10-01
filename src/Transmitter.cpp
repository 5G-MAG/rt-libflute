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
static uint16_t calculate_sum( uint16_t *buffer, size_t len );

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
    memcpy(_data, other._data, _data_length);
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
    memcpy(_data, other._data, _data_length);
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
    _file_entry.toi = 0;
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
    _file_entry.toi = 0;
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
      _file_entry.toi = 0;
    } else if (data) {
      if (!_data) {
	if (data_length) {
          /* data being added, reset the TOI */
          _file_entry.toi = 0;
        }
      } else if (data_length) {
        /* had data before and have new data now, but are they the same? */
        unsigned char md5[MD5_DIGEST_LENGTH];
        MD5(reinterpret_cast<const unsigned char*>(data), data_length, md5);
        if (_file_entry.content_md5 != base64_encode(md5, sizeof(md5))) {
          /* data contents are different, reset TOI */
          _file_entry.toi = 0;
        }
      }
    } else if (_data) {
      /* data being removed, reset the TOI */
      _file_entry.toi = 0;
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
  _file_entry.cache_control.cache_expires = _file_entry.expires;

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
  read(_file_handle, data, _data_length);
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

Transmitter::Transmitter ( const std::string& address, short port,
                           uint64_t tsi, unsigned short mtu, uint32_t rate_limit,
                           boost::asio::io_context& io_context,
                           const std::optional<boost::asio::ip::udp::endpoint> &tunnel_endpoint,
                           Transmitter::FdtNamespace fdt_namespace )
    : _endpoint(boost::asio::ip::make_address(address), port)
    , _socket(io_context, _endpoint.protocol())
    , _io_context(io_context)
    , _send_timer(io_context)
    , _fdt_timer(io_context)
    , _tsi(tsi)
    , _mtu(mtu)
    , _files()
    , _files_mutex()
    , _mcast_address(address)
    , _rate_limit(rate_limit)
    , _tunnel_endpoint(tunnel_endpoint)
    , _tunnel_local_address()
{
  _max_payload = mtu -
    20 - // IPv4 header
     8 - // UDP header
    32 - // ALC Header with EXT_FDT and EXT_FTI
     4;  // SBN and ESI for compact no-code FEC
  if (_tunnel_endpoint.has_value()) {
    // Remove extra overhead for UDP tunnelling, if set
    _max_payload -= 20 - // IPv4 header
                    8; // UDP header
    boost::asio::ip::udp::socket local_socket(_io_context, _tunnel_endpoint.value().protocol());
    local_socket.connect(_tunnel_endpoint.value());
    _tunnel_local_address = local_socket.local_endpoint().address();
  }
  uint32_t max_source_block_length = 64;

  _socket.set_option(boost::asio::ip::multicast::enable_loopback(true));
  _socket.set_option(boost::asio::ip::udp::socket::reuse_address(true));

  _fec_oti = FecOti{
    .encoding_id = FecScheme::CompactNoCode,
    .encoding_symbol_length = _max_payload,
    .max_source_block_length = max_source_block_length};
  _fdt = std::make_unique<FileDeliveryTable>(1, _fec_oti, fdt_namespace);

  _fdt_timer.expires_from_now(boost::posix_time::seconds(_fdt_repeat_interval));
  _fdt_timer.async_wait( boost::bind(&Transmitter::fdt_send_tick, this));

  send_next_packet();
}

Transmitter::~Transmitter() = default;

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
  _fdt->set_expires(seconds_since_epoch() + _fdt_repeat_interval * 2);
  auto fdt = _fdt->to_string();
  auto file = std::make_shared<File>(
        0,
        _fec_oti,
        "",
        "",
        seconds_since_epoch() + _fdt_repeat_interval * 2,
        (char*)fdt.c_str(),
        fdt.length(),
        true);
  if (file) {
    file->set_fdt_instance_id( _fdt->instance_id() );
    spdlog::debug("Sending FDT instance {}:\n{}", _fdt->instance_id(), _fdt->to_string());
    _files.insert_or_assign(0, file);
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

  _fdt->add(file->meta());
  send_fdt();
  _files.insert({toi, file});
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
    file_description->toi(_toi);
    _toi++;
    if (_toi == 0) _toi = 1; // clamp to >= 1 in case it wraps
    spdlog::debug("Assigned new TOI {}", file_description->toi());
  }

  // Copy in default FEC parameters if not already set
  file_description->merge_fec_oti(_fec_oti);

  auto file = std::make_shared<File>(file_description);
  _fdt->add(file->meta());
  send_fdt();
  _files.insert({file_description->toi(), file});
  return file_description->toi();
}

auto Transmitter::fdt_send_tick() -> void
{
  send_fdt();
  _fdt_timer.expires_from_now(boost::posix_time::seconds(_fdt_repeat_interval));
  _fdt_timer.async_wait( boost::bind(&Transmitter::fdt_send_tick, this));
}

auto Transmitter::file_transmitted(uint32_t toi) -> void
{
  if (toi != 0) {
    _files.erase(toi);
    _fdt->remove(toi);
    send_fdt();

    if (_completion_cb) {
      _completion_cb(toi);
    }
  }
}

auto Transmitter::send_next_packet() -> void
{
  uint32_t bytes_queued = 0;

  if (_files.size()) {
    for (auto& file_m : _files) {
      auto file = file_m.second;

      if (file && !file->complete()) {
        auto symbols = file->get_next_symbols(_max_payload);

        if (symbols.size()) {
          for(const auto& symbol : symbols) {
            spdlog::debug("sending TOI {} SBN {} ID {}", file->meta().toi, symbol.source_block_number(), symbol.id() );
          }
          auto packet = std::make_shared<AlcPacket>(_tsi, file->meta().toi, file->meta().fec_oti, symbols, _max_payload, file->fdt_instance_id());
          bytes_queued += packet->size();

	  boost::asio::ip::udp::endpoint send_endpoint;
          char *data = nullptr;
          size_t data_size = 0;
	  if (_tunnel_endpoint) {
	    send_endpoint = _tunnel_endpoint.value();
	    data_size = packet->size() + 20 /* IP header */ + 8 /* UDP header */;
            data = new char[data_size];
	    create_udp_pkt(data+20, _endpoint, packet->data(), packet->size(), _tunnel_local_address);
	    create_ip_hdr(data, _endpoint, data_size, _tunnel_local_address);
	  } else {
            send_endpoint = _endpoint;
	    data = packet->data();
            data_size = packet->size();
          }
          _socket.async_send_to(
              boost::asio::buffer(data, data_size), send_endpoint,
              [file, symbols, packet, this](
                const boost::system::error_code& error,
                std::size_t bytes_transferred)
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
          if (_tunnel_endpoint) {
	    delete[] data;
          }
        }
        break;
      }
    }
  }
  if (!bytes_queued) {
    _send_timer.expires_from_now(boost::posix_time::milliseconds(10));
    _send_timer.async_wait( boost::bind(&Transmitter::send_next_packet, this));
  } else {
    if (_rate_limit == 0) {
      boost::asio::post(_io_context, boost::bind(&Transmitter::send_next_packet, this));
    } else {
      auto send_duration = ((bytes_queued * 8.0) / (double)_rate_limit/1000.0) * 1000.0 * 1000.0;
      spdlog::trace("Rate limiter: queued {} bytes, limit {} kbps, next send in {} us",
          bytes_queued, _rate_limit, send_duration);
      _send_timer.expires_from_now(boost::posix_time::microseconds(
            static_cast<int>(ceil(send_duration))));
      _send_timer.async_wait( boost::bind(&Transmitter::send_next_packet, this));
    }
  }
}

static void create_udp_pkt(char *udp_buffer, const boost::asio::ip::udp::endpoint &endpoint, const char *data, size_t data_len, const boost::asio::ip::address &local_address)
{
  struct udp_pseudo_hdr {
    in_addr_t source;
    in_addr_t dest;
    uint8_t reserved;
    uint8_t protocol;
    uint16_t length;
  } *pseudo_hdr = reinterpret_cast<struct udp_pseudo_hdr*>(udp_buffer - sizeof(*pseudo_hdr));
  struct udphdr *udp_hdr = reinterpret_cast<struct udphdr*>(udp_buffer);

  pseudo_hdr->source = htonl(local_address.to_v4().to_uint());
  pseudo_hdr->dest = htonl(endpoint.address().to_v4().to_uint());
  pseudo_hdr->reserved = 0;
  pseudo_hdr->protocol = endpoint.protocol().protocol();
  pseudo_hdr->length = htons(data_len + 8);

  udp_hdr->uh_sport = htons(endpoint.port());
  udp_hdr->uh_dport = udp_hdr->uh_sport;
  udp_hdr->uh_ulen = pseudo_hdr->length;
  udp_hdr->uh_sum = 0;
  memcpy(udp_buffer+8, data, data_len);

  udp_hdr->uh_sum = calculate_sum(reinterpret_cast<uint16_t*>(pseudo_hdr), data_len + 8 + 12);
}

static void create_ip_hdr(char *ip_buffer, const boost::asio::ip::udp::endpoint &endpoint, size_t pkt_size, const boost::asio::ip::address &local_address)
{
  struct iphdr *ip_hdr = reinterpret_cast<struct iphdr*>(ip_buffer);

  ip_hdr->version = IPVERSION;
  ip_hdr->ihl = 5; // 20 bytes
  ip_hdr->tos = 0;
  ip_hdr->tot_len = htons(pkt_size);
  ip_hdr->id = 0;
  ip_hdr->frag_off = 0; // not fragmenting
  ip_hdr->ttl = 63; // TTL 63 hops
  ip_hdr->protocol = endpoint.protocol().protocol();
  ip_hdr->check = 0;
  ip_hdr->saddr = htonl(local_address.to_v4().to_uint());
  ip_hdr->daddr = htonl(endpoint.address().to_v4().to_uint());

  ip_hdr->check = calculate_sum(reinterpret_cast<uint16_t*>(ip_hdr), 20);
}

static uint16_t calculate_sum(uint16_t *buffer, size_t len)
{
    uint32_t cksum = 0;

    while (len > 1) {
	cksum += ntohs(*buffer);
        len -= 2;
        buffer++;
    }
    if (len > 0) {
        cksum = *reinterpret_cast<uint8_t*>(buffer);
    }
    uint16_t result = ~htons(static_cast<uint16_t>(cksum & 0xFFFF) + static_cast<uint16_t>(cksum >> 16));

    return result;
}

} // End namespace LibFlute

