/**
 * FileName: ToastNotifier.cpp
 *
 * This file is part of DWMBlurGlass.
 * DWMBlurGlass is free software: you can redistribute it and/or modify it under the terms of the
 * GNU Lesser General Public License as published by the Free Software Foundation, either version 3
 * of the License, or any later version.
*/
#include "ToastNotifier.h"
#include "Common.h"
#include "NotifyActivator.h"
#include "../resource.h"
#include <Unknwn.h>
#include <ShObjIdl.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wincodec.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>

#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "propsys.lib")

namespace MDWMBlurGlass::Toast
{
	namespace
	{
		using namespace winrt::Windows::UI::Notifications;
		using winrt::Windows::Data::Xml::Dom::XmlDocument;

		constexpr wchar_t AppId[] = L"Maplespe.DWMBlurGlass";
		constexpr wchar_t AppName[] = L"DWMBlurGlass";
		constexpr wchar_t AppIdKey[] = L"Software\\Classes\\AppUserModelId\\Maplespe.DWMBlurGlass";
		// Every status notification carries this tag + group, so a new one replaces the old one.
		constexpr wchar_t StatusTag[] = L"status";
		constexpr wchar_t StatusGroup[] = L"host";

		// Deliberately never freed: releasing WinRT objects during static destruction, after the
		// apartment may be gone, can hang or crash the exit.
		ToastNotifier& GetNotifier()
		{
			struct Holder { ToastNotifier notifier{ nullptr }; };
			static Holder* holder = new Holder;
			return holder->notifier;
		}

		// The main thread has no apartment of its own; give it an STA once and keep it. The host's
		// message loop pumps it.
		bool EnsureApartment()
		{
			static const bool ready = []
			{
				const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
				return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
			}();
			return ready;
		}

		std::wstring GuiPath()
		{
			return Utils::GetCurrentDir() + L"\\DWMBlurGlassGUI.exe";
		}

		std::wstring ActivatorPath()
		{
			return Utils::GetCurrentDir() + L"\\" + DWMBlurGlassNotifyExe;
		}

		bool SetRegString(HKEY root, const std::wstring& subKey, const wchar_t* name, const std::wstring& value)
		{
			HKEY key = nullptr;
			if (RegCreateKeyExW(root, subKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
				return false;
			const bool ok = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
				static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
			RegCloseKey(key);
			return ok;
		}

		// Windows only hands a click on an unpackaged app's notification to a registered COM activator,
		// and won't start or reach an elevated one, which the host is. So DWMBlurGlassNotify.exe, small
		// and unelevated, is registered as the activator: Windows starts it for a click, from the pop-up
		// or the notification centre and whether or not the host is still running, and it opens the GUI.
		//
		// The registration is machine-wide (HKLM), which the elevated host can write. Windows may ignore
		// per-user (HKCU) registrations of COM servers that run in their own process; Windows 11 build
		// 26300 does, so a click then does nothing. HKCU is only the fallback for an unelevated host.
		void RegisterActivator(const std::wstring& activator)
		{
			const std::wstring clsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + DWMBlurGlassNotifyActivatorClsid;
			const std::wstring server = L"\"" + activator + L"\"";
			if (SetRegString(HKEY_LOCAL_MACHINE, clsidKey + L"\\LocalServer32", nullptr, server))
			{
				SetRegString(HKEY_LOCAL_MACHINE, clsidKey, nullptr, L"DWMBlurGlass notification activator");
				// Where per-user registrations do count, a stale one would shadow this.
				RegDeleteTreeW(HKEY_CURRENT_USER, clsidKey.c_str());
			}
			else
			{
				SetRegString(HKEY_CURRENT_USER, clsidKey, nullptr, L"DWMBlurGlass notification activator");
				SetRegString(HKEY_CURRENT_USER, clsidKey + L"\\LocalServer32", nullptr, server);
			}
			SetRegString(HKEY_CURRENT_USER, AppIdKey, L"CustomActivator", DWMBlurGlassNotifyActivatorClsid);
		}

		// Notifications take the app icon from an image file, so the host writes its own icon out as
		// a PNG the first time. Returns the path, or empty if that failed (the notification then just
		// shows without an icon).
		std::wstring EnsureIconFile()
		{
			const std::wstring path = Utils::GetCurrentDir() + L"\\data\\notifyicon.png";
			if (PathFileExistsW(path.c_str()))
				return path;

			const auto icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_ICON1),
				IMAGE_ICON, 64, 64, LR_DEFAULTCOLOR));
			if (!icon)
				return {};
			auto destroyIcon = RAIIHelper::scope_exit([icon] { DestroyIcon(icon); });

