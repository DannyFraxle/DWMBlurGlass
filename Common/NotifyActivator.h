/**
 * FileName: NotifyActivator.h
 *
 * This file is part of DWMBlurGlass.
 * DWMBlurGlass is free software: you can redistribute it and/or modify it under the terms of the
 * GNU Lesser General Public License as published by the Free Software Foundation, either version 3
 * of the License, or any later version.
 *
 * Shared by the host (which registers it, see DWMBlurGlassHost/Helper/ToastNotifier.cpp) and
 * DWMBlurGlassNotify.exe (which serves it): the COM class Windows starts when one of the host's
 * notifications is clicked.
*/
#pragma once
#include <guiddef.h>

// {ADB7320E-D073-4D36-BFB5-622EB88A2F52}
inline constexpr GUID CLSID_DWMBlurGlassNotifyActivator =
	{ 0xadb7320e, 0xd073, 0x4d36, { 0xbf, 0xb5, 0x62, 0x2e, 0xb8, 0x8a, 0x2f, 0x52 } };
inline constexpr wchar_t DWMBlurGlassNotifyActivatorClsid[] = L"{ADB7320E-D073-4D36-BFB5-622EB88A2F52}";

inline constexpr wchar_t DWMBlurGlassNotifyExe[] = L"DWMBlurGlassNotify.exe";
