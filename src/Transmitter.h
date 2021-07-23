
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
  class Transmitter {
    public:
      typedef std::function<void(uint32_t)> completion_callback_t;

      Transmitter( const std::string& address, 
          short port, uint64_t tsi, unsigned short mtu,
          boost::asio::io_service& io_service);

      virtual ~Transmitter();

      uint64_t send(const std::string& content_location,
          const std::string& content_type,
          uint32_t expires,
          char* data,
          size_t length);
      uint64_t seconds_since_epoch();

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
  };
};
