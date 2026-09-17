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
#include <afx.h>
#include <afxext.h>
#include <afxdisp.h>
#include <winhttp.h>
#ifndef _AFX_NO_AFXCMN_SUPPORT
#include <afxcmn.h>
#endif 
#include "resource.h"
#pragma comment(lib, "winhttp.lib")
module WinMTR.Dialog:display;
import :ClassDef;
import <format>;
import <string>;
import <string_view>;
import <vector>;
import <thread>;
import <memory>;
import <mutex>;
import <unordered_map>;
import <unordered_set>;
import <algorithm>;

import WinMTRVerUtil;
import WinMTRIPUtils;
import WinMTRUtils;

using namespace std::literals;

namespace {
	constexpr UINT WM_PUBLIC_NETWORK_INFO = WM_APP + 42;
	constexpr UINT WM_HOP_NETWORK_INFO = WM_APP + 43;

	struct public_network_info {
		std::wstring ip;
		std::wstring country;
		std::wstring city;
		std::wstring asn;
		std::wstring hostname;
		std::wstring isp;
		bool success = false;
	};

	std::mutex hop_info_mutex;
	std::unordered_map<std::wstring, public_network_info> hop_info_cache;
	std::unordered_set<std::wstring> hop_info_pending;

	struct winhttp_handle {
		HINTERNET value = nullptr;
		~winhttp_handle() { if (value) WinHttpCloseHandle(value); }
		operator HINTERNET() const noexcept { return value; }
	};

	std::wstring utf8_to_wide(const std::string& input)
	{
		if (input.empty()) return {};
		const auto length = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
		std::wstring output(length, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), length);
		return output;
	}

	std::wstring json_string(const std::wstring& json, std::wstring_view key)
	{
		const auto marker = std::wstring(L"\"") + std::wstring(key) + L"\"";
		auto position = json.find(marker);
		if (position == std::wstring::npos) return {};
		position = json.find(L':', position + marker.size());
		position = json.find(L'\"', position + 1);
		if (position == std::wstring::npos) return {};
		const auto end = json.find(L'\"', position + 1);
		return end == std::wstring::npos ? std::wstring{} : json.substr(position + 1, end - position - 1);
	}

	std::wstring json_number(const std::wstring& json, std::wstring_view key)
	{
		const auto marker = std::wstring(L"\"") + std::wstring(key) + L"\"";
		auto position = json.find(marker);
		if (position == std::wstring::npos) return {};
		position = json.find(L':', position + marker.size());
		if (position == std::wstring::npos) return {};
		position = json.find_first_of(L"0123456789", position + 1);
		const auto end = json.find_first_not_of(L"0123456789", position);
		return position == std::wstring::npos ? std::wstring{} : json.substr(position, end - position);
	}

	std::wstring traditional_country_name(std::wstring countryCode)
	{
		if (countryCode == L"TW") return L"台灣";
		if (countryCode.empty()) return {};
		wchar_t name[128]{};
		if (GetGeoInfoEx(countryCode.data(), GEO_FRIENDLYNAME, name, static_cast<int>(std::size(name))) > 0) {
			return name;
		}
		return countryCode;
	}

	public_network_info query_ip_info(std::wstring path = L"/json")
	{
		public_network_info info;
		winhttp_handle session{ WinHttpOpen(L"DiamondHost-WinMTR/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0) };
		if (!session) return info;
		WinHttpSetTimeouts(session, 4000, 4000, 4000, 4000);
		winhttp_handle connection{ WinHttpConnect(session, L"ipinfo.io", INTERNET_DEFAULT_HTTPS_PORT, 0) };
		if (!connection) return info;
		winhttp_handle request{ WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) };
		if (!request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
			WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request, nullptr)) return info;

		std::string response;
		for (;;) {
			DWORD available = 0;
			if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
			std::vector<char> buffer(available);
			DWORD read = 0;
			if (!WinHttpReadData(request, buffer.data(), available, &read)) return info;
			response.append(buffer.data(), read);
		}
		const auto json = utf8_to_wide(response);
		info.ip = json_string(json, L"ip");
		info.country = traditional_country_name(json_string(json, L"country"));
		info.city = json_string(json, L"city");
		info.hostname = json_string(json, L"hostname");
		const auto organization = json_string(json, L"org");
		const auto separator = organization.find(L' ');
		if (organization.starts_with(L"AS") && separator != std::wstring::npos) {
			info.asn = organization.substr(2, separator - 2);
			info.isp = organization.substr(separator + 1);
		}
		else {
			info.isp = organization;
		}
		info.success = !info.ip.empty();
		return info;
	}
	constexpr auto DEFAULT_PING_SIZE = 64;
	constexpr auto DEFAULT_INTERVAL = 1.0;
	constexpr auto DEFAULT_MAX_LRU = 128;
	constexpr auto DEFAULT_DNS = true;

