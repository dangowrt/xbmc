/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "UplinkAddress.h"

#include "utils/URIUtils.h"
#include "utils/log.h"

#include <cstdlib>
#include <string>

#if defined(TARGET_POSIX)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace KODI::NETWORK
{

std::string ResolveUplinkAddress(const std::string& spec)
{
  if (spec.empty())
    return {};

#if !defined(TARGET_POSIX)
  return spec;
#else
  const size_t slash = spec.find('/');
  in_addr network = {};
  in_addr_t mask = 0;

  if (slash == std::string::npos)
  {
    in_addr literal = {};
    if (inet_pton(AF_INET, spec.c_str(), &literal) == 1)
      return spec;
  }
  else
  {
    const int bits = std::atoi(spec.substr(slash + 1).c_str());

    if (bits < 0 || bits > 32 ||
        inet_pton(AF_INET, spec.substr(0, slash).c_str(), &network) != 1)
    {
      CLog::LogF(LOGERROR, "malformed uplink specification '{}'", spec);
      return {};
    }

    mask = bits == 0 ? 0 : htonl(~((1u << (32 - bits)) - 1));
  }

  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0)
    return {};

  std::string address;
  for (ifaddrs* interface = interfaces; interface && address.empty();
       interface = interface->ifa_next)
  {
    if (!interface->ifa_addr || interface->ifa_addr->sa_family != AF_INET ||
        !(interface->ifa_flags & IFF_UP))
      continue;

    const in_addr& candidate = reinterpret_cast<sockaddr_in*>(interface->ifa_addr)->sin_addr;

    if (slash == std::string::npos)
    {
      if (spec != interface->ifa_name)
        continue;
    }
    else if ((candidate.s_addr & mask) != (network.s_addr & mask))
    {
      continue;
    }

    char text[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &candidate, text, sizeof(text)))
      address = text;
  }

  freeifaddrs(interfaces);

  if (address.empty())
    CLog::LogF(LOGWARNING, "no local address matches uplink '{}'", spec);

  return address;
#endif
}

bool IsDirectDestination(const std::string& host)
{
  if (host.empty())
    return true;

  return URIUtils::IsHostOnLAN(host, LanCheckMode::ANY_PRIVATE_SUBNET);
}

} /* namespace KODI::NETWORK */
