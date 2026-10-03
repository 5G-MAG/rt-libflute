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
#pragma once
#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <map>
#include <mutex>
#include <set>
#include <vector>
#include "File.h"
#include "FileDeliveryTable.h"

namespace LibFlute {
  /**
   *  FLUTE receiver class. Construct an instance of this to receive files from a FLUTE/ALC session.
   */
  class Receiver {
    public:
     /**
      *  Definition of a file reception completion callback function that can be
      *  registered through ::register_completion_callback.
      *
      *  @returns shared_ptr to the received file
      */
      typedef std::function<void(std::shared_ptr<LibFlute::File>)> completion_callback_t;

     /**
      *  Definition of a callback invoked whenever an incoming packet carries the LCT Close
      *  Session and/or Close Object flag (RFC 3451 clause 5.1, 'A' and 'B' bits), registered through
      *  ::register_close_notification_callback.
      *
      *  @param session_closed True if the sender signalled the whole session is ending
      *  @param object_closed True if the sender signalled no further data for this TOI
      *  @param toi The TOI the packet carrying the flag(s) was for
      */
      typedef std::function<void(bool session_closed, bool object_closed, uint32_t toi)> close_notification_callback_t;

     /**
      *  Definition of the caller-supplied hook that locates the ALC/LCT payload inside a
      *  tunnelled datagram.
      *
      *  https://github.com/5G-MAG/rt-libflute/issues/66 : the Receiver-side counterpart to
      *  Transmitter's existing udp_tunnel_address() support (see its _tunnel_endpoint /
      *  create_ip_hdr() / create_udp_pkt()). Per the issue discussion: the library has no
      *  business knowing about any particular encapsulation format -- that is entirely the
      *  controlling application's concern ("a better design pattern would be for the
      *  controlling application to pass in a 'helper' function that the library invokes to
      *  do application-specific mangling of packets before the generic code in the library
      *  starts processing the ALC/LCT payload", rjb1000). This is that helper's signature.
      *
      *  This mirrors the de-tunnelling logic that already exists, hand-written, in
      *  tests/test_end_to_end.cpp's run_tunnel_bridge() (added alongside Transmitter's own
      *  tunnel mode) -- a std::thread there receives on a plain UDP socket, manually parses
      *  a hand-built inner IPv4+UDP header out of the payload (no GTP-U or other framing;
      *  Transmitter's tunnel mode wraps the ALC packet in exactly this and nothing else),
      *  and forwards just the FLUTE bytes onward over loopback to a receiver that has no
      *  tunnel-awareness at all. This issue asks for that same logic to move inside Receiver
      *  proper, as a caller-supplied, protocol-agnostic hook rather than a hardcoded parser
      *  baked into the library, so any encapsulation a given deployment actually needs
      *  (matching Transmitter's own wrapper, real GTP-U, or anything else) is expressed
      *  purely by what modifier the caller passes in.
      *
      *  @param payload The whole datagram as received on the tunnel socket. The callback may
      *         freely inspect and/or edit its contents in place (e.g. to decrypt a payload
      *         that arrives encrypted under the encapsulation, not just to locate it).
      *  @return The byte offset within (the, possibly now-modified) @p payload at which the
      *          ALC/LCT payload begins. A return value >= payload.size() (e.g. SIZE_MAX)
      *          tells the Receiver to silently discard this datagram without attempting ALC
      *          parsing -- there is no separate bool/optional discard signal; encoding
      *          "nothing usable here" as "there are no bytes left to read" keeps this one
      *          consistent, easy-to-satisfy contract rather than two.
      */
      typedef std::function<size_t(std::vector<uint8_t>& payload)> packet_modifier_t;