#define MTR_NR_COLS 14
	constexpr wchar_t MTR_COLS[MTR_NR_COLS][10] = {
		L"主機",
		L"跳",
		L"丟包",
		L"已送",
		L"已收",
		L"最佳",
		L"平均",
		L"最差",
		L"最近",
		L"抖動",
		L"標準差",
		L"國家",
		L"ASN",
		L"ISP"
	};

	constexpr int MTR_COL_LENGTH[MTR_NR_COLS] = {
			255, 38, 55, 50, 50, 55, 55, 55, 55, 58, 65, 65, 65, 190
	};
	constexpr auto WINMTR_DIALOG_TIMER = 100;

}

//*****************************************************************************
// BEGIN_MESSAGE_MAP
//
// 
//*****************************************************************************
BEGIN_MESSAGE_MAP(WinMTRDialog, CDialog)
	ON_WM_PAINT()
	ON_WM_SIZE()
	ON_WM_SIZING()
	ON_WM_QUERYDRAGICON()
	ON_BN_CLICKED(ID_RESTART, OnRestart)
	ON_BN_CLICKED(ID_OPTIONS, OnOptions)
	ON_BN_CLICKED(ID_RESET_STATS, OnResetStats)
	ON_BN_CLICKED(ID_SCREENSHOT, OnScreenshot)
	ON_MESSAGE(WM_PUBLIC_NETWORK_INFO, OnPublicNetworkInfo)
	ON_MESSAGE(WM_HOP_NETWORK_INFO, OnHopNetworkInfo)
	ON_CBN_SELCHANGE(IDC_COMBO_HOST, &WinMTRDialog::OnCbnSelchangeComboHost)
	ON_CBN_SELENDOK(IDC_COMBO_HOST, &WinMTRDialog::OnCbnSelendokComboHost)
	ON_CBN_CLOSEUP(IDC_COMBO_HOST, &WinMTRDialog::OnCbnCloseupComboHost)
	ON_WM_TIMER()
	ON_WM_CLOSE()
	ON_BN_CLICKED(IDCANCEL, &WinMTRDialog::OnBnClickedCancel)
END_MESSAGE_MAP()


//*****************************************************************************
// WinMTRDialog::WinMTRDialog
//
// 
//*****************************************************************************
WinMTRDialog::WinMTRDialog(CWnd* pParent) noexcept
	: CDialog(WinMTRDialog::IDD, pParent),
	interval(DEFAULT_INTERVAL),
	state(STATES::IDLE),
	transition(STATE_TRANSITIONS::IDLE_TO_IDLE),
	pingsize(DEFAULT_PING_SIZE),
	maxLRU(DEFAULT_MAX_LRU),
	useDNS(DEFAULT_DNS)

{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);

	wmtrnet = std::make_shared<WinMTRNet>(this);
}

//*****************************************************************************
// WinMTRDialog::DoDataExchange
//
// 
//*****************************************************************************
void WinMTRDialog::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	DDX_Control(pDX, ID_OPTIONS, m_buttonOptions);
	DDX_Control(pDX, IDCANCEL, m_buttonExit);
	DDX_Control(pDX, ID_RESTART, m_buttonStart);
	DDX_Control(pDX, IDC_COMBO_HOST, m_comboHost);
	DDX_Control(pDX, IDC_LIST_MTR, m_listMTR);
	DDX_Control(pDX, IDC_STATICS, m_staticS);
	DDX_Control(pDX, IDC_STATICJ, m_staticJ);
}


