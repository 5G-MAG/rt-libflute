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
#include <stdexcept>
#include <string>
#include <cstring>
#include <vector>
#include <iostream>
#include "spdlog/spdlog.h"
#include <netlink/netlink.h>
#include <netlink/attr.h>
#include <netlink/msg.h>
#include <linux/rtnetlink.h>
#include <linux/in.h>
#include <linux/xfrm.h>
#include <linux/ipsec.h>
#include <arpa/inet.h>
#include "IpSec.h"
#include <boost/algorithm/hex.hpp>

/* The one-shot SHA256() API is deprecated in later OpenSSL versions. Transmitter.cpp uses the
   equally-deprecated one-shot MD5() for the same reason, so the two stay consistent. */
#define OPENSSL_SUPPRESS_DEPRECATED 1
#include <openssl/sha.h>

namespace LibFlute::IpSec {
  namespace {
    // A colon unambiguously identifies IPv6 text notation (dotted-decimal IPv4 never contains
    // one) -- avoids pulling in a full address-parsing library just to pick a netlink family.
    bool is_ipv6_address(const std::string& address) {
      return address.find(':') != std::string::npos;
    }

    // Fills in an xfrm_address_t's a4 (IPv4) or a6 (IPv6) member, and returns the address
    // family/prefix length pair the caller should set alongside it -- the two are always used
    // together (sel.family+sel.daddr, tmpl.family+tmpl.id.daddr, xsinfo.family+xsinfo.id.daddr),
    // so keeping the parse and the family selection in one place avoids them silently drifting
    // apart if only one were updated in a future edit.
    struct addr_family_info { int family; uint8_t prefixlen; };
    addr_family_info fill_xfrm_address(xfrm_address_t& addr, const std::string& text_address) {
      if (is_ipv6_address(text_address)) {
        if (inet_pton(AF_INET6, text_address.c_str(), &addr.a6) != 1) {
          throw std::runtime_error("Invalid IPv6 address: " + text_address);
        }
        return {AF_INET6, 128};
      }
      addr.a4 = inet_addr(text_address.c_str());
      return {AF_INET, 32};
    }
  }