     /**
      *  Default constructor.
      *
      *  @param iface Address of the (local) interface to bind the receiving socket to. 0.0.0.0 = any.
      *  @param address Multicast address
      *  @param port Target port
      *  @param tsi TSI value of the session
      *  @param io_context Boost io_context to run the socket operations in (must be provided by the caller)
      *  @param source_address If non-empty, join as source-specific multicast (SSM, IPv4
      *         only) admitting only packets from this source -- otherwise ASM (any-source).
      *         Unrelated to the tunnel parameters below; this is the pre-existing plain
      *         multicast join, unchanged.
      *  @param tunnel_address If given, ALSO bind a plain unicast UDP socket to this local
      *         endpoint and accept tunnelled datagrams there, in addition to the normal
      *         multicast join above. Motivating cases (see the issue): a deployment that
      *         needs to receive FLUTE content arriving encapsulated (e.g. GTP-U) rather than
      *         as plain IP multicast, or one where the platform's local multicast delivery
      *         isn't available on the path content actually arrives over at all (confirmed
      *         live: a datagram written to a software TUN device, as a UE simulator's own
      *         decapsulated-content path does, never reaches a socket joined to its
      *         destination multicast group on that interface -- the platform's multicast
      *         delivery never triggers for it at all -- even though the identical datagram
      *         delivers correctly on a real network interface). The two paths are
      *         independent and both feed the same session state, so either one arriving is
      *         enough; this is deliberately not an either/or choice like Transmitter's
      *         tunnel mode, since a Receiver has no way to know in advance which path will
      *         actually work in a given deployment.
      *  @param tunnel_source If given, only accept tunnel datagrams whose UDP source address
      *         matches this value -- the tunnel-socket equivalent of @p source_address's SSM
      *         admit-only-this-source semantics, since the tunnel socket itself is a plain
      *         unicast bind with no multicast-layer source filtering of its own. This is
      *         the "extra address checking" the library itself does, on top of whatever
      *         @p packet_modifier does -- source-address admission is a generic,
      *         encapsulation-agnostic concept the library can reasonably own, unlike parsing
      *         any particular header format.
      *  @param packet_modifier Required whenever tunnel_address is set (ignored otherwise,
      *         and if omitted while tunnel_address is set, every tunnel datagram is
      *         discarded -- silently failing open would be far worse than silently
      *         discarding). See packet_modifier_t; there is no default implementation, since
      *         any default would itself bake an assumption about the tunnel's wire format
      *         into the library, exactly what this issue asks not to do.
      */
      Receiver( const std::string& iface, const std::string& address,
          short port, uint64_t tsi,
          boost::asio::io_context& io_context,
          const std::string& source_address = "",
          const std::optional<boost::asio::ip::udp::endpoint>& tunnel_address = std::nullopt,
          const std::optional<boost::asio::ip::address>& tunnel_source = std::nullopt,
          const std::optional<packet_modifier_t>& packet_modifier = std::nullopt);

     /**
      *  Destructor. Marks the receiver as no longer alive so that any async_receive_from
      *  completion already queued on the io_context when this object is destroyed (e.g. the
      *  owner recreated the receiver, or a caller destroys it mid-flight) finds out before
      *  touching a freed `this` -- boost::asio only guarantees a cancelled operation's
      *  handler eventually runs with operation_aborted, not that it runs before the
      *  destructor returns, so a raw `this`-bound handler left queued past that point would
      *  otherwise be a use-after-free.
      */
      virtual ~Receiver();

     /**
      *  Enable IPSEC ESP decryption of FLUTE payloads.
      *
      *  @param spi Security Parameter Index value to use
      *  @param key AES key as a hex string (without leading 0x). Must be an even number of characters long.
      */
      void enable_ipsec( uint32_t spi, const std::string& aes_key, const std::string& auth_key = "");

     /**
      *  List all current files
      *
      *  @return Vector of all files currently in the FDT
      */
      std::vector<std::shared_ptr<LibFlute::File>> file_list();

     /**
      *  Remove files from the list that are older than max_age seconds
      */
      void remove_expired_files(unsigned max_age);

     /**
      *  Remove a file from the list that matches the passed content location
      */
      void remove_file_with_content_location(const std::string& cl);