//*****************************************************************************
// WinMTRDialog::OnInitDialog
//
// 
//*****************************************************************************
BOOL WinMTRDialog::OnInitDialog()
{
	CDialog::OnInitDialog();
	const auto verNumber = WinMTRVerUtil::getExeVersion();
#ifndef  _WIN64
	constexpr auto bitness = 32;
#else
	constexpr auto bitness = 64;
#endif
	const auto caption = std::format(L"DiamondHost WinMTR v{}（{} 位元）"sv, verNumber, bitness);
	SetTimer(1, WINMTR_DIALOG_TIMER, nullptr);
	SetWindowTextW(caption.c_str());

	SetIcon(m_hIcon, TRUE);
	SetIcon(m_hIcon, FALSE);

	if (!statusBar.Create(this))
		AfxMessageBox(L"無法建立狀態列。", MB_ICONERROR);
	statusBar.GetStatusBarCtrl().SetMinHeight(23);

	UINT sbi[1] = { IDS_STRING_SB_NAME };
	statusBar.SetIndicators(sbi);
	statusBar.SetPaneInfo(0, statusBar.GetItemID(0), SBPS_STRETCH, 0);
	// removing for now but leaving commenteded this goes to a domain buying site, so either they lost the domain or are
	// out of business. I'll fix this when that changes.
	//{ // Add appnor URL
	//	//std::unique_ptr<CMFCLinkCtrl> m_pWndButton = std::make_unique<CMFCLinkCtrl>();
	//	if (!m_pWndButton.Create(_T("www.appnor.com"), WS_CHILD|WS_VISIBLE|WS_TABSTOP, CRect(0,0,0,0), &statusBar, 1234)) {
	//		TRACE(_T("Failed to create button control.\n"));
	//		return FALSE;
	//	}

	//	m_pWndButton.SetURL(L"http://www.appnor.com/?utm_source=winmtr&utm_medium=desktop&utm_campaign=software");
	//		
	//	if(!statusBar.AddPane(1234,1)) {
	//		AfxMessageBox(_T("Pane index out of range\nor pane with same ID already exists in the status bar"), MB_ICONERROR);
	//		return FALSE;
	//	}
	//		
	//	statusBar.SetPaneWidth(statusBar.CommandToIndex(1234), 100);
	//	statusBar.AddPaneControl(&m_pWndButton, 1234, true);
	//}

	for (int i = 0; i < MTR_NR_COLS; i++) {
		m_listMTR.InsertColumn(i, MTR_COLS[i], LVCFMT_LEFT, MTR_COL_LENGTH[i], -1);
	}
	m_listMTR.SetExtendedStyle(m_listMTR.GetExtendedStyle() | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
	if (m_mtrFont.CreatePointFont(90, L"Consolas")) {
		m_listMTR.SetFont(&m_mtrFont);
	}

	m_comboHost.SetFocus();

	// We need to resize the dialog to make room for control bars.
	// First, figure out how big the control bars are.
	CRect rcClientStart;
	CRect rcClientNow;
	GetClientRect(rcClientStart);
	RepositionBars(AFX_IDW_CONTROLBAR_FIRST, AFX_IDW_CONTROLBAR_LAST,
		0, reposQuery, rcClientNow);

	// Now move all the controls so they are in the same relative
	// position within the remaining client area as they would be
	// with no control bars.
	//CPoint ptOffset(rcClientNow.left - rcClientStart.left,
					//rcClientNow.top - rcClientStart.top);
	const auto ptOffset = rcClientNow.TopLeft() - rcClientStart.TopLeft();

	CRect  rcChild;
	CWnd* pwndChild = GetWindow(GW_CHILD);
	while (pwndChild)
	{
		pwndChild->GetWindowRect(rcChild);
		ScreenToClient(rcChild);
		rcChild.OffsetRect(ptOffset);
		pwndChild->MoveWindow(rcChild, FALSE);
		pwndChild = pwndChild->GetNextWindow();
	}

	// Adjust the dialog window dimensions
	CRect rcWindow;
	GetWindowRect(rcWindow);
	rcWindow.right += rcClientStart.Width() - rcClientNow.Width();
	rcWindow.bottom += rcClientStart.Height() - rcClientNow.Height();
	MoveWindow(rcWindow, FALSE);

	// And position the control bars
	RepositionBars(AFX_IDW_CONTROLBAR_FIRST, AFX_IDW_CONTROLBAR_LAST, 0);

	// Set the horizontal layout once at startup. During tracing only the
	// height is allowed to respond to changing route rows.
	CRect startupClient;
	GetClientRect(&startupClient);
	CRect startupList;
	m_listMTR.GetWindowRect(&startupList);
	ScreenToClient(&startupList);
	int columnsWidth = 0;
	for (int column = 0; column < MTR_NR_COLS; ++column) columnsWidth += m_listMTR.GetColumnWidth(column);
	RECT desiredStartup{ 0, 0, startupList.left + columnsWidth + 28, startupClient.Height() };
	const auto dpi = GetDpiForWindow(m_hWnd);
	AdjustWindowRectExForDpi(&desiredStartup, static_cast<DWORD>(GetStyle()), FALSE,
		static_cast<DWORD>(GetExStyle()), dpi);
	CRect startupWindow;
	GetWindowRect(&startupWindow);
	MONITORINFO startupMonitor{ sizeof(startupMonitor) };
	GetMonitorInfoW(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST), &startupMonitor);
	const CRect startupWorkArea(startupMonitor.rcWork);
	const auto requestedStartupWidth = desiredStartup.right - desiredStartup.left;
	const auto startupWidth = requestedStartupWidth < startupWorkArea.Width()
		? requestedStartupWidth : startupWorkArea.Width();
	const auto startupLeft = std::clamp(startupWindow.CenterPoint().x - startupWidth / 2,
		startupWorkArea.left, startupWorkArea.right - startupWidth);
	SetWindowPos(nullptr, startupLeft, startupWindow.top, startupWidth, startupWindow.Height(),
		SWP_NOZORDER | SWP_NOACTIVATE);

	if (startupWidth < desiredStartup.right - desiredStartup.left) {
		m_mtrFont.DeleteObject();
		if (m_mtrFont.CreatePointFont(80, L"Consolas")) m_listMTR.SetFont(&m_mtrFont);
		CRect fittedClient;
		GetClientRect(&fittedClient);
		const auto availableWidth = fittedClient.Width() - startupList.left - 28;
		for (int column = 0; column < MTR_NR_COLS; ++column) {
			const auto minimumColumnWidth = MulDiv(36, dpi, 96);
			const auto fittedColumnWidth = MulDiv(MTR_COL_LENGTH[column], availableWidth, columnsWidth);
			m_listMTR.SetColumnWidth(column,
				fittedColumnWidth > minimumColumnWidth ? fittedColumnWidth : minimumColumnWidth);
		}
	}

	InitRegistry();
	ApplyNetworkInfoOption();

	if (m_autostart) {
		m_comboHost.SetWindowText(msz_defaulthostname.c_str());
		OnRestart();
	}

	return FALSE;
}

