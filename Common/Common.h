/**
 * FileName: Common.h
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
#pragma once
#include <Windows.h>
#include <string>

EXTERN_C IMAGE_DOS_HEADER __ImageBase;
namespace MDWMBlurGlass
{
	extern const HINSTANCE this_inst;

	enum class blurMethod
	{
		CustomBlur,
		AccentBlur,
		DWMAPIBlur
	};

	enum class effectType
	{
		None = -1,
		Blur,
		Aero,
		Acrylic,
		Mica,
		LiquidGlass
	};

	enum class blurQuality
	{
		Speed = 0,
		Quality = 1
	};

	// Optional gradient across the titlebar, laid over the normal backdrop:
	// it fades from fully clear at the start edge (so the normal blend colour
	// shows there) to the gradient colour at the far edge.
	enum class gradientDirection
	{
		None = 0,
		Horizontal = 1,	// left -> right, as in Windows 98
		Vertical = 2	// top -> bottom, as in XP Media Center Edition
	};

	struct ConfigData
	{
		bool applyglobal = false;
		bool extendBorder = false;
		bool reflection = false;
		bool oldBtnHeight = false;
		bool customAmount = false;
		bool useAccentColor = false;
		bool crossFade = true;

		bool overrideAccent = false;
		bool powerSavingMode = false;
		bool disableOnBattery = true;

		bool scaleOptimizer = false;
		bool titlebtnGlow = false;

		bool disableFramerateLimit = false;
		bool customAeroTexture = false;

		bool autoDownloadSymbols = true;

		bool customTitleBtnSize = false;

		//Options without GUI
		int extendRound = 10;
		int clipRound = 0;
		int titlebtnOffsetX = -1;
		// LiquidGlass tuning, config.ini only.
		// glassRefractionAmount: how wide a strip of the desktop just outside a
		// glass edge gets compacted into the rim, px (the "pinch" reach).
		// glassRimThickness: how wide the rim it is compacted into is, px.
		float glassRefractionAmount = 24.f;
		int glassRimThickness = 12;
		// The edge pinch is drawn by a D3D pass inside dwm.exe on Windows 11.
		// false turns it off and leaves the Composition material alone; so does
		// creating C:\ProgramData\DWMBlurGlassRecon\no_pinch.
		bool glassPinch = true;
		// Bring-up aid: floods the rim band with flat magenta so the published
		// rects can be checked at a glance. config.ini only, not in the GUI.
		bool glassDebugRim = false;
		// One-shot: logs which D2D effects Composition will actually build,
		// to C:\ProgramData\DWMBlurGlassRecon\effects.log. config.ini only.
		bool glassProbeEffects = false;
		//

		int customCloseBtnW = 49;
		int customMaxBtnW = 27;
		int customMinBtnW = 29;
		int customBtnFrameH = 20;

		float blurAmount = 20.f;
		float customBlurAmount = 20.f;
		float luminosityOpacity = 0.65f;
		float glassIntensity = 1.f;

		UINT crossfadeTime = 160;

		// these settings are optimal for the default Sky color from Windows 7
		// newly added params since 2.1.0
		float aeroColorBalance = 0.08f;
		float aeroAfterglowBalance = 0.43f;
		float aeroBlurBalance = 0.49f;

		COLORREF activeTextColor = 0xFF000000;
		COLORREF inactiveTextColor = 0xFFB4B4B4;
		COLORREF activeBlendColor = 0x64FFFFFF;
		COLORREF inactiveBlendColor = 0x64FFFFFF;

		COLORREF activeTextColorDark = 0xFFFFFFFF;
		COLORREF inactiveTextColorDark = 0xFFB4B4B4;
		COLORREF activeBlendColorDark = 0x64000000;
		COLORREF inactiveBlendColorDark = 0x64000000;

		// Far-edge colours of the titlebar gradient (0xAABBGGRR, like the
		// blend colours). Defaults are the Windows 98 gradient ends.
		gradientDirection titleGradient = gradientDirection::None;
		COLORREF activeGradientColor = 0xFFD08410;		// 16,132,208
		COLORREF inactiveGradientColor = 0xFFB5B5B5;	// 181,181,181
		COLORREF activeGradientColorDark = 0xFFD08410;
		COLORREF inactiveGradientColorDark = 0xFF505050;
		// Optional third point between the blend colour and the end colour,
		// titleGradientMidPos percent of the way across - for the glossy XP
		// look (bright top, mid blue about two thirds down, dark bottom).
		bool titleGradientMid = false;
		int titleGradientMidPos = 64;
		COLORREF activeGradientMidColor = 0xFFC77738;		// 56,119,199
		COLORREF inactiveGradientMidColor = 0xFF969696;		// 150,150,150
		COLORREF activeGradientMidColorDark = 0xFFC77738;
		COLORREF inactiveGradientMidColorDark = 0xFF404040;


		blurMethod blurmethod = blurMethod::CustomBlur;
		effectType effectType = effectType::Blur;
		blurQuality blurQuality = blurQuality::Speed;

		std::wstring customAeroTexturePath;

		bool isDefault()
		{
			static ConfigData _default;
			return memcmp(this, &_default, sizeof ConfigData) == 0;
		}

		static ConfigData LoadFromFile(std::wstring_view path);
	};

	enum class MHostNotifyType
	{
		Refresh,
		Shutdown,
		EnableTransparency
	};
	enum class MClientNotifyType
	{
		Shutdown = 1,
		QueryTransparency
	};

	enum MHostModuleType
	{
		dwmcore,
		udwm
	};

	auto constexpr DWMBlurGlassNotifyClassName = L"MDWMBlurGlassExtNotify";
	auto constexpr DWMBlurGlassHostNotifyClassName = L"MDWMBlurGlassHostNotify";

	namespace RAIIHelper
	{
		template <typename TLambda>
		class lambda_call
		{
		public:
			lambda_call(const lambda_call&) = delete;
			lambda_call& operator=(const lambda_call&) = delete;
			lambda_call& operator=(lambda_call&& other) = delete;

			inline explicit lambda_call(TLambda&& lambda) noexcept : m_lambda(std::move(lambda))
			{
				static_assert(std::is_same<decltype(lambda()), void>::value, "scope_exit的lambda函数一定不能有返回值");
				static_assert(!std::is_lvalue_reference<TLambda>::value && !std::is_rvalue_reference<TLambda>::value, "scope_exit只能直接与lambda函数一起使用");
			}
			inline lambda_call(lambda_call&& other) noexcept : m_lambda(std::move(other.m_lambda)), m_call(other.m_call)
			{
				other.m_call = false;
			}
			inline ~lambda_call() noexcept
			{
				reset();
			}
			// 确保scope_exit lambda不会被调用
			inline void release() noexcept
			{
				m_call = false;
			}
			// 立刻执行scope_exit lambda如果还没有运行的话；确保它不再次运行
			inline void reset() noexcept
			{
				if (m_call)
				{
					m_call = false;
					m_lambda();
				}
			}
			// 返回true如果scope_exit lambda仍要被执行
			[[nodiscard]] inline explicit operator bool() const noexcept
			{
				return m_call;
			}
		protected:
			TLambda m_lambda;
			bool m_call = true;
		};
		/*
			返回一个对象，该对象在销毁时执行给定的lambda函数，请使用auto捕获返回的对象
			使用reset()提前执行lambda或使用release()避免执行
			在lambda中抛出的异常将引发快速失败
			你可以认为这个对象的作用跟finally差不多
		*/
		template <typename TLambda>
		[[nodiscard]] inline auto scope_exit(TLambda&& lambda) noexcept
		{
			return lambda_call<TLambda>(std::forward<TLambda>(lambda));
		}
	}

	namespace Utils
	{
		extern std::wstring GetCurrentDir();
		extern std::wstring GetIniString(std::wstring_view path, std::wstring_view appName, std::wstring_view keyName);
		extern bool IsAppUseLightMode();
	}
}