  void configure_policy(uint32_t spi, const std::string& dest_address, unsigned short dest_port,
                        Direction direction)
  {
    struct nl_sock *sk;
    struct nl_msg *msg;

    struct xfrm_userpolicy_info	xpinfo = {};
    xpinfo.lft.soft_byte_limit = XFRM_INF;
    xpinfo.lft.hard_byte_limit = XFRM_INF;
    xpinfo.lft.soft_packet_limit = XFRM_INF;
    xpinfo.lft.hard_packet_limit = XFRM_INF;
    xpinfo.dir = (direction == Direction::In) ? XFRM_POLICY_IN : XFRM_POLICY_OUT;

    // sel.saddr is left all-zero (INADDR_ANY for v4, "::" for v6 -- the same all-zero
    // xfrm_address_t union represents both) regardless of family: this policy selector matches
    // any source address, only the destination is pinned.
    auto dest_info = fill_xfrm_address(xpinfo.sel.daddr, dest_address);
    xpinfo.sel.family = dest_info.family;
    xpinfo.sel.prefixlen_d = dest_info.prefixlen;

    /* The selector names the protocol and the session's own UDP port, not the destination address
       alone. Without them the policy captures every datagram to that group whatever it is for, so
       another session sharing the group on a different port, or any other protocol addressed
       there, would be pushed through this association too.

       RFC 5775 clause 5.1.1:
       "The sender IPsec SPD entry MUST be configured to process outbound packets to the
       destination address and UDP port number of the applicable ALC session."

       RFC 5775 clause 5.1.2.1:
       "The implementation MUST be able to use the source address, destination address, protocol
       (UDP), and UDP port numbers as selectors in the SPD."

       Brought into FLUTE by the version 2 specification.
       RFC 6726 clause 7.5:
       "Since FLUTE relies on ALC/LCT, it inherits the "baseline secure ALC operation" of
       [RFC5775]."

       The source port is deliberately not selected on. A sender's source port is not part of the
       session description, so pinning it would exclude legitimate traffic; the destination pair is
       what identifies the session's channel. */
    xpinfo.sel.proto = IPPROTO_UDP;
    xpinfo.sel.dport = htons(dest_port);
    xpinfo.sel.dport_mask = 0xFFFF;

    struct xfrm_user_tmpl tmpl = {};
    fill_xfrm_address(tmpl.id.daddr, dest_address);
    tmpl.id.spi = htonl(spi);
    tmpl.id.proto = IPPROTO_ESP;
    // tmpl.saddr left all-zero, same reasoning as sel.saddr above.
    tmpl.reqid = spi;
    tmpl.mode = XFRM_MODE_TRANSPORT;
    tmpl.aalgos = (~(__u32)0);
    tmpl.ealgos = (~(__u32)0);
    tmpl.calgos = (~(__u32)0);
    tmpl.family = dest_info.family;

    msg = nlmsg_alloc_simple(XFRM_MSG_UPDPOLICY, 0);
    nlmsg_append(msg, &xpinfo, sizeof(xpinfo), NLMSG_ALIGNTO);
    nla_put(msg, XFRMA_TMPL, sizeof(tmpl), &tmpl);

    sk = nl_socket_alloc();
    nl_connect(sk, NETLINK_XFRM);
    nl_send_auto(sk, msg);
    nlmsg_free(msg);
    /* Without this the netlink socket and its fd leak on every call. */
    nl_socket_free(sk);
  }
  void configure_state(uint32_t spi, const std::string& dest_address, Direction direction, const std::string& key,
                        const std::string& auth_key)
  {
    struct nl_sock *sk;
    struct nl_msg *msg;

    struct xfrm_usersa_info	xsinfo = {};

    // sel.saddr and (further below) xsinfo.saddr are left all-zero -- same reasoning as
    // configure_policy() above, this SA's own source isn't pinned to a specific address.
    auto dest_info = fill_xfrm_address(xsinfo.sel.daddr, dest_address);
    xsinfo.sel.family = dest_info.family;
    xsinfo.sel.prefixlen_d = dest_info.prefixlen;

    fill_xfrm_address(xsinfo.id.daddr, dest_address);
    xsinfo.id.spi = htonl(spi);
    xsinfo.id.proto = IPPROTO_ESP;

    xsinfo.lft.soft_byte_limit = XFRM_INF;
    xsinfo.lft.hard_byte_limit = XFRM_INF;
    xsinfo.lft.soft_packet_limit = XFRM_INF;
    xsinfo.lft.hard_packet_limit = XFRM_INF;

    xsinfo.reqid = spi;
    xsinfo.family = dest_info.family;
    xsinfo.mode = XFRM_MODE_TRANSPORT;

    /* RFC 6726 clause 7.5 makes IPsec/ESP in transport mode the mandatory-to-implement security
       configuration for FLUTE, and takes its service set from ALC: "[RFC5775] specifies that the
       data origin authentication, content integrity, and anti-replay services SHALL be supported,
       and that the confidentiality service is RECOMMENDED." The authentication algorithm attached
       below covers the first two. Anti-replay is a property of the association rather than of an
       algorithm, and needs a non-zero window; left at zero the kernel accepts a replayed packet.
       32 is the largest window this attribute can express: the legacy replay state the kernel
       keeps for it (struct xfrm_replay_state) holds its bitmap in a __u32, and a request for more
       is clamped to 32. A larger window needs the extended sequence-number attribute
       (XFRMA_REPLAY_ESN_VAL) instead, which is not needed here. Applies to both FLUTE versions:
       nothing in RFC 3926 argues against it. */
    xsinfo.replay_window = 32;

    std::vector<char> algo_buf(sizeof(struct xfrm_algo) + 512, 0);
    auto* algo = reinterpret_cast<struct xfrm_algo*>(algo_buf.data());

    std::vector<char> binary_key;
    for (unsigned int i = 0; i < key.length(); i += 2) {
      binary_key.emplace_back((char)strtol(key.substr(i, 2).c_str(), nullptr, 16));
    }
    if (binary_key.size() > 512) {
      throw std::runtime_error("Key is too long");
    }
    strcpy(algo->alg_name, "aes");
    algo->alg_key_len = binary_key.size() * 8;
    memcpy(algo->alg_key, &binary_key[0], binary_key.size());

    /* RFC 4303 clause 1: "Using encryption without a strong integrity mechanism on top of it
       (either in ESP or separately via AH) may render the confidentiality service insecure against
       some forms of active attacks". The same clause makes confidentiality with integrity a MUST
       for an ESP implementation and confidentiality alone a MAY, and RFC 3926 clause 7 recommends
       packet level authentication for a FLUTE session, which this is the only means of providing.
       So HMAC-SHA256 is attached alongside the AES encryption above. Neither document names an
       algorithm; HMAC-SHA256 is an engineering choice, not a quoted requirement. */
    std::vector<char> auth_algo_buf(sizeof(struct xfrm_algo) + 512, 0);
    auto* auth_algo = reinterpret_cast<struct xfrm_algo*>(auth_algo_buf.data());

    std::vector<unsigned char> auth_key_bytes;
    if (!auth_key.empty()) {
      for (unsigned int i = 0; i < auth_key.length(); i += 2) {
        auth_key_bytes.push_back((unsigned char)strtol(auth_key.substr(i, 2).c_str(), nullptr, 16));
      }
    } else {
      /* A caller that supplies one key predates this parameter. Deriving the authentication key
         from the encryption key gives such a caller integrity protection without an API break,
         and gives the two algorithms distinct key bytes. It is weaker than two independent keys,
         which is why the parameter exists. */
      static const std::string context = "libflute-ipsec-auth-key-v1";
      std::vector<unsigned char> input(binary_key.begin(), binary_key.end());
      input.insert(input.end(), context.begin(), context.end());
      unsigned char digest[SHA256_DIGEST_LENGTH];
      SHA256(input.data(), input.size(), digest);
      auth_key_bytes.assign(digest, digest + SHA256_DIGEST_LENGTH);
    }
    if (auth_key_bytes.size() > 512) {
      throw std::runtime_error("Authentication key is too long");
    }
    strcpy(auth_algo->alg_name, "hmac(sha256)");
    auth_algo->alg_key_len = auth_key_bytes.size() * 8;
    memcpy(auth_algo->alg_key, auth_key_bytes.data(), auth_key_bytes.size());

    msg = nlmsg_alloc_simple(XFRM_MSG_NEWSA, 0);
    nlmsg_append(msg, &xsinfo, sizeof(xsinfo), NLMSG_ALIGNTO);
    nla_put(msg, XFRMA_ALG_CRYPT, algo_buf.size(), algo);
    nla_put(msg, XFRMA_ALG_AUTH, auth_algo_buf.size(), auth_algo);

    sk = nl_socket_alloc();
    nl_connect(sk, NETLINK_XFRM);
    nl_send_auto(sk, msg);
    nlmsg_free(msg);
    /* Without this the netlink socket and its fd leak on every call. */
    nl_socket_free(sk);
  }

  void enable_esp(uint32_t spi, const std::string& dest_address, unsigned short dest_port,
                  Direction direction, const std::string& key,
                   const std::string& auth_key)
  {
    configure_state(spi, dest_address, direction, key, auth_key);
    configure_policy(spi, dest_address, dest_port, direction);
  }
};
