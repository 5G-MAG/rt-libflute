// libflute - FLUTE/ALC library
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
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
#include "Receiver.h"
#include "AlcPacket.h"
#include <iostream>
#include <string>
#include "spdlog/spdlog.h"
#include "IpSec.h"


LibFlute::Receiver::Receiver ( const std::string& iface, const std::string& address,
    short port, uint64_t tsi, boost::asio::io_service& io_service)
    : _socket(io_service)
    , _tsi(tsi)
    , _mcast_address(address)
{
    boost::asio::ip::udp::endpoint listen_endpoint(
        boost::asio::ip::address::from_string(iface), port);
    _socket.open(listen_endpoint.protocol());
    _socket.set_option(boost::asio::ip::multicast::enable_loopback(true));
    _socket.set_option(boost::asio::ip::udp::socket::reuse_address(true));
    _socket.set_option(boost::asio::socket_base::receive_buffer_size(16*1024*1024));
    _socket.bind(listen_endpoint);

    // Join the multicast group.
    _socket.set_option(
        boost::asio::ip::multicast::join_group(
          boost::asio::ip::address::from_string(address)));

    _socket.async_receive_from(
        boost::asio::buffer(_data, max_length), _sender_endpoint,
        boost::bind(&LibFlute::Receiver::handle_receive_from, this,
          boost::asio::placeholders::error,
          boost::asio::placeholders::bytes_transferred));
}

auto LibFlute::Receiver::enable_ipsec(uint32_t spi, const std::string& key) -> void 
{
  LibFlute::IpSec::enable_esp(spi, _mcast_address, LibFlute::IpSec::Direction::In, key);
}

auto LibFlute::Receiver::handle_receive_from(const boost::system::error_code& error,
    size_t bytes_recvd) -> void
{
  if (!_running) {
#ifdef SIMULATED_PKT_LOSS
    spdlog::warn("Stopping reception: total packets dropped {}", packets_dropped);
#endif // SIMULATED_PKT_LOSS
    return;
  }

  if (!error)
  {
    spdlog::trace("Received {} bytes", bytes_recvd);
    try {
      auto alc = LibFlute::AlcPacket(_data, bytes_recvd);

      if (alc.tsi() != _tsi) {
        spdlog::warn("Discarding packet for unknown TSI {}", alc.tsi());
        return;
      }

      const std::lock_guard<std::mutex> lock(_files_mutex);

      if (alc.toi() == 0 && (!_fdt || _fdt->instance_id() != alc.fdt_instance_id())) {
        if (_files.find(alc.toi()) == _files.end()) {
          FileDeliveryTable::FileEntry fe{0, "", static_cast<uint32_t>(alc.fec_oti().transfer_length), "", "", 0, alc.fec_oti(), 0};
          _files.emplace(alc.toi(), std::make_shared<LibFlute::File>(fe));
        }
      }

      if (_files.find(alc.toi()) != _files.end() && !_files[alc.toi()]->complete()) {
        auto encoding_symbols = LibFlute::EncodingSymbol::from_payload(
            _data + alc.header_length(), 
            bytes_recvd - alc.header_length(),
            _files[alc.toi()]->fec_oti(),
            alc.content_encoding());

        for (const auto& symbol : encoding_symbols) {

          spdlog::debug("received TOI {} SBN {} ID {}", alc.toi(), symbol.source_block_number(), symbol.id() );
            _files[alc.toi()]->put_symbol(symbol);
        }

        auto file = _files[alc.toi()].get();
        if (_files[alc.toi()]->complete()) {
          for (auto it = _files.begin(); it != _files.end();)
          {
            if (it->second.get() != file && it->second->meta().content_location == file->meta().content_location)
            {
              spdlog::debug("Replacing file with TOI {}", it->first);
              if (it->second.get()->meta().fec_transformer){
                delete it->second.get()->meta().fec_transformer;
                it->second.get()->meta().fec_transformer = 0;
              }
              it = _files.erase(it);
            }
            else
            {
              ++it;
            }
          }

          spdlog::debug("File with TOI {} completed", alc.toi());
          if (alc.toi() != 0 && _completion_cb) {
            _completion_cb(_files[alc.toi()]);
            if (_files[alc.toi()]->meta().fec_transformer){
              delete _files[alc.toi()]->meta().fec_transformer;
              _files[alc.toi()]->meta().fec_transformer = 0;
            }
            _files.erase(alc.toi());
          }

          if (alc.toi() == 0) { // parse complete FDT
            _fdt = std::make_unique<LibFlute::FileDeliveryTable>(
                alc.fdt_instance_id(), _files[alc.toi()]->buffer(), _files[alc.toi()]->length());

            _files.erase(alc.toi());
            for (const auto& file_entry : _fdt->file_entries()) {
              // automatically receive all files in the FDT
              if (_files.find(file_entry.toi) == _files.end()) {
                spdlog::debug("Starting reception for file with TOI {}: {} ({})", file_entry.toi,
                    file_entry.content_location, file_entry.content_type);
                _files.emplace(file_entry.toi, std::make_shared<LibFlute::File>(file_entry));
              }
            }
          }
        }
      } else {
        spdlog::trace("Discarding packet for unknown or already completed file with TOI {}", alc.toi());
      }
    } catch (std::exception ex) {
      spdlog::warn("Failed to decode ALC/FLUTE packet: {}", ex.what());
    }

    _socket.async_receive_from(
        boost::asio::buffer(_data, max_length), _sender_endpoint,
        boost::bind(&LibFlute::Receiver::handle_receive_from, this,
          boost::asio::placeholders::error,
          boost::asio::placeholders::bytes_transferred));
  }
  else 
  {
    spdlog::error("receive_from error: {}", error.message());
  }
}

auto LibFlute::Receiver::file_list() -> std::vector<std::shared_ptr<LibFlute::File>>
{
  std::vector<std::shared_ptr<LibFlute::File>> files;
  for (auto& f : _files) {
    files.push_back(f.second);
  }
  return files;
}

auto LibFlute::Receiver::remove_expired_files(unsigned max_age) -> void
{
  const std::lock_guard<std::mutex> lock(_files_mutex);
  for (auto it = _files.cbegin(); it != _files.cend();)
  {
    auto age = time(nullptr) - it->second->received_at();
    if ( it->second->meta().content_location != "bootstrap.multipart"  && age > max_age) {
      if (it->second.get()->meta().fec_transformer){
        delete it->second.get()->meta().fec_transformer;
        it->second.get()->meta().fec_transformer = 0;
      }
      it = _files.erase(it);
    } else {
      ++it;
    }
  }
}

auto LibFlute::Receiver::remove_file_with_content_location(const std::string& cl) -> void
{
  const std::lock_guard<std::mutex> lock(_files_mutex);
  for (auto it = _files.cbegin(); it != _files.cend();)
  {
    if ( it->second->meta().content_location == cl) {
      if (it->second.get()->meta().fec_transformer){
        delete it->second.get()->meta().fec_transformer;
        it->second.get()->meta().fec_transformer = 0;
      }
      it = _files.erase(it);
    } else {
      ++it;
    }
  }
}