//*****************************************************************************
// WinMTRDialog::OnSizing
//
// 
//*****************************************************************************
void WinMTRDialog::OnSizing(UINT fwSide, LPRECT pRect)
{
	CDialog::OnSizing(fwSide, pRect);

	int iWidth = (pRect->right) - (pRect->left);
	int iHeight = (pRect->bottom) - (pRect->top);

	if (iWidth < 820)
		pRect->right = pRect->left + 820;
	if (iHeight < 400)
		pRect->bottom = pRect->top + 400;
}


//*****************************************************************************
// WinMTRDialog::OnSize
//
// 
//*****************************************************************************
void WinMTRDialog::OnSize(UINT nType, int cx, int cy)
{
	CDialog::OnSize(nType, cx, cy);
	CRect r;
	GetClientRect(&r);
	CRect lb;
	if (::IsWindow(m_staticS.m_hWnd)) {
		const auto dpi = GetDpiForWindow(m_staticS.m_hWnd);
		m_staticS.GetWindowRect(&lb);
		ScreenToClient(&lb);
		const auto scaledXOffset = MulDiv(10, dpi, 96);
		m_staticS.SetWindowPos(nullptr, lb.TopLeft().x, lb.TopLeft().y, r.Width() - lb.TopLeft().x - scaledXOffset, lb.Height(), SWP_NOMOVE | SWP_NOZORDER);
	}

	if (::IsWindow(m_staticJ.m_hWnd)) {
		const auto dpi = GetDpiForWindow(m_staticJ.m_hWnd);
		m_staticJ.GetWindowRect(&lb);
		ScreenToClient(&lb);
		const auto scaledXOffset = MulDiv(21, dpi, 96);
		m_staticJ.SetWindowPos(nullptr, lb.TopLeft().x, lb.TopLeft().y, r.Width() - scaledXOffset, lb.Height(), SWP_NOMOVE | SWP_NOZORDER);
	}

	if (::IsWindow(m_buttonExit.m_hWnd)) {
		const auto dpi = GetDpiForWindow(m_buttonExit.m_hWnd);
		m_buttonExit.GetWindowRect(&lb);
		ScreenToClient(&lb);
		const auto scaledXOffset = MulDiv(21, dpi, 96);
		m_buttonExit.SetWindowPos(nullptr, r.Width() - lb.Width() - scaledXOffset, lb.TopLeft().y, lb.Width(), lb.Height(), SWP_NOSIZE | SWP_NOZORDER);
	}


	if (::IsWindow(m_listMTR.m_hWnd)) {
		const auto dpi = GetDpiForWindow(m_listMTR.m_hWnd);
		m_listMTR.GetWindowRect(&lb);
		ScreenToClient(&lb);
		const auto scaledX = MulDiv(21, dpi, 96);
		const auto scaledY = MulDiv(25, dpi, 96);
		m_listMTR.SetWindowPos(nullptr, lb.TopLeft().x, lb.TopLeft().y, r.Width() - scaledX, r.Height() - lb.top - scaledY, SWP_NOMOVE | SWP_NOZORDER);
	}

	RepositionBars(AFX_IDW_CONTROLBAR_FIRST, AFX_IDW_CONTROLBAR_LAST,
		0, reposQuery, r);

	RepositionBars(AFX_IDW_CONTROLBAR_FIRST, AFX_IDW_CONTROLBAR_LAST, 0);

}

