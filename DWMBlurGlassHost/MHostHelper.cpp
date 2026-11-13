/**
 * FileName: MHostHelper.cpp
 *
 * Copyright (C) 2024 Maplespe
 *
 * This file is part of MToolBox and DWMBlurGlass.
 * DWMBlurGlass is free software: you can redistribute it and/or modify it under the terms of the
 * GNU Lesser General Public License as published by the Free Software Foundation, either version 3
 * of the License, or any later version.
 *
 * DWMBlurGlass is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with Foobar.
 * If not, see <https://www.gnu.org/licenses/lgpl-3.0.html>.
*/
#include "MHostHelper.h"
#include "Helper/SymbolResolver.h"
#include "Helper/SymbolDownloader.h"
#include "Helper/OffsetCache.h"
#include "Helper/ToastNotifier.h"
#include "Helper/Helper.h"
#include "UIManager.h"
#include "Common.h"
#include "../DWMBlurGlassExt/Common/DefFunctionList.h"
#include <TlHelp32.h>
#include <optional>
#include "Helper/wil.h"

#pragma data_seg(".DWMBlurGlassShared")

DWORD64 g_hookFunOffsetList[MDWMBlurGlassExt::g_hookFunList.size()] = { 0 };

size_t g_dwmcoreFunCount = 0;
size_t g_udwmFunCount = 0;

HWND g_hostMsgWnd = nullptr;

#pragma data_seg()
#pragma comment(linker,"/SECTION:.DWMBlurGlassShared,RWS")

namespace MDWMBlurGlass
{
    SymbolResolver g_symResolver;

	std::vector<DWORD> g_proclist;
	UINT_PTR g_timerID = 0;

	// How the startup symbol download went, if one ran; tells the notification why symbols are missing.
	std::optional<SymbolDownloadOutcome> g_symbolDownload;

	extern void OnProcessStart(DWORD id);

	void RefreshProcessList()
	{
		PROCESSENTRY32W pe;
		HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		pe.dwSize = sizeof(PROCESSENTRY32W);
		if (!Process32FirstW(hSnapshot, &pe))
			return;

		std::vector<DWORD> list;

		while (Process32NextW(hSnapshot, &pe) != FALSE)
		{
			std::wstring exe = pe.szExeFile;
			std::ranges::transform(exe, exe.begin(), ::tolower);

			if(exe == L"dwm.exe")
				list.push_back(pe.th32ProcessID);
		}
		CloseHandle(hSnapshot);

		std::vector<DWORD> started;
		for (const auto& id : list)
		{
			if (std::ranges::find(g_proclist, id) == g_proclist.end())
				started.push_back(id);
		}

		// Record the list before starting anything. OnProcessStart can sit in a modal loop (the crash
		// prompt), which keeps this timer firing; with the old list still in place it would start the
		// same process again, stacking up one prompt per tick.
		g_proclist = list;
		for (const auto& id : started)
			OnProcessStart(id);
	}

