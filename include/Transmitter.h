// libflute - FLUTE/ALC library
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#pragma once
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <queue>
#include <string>
#include <map>
#include <mutex>
#include "File.h"
#include "AlcPacket.h"
#include "FileDeliveryTable.h"

namespace LibFlute {
  /**
   *  FLUTE transmitter class. Construct an instance of this to send data through a FLUTE/ALC session.
   */
  class Transmitter {
    public:
     /**
      *  Definition of a file transmission completion callback function that can be
      *  registered through ::register_completion_callback.
      *
      *  @param toi TOI of the file that has completed transmission
      */
      typedef std::function<void(uint32_t)> completion_callback_t;

     /**
      *  Default constructor.
      *
      *  @param address Target multicast address
      *  @param port Target port 
      *  @param tsi TSI value for the session 
      *  @param mtu Path MTU to size FLUTE packets for 
      *  @param rate_limit Transmit rate limit (in kbps)
      *  @param io_service Boost io_service to run the socket operations in (must be provided by the caller)
      */
      Transmitter( const std::string& address, 
          short port, uint64_t tsi, unsigned short mtu,
          uint32_t rate_limit,
          boost::asio::io_service& io_service);

     /**
      *  Default destructor.
      */
      virtual ~Transmitter();

     /**
      *  Enable IPSEC ESP encryption of FLUTE payloads.
      *
      *  @param spi Security Parameter Index value to use
      *  @param key AES key as a hex string (without leading 0x). Must be an even number of characters long.
      */
      void enable_ipsec( uint32_t spi, const std::string& aes_key);

     /**
      *  Transmit a file. 
      *  The caller must ensure the data buffer passed here remains valid until the completion callback 
      *  for this file is called.
      *
      *  @param content_location URI to set in the content location field of the generated FDT entry
      *  @param content_type MIME type to set in the content type field of the generated FDT entry
      *  @param expires Expiry timestamp (based on NTP epoch)
      *  @param data Pointer to the data buffer (managed by caller)
      *  @param length Length of the data buffer (in bytes)
      *
      *  @return TOI of the file
      */
      uint16_t send(const std::string& content_location,
          const std::string& content_type,
          uint32_t expires,
          char* data,
          size_t length);

     /**
      *  Convenience function to get the current timestamp for expiry calculation
      *
      *  @return seconds since the NTP epoch
      */
      uint64_t seconds_since_epoch();

     /**
      *  Register a callback for file transmission completion notifications
      *
      *  @param cb Function to call on file completion
      */
      void register_completion_callback(completion_callback_t cb) { _completion_cb = cb; };

    private:
      void send_fdt();
      void send_next_packet();
      void fdt_send_tick();

      void file_transmitted(uint32_t toi);

      void handle_send_to(const boost::system::error_code& error);
      boost::asio::ip::udp::socket _socket;
      boost::asio::ip::udp::endpoint _endpoint;
      boost::asio::io_service& _io_service;
      boost::asio::deadline_timer _send_timer;
      boost::asio::deadline_timer _fdt_timer;

      uint64_t _tsi;
      uint16_t _mtu;

      std::unique_ptr<LibFlute::FileDeliveryTable> _fdt;
      std::map<uint32_t, std::shared_ptr<LibFlute::File>> _files;
      std::mutex _files_mutex;

      unsigned _fdt_repeat_interval = 5;
      uint16_t _toi = 1;

      uint32_t _max_payload;
      FecOti _fec_oti;

      completion_callback_t _completion_cb = nullptr;
      std::string _mcast_address;

      uint32_t _rate_limit = 0;
  };
};