void WinMTRDialog::AutoSizeToContent()
{
	if (!autoResizeHeight || !::IsWindow(m_listMTR.m_hWnd) || IsIconic() || IsZoomed()) return;
	const auto itemCount = m_listMTR.GetItemCount();
	if (itemCount != m_pendingAutoSizeRowCount) {
		m_pendingAutoSizeRowCount = itemCount;
	}
	if (itemCount == m_lastAutoSizeRowCount) return;
	m_lastAutoSizeRowCount = itemCount;

	const auto dpi = GetDpiForWindow(m_hWnd);
	CRect listRect;
	m_listMTR.GetWindowRect(&listRect);
	ScreenToClient(&listRect);

	int rowHeight = MulDiv(20, dpi, 96);
	if (itemCount > 0) {
		CRect itemRect;
		if (m_listMTR.GetItemRect(0, &itemRect, LVIR_BOUNDS)) rowHeight = itemRect.Height();
	}
	int headerHeight = MulDiv(24, dpi, 96);
	if (const auto* header = m_listMTR.GetHeaderCtrl(); header && ::IsWindow(header->m_hWnd)) {
		CRect headerRect;
		header->GetWindowRect(&headerRect);
		headerHeight = headerRect.Height();
	}

	const auto desiredListHeight = headerHeight + itemCount * rowHeight + MulDiv(6, dpi, 96);
	const auto desiredClientHeight = listRect.top + desiredListHeight + MulDiv(29, dpi, 96);

	CRect currentClient;
	GetClientRect(&currentClient);
	RECT desiredWindow{ 0, 0, currentClient.Width(), desiredClientHeight };
	AdjustWindowRectExForDpi(&desiredWindow, static_cast<DWORD>(GetStyle()), FALSE,
		static_cast<DWORD>(GetExStyle()), dpi);
	int desiredHeight = desiredWindow.bottom - desiredWindow.top;

	MONITORINFO monitorInfo{ sizeof(monitorInfo) };
	GetMonitorInfoW(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST), &monitorInfo);
	const CRect workArea(monitorInfo.rcWork);
	desiredHeight = std::clamp(desiredHeight, MulDiv(400, dpi, 96), workArea.Height());

	CRect currentWindow;
	GetWindowRect(&currentWindow);
	if (std::abs(currentWindow.Height() - desiredHeight) <= 2) return;

	const auto top = std::clamp(currentWindow.top, workArea.top, workArea.bottom - desiredHeight);
	SetRedraw(FALSE);
	SetWindowPos(nullptr, currentWindow.left, top, currentWindow.Width(), desiredHeight,
		SWP_NOZORDER | SWP_NOACTIVATE);
	SetRedraw(TRUE);
	RedrawWindow(nullptr, nullptr,
		RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
}


