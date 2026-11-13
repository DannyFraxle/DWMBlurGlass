/**
 * FileName: OffsetCache.h
 *
 * This file is part of DWMBlurGlass.
 * DWMBlurGlass is free software: you can redistribute it and/or modify it under the terms of the
 * GNU Lesser General Public License as published by the Free Software Foundation, either version 3
 * of the License, or any later version.
 *
 * Per-build cache of the hooked symbol offsets, plus a byte-pattern fallback that re-locates them
 * in a new build of dwmcore.dll / uDwm.dll without downloading that build's PDB.
 *
 * How it works:
 *  - Whenever offsets are resolved (from a PDB, or by this fallback), the cache records, for that
 *    exact DLL build (PDB GUID+age), each symbol's RVA plus a masked byte pattern of the function
 *    body ("self" pattern). Bytes that legitimately change between builds even when the function
 *    did not - rel32 call/jump distances, RIP-relative displacements and relocated absolute
 *    addresses - are masked out.
 *  - Symbols whose self pattern is not unique in the image (tiny getters) and data symbols get
 *    "anchors": a unique function that references them, plus where in it the reference sits.
 *  - On an unknown build, every symbol must be re-found from a previous build's patterns with
 *    exactly one match, every code symbol's body must still match its self pattern (so its code
 *    is unchanged and the hook is still valid), and all locators must agree. Any failure -> the
 *    caller falls back to downloading the PDB as before.
 *  - The fallback is only tried between builds with the same OS build number (cumulative updates).
 *    Feature updates always go through the PDB.
*/
#pragma once
#include "../framework.h"
#include <string>
#include <vector>
#include <utility>

namespace MDWMBlurGlass::OffsetCache
{
	// (index in g_hookFunList, symbol name) for the symbols of one module.
	using NameList = std::vector<std::pair<size_t, std::string>>;

	enum class Source
	{
		None,
		Pdb,		// recorded from the build's PDB
		Pattern		// derived by matching a previous build's patterns
	};

	// Fill `offsets` (indexed like g_hookFunList) from the cache entry for the currently installed
	// build of `dllName`. Returns Source::None when there is no complete entry for this build.
	Source LoadExact(std::wstring_view dllName, const NameList& names, std::vector<DWORD64>& offsets);

	// Record the offsets of the currently installed build (fills the cache for future builds).
	void Record(std::wstring_view dllName, const NameList& names, const std::vector<DWORD64>& offsets, Source source);

	// Try to resolve every symbol of the currently installed build from previous builds' patterns.
	// On success fills `offsets`, records the build and returns true.
	bool Derive(std::wstring_view dllName, const NameList& names, std::vector<DWORD64>& offsets);

	// True if the matching PDB for the currently installed build is already in data\symbols.
	bool LocalPdbExists(std::wstring_view dllName);
}
