/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <string>

namespace KODI::NETWORK
{

std::string ResolveUplinkAddress(const std::string& spec);
bool IsDirectDestination(const std::string& host);

} /* namespace KODI::NETWORK */