//*****************************************************************************
// WinMTRDialog::OnPaint
//
// 
//*****************************************************************************
void WinMTRDialog::OnPaint()
{
	if (IsIconic())
	{
		CPaintDC dc(this);

		SendMessage(WM_ICONERASEBKGND, (WPARAM)dc.GetSafeHdc(), 0);
		const auto dpi = GetDpiForWindow(*this);
		const int cxIcon = GetSystemMetricsForDpi(SM_CXICON, dpi);
		const int cyIcon = GetSystemMetricsForDpi(SM_CYICON, dpi);
		CRect rect;
		GetClientRect(&rect);
		const int x = (rect.Width() - cxIcon + 1) / 2;
		const int y = (rect.Height() - cyIcon + 1) / 2;

		dc.DrawIcon(x, y, m_hIcon);
	}
	else
	{
		CDialog::OnPaint();
	}
}


//*****************************************************************************
// WinMTRDialog::OnQueryDragIcon
//
// 
//*****************************************************************************
HCURSOR WinMTRDialog::OnQueryDragIcon()
{
	return (HCURSOR)m_hIcon;
}


//*****************************************************************************
// WinMTRDialog::SetHostName
//
//*****************************************************************************
void WinMTRDialog::SetHostName(std::wstring host)
{
	m_autostart = true;
	msz_defaulthostname = std::move(host);
}


//*****************************************************************************
// WinMTRDialog::SetPingSize
//
//*****************************************************************************
void WinMTRDialog::SetPingSize(unsigned ps, options_source fromCmdLine) noexcept
{
	pingsize = ps;
	hasPingsizeFromCmdLine = static_cast<bool>(fromCmdLine);
}

//*****************************************************************************
// WinMTRDialog::SetMaxLRU
//
//*****************************************************************************
void WinMTRDialog::SetMaxLRU(int mlru, options_source fromCmdLine) noexcept
{
	maxLRU = mlru;
	hasMaxLRUFromCmdLine = static_cast<bool>(fromCmdLine);
}


//*****************************************************************************
// WinMTRDialog::SetInterval
//
//*****************************************************************************
void WinMTRDialog::SetInterval(float i, options_source fromCmdLine) noexcept
{
	interval = i;
	hasMaxLRUFromCmdLine = static_cast<bool>(fromCmdLine);
}

//*****************************************************************************
// WinMTRDialog::SetUseDNS
//
//*****************************************************************************
void WinMTRDialog::SetUseDNS(bool udns, options_source fromCmdLine) noexcept
{
	useDNS = udns;
	hasUseDNSFromCmdLine = static_cast<bool>(fromCmdLine);
}


//*****************************************************************************
// WinMTRDialog::WinMTRDialog
//
// 
//*****************************************************************************
void WinMTRDialog::OnCancel()
{
}


