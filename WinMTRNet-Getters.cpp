/*
WinMTR
Copyright (C)  2010-2019 Appnor MSP S.A. - http://www.appnor.com
Copyright (C) 2019-2023 Leetsoftwerx

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; version 2
of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/
module;
#pragma warning (disable : 4005)
#include "targetver.h"
#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#define NOMCX
#define NOIME
#define NOGDI
#define NONLS
#define NOAPISET
#define NOSERVICE
#define NOMINMAX
#include <winsock2.h>
module WinMTR.Net:Getters;

import <cstring>;
import <vector>;
import <iterator>;
import <mutex>;
import <algorithm>;
import WinMTRSNetHost;
import WinMTRIPUtils;
import :ClassDef;

[[nodiscard]]
inline bool operator==(const SOCKADDR_INET& lhs, const SOCKADDR_INET& rhs) noexcept {
	return std::memcmp(&lhs, &rhs, sizeof(SOCKADDR_INET)) == 0;
}


[[nodiscard]]
std::vector<s_nethost> WinMTRNet::getCurrentState() const
{
	std::unique_lock lock(ghMutex);
	auto max = GetMax();
	auto end = std::cbegin(host);
	std::advance(end, max);
	return std::vector<s_nethost>(std::cbegin(host), end);
}

[[nodiscard]]
std::vector<std::vector<s_nethost>> WinMTRNet::getCurrentRoutes() const
{
	std::unique_lock lock(ghMutex);
	auto max = GetMax();
	auto end = std::cbegin(routes);
	std::advance(end, max);
	return std::vector<std::vector<s_nethost>>(std::cbegin(routes), end);
}

[[nodiscard]]
int WinMTRNet::GetMax() const
{
	std::unique_lock lock(ghMutex);
	const int configuredMax = std::clamp(static_cast<int>(options->getMaxHops()), 1, MAX_HOPS);
	int max = configuredMax;
	bool targetFound = false;

	// first match: traced address responds on ping requests, and the address is in the hosts list
	for (int i = 1; const auto & h : host) {
		if (i > configuredMax) break;
		const auto& paths = routes[i - 1];
		if (h.addr == last_remote_addr || std::ranges::any_of(paths, [this](const s_nethost& path) {
			return path.addr == last_remote_addr;
		})) {
			max = i;
			targetFound = true;
			break;
		}
		++i;
	}

	// second match:  traced address doesn't responds on ping requests
	if (!targetFound) {
		while ((max > 1) && (host[max - 1].addr == host[max - 2].addr && isValidAddress(host[max - 1].addr))) max--;
	}
	return max;
}