     /**
      *  Register a callback for file reception notifications
      *
      *  @param cb Function to call on file completion
      */
      void register_completion_callback(completion_callback_t cb) { _completion_cb = cb; };

     /**
      *  Register a callback for LCT Close Session / Close Object notifications
      *
      *  @param cb Function to call when an incoming packet carries either flag
      */
      void register_close_notification_callback(close_notification_callback_t cb) { _close_cb = cb; };

      void stop() { _running = false; }
    private:

      void handle_receive_from(const boost::system::error_code& error,
          size_t bytes_recvd);
      void arm_receive();
      // The actual ALC/FLUTE processing, shared by both the normal multicast-socket path
      // (handle_receive_from(), data already at offset 0 in _data) and the tunnel path
      // (handle_tunnel_receive_from(), data at whatever offset packet_modifier_t returns
      // inside _tunnel_data) -- see packet_modifier_t's comment in the header for why these
      // are two independent, simultaneously-armed receive loops rather than a single one.
      void process_alc_datagram(char* data, size_t len);
      void handle_tunnel_receive_from(const boost::system::error_code& error,
          size_t bytes_recvd);
      void arm_tunnel_receive();
      boost::asio::ip::udp::socket _socket;
      boost::asio::ip::udp::endpoint _sender_endpoint;

      std::unique_ptr<boost::asio::ip::udp::socket> _tunnel_socket;
      boost::asio::ip::udp::endpoint _tunnel_sender_endpoint;
      std::optional<boost::asio::ip::address> _tunnel_source;
      packet_modifier_t _packet_modifier;
      // Sized like _data (see max_length below) but as a resizable buffer rather than a
      // fixed char array: packet_modifier_t takes a std::vector<uint8_t>& so a caller-supplied
      // modifier can shrink/grow the datagram in place (e.g. after decrypting a payload that
      // changes size), not just report where to start reading a fixed buffer.
      std::vector<uint8_t> _tunnel_data;

      // Must hold the largest UDP datagram that can actually arrive: with the
      // Compact No-Code FEC scheme, a packet is a 4-byte SBN+ID header plus one
      // full encoding symbol, and FEC-OTI-Encoding-Symbol-Length is a per-session
      // configuration value with no fixed small upper bound (e.g. large symbols
      // sized close to the path MTU, or, as over loopback/jumbo-capable links,
      // sized close to the max IPv4 UDP payload). A too-small buffer here doesn't
      // error out -- recvfrom() on a datagram socket silently truncates to
      // whatever fits, so every symbol beyond that size is completed with
      // whatever partial prefix arrived, and the truncation is invisible until a
      // Content-MD5 check (if present in the FDT) catches the corruption. 65536
      // covers the maximum possible IPv4 UDP payload (65507 bytes) with margin.
      enum { max_length = 65536 };
      char _data[max_length];
      uint64_t _tsi;
      std::unique_ptr<LibFlute::FileDeliveryTable> _fdt;
      // FDT instance currently being reassembled at TOI 0 (0xFFFFFFFF = none).
      // Used to discard a partial FDT object when a newer instance starts
      // arriving, so two instances never splice into one corrupt buffer.
      uint32_t _fdt_in_progress_instance_id = 0xFFFFFFFF;
      std::map<uint64_t, std::shared_ptr<LibFlute::File>> _files;
      std::mutex _files_mutex;
      /** The session's source address where the caller named one, parsed once at construction so
       *  the per-packet check costs no parsing. Empty for an any-source session. */
      std::optional<boost::asio::ip::address> _expected_source;

      std::string _mcast_address;
      unsigned short _mcast_port;

      completion_callback_t _completion_cb = nullptr;
      close_notification_callback_t _close_cb = nullptr;

      bool _running = true;

      // See ~Receiver()'s comment. Copied into each async_receive_from completion handler;
      // outlives `this` if the receiver is destroyed while a read is in flight.
      std::shared_ptr<std::atomic<bool>> _alive = std::make_shared<std::atomic<bool>>(true);
  };
};
