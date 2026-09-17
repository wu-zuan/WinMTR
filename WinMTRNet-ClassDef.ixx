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
#include <ws2ipdef.h>
export module WinMTR.Net:ClassDef;

import <optional>;
import <atomic>;
import <array>;
import <vector>;
import <algorithm>;
import <mutex>;
import <memory>;
import <stop_token>;
import <winrt/base.h>;
import <winrt/Windows.Foundation.h>;
import WinMTRSNetHost;
import WinMTRIPUtils;
import WinMTROptionsProvider;
import winmtr.helper;

struct trace_thread;

//*****************************************************************************
// CLASS:  WinMTRNet
//
//
//*****************************************************************************
export class WinMTRNet final : public std::enable_shared_from_this<WinMTRNet> {
	WinMTRNet(const WinMTRNet&) = delete;
	WinMTRNet& operator=(const WinMTRNet&) = delete;
public:

	WinMTRNet(const IWinMTROptionsProvider* wp)
		:host(),
		last_remote_addr(),
		options(wp),
		wsaHelper(MAKEWORD(2, 2)),
		tracing() {

		if (!wsaHelper) [[unlikely]] {
			//AfxMessageBox(IDP_SOCKETS_INIT_FAILED);
			return;
		}
	}
	~WinMTRNet() noexcept = default;


	[[nodiscard("The task should be awaited")]]
	winrt::Windows::Foundation::IAsyncAction	DoTrace(std::stop_token stop_token, SOCKADDR_INET address);

	void	ResetHops() noexcept
	{
		std::unique_lock lock(ghMutex);
		for (auto& h : this->host) {
			h = s_nethost();
		}
		for (auto& paths : this->routes) {
			paths.clear();
		}
	}
	[[nodiscard]]
	int		GetMax() const;

	[[nodiscard]]
	std::vector<s_nethost> getCurrentState() const;
	std::vector<std::vector<s_nethost>> getCurrentRoutes() const;
	s_nethost getStateAt(int at) const
	{
		std::unique_lock lock(ghMutex);
		return host[at];
	}

	static constexpr auto MAX_HOPS = 30;
private:
	static constexpr auto MAX_ROUTE_PATHS = 128;
	std::array<s_nethost, WinMTRNet::MAX_HOPS>	host;
	std::array<std::vector<s_nethost>, WinMTRNet::MAX_HOPS>	routes;
	SOCKADDR_INET last_remote_addr;
	mutable std::recursive_mutex	ghMutex;
	std::optional<winrt::Windows::Foundation::IAsyncAction> tracer;
	std::optional<winrt::apartment_context> context;
	const IWinMTROptionsProvider* options;
	winmtr::helper::WSAHelper wsaHelper;
	std::atomic_bool	tracing;

	[[nodiscard]]
	SOCKADDR_INET GetAddr(int at) const
	{
		std::unique_lock lock(ghMutex);
		return host[at].addr;
	}
	winrt::fire_and_forget	ResolveRouteName(int at, SOCKADDR_INET addr);
	void	SetName(int at, std::wstring n)
	{
		std::unique_lock lock(ghMutex);
		host[at].name = std::move(n);
	}

	bool addNewReturn(int at, int last, SOCKADDR_INET addr)
	{
		std::unique_lock lock(ghMutex);
		auto updateStatistics = [last](s_nethost& target) {
			if (target.returned > 0) {
				const auto difference = last > target.last ? last - target.last : target.last - last;
				target.jitter_total += static_cast<unsigned long long>(difference);
			}
			target.last = last;
			target.total += last;
			target.total_squared += static_cast<unsigned long long>(last) * last;
			if (target.best > last || target.returned == 0) target.best = last;
			if (target.worst < last) target.worst = last;
			target.returned++;
		};

		const auto address = addr_to_string(addr);
		const bool currentAddressChanged = addr_to_string(host[at].addr) != address;
		if (currentAddressChanged) {
			host[at].addr = addr;
			host[at].name.clear();
		}
		updateStatistics(host[at]);

		auto& paths = routes[at];
		auto found = std::ranges::find_if(paths, [&address](const s_nethost& path) {
			return addr_to_string(path.addr) == address;
		});
		const bool isNewRoute = found == paths.end();
		if (isNewRoute && paths.size() < MAX_ROUTE_PATHS) {
			s_nethost path;
			path.addr = addr;
			paths.push_back(std::move(path));
			found = paths.end() - 1;
		}
		if (found != paths.end()) {
			updateStatistics(*found);
			if (currentAddressChanged && !found->name.empty()) host[at].name = found->name;
		}
		return isNewRoute || (currentAddressChanged && host[at].name.empty());
	}
	void	AddXmit(int at)
	{
		std::unique_lock lock(ghMutex);
		host[at].xmit++;
	}

	template<class T>
	[[nodiscard("The task should be awaited")]]
	winrt::Windows::Foundation::IAsyncAction handleICMP(T remote_addr, std::stop_token stop_token, trace_thread& current);
};
