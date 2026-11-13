/**
 * FileName: main.cpp (DWMBlurGlassNotify)
 *
 * This file is part of DWMBlurGlass.
 * DWMBlurGlass is free software: you can redistribute it and/or modify it under the terms of the
 * GNU Lesser General Public License as published by the Free Software Foundation, either version 3
 * of the License, or any later version.
 *
 * Opens the settings GUI when one of the host's notifications is clicked.
 *
 * Windows only hands a notification click to an unpackaged app through a registered COM activator,
 * and it won't start or reach an elevated one, which the host is. So the host registers this small
 * unelevated exe as its activator (COM local server, see DWMBlurGlassHost/Helper/ToastNotifier.cpp).
 * Windows starts it with -ToastActivated and calls INotificationActivationCallback::Activate with
 * the notification's arguments, a GUI page name; this then opens DWMBlurGlassGUI.exe on that page,
 * and the GUI asks for elevation itself. Started any other way, it does nothing.
*/
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include <NotificationActivationCallback.h>
#include <string>
#include "../Common/NotifyActivator.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
	std::wstring g_page;
	bool g_activated = false;

	// Both COM objects are single static instances that live as long as the process, so they don't
	// reference-count.
	class Activator final : public INotificationActivationCallback
	{
	public:
		IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
		{
			if (!ppv)
				return E_POINTER;
			if (riid == IID_IUnknown || riid == __uuidof(INotificationActivationCallback))
			{
				*ppv = static_cast<INotificationActivationCallback*>(this);
				return S_OK;
			}
			*ppv = nullptr;
			return E_NOINTERFACE;
		}
		IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
		IFACEMETHODIMP_(ULONG) Release() override { return 1; }

		// Runs on the main thread, inside the message loop in wWinMain.
		IFACEMETHODIMP Activate(LPCWSTR, LPCWSTR invokedArgs, const NOTIFICATION_USER_INPUT_DATA*, ULONG) override
		{
			g_page = invokedArgs ? invokedArgs : L"";
			g_activated = true;
			return S_OK;
		}
	};

	class Factory final : public IClassFactory
	{
	public:
		IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
		{
			if (!ppv)
				return E_POINTER;
			if (riid == IID_IUnknown || riid == IID_IClassFactory)
			{
				*ppv = static_cast<IClassFactory*>(this);
				return S_OK;
			}
			*ppv = nullptr;
			return E_NOINTERFACE;
		}
		IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
		IFACEMETHODIMP_(ULONG) Release() override { return 1; }

		IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
		{
			if (outer)
				return CLASS_E_NOAGGREGATION;
			static Activator activator;
			return activator.QueryInterface(riid, ppv);
		}
		IFACEMETHODIMP LockServer(BOOL) override { return S_OK; }
	};

	std::wstring ExeDir()
	{
		wchar_t path[MAX_PATH]{};
		GetModuleFileNameW(nullptr, path, MAX_PATH);
		const std::wstring full = path;
		return full.substr(0, full.rfind(L'\\'));
	}

	// The GUI allows one instance and shows a message box for a second, so if it's already open,
	// bring it forward instead of starting another.
	bool FocusRunningGui()
	{
		// Created by the elevated GUI: from here it either opens or is access-denied, both mean "running".
		HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, L"_DWMBlurGlassGUI_");
		if (mutex)
			CloseHandle(mutex);
		else if (GetLastError() != ERROR_ACCESS_DENIED)
			return false;

		const std::wstring gui = ExeDir() + L"\\DWMBlurGlassGUI.exe";
		struct Search { const std::wstring* gui; HWND found; } search{ &gui, nullptr };
		EnumWindows([](HWND hwnd, LPARAM param) -> BOOL
		{
			auto& s = *reinterpret_cast<Search*>(param);
			if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
				return TRUE;
			DWORD pid = 0;
			GetWindowThreadProcessId(hwnd, &pid);
			HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
			if (!process)
				return TRUE;
			wchar_t image[MAX_PATH]{};
			DWORD size = MAX_PATH;
			const bool match = QueryFullProcessImageNameW(process, 0, image, &size) && _wcsicmp(image, s.gui->c_str()) == 0;
			CloseHandle(process);
			if (!match)
				return TRUE;
			s.found = hwnd;
			return FALSE;
		}, reinterpret_cast<LPARAM>(&search));

		if (search.found)
		{
			if (IsIconic(search.found))
				ShowWindowAsync(search.found, SW_RESTORE);
			SetForegroundWindow(search.found);
		}
		return true;
	}

	void OpenGui(const std::wstring& page)
	{
		if (FocusRunningGui())
			return;

		const std::wstring dir = ExeDir();
		const std::wstring gui = dir + L"\\DWMBlurGlassGUI.exe";
		std::wstring params;
		// Page names are plain words; pass nothing else on.
		if (!page.empty() && page.find_first_not_of(L"abcdefghijklmnopqrstuvwxyz") == std::wstring::npos)
			params = L"--page " + page;

		SHELLEXECUTEINFOW info{ sizeof(info) };
		info.fMask = SEE_MASK_NOASYNC;
		info.lpVerb = L"open";
		info.lpFile = gui.c_str();
		info.lpParameters = params.empty() ? nullptr : params.c_str();
		info.lpDirectory = dir.c_str();
		info.nShow = SW_SHOWNORMAL;
		ShellExecuteExW(&info);
	}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR cmdLine, int)
{
	if (!cmdLine || (!wcsstr(cmdLine, L"-ToastActivated") && !wcsstr(cmdLine, L"-Embedding")))
		return 0;

	if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
		return 1;

	static Factory factory;
	DWORD cookie = 0;
	if (SUCCEEDED(CoRegisterClassObject(CLSID_DWMBlurGlassNotifyActivator, &factory, CLSCTX_LOCAL_SERVER, REGCLS_SINGLEUSE, &cookie)))
	{
		// Windows calls Activate right after start-up. Give up if it never does.
		const ULONGLONG deadline = GetTickCount64() + 15000;
		while (!g_activated)
		{
			const ULONGLONG now = GetTickCount64();
			if (now >= deadline)
				break;
			MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(deadline - now), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
			MSG msg;
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
		}
		CoRevokeClassObject(cookie);
	}

	if (g_activated)
		OpenGui(g_page);

	CoUninitialize();
	return 0;
}
