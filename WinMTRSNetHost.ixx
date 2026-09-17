/*
WinMTR
Copyright (C)  2010-2019 Appnor MSP S.A. - http://www.appnor.com
Copyright (C) 2019-2022 Leetsoftwerx

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
#include <WinSock2.h>
#include <ws2ipdef.h>
export module WinMTRSNetHost;

import WinMTRIPUtils;
import <string>;
import <cmath>;


export struct s_nethost final {
	SOCKADDR_INET addr = {};
	std::wstring name;
	int xmit = 0;			// number of PING packets sent
	int returned = 0;		// number of ICMP echo replies received
	unsigned long total = 0;	// total time
	unsigned long long total_squared = 0; // sum of squared response times
	unsigned long long jitter_total = 0;  // sum of consecutive response differences
	int last = 0;				// last time
	int best = 0;				// best time
	int worst = 0;			// worst time
	[[nodiscard]]
	inline auto getPercent() const noexcept {
		return (xmit == 0) ? 0 : (100 - (100 * returned / xmit));
	}
	[[nodiscard]]
	inline int getAvg() const noexcept {
		return returned == 0 ? 0 : total / returned;
	}
	[[nodiscard]]
	inline double getJitter() const noexcept {
		return returned < 2 ? 0.0 : static_cast<double>(jitter_total) / (returned - 1);
	}
	[[nodiscard]]
	inline double getStdDev() const noexcept {
		if (returned == 0) return 0.0;
		const auto mean = static_cast<double>(total) / returned;
		const auto variance = static_cast<double>(total_squared) / returned - mean * mean;
		return std::sqrt(variance > 0.0 ? variance : 0.0);
	}
	[[nodiscard]]
	auto getName() const -> std::wstring {
		if (name.empty()) {
			return addr_to_string(addr);
		}
		return name;
	}
};