	bool IsEnableTransparency()
	{
		HKEY hKey = nullptr;
		DWORD dwValue = 1;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\themes\personalize)", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
		{
			DWORD dwType = REG_DWORD;
			DWORD dwDataSize = sizeof(DWORD);

			RegQueryValueExW(hKey, L"EnableTransparency", nullptr, &dwType, (LPBYTE)&dwValue, &dwDataSize);

			RegCloseKey(hKey);
		}
		return dwValue;
	}

	HWND FindMessageWnd(DWORD pid)
	{
		if (!pid)
			return nullptr;

		HWND hwnd = nullptr;
		do
		{
			hwnd = FindWindowExW(HWND_MESSAGE, hwnd, DWMBlurGlassNotifyClassName, nullptr);
			if (hwnd != nullptr)
			{
				DWORD ProcessId = NULL;
				GetWindowThreadProcessId(hwnd, &ProcessId);
				if (ProcessId == pid)
					return hwnd;
			}
		} while (hwnd != nullptr);
		return nullptr;
	}

	void ParsingSymbol(PSYMBOL_INFO symInfo, std::string& funName, std::string& fullName, DWORD64& offset)
	{
		std::string_view functionName{ symInfo->Name, symInfo->NameLen };

		fullName = functionName;

		CHAR fullyUnDecoratedFunctionName[MAX_PATH + 1]{};
		UnDecorateSymbolName(
			functionName.data(), fullyUnDecoratedFunctionName, MAX_PATH,
			UNDNAME_NAME_ONLY
		);

		funName = fullyUnDecoratedFunctionName;
		offset = symInfo->Address - symInfo->ModBase;
	}

	bool SymCallback_Dwmcore(PSYMBOL_INFO symInfo, ULONG symbolSize)
	{
		DWORD64 offset = 0;
		std::string funName, fullName;
		ParsingSymbol(symInfo, funName, fullName, offset);

		size_t funCount = 0;
		for (size_t i = 0; i < MDWMBlurGlassExt::g_hookFunList.size(); ++i)
		{
			auto& [type, _funName] = MDWMBlurGlassExt::g_hookFunList[i];
			if (type == dwmcore && (funName == _funName || fullName == _funName))
			{
				g_hookFunOffsetList[i] = offset;
				funCount++;
			}
		}

		return 1;
	}

	bool SymCallback_uDwm(PSYMBOL_INFO symInfo, ULONG symbolSize)
	{
		DWORD64 offset = 0;
		std::string funName, fullName;
		ParsingSymbol(symInfo, funName, fullName, offset);

		size_t funCount = 0;
		for (size_t i = 0; i < MDWMBlurGlassExt::g_hookFunList.size(); ++i)
		{
			auto& [type, _funName] = MDWMBlurGlassExt::g_hookFunList[i];
			if (type == udwm && (funName == _funName || fullName == _funName))
			{
				g_hookFunOffsetList[i] = offset;
				funCount++;
			}
		}

		return g_udwmFunCount != funCount;
	}

    bool MHostGetSymbolState()
    {
		std::wstring symPath = L"SRV*" + Utils::GetCurrentDir() + L"\\data\\symbols";
		auto callback = [](PSYMBOL_INFO, ULONG) { return false; };
        HRESULT hr = g_symResolver.Walk(L"dwmcore.dll", "*!*", symPath, callback);
		if (FAILED(hr))
			return false;
		hr = g_symResolver.Walk(L"uDwm.dll", "*!*", symPath, callback);
        return SUCCEEDED(hr);
    }

	std::wstring LangOr(std::wstring_view key, std::wstring_view fallback)
	{
		std::wstring text = GetLanguageString(key);
		return text.empty() ? std::wstring(fallback) : text;
	}

	// A failed load is reported as a notification in the notification centre, not a message box:
	// it doesn't block the host, and repeats replace each other instead of piling up.
	void NotifyLoadFailure(const std::wstring& err, bool symbolsFailed)
	{
		bool shown;
		if (symbolsFailed)
		{
			std::wstring body;
			if (g_symbolDownload == SymbolDownloadOutcome::NotPublished)
				body = LangOr(L"notify_symnotpublished", L"Microsoft hasn't published symbol files for this Windows build yet. "
					L"This usually takes a few days after an update. The glass effects are paused until then.");
			else if (g_symbolDownload == SymbolDownloadOutcome::Failed)
				body = LangOr(L"notify_symdownfail", L"New symbol files couldn't be downloaded, so the glass effects are paused. "
					L"Check your connection, then download them from the Symbols page.");
			else
				body = LangOr(L"notify_symoutdated", L"Windows has been updated and the symbol files no longer match it, "
					L"so the glass effects are paused. Download new ones from the Symbols page.");

			shown = Toast::ShowStatus(LangOr(L"notify_symtitle", L"Symbol files need updating"), body,
				LangOr(L"notify_opensymbols", L"Open Symbols page"), L"symbols");
		}
		else
		{
			shown = Toast::ShowStatus(LangOr(L"notify_loadfailtitle", L"DWMBlurGlass couldn't start"),
				LangOr(L"loadfail", L"Failed to load component! Error message: ") + err,
				LangOr(L"notify_open", L"Open DWMBlurGlass"), L"general");
		}

		// Notifications unavailable or turned off: one message box rather than failing silently.
		if (!shown)
			MessageBoxW(nullptr, err.c_str(), L"DWMBlurGlass Error", MB_ICONERROR);
	}

	void OnProcessStart(DWORD id)
	{
		//单独记录每个用户dwm进程的启动间隔状态
		using namespace std::chrono;
		struct sessionData
		{
			steady_clock::duration times = steady_clock::now().time_since_epoch();
			bool init = true;
		};
		static std::unordered_map<DWORD, sessionData> sessionList;

		DWORD sessionId = 0;
		ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);

		if(auto iter = sessionList.find(sessionId); iter != sessionList.end())
		{
			bool retry = false;
			//dwm进程不应启动这么频繁 这有可能是频繁崩溃 停止自动加载
			if (!iter->second.init && steady_clock::now().time_since_epoch() - iter->second.times < 10s)
			{
				KillTimer(g_hostMsgWnd, g_timerID);
				auto ret = MessageBoxW(nullptr, L"Checked that the process is suspected to have crashed abnormally and has stopped autoloading.",
					L"DWMBlurGlass Error", MB_ICONERROR | MB_RETRYCANCEL);

				iter->second.times = steady_clock::now().time_since_epoch();

				if (ret != IDRETRY)
					return;
				else
					retry = true;
			}
			iter->second.times = steady_clock::now().time_since_epoch();
			if(!retry)
				iter->second.init = false;
		}
		else
			sessionList.insert(std::make_pair(sessionId, sessionData()));

		std::wstring err;
		bool symbolsFailed = false;
		if (LoadDWMExtensionBase(err, id, &symbolsFailed))
			Toast::ClearStatus();	// a notice left over from an earlier failure no longer applies
		else
			NotifyLoadFailure(err, symbolsFailed);
	}

	LRESULT CALLBACK MsgWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		if(message == WM_APP + 20 && MClientNotifyType::Shutdown == (MClientNotifyType)wParam)
		{
			for(auto& id : g_proclist)
			{
				HWND msgwnd = FindMessageWnd(GetProcessId(L"dwm.exe"));
				if (IsWindow(msgwnd))
					SendMessageW(msgwnd, WM_APP + 20, (WPARAM)MHostNotifyType::Shutdown, 0);

				std::wstring err;
				UnInject(id, Utils::GetCurrentDir() + L"\\DWMBlurGlassExt.dll", err);
			}
		}
		else if(message == WM_APP + 20 && MClientNotifyType::QueryTransparency == (MClientNotifyType)wParam)
		{
			MHostNotify(MHostNotifyType::EnableTransparency, IsEnableTransparency());
		}
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}

	constexpr std::pair<MHostModuleType, const wchar_t*> g_symbolModules[] =
	{
		{ dwmcore, L"dwmcore.dll" },
		{ udwm, L"uDwm.dll" }
	};

	OffsetCache::NameList GetModuleNameList(MHostModuleType type)
	{
		OffsetCache::NameList names;
		for (size_t i = 0; i < MDWMBlurGlassExt::g_hookFunList.size(); ++i)
		{
			if (MDWMBlurGlassExt::g_hookFunList[i].first == type)
				names.emplace_back(i, MDWMBlurGlassExt::g_hookFunList[i].second);
		}
		return names;
	}

	// True if every module's offsets can be had without downloading anything: its PDB is already
	// local, its build is in the offset cache, or it can be re-located from a previous build.
	bool CanResolveWithoutDownload()
	{
		for (auto& [type, dllName] : g_symbolModules)
		{
			if (OffsetCache::LocalPdbExists(dllName))
				continue;
			auto names = GetModuleNameList(type);
			std::vector<DWORD64> offsets(MDWMBlurGlassExt::g_hookFunList.size(), 0);
			if (OffsetCache::LoadExact(dllName, names, offsets) != OffsetCache::Source::None)
				continue;
			if (!OffsetCache::Derive(dllName, names, offsets))
				return false;
		}
		return true;
	}

	// 检查并自动下载符号文件
	void CheckAndDownloadSymbols()
	{
		// 加载配置文件
		std::wstring configPath = Utils::GetCurrentDir() + L"\\data\\config.ini";
		ConfigData config = ConfigData::LoadFromFile(configPath);

		// 如果禁用了自动下载，直接返回
		if (!config.autoDownloadSymbols)
			return;

		// 检查符号是否有效
		SymbolDownloader downloader;
		if (downloader.CheckSymbolsValid())
			return;

		// The PDBs don't match the installed build. A cumulative update usually leaves the hooked
		// functions' code unchanged, so try to re-locate them from the cached previous build first.
		if (CanResolveWithoutDownload())
			return;

		// 符号无效，显示下载对话框
		g_symbolDownload = SymbolDownloadDialog::Show(nullptr);
	}

	void MHostStartProcess()
	{
		// 检查并自动下载符号文件. A failed download isn't reported here: if it leaves the symbols
		// unusable, loading fails next and NotifyLoadFailure says why, in a single notification.
		CheckAndDownloadSymbols();

		WNDCLASSEXW wx = {};
		wx.cbSize = sizeof(WNDCLASSEX);
		wx.lpfnWndProc = MsgWndProc;
		wx.hInstance = GetModuleHandleW(nullptr);
		wx.lpszClassName = DWMBlurGlassHostNotifyClassName;
		if (!RegisterClassExW(&wx))
			return;

		g_hostMsgWnd = CreateWindowExW(0, wx.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);

		g_timerID = SetTimer(g_hostMsgWnd, 1000, 500, [](HWND, UINT, UINT_PTR, DWORD)
		{
			RefreshProcessList();
		});

		MSG msg;
		while (GetMessageW(&msg, nullptr, 0, 0))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}

		KillTimer(g_hostMsgWnd, g_timerID);
		UnregisterClassW(wx.lpszClassName, wx.hInstance);
	}

	// Resolve one module's offsets: offset cache for this exact build -> local PDB -> re-locate
	// from a previous build's cached patterns.
	bool LoadModuleOffsets(MHostModuleType type, const wchar_t* dllName)
	{
		const auto names = GetModuleNameList(type);
		std::vector<DWORD64> offsets(MDWMBlurGlassExt::g_hookFunList.size(), 0);
		auto apply = [&]
		{
			for (auto& [index, _] : names)
				g_hookFunOffsetList[index] = offsets[index];
		};

		const auto cached = OffsetCache::LoadExact(dllName, names, offsets);
		// Offsets derived by pattern are replaced by the PDB's once that PDB is available.
		if (cached == OffsetCache::Source::Pdb || (cached == OffsetCache::Source::Pattern && !OffsetCache::LocalPdbExists(dllName)))
		{
			apply();
			return true;
		}

		for (auto& [index, _] : names)
			g_hookFunOffsetList[index] = 0;

		const std::wstring symPath = L"SRV*" + Utils::GetCurrentDir() + L"\\data\\symbols";
		const HRESULT hr = g_symResolver.Walk(dllName, "*!*", symPath, type == dwmcore ? SymCallback_Dwmcore : SymCallback_uDwm);
		if (SUCCEEDED(hr))
		{
			for (auto& [index, _] : names)
				offsets[index] = g_hookFunOffsetList[index];
			OffsetCache::Record(dllName, names, offsets, OffsetCache::Source::Pdb);
			return true;
		}

		if (cached == OffsetCache::Source::Pattern || OffsetCache::Derive(dllName, names, offsets))
		{
			apply();
			return true;
		}
		return false;
	}

	bool LoadSymbolOffset()
	{
		g_dwmcoreFunCount = g_udwmFunCount = 0;
		for (auto& list : MDWMBlurGlassExt::g_hookFunList)
		{
			if (list.first == dwmcore)
				g_dwmcoreFunCount++;
			else if (list.first == udwm)
				g_udwmFunCount++;
		}

		for (auto& [type, dllName] : g_symbolModules)
		{
			if (!LoadModuleOffsets(type, dllName))
				return false;
		}
		return true;
	}

	bool LoadDWMExtensionBase(std::wstring& err, DWORD pid, bool* symbolsFailed)
	{
		if(!LoadSymbolOffset())
		{
			err = GetLanguageString(L"symloadfail");
			if (symbolsFailed)
				*symbolsFailed = true;
			return false;
		}
		const bool ret = Inject(GetProcessId(L"dwm.exe"), Utils::GetCurrentDir() + L"\\DWMBlurGlassExt.dll", err);
		if(ret)
		{
			auto enumproc = [](HWND hWnd, LPARAM _pid) -> BOOL
			{
				DWORD dwPid = 0;
				GetWindowThreadProcessId(hWnd, &dwPid);
				if(_pid == dwPid)
				{
					PostMessageW(hWnd, WM_THEMECHANGED, 0, 0);
					return FALSE;
				}
				return TRUE;
			};
			EnumWindows(enumproc, pid);

			//PostMessageW(FindWindowW(L"Dwm", nullptr), WM_THEMECHANGED, 0, 0);
		}
		return ret;
	}

	void MHostNotify(MHostNotifyType type, LPARAM lParam)
	{
		HWND msgwnd = FindMessageWnd(GetProcessId(L"dwm.exe"));
		if (!IsWindow(msgwnd)) return;

		SendMessageW(msgwnd, WM_APP + 20, (WPARAM)type, lParam);
	}
}

extern "C" __declspec(dllexport) DWORD64 __stdcall GetModuleOffset(size_t index)
{
	using namespace MDWMBlurGlass;
	if (index < sizeof g_hookFunOffsetList)
		return g_hookFunOffsetList[index];
	return 0;
}