			try
			{
				const auto factory = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
				winrt::com_ptr<IWICBitmap> bitmap;
				winrt::check_hresult(factory->CreateBitmapFromHICON(icon, bitmap.put()));
				UINT width = 0, height = 0;
				winrt::check_hresult(bitmap->GetSize(&width, &height));

				winrt::com_ptr<IWICStream> stream;
				winrt::check_hresult(factory->CreateStream(stream.put()));
				winrt::check_hresult(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));

				winrt::com_ptr<IWICBitmapEncoder> encoder;
				winrt::check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
				winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));

				winrt::com_ptr<IWICBitmapFrameEncode> frame;
				winrt::check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
				winrt::check_hresult(frame->Initialize(nullptr));
				winrt::check_hresult(frame->SetSize(width, height));
				WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
				winrt::check_hresult(frame->SetPixelFormat(&format));
				winrt::check_hresult(frame->WriteSource(bitmap.get(), nullptr));
				winrt::check_hresult(frame->Commit());
				winrt::check_hresult(encoder->Commit());
			}
			catch (...)
			{
				// The stream is released by now; don't leave a truncated file to be picked up next time.
				DeleteFileW(path.c_str());
				return {};
			}
			return path;
		}

		// An unpackaged app's notifications need its AppUserModelID registered to show under a name
		// and icon. This registry entry does that without a Start-menu shortcut.
		void RegisterAppId(const std::wstring& iconPath)
		{
			HKEY key = nullptr;
			if (RegCreateKeyExW(HKEY_CURRENT_USER, AppIdKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
				return;

			auto setString = [key](const wchar_t* name, const std::wstring& value)
			{
				RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
					static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
			};
			setString(L"DisplayName", AppName);
			if (!iconPath.empty())
				setString(L"IconUri", iconPath);
			RegCloseKey(key);
		}

		enum class ShortcutState { Failed, Existing, Created };

		// The notification centre only lists notifications from apps it can find in the Start menu.
		// With just the registry entry above, Windows accepts them but never shows them. So the app ID
		// also goes on a "DWMBlurGlass" Start-menu shortcut to the GUI, which is a handy entry anyway.
		// The shortcut also names the click activator, when there is one.
		ShortcutState EnsureStartMenuShortcut(bool withActivator)
		{
			PWSTR programs = nullptr;
			if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs)))
				return ShortcutState::Failed;
			const std::wstring linkPath = std::wstring(programs) + L"\\DWMBlurGlass.lnk";
			CoTaskMemFree(programs);

			std::wstring target = GuiPath();
			if (!PathFileExistsW(target.c_str()))
			{
				wchar_t self[MAX_PATH]{};
				GetModuleFileNameW(nullptr, self, MAX_PATH);
				target = self;
			}

			try
			{
				const auto link = winrt::create_instance<IShellLinkW>(CLSID_ShellLink);
				const auto file = link.as<IPersistFile>();
				const auto store = link.as<IPropertyStore>();

				// Leave it alone if it already points at this install with our app ID and activator.
				if (SUCCEEDED(file->Load(linkPath.c_str(), STGM_READ)))
				{
					wchar_t current[MAX_PATH]{};
					const bool sameTarget = SUCCEEDED(link->GetPath(current, MAX_PATH, nullptr, SLGP_RAWPATH))
						&& _wcsicmp(current, target.c_str()) == 0;
					PROPVARIANT value;
					PropVariantInit(&value);
					const bool sameId = SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &value))
						&& value.vt == VT_LPWSTR && wcscmp(value.pwszVal, AppId) == 0;
					PropVariantClear(&value);
					const bool hasActivator = SUCCEEDED(store->GetValue(PKEY_AppUserModel_ToastActivatorCLSID, &value))
						&& value.vt == VT_CLSID && *value.puuid == CLSID_DWMBlurGlassNotifyActivator;
					PropVariantClear(&value);
					if (sameTarget && sameId && hasActivator == withActivator)
						return ShortcutState::Existing;
				}

				winrt::check_hresult(link->SetPath(target.c_str()));
				winrt::check_hresult(link->SetArguments(L""));
				winrt::check_hresult(link->SetWorkingDirectory(Utils::GetCurrentDir().c_str()));
				winrt::check_hresult(link->SetDescription(L"DWMBlurGlass settings"));

				PROPVARIANT id;
				winrt::check_hresult(InitPropVariantFromString(AppId, &id));
				HRESULT hr = store->SetValue(PKEY_AppUserModel_ID, id);
				PropVariantClear(&id);
				winrt::check_hresult(hr);

				PROPVARIANT activator;
				PropVariantInit(&activator);	// VT_EMPTY clears it
				if (withActivator)
					winrt::check_hresult(InitPropVariantFromCLSID(CLSID_DWMBlurGlassNotifyActivator, &activator));
				hr = store->SetValue(PKEY_AppUserModel_ToastActivatorCLSID, activator);
				PropVariantClear(&activator);
				winrt::check_hresult(hr);
				winrt::check_hresult(store->Commit());
				winrt::check_hresult(file->Save(linkPath.c_str(), TRUE));
				return ShortcutState::Created;
			}
			catch (...)
			{
				return ShortcutState::Failed;
			}
		}

		// Windows takes a moment to index a new Start-menu shortcut, and a notification sent before then
		// is accepted but never shown. Wait, briefly, until the shell can resolve our app ID. Only
		// happens once: the first time this account ever gets a notification.
		void WaitForShortcutIndexed()
		{
			const std::wstring item = std::wstring(L"shell:AppsFolder\\") + AppId;
			for (int i = 0; i < 40; ++i)	// up to ~10 s
			{
				winrt::com_ptr<IShellItem> found;
				if (SUCCEEDED(SHCreateItemFromParsingName(item.c_str(), nullptr, IID_PPV_ARGS(found.put()))))
					return;
				Sleep(250);
			}
		}

		bool AppIdRegistered()
		{
			HKEY key = nullptr;
			if (RegOpenKeyExW(HKEY_CURRENT_USER, AppIdKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
				return false;
			RegCloseKey(key);
			return true;
		}

		std::wstring XmlEscape(std::wstring_view text)
		{
			std::wstring out;
			out.reserve(text.size());
			for (const wchar_t c : text)
			{
				switch (c)
				{
				case L'&': out += L"&amp;"; break;
				case L'<': out += L"&lt;"; break;
				case L'>': out += L"&gt;"; break;
				case L'"': out += L"&quot;"; break;
				case L'\'': out += L"&apos;"; break;
				default: out += c; break;
				}
			}
			return out;
		}
	}

	bool ShowStatus(std::wstring_view title, std::wstring_view body, std::wstring_view buttonText, std::wstring_view guiPage)
	{
		if (!EnsureApartment())
			return false;

		try
		{
			// Clicking needs both the GUI to open and the activator that opens it; without them the
			// notification gets no click target and no button.
			const std::wstring activator = ActivatorPath();
			const bool canOpenGui = PathFileExistsW(GuiPath().c_str()) && PathFileExistsW(activator.c_str());

			ToastNotifier& notifier = GetNotifier();
			if (!notifier)
			{
				RegisterAppId(EnsureIconFile());
				if (canOpenGui)
					RegisterActivator(activator);
				if (EnsureStartMenuShortcut(canOpenGui) == ShortcutState::Created)
					WaitForShortcutIndexed();
				notifier = ToastNotificationManager::CreateToastNotifier(AppId);
			}

			// Show() succeeds even when the user has turned notifications off (for everything, or
			// just for us); the notification is then silently dropped. Report that as unavailable.
			if (notifier.Setting() != NotificationSetting::Enabled)
				return false;

			// Handed to the activator as is, on a click; it only accepts a plain page name.
			const std::wstring page = XmlEscape(guiPage);

			std::wstring xml = L"<toast";
			if (canOpenGui)
				xml += L" launch=\"" + page + L"\"";
			xml += L"><visual><binding template=\"ToastGeneric\">";
			xml += L"<text>" + XmlEscape(title) + L"</text>";
			xml += L"<text>" + XmlEscape(body) + L"</text>";
			xml += L"</binding></visual>";
			if (canOpenGui && !buttonText.empty())
				xml += L"<actions><action content=\"" + XmlEscape(buttonText) + L"\" arguments=\"" + page + L"\"/></actions>";
			xml += L"</toast>";

			XmlDocument doc;
			doc.LoadXml(xml);
			ToastNotification toast(doc);
			toast.Tag(StatusTag);
			toast.Group(StatusGroup);
			notifier.Show(toast);
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	void ClearStatus()
	{
		// Never notified on this account: nothing to clear, and no reason to touch WinRT.
		if (!AppIdRegistered() || !EnsureApartment())
			return;

		try
		{
			ToastNotificationManager::History().Remove(StatusTag, StatusGroup, AppId);
		}
		catch (...) {}
	}
}
