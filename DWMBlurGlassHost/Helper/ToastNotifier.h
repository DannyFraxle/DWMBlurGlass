/**
 * FileName: ToastNotifier.h
 *
 * This file is part of DWMBlurGlass.
 * DWMBlurGlass is free software: you can redistribute it and/or modify it under the terms of the
 * GNU Lesser General Public License as published by the Free Software Foundation, either version 3
 * of the License, or any later version.
 *
 * Reports host problems (out-of-date symbols, a failed load) as a Windows notification instead of
 * a message box. The notification lands in the notification centre, never blocks the host, and is
 * a single slot: each one replaces the last instead of stacking up.
 *
 * The host is an unpackaged app, so the first time it notifies it registers, per user:
 *  - an AppUserModelID under HKCU\Software\Classes\AppUserModelId (display name + icon);
 *  - a "DWMBlurGlass" Start-menu shortcut to the GUI carrying that ID. Without it Windows accepts
 *    the notifications but never shows them;
 *  - DWMBlurGlassNotify.exe as the notifications' COM activator (HKLM\Software\Classes\CLSID, as
 *    Windows may ignore a per-user one; named on the shortcut too). Windows starts it when a
 *    notification is clicked, and it opens the GUI.
 * The GUI's Uninstall removes all three.
*/
#pragma once
#include "../framework.h"
#include <string_view>

namespace MDWMBlurGlass::Toast
{
	// Shows (or replaces) the host's status notification. Clicking it, or its button, opens the
	// settings GUI on `guiPage` ("symbols", or "general"). Must be called on the host's main thread.
	// Returns false if it can't be shown: no notification platform, or the user has turned
	// notifications off.
	bool ShowStatus(std::wstring_view title, std::wstring_view body, std::wstring_view buttonText, std::wstring_view guiPage);

	// Withdraws the status notification from the notification centre, e.g. once DWM has loaded the
	// extension. Harmless if there is none. Must be called on the host's main thread.
	void ClearStatus();
}