//*****************************************************************************
// WinMTRDialog::DisplayRedraw
//
// 
//*****************************************************************************
int WinMTRDialog::DisplayRedraw()
{
	wchar_t buf[255] = {};
	const auto netstate = wmtrnet->getCurrentState();
	const auto routeState = wmtrnet->getCurrentRoutes();

	static CString noResponse((LPCWSTR)IDS_STRING_NO_RESPONSE_FROM_HOST);
	int row = 0;

	auto setInteger = [&](int rowIndex, int column, int value) {
		const auto result = std::format_to_n(buf, std::size(buf) - 1, L"{}", value);
		*result.out = L'\0';
		m_listMTR.SetItem(rowIndex, column, LVIF_TEXT, buf, 0, 0, 0, 0);
	};
	auto setFloat = [&](int rowIndex, int column, double value) {
		const auto result = std::format_to_n(buf, std::size(buf) - 1, L"{:.1f}", value);
		*result.out = L'\0';
		m_listMTR.SetItem(rowIndex, column, LVIF_TEXT, buf, 0, 0, 0, 0);
	};

	auto populateNetworkInfo = [&](int rowIndex, const s_nethost& item) {
		if (!networkInfoEnabled) {
			for (int column = 11; column <= 13; ++column) {
				m_listMTR.SetItem(rowIndex, column, LVIF_TEXT, L"", 0, 0, 0, 0);
			}
			return;
		}
		const auto address = isValidAddress(item.addr) ? addr_to_string(item.addr) : std::wstring{};
		public_network_info hopInfo;
		bool hasHopInfo = false;
		bool startLookup = false;
		if (!address.empty()) {
			std::unique_lock lock(hop_info_mutex);
			if (const auto found = hop_info_cache.find(address); found != hop_info_cache.end()) {
				hopInfo = found->second;
				hasHopInfo = hopInfo.success;
			}
			else {
				startLookup = hop_info_pending.insert(address).second;
			}
		}
		m_listMTR.SetItem(rowIndex, 11, LVIF_TEXT, hasHopInfo ? hopInfo.country.c_str() : L"", 0, 0, 0, 0);
		m_listMTR.SetItem(rowIndex, 12, LVIF_TEXT, hasHopInfo ? hopInfo.asn.c_str() : L"", 0, 0, 0, 0);
		m_listMTR.SetItem(rowIndex, 13, LVIF_TEXT, hasHopInfo ? hopInfo.isp.c_str() : L"", 0, 0, 0, 0);

		if (startLookup) {
			auto window = GetSafeHwnd();
			std::thread([window, address]() {
				auto info = query_ip_info(L"/" + address + L"/json");
				{
					std::unique_lock lock(hop_info_mutex);
					hop_info_cache.insert_or_assign(address, std::move(info));
					hop_info_pending.erase(address);
				}
				::PostMessageW(window, WM_HOP_NETWORK_INFO, 0, 0);
			}).detach();
		}
	};

	auto populateRow = [&](const s_nethost& item, int hop, bool alternative) {
		auto name = item.getName();
		if (name.empty()) name = noResponse;
		const auto itemAddress = isValidAddress(item.addr) ? addr_to_string(item.addr) : std::wstring{};
		if (showIpWithHostname && !itemAddress.empty() && name != itemAddress) {
			name += L" (" + itemAddress + L")";
		}
		if (alternative) name = L"  + " + name;

		if (m_listMTR.GetItemCount() <= row) m_listMTR.InsertItem(row, name.c_str());
		else m_listMTR.SetItem(row, 0, LVIF_TEXT, name.c_str(), 0, 0, 0, 0);

		setInteger(row, 1, hop);
		if (alternative) {
			m_listMTR.SetItem(row, 2, LVIF_TEXT, L"", 0, 0, 0, 0);
			m_listMTR.SetItem(row, 3, LVIF_TEXT, L"", 0, 0, 0, 0);
		}
		else {
			const auto loss = std::format(L"{}%", item.getPercent());
			m_listMTR.SetItem(row, 2, LVIF_TEXT, loss.c_str(), 0, 0, 0, 0);
			setInteger(row, 3, item.xmit);
		}
		setInteger(row, 4, item.returned);
		setFloat(row, 5, static_cast<double>(item.best));
		setFloat(row, 6, static_cast<double>(item.getAvg()));
		setFloat(row, 7, static_cast<double>(item.worst));
		setFloat(row, 8, static_cast<double>(item.last));
		setFloat(row, 9, item.getJitter());
		setFloat(row, 10, item.getStdDev());
		populateNetworkInfo(row, item);
		++row;
	};

	for (size_t hopIndex = 0; hopIndex < netstate.size(); ++hopIndex) {
		populateRow(netstate[hopIndex], static_cast<int>(hopIndex + 1), false);
		if (hopIndex < routeState.size()) {
			const auto& paths = routeState[hopIndex];
			const auto currentAddress = addr_to_string(netstate[hopIndex].addr);
			size_t displayedPaths = 1;
			for (const auto& path : paths) {
				if (displayedPaths >= maxDisplayPaths) break;
				if (addr_to_string(path.addr) == currentAddress) continue;
				populateRow(path, static_cast<int>(hopIndex + 1), true);
				++displayedPaths;
			}
		}
	}
	while (m_listMTR.GetItemCount() > row) m_listMTR.DeleteItem(m_listMTR.GetItemCount() - 1);
	AutoSizeToContent();

	return 0;
}

void WinMTRDialog::OnCbnSelchangeComboHost()
{
}

void WinMTRDialog::OnCbnSelendokComboHost()
{
}


void WinMTRDialog::OnCbnCloseupComboHost()
{
	if (m_comboHost.GetCurSel() == m_comboHost.GetCount() - 1) {
		ClearHistory();
	}
}

void WinMTRDialog::LoadPublicNetworkInfo()
{
	auto window = GetSafeHwnd();
	std::thread([window]() {
		auto* info = new public_network_info(query_ip_info());
		if (!::PostMessageW(window, WM_PUBLIC_NETWORK_INFO, 0, reinterpret_cast<LPARAM>(info))) delete info;
	}).detach();
}

