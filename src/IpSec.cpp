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

  void configure_policy(uint32_t spi, const std::string& dest_address, Direction direction)
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
  void configure_state(uint32_t spi, const std::string& dest_address, Direction direction, const std::string& key)
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

    msg = nlmsg_alloc_simple(XFRM_MSG_NEWSA, 0);
    nlmsg_append(msg, &xsinfo, sizeof(xsinfo), NLMSG_ALIGNTO);
    nla_put(msg, XFRMA_ALG_CRYPT, algo_buf.size(), algo);

    sk = nl_socket_alloc();
    nl_connect(sk, NETLINK_XFRM);
    nl_send_auto(sk, msg);
    nlmsg_free(msg);
    /* Without this the netlink socket and its fd leak on every call. */
    nl_socket_free(sk);
  }

  void enable_esp(uint32_t spi, const std::string& dest_address, Direction direction, const std::string& key)
  {
    configure_state(spi, dest_address, direction, key);
    configure_policy(spi, dest_address, direction);
  }
};
