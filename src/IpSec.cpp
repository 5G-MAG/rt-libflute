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

    xpinfo.sel.family = AF_INET;
    xpinfo.sel.saddr.a4 = INADDR_ANY;
    xpinfo.sel.daddr.a4 = inet_addr(dest_address.c_str());
    xpinfo.sel.prefixlen_d = 32;

    struct xfrm_user_tmpl tmpl = {};
    tmpl.id.daddr.a4 = inet_addr(dest_address.c_str());
    tmpl.id.spi = htonl(spi);
    tmpl.id.proto = IPPROTO_ESP;
    tmpl.saddr.a4 = INADDR_ANY;
    tmpl.reqid = spi;
    tmpl.mode = XFRM_MODE_TRANSPORT;
    tmpl.aalgos = (~(__u32)0);
    tmpl.ealgos = (~(__u32)0);
    tmpl.calgos = (~(__u32)0);
    tmpl.family = AF_INET;

    msg = nlmsg_alloc_simple(XFRM_MSG_UPDPOLICY, 0);
    nlmsg_append(msg, &xpinfo, sizeof(xpinfo), NLMSG_ALIGNTO);
    nla_put(msg, XFRMA_TMPL, sizeof(tmpl), &tmpl);

    sk = nl_socket_alloc();
    nl_connect(sk, NETLINK_XFRM);
    nl_send_auto(sk, msg);
    nlmsg_free(msg);
  }
  void configure_state(uint32_t spi, const std::string& dest_address, Direction direction, const std::string& key)
  {
    struct nl_sock *sk;
    struct nl_msg *msg;

    struct xfrm_usersa_info	xsinfo = {};

    xsinfo.sel.family = AF_INET;
    xsinfo.sel.saddr.a4 = INADDR_ANY;
    xsinfo.sel.daddr.a4 = inet_addr(dest_address.c_str());
    xsinfo.sel.prefixlen_d = 32;
    
    xsinfo.id.daddr.a4 = inet_addr(dest_address.c_str());
    xsinfo.id.spi = htonl(spi);
    xsinfo.id.proto = IPPROTO_ESP;
    
    xsinfo.saddr.a4 = INADDR_ANY;

    xsinfo.lft.soft_byte_limit = XFRM_INF;
    xsinfo.lft.hard_byte_limit = XFRM_INF;
    xsinfo.lft.soft_packet_limit = XFRM_INF;
    xsinfo.lft.hard_packet_limit = XFRM_INF;

    xsinfo.reqid = spi;
    xsinfo.family = AF_INET;
    xsinfo.mode = XFRM_MODE_TRANSPORT;

    struct {
      char buf[512];
      struct xfrm_algo xa;
    } algo = {};

    std::vector<char> binary_key;
    for (unsigned int i = 0; i < key.length(); i += 2) {
      binary_key.emplace_back((char)strtol(key.substr(i, 2).c_str(), nullptr, 16));
    }
    if (binary_key.size() > 512) {
      throw "Key is too long";
    }
    strcpy(algo.xa.alg_name, "aes");
    algo.xa.alg_key_len = binary_key.size() * 8;
    memcpy(algo.buf, &binary_key[0], binary_key.size());

    msg = nlmsg_alloc_simple(XFRM_MSG_NEWSA, 0);
    nlmsg_append(msg, &xsinfo, sizeof(xsinfo), NLMSG_ALIGNTO);
    nla_put(msg, XFRMA_ALG_CRYPT, sizeof(algo), &algo);

    sk = nl_socket_alloc();
    nl_connect(sk, NETLINK_XFRM);
    nl_send_auto(sk, msg);
    nlmsg_free(msg);
  }

  void enable_esp(uint32_t spi, const std::string& dest_address, Direction direction, const std::string& key)
  {
    configure_state(spi, dest_address, direction, key);
    configure_policy(spi, dest_address, direction);
  }
};