void WinMTRDialog::ApplyNetworkInfoOption()
{
	if (networkInfoEnabled) {
		for (int column = 11; column <= 13; ++column) {
			if (m_listMTR.GetColumnWidth(column) == 0) {
				m_listMTR.SetColumnWidth(column, MTR_COL_LENGTH[column]);
			}
		}
		SetDlgItemTextW(IDC_INFO_IP, L"IP：正在取得…");
		SetDlgItemTextW(IDC_INFO_COUNTRY, L"國家：—");
		SetDlgItemTextW(IDC_INFO_CITY, L"城市：—");
		SetDlgItemTextW(IDC_INFO_ASN, L"ASN：—");
		SetDlgItemTextW(IDC_INFO_HOSTNAME, L"Hostname：—");
		SetDlgItemTextW(IDC_INFO_ISP, L"ISP：—");
		LoadPublicNetworkInfo();
	}
	else {
		for (int column = 11; column <= 13; ++column) m_listMTR.SetColumnWidth(column, 0);
		SetDlgItemTextW(IDC_INFO_IP, L"IP：網路資訊查詢已關閉");
		SetDlgItemTextW(IDC_INFO_COUNTRY, L"國家：—");
		SetDlgItemTextW(IDC_INFO_CITY, L"城市：—");
		SetDlgItemTextW(IDC_INFO_ASN, L"ASN：—");
		SetDlgItemTextW(IDC_INFO_HOSTNAME, L"Hostname：—");
		SetDlgItemTextW(IDC_INFO_ISP, L"ISP：—");
	}
	DisplayRedraw();
}

LRESULT WinMTRDialog::OnHopNetworkInfo([[maybe_unused]] WPARAM wParam, [[maybe_unused]] LPARAM lParam)
{
	DisplayRedraw();
	return 0;
}

LRESULT WinMTRDialog::OnPublicNetworkInfo([[maybe_unused]] WPARAM wParam, LPARAM lParam)
{
	std::unique_ptr<public_network_info> info(reinterpret_cast<public_network_info*>(lParam));
	if (!networkInfoEnabled) return 0;
	if (!info || !info->success) {
		SetDlgItemTextW(IDC_INFO_IP, L"IP：無法取得");
		return 0;
	}
	SetDlgItemTextW(IDC_INFO_IP, (L"IP：" + info->ip).c_str());
	SetDlgItemTextW(IDC_INFO_COUNTRY, (L"國家：" + info->country).c_str());
	SetDlgItemTextW(IDC_INFO_CITY, (L"城市：" + info->city).c_str());
	SetDlgItemTextW(IDC_INFO_ASN, (L"ASN：" + info->asn).c_str());
	SetDlgItemTextW(IDC_INFO_HOSTNAME, (L"Hostname：" + info->hostname).c_str());
	SetDlgItemTextW(IDC_INFO_ISP, (L"ISP：" + info->isp).c_str());
	return 0;
}

void WinMTRDialog::OnResetStats() noexcept
{
	wmtrnet->ResetHops();
	m_listMTR.DeleteAllItems();
	m_pendingAutoSizeRowCount = -1;
	m_lastAutoSizeRowCount = -1;
	AutoSizeToContent();
	statusBar.SetPaneText(0, L"統計資料已重新開始。追蹤工作仍在繼續。");
}

void WinMTRDialog::OnScreenshot()
{
	CRect windowRect;
	GetWindowRect(&windowRect);
	CWindowDC windowDc(this);
	CDC memoryDc;
	if (!memoryDc.CreateCompatibleDC(&windowDc)) {
		AfxMessageBox(L"無法建立截圖。", MB_ICONERROR);
		return;
	}

	CBitmap bitmap;
	if (!bitmap.CreateCompatibleBitmap(&windowDc, windowRect.Width(), windowRect.Height())) {
		AfxMessageBox(L"無法建立截圖。", MB_ICONERROR);
		return;
	}

	auto* previous = memoryDc.SelectObject(&bitmap);
	const BOOL captured = PrintWindow(&memoryDc, 0x00000002);
	memoryDc.SelectObject(previous);
	if (!captured || !OpenClipboard()) {
		AfxMessageBox(L"無法將截圖複製到剪貼簿。", MB_ICONERROR);
		return;
	}

	EmptyClipboard();
	if (SetClipboardData(CF_BITMAP, bitmap.GetSafeHandle()) != nullptr) {
		bitmap.Detach();
		statusBar.SetPaneText(0, L"已將視窗截圖複製到剪貼簿。");
	}
	else {
		AfxMessageBox(L"無法將截圖複製到剪貼簿。", MB_ICONERROR);
	}
	CloseClipboard();
}
