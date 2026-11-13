/**
 * FileName: OffsetCache.cpp
 *
 * This file is part of DWMBlurGlass.
 * DWMBlurGlass is free software: you can redistribute it and/or modify it under the terms of the
 * GNU Lesser General Public License as published by the Free Software Foundation, either version 3
 * of the License, or any later version.
 *
 * See OffsetCache.h for an overview.
*/
#include "OffsetCache.h"
#include "Common.h"
#include "../../DWMBlurGlassExt/minhook/hde/hde64.h"
#include <ImageHlp.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <unordered_map>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "version.lib")

namespace fs = std::filesystem;

namespace MDWMBlurGlass::OffsetCache
{
	namespace
	{
		constexpr int kFormatVersion = 1;
		constexpr DWORD kMaxSelfLen = 160;		// bytes of a function body kept as its self pattern
		constexpr DWORD kMaxAnchorRefEnd = 384;	// only use references within this many bytes of an anchor's start
		constexpr DWORD kAnchorTail = 24;		// extra bytes kept after the referencing instruction
		constexpr size_t kMinFixed = 16;		// a locator pattern needs at least this many unmasked bytes
		constexpr size_t kMaxAnchors = 2;
		constexpr size_t kAnchorTries = 24;
		constexpr size_t kKeepBuilds = 4;		// cached builds kept per module

		// ---------- misc helpers ----------

		std::wstring CacheDir()
		{
			return Utils::GetCurrentDir() + L"\\data\\symbols\\offsetcache";
		}

		std::string Narrow(std::wstring_view s)
		{
			std::string r;
			r.reserve(s.size());
			for (wchar_t c : s)
				r.push_back(c < 0x80 ? static_cast<char>(c) : '_');
			return r;
		}

		std::string Lower(std::string s)
		{
			std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
			return s;
		}

		std::vector<std::string> Split(const std::string& s, char sep)
		{
			std::vector<std::string> out;
			size_t start = 0;
			while (true)
			{
				size_t pos = s.find(sep, start);
				out.push_back(s.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
				if (pos == std::string::npos)
					break;
				start = pos + 1;
			}
			return out;
		}

		void Log(std::wstring_view dllName, const std::string& msg)
		{
			try
			{
				fs::create_directories(CacheDir());
				const fs::path logPath = CacheDir() + L"\\offsetcache.log";
				std::error_code ec;
				if (fs::exists(logPath, ec) && fs::file_size(logPath, ec) > 256 * 1024)
					fs::remove(logPath, ec);

				SYSTEMTIME st{};
				GetLocalTime(&st);
				char stamp[32]{};
				sprintf_s(stamp, "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

				std::ofstream f(logPath, std::ios::app);
				f << stamp << " [" << Narrow(dllName) << "] " << msg << "\n";
			}
			catch (...) {}
		}

		// ---------- byte patterns ----------

		struct Pattern
		{
			std::vector<BYTE> bytes;
			std::vector<BYTE> mask;	// 1 = byte must match, 0 = wildcard

			size_t Size() const { return bytes.size(); }
			size_t Fixed() const { return static_cast<size_t>(std::ranges::count(mask, BYTE(1))); }

			bool MatchesAt(const BYTE* p) const
			{
				for (size_t i = 0; i < bytes.size(); ++i)
					if (mask[i] && p[i] != bytes[i])
						return false;
				return true;
			}

			std::string ToString() const
			{
				static constexpr char hex[] = "0123456789ABCDEF";
				std::string s;
				s.reserve(bytes.size() * 2);
				for (size_t i = 0; i < bytes.size(); ++i)
				{
					if (mask[i])
					{
						s.push_back(hex[bytes[i] >> 4]);
						s.push_back(hex[bytes[i] & 0xF]);
					}
					else
						s += "??";
				}
				return s;
			}

			static bool Parse(const std::string& s, Pattern& out)
			{
				out = {};
				if (s.size() % 2)
					return false;
				auto nib = [](char c) -> int
				{
					if (c >= '0' && c <= '9') return c - '0';
					if (c >= 'A' && c <= 'F') return c - 'A' + 10;
					if (c >= 'a' && c <= 'f') return c - 'a' + 10;
					return -1;
				};
				for (size_t i = 0; i < s.size(); i += 2)
				{
					if (s[i] == '?' && s[i + 1] == '?')
					{
						out.bytes.push_back(0);
						out.mask.push_back(0);
						continue;
					}
					const int hi = nib(s[i]), lo = nib(s[i + 1]);
					if (hi < 0 || lo < 0)
						return false;
					out.bytes.push_back(static_cast<BYTE>(hi << 4 | lo));
					out.mask.push_back(1);
				}
				return true;
			}
		};

		// ---------- the mapped DLL ----------

		struct Image
		{
			HMODULE mod = nullptr;
			const BYTE* base = nullptr;
			DWORD size = 0;
			std::vector<std::pair<DWORD, DWORD>> execRanges;	// [begin, end) RVAs
			std::vector<std::pair<DWORD, DWORD>> dataRanges;
			const IMAGE_RUNTIME_FUNCTION_ENTRY* pdata = nullptr;
			size_t pdataCount = 0;
			std::vector<DWORD> relocs;	// sorted RVAs of 64-bit absolute relocations
			std::string key;			// PDB GUID + age, symbol-store style
			std::wstring pdbName;
			WORD ver[4]{};

			Image() = default;
			Image(const Image&) = delete;
			Image& operator=(const Image&) = delete;
			~Image()
			{
				if (mod)
					FreeLibrary(mod);
			}

			bool Open(std::wstring_view dllName)
			{
				mod = LoadLibraryExW(std::wstring(dllName).c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES | LOAD_LIBRARY_SEARCH_SYSTEM32);
				if (!mod)
					return false;
				base = reinterpret_cast<const BYTE*>(mod);

				auto nt = ImageNtHeader(const_cast<BYTE*>(base));
				if (!nt || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
					return false;
				size = nt->OptionalHeader.SizeOfImage;

				auto sec = IMAGE_FIRST_SECTION(nt);
				for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
				{
					DWORD vs = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
					DWORD begin = sec[i].VirtualAddress, end = (std::min<DWORD>)(begin + vs, size);
					if (end <= begin)
						continue;
					if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
						execRanges.emplace_back(begin, end);
					else
						dataRanges.emplace_back(begin, end);
				}

				ULONG dirSize = 0;
				pdata = static_cast<const IMAGE_RUNTIME_FUNCTION_ENTRY*>(
					ImageDirectoryEntryToData(const_cast<BYTE*>(base), TRUE, IMAGE_DIRECTORY_ENTRY_EXCEPTION, &dirSize));
				pdataCount = pdata ? dirSize / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY) : 0;

				dirSize = 0;
				auto reloc = static_cast<const BYTE*>(
					ImageDirectoryEntryToData(const_cast<BYTE*>(base), TRUE, IMAGE_DIRECTORY_ENTRY_BASERELOC, &dirSize));
				for (ULONG off = 0; reloc && off + sizeof(IMAGE_BASE_RELOCATION) <= dirSize;)
				{
					auto block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(reloc + off);
					if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) || off + block->SizeOfBlock > dirSize)
						break;
					auto entries = reinterpret_cast<const WORD*>(block + 1);
					const size_t count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
					for (size_t i = 0; i < count; ++i)
					{
						if ((entries[i] >> 12) == IMAGE_REL_BASED_DIR64)
							relocs.push_back(block->VirtualAddress + (entries[i] & 0xFFF));
					}
					off += block->SizeOfBlock;
				}
				std::ranges::sort(relocs);

				if (!ReadDebugKey() || !ReadVersion())
					return false;
				return !execRanges.empty();
			}

			bool ReadDebugKey()
			{
				ULONG dirSize = 0;
				auto dbg = static_cast<const IMAGE_DEBUG_DIRECTORY*>(
					ImageDirectoryEntryToData(const_cast<BYTE*>(base), TRUE, IMAGE_DIRECTORY_ENTRY_DEBUG, &dirSize));
				if (!dbg)
					return false;
				for (size_t i = 0; i < dirSize / sizeof(IMAGE_DEBUG_DIRECTORY); ++i)
				{
					if (dbg[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || !dbg[i].AddressOfRawData || dbg[i].SizeOfData < 24)
						continue;
					auto cv = base + dbg[i].AddressOfRawData;
					if (*reinterpret_cast<const DWORD*>(cv) != 0x53445352) // RSDS
						continue;
					GUID guid;
					memcpy(&guid, cv + 4, sizeof(GUID));
					DWORD age = *reinterpret_cast<const DWORD*>(cv + 20);
					const char* path = reinterpret_cast<const char*>(cv + 24);

					char buf[64]{};
					sprintf_s(buf, "%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%X",
						guid.Data1, guid.Data2, guid.Data3,
						guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
						guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7], age);
					key = buf;

					std::string p(path, strnlen(path, dbg[i].SizeOfData - 24));
					if (auto slash = p.find_last_of("\\/"); slash != std::string::npos)
						p = p.substr(slash + 1);
					pdbName.assign(p.begin(), p.end());
					return !pdbName.empty();
				}
				return false;
			}

			bool ReadVersion()
			{
				wchar_t path[MAX_PATH]{};
				if (!GetModuleFileNameW(mod, path, MAX_PATH))
					return false;
				DWORD handle = 0;
				DWORD len = GetFileVersionInfoSizeW(path, &handle);
				if (!len)
					return false;
				std::vector<BYTE> data(len);
				if (!GetFileVersionInfoW(path, 0, len, data.data()))
					return false;
				VS_FIXEDFILEINFO* info = nullptr;
				UINT infoLen = 0;
				if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoLen) || !info)
					return false;
				ver[0] = HIWORD(info->dwFileVersionMS);
				ver[1] = LOWORD(info->dwFileVersionMS);
				ver[2] = HIWORD(info->dwFileVersionLS);
				ver[3] = LOWORD(info->dwFileVersionLS);
				return true;
			}

			std::string VersionString() const
			{
				char buf[48]{};
				sprintf_s(buf, "%u.%u.%u.%u", ver[0], ver[1], ver[2], ver[3]);
				return buf;
			}

			static bool InRanges(const std::vector<std::pair<DWORD, DWORD>>& ranges, DWORD rva, DWORD len)
			{
				for (auto& [b, e] : ranges)
					if (rva >= b && rva < e && len <= e - rva)
						return true;
				return false;
			}
			bool InExec(DWORD rva, DWORD len = 1) const { return InRanges(execRanges, rva, len); }
			bool InData(DWORD rva, DWORD len = 1) const { return InRanges(dataRanges, rva, len); }

			// End RVA of the function starting exactly at `rva`, or 0 if it has no unwind entry.
			DWORD FuncEnd(DWORD rva) const
			{
				size_t lo = 0, hi = pdataCount;
				while (lo < hi)
				{
					size_t mid = (lo + hi) / 2;
					if (pdata[mid].BeginAddress < rva)
						lo = mid + 1;
					else
						hi = mid;
				}
				return lo < pdataCount && pdata[lo].BeginAddress == rva ? pdata[lo].EndAddress : 0;
			}
		};

		// ---------- decoding ----------

		bool Decode(const Image& img, DWORD rva, hde64s& hs)
		{
			// hde64 may look at up to 15 bytes; keep that inside the mapped image.
			if (!img.InExec(rva, 1) || rva + 15 >= img.size)
				return false;
			hde64_disasm(img.base + rva, &hs);
			return !(hs.flags & F_ERROR) && hs.len && img.InExec(rva, hs.len);
		}

		// Address-dependent 4-byte field of an instruction: a rel32 branch/call or a RIP-relative
		// displacement. Returns its offset inside the instruction and the RVA it points to.
		bool RefField(const hde64s& hs, DWORD insnRva, DWORD& fieldOff, DWORD& target)
		{
			if ((hs.flags & F_RELATIVE) && (hs.flags & F_IMM32))
			{
				fieldOff = hs.len - 4u;
				target = static_cast<DWORD>(static_cast<INT64>(insnRva) + hs.len + static_cast<INT32>(hs.imm.imm32));
				return true;
			}
			if ((hs.flags & F_MODRM) && (hs.flags & F_DISP32) && hs.modrm_mod == 0 && hs.modrm_rm == 5)
			{
				DWORD immSize = 0;
				if (hs.flags & F_IMM8) immSize += 1;
				if (hs.flags & F_IMM16) immSize += 2;
				if (hs.flags & F_IMM32) immSize += 4;
				if (hs.flags & F_IMM64) immSize += 8;
				if (hs.len < immSize + 4u)
					return false;
				fieldOff = hs.len - immSize - 4u;
				target = static_cast<DWORD>(static_cast<INT64>(insnRva) + hs.len + static_cast<INT32>(hs.disp.disp32));
				return true;
			}
			return false;
		}

		// Masked pattern of the code starting at `start`, at most `maxLen` bytes, whole instructions only.
		bool MakePattern(const Image& img, DWORD start, DWORD maxLen, Pattern& pat)
		{
			pat = {};
			const DWORD funcEnd = img.FuncEnd(start);
			DWORD limit = start + maxLen;
			if (funcEnd)
				limit = (std::min)(limit, funcEnd);

			DWORD cur = start;
			while (cur < limit)
			{
				hde64s hs{};
				if (!Decode(img, cur, hs) || cur + hs.len > limit)
					break;
				const size_t at = pat.bytes.size();
				pat.bytes.insert(pat.bytes.end(), img.base + cur, img.base + cur + hs.len);
				pat.mask.insert(pat.mask.end(), hs.len, BYTE(1));

				DWORD fieldOff = 0, target = 0;
				if (RefField(hs, cur, fieldOff, target))
					std::fill_n(pat.mask.begin() + at + fieldOff, 4, BYTE(0));

				cur += hs.len;

				// Without unwind info we don't know where the function ends: stop at the first
				// return / tail jump / padding.
				if (!funcEnd && (hs.opcode == 0xC3 || hs.opcode == 0xC2 || hs.opcode == 0xCC || hs.opcode == 0xE9 || hs.opcode == 0xEB))
					break;
			}

			// Absolute addresses (jump tables, mov r64, imm64) change with the load address.
			auto it = std::ranges::lower_bound(img.relocs, start >= 7 ? start - 7 : 0);
			for (; it != img.relocs.end() && *it < cur; ++it)
			{
				for (DWORD b = *it; b < *it + 8; ++b)
					if (b >= start && b < cur)
						pat.mask[b - start] = 0;
			}
			return !pat.bytes.empty();
		}

		// Number of places the pattern matches in executable code (0, 1, or 2 meaning "2 or more").
		int Find(const Image& img, const Pattern& pat, DWORD& firstMatch)
		{
			size_t bestOff = 0, bestLen = 0;
			for (size_t i = 0; i < pat.Size();)
			{
				if (!pat.mask[i]) { ++i; continue; }
				size_t j = i;
				while (j < pat.Size() && pat.mask[j]) ++j;
				if (j - i > bestLen) { bestLen = j - i; bestOff = i; }
				i = j;
			}
			if (bestLen < 4)
				return 2; // too weak to search for: treat as ambiguous

			const std::boyer_moore_horspool_searcher searcher(pat.bytes.begin() + bestOff, pat.bytes.begin() + bestOff + bestLen);
			int count = 0;
			for (auto& [b, e] : img.execRanges)
			{
				const BYTE* first = img.base + b;
				const BYTE* last = img.base + e;
				for (const BYTE* it = first;;)
				{
					auto [hit, hitEnd] = searcher(it, last);
					if (hit == last)
						break;
					const BYTE* cand = hit - bestOff;
					if (cand >= first && static_cast<size_t>(last - cand) >= pat.Size() && pat.MatchesAt(cand))
					{
						if (count == 0)
							firstMatch = static_cast<DWORD>(cand - img.base);
						if (++count >= 2)
							return 2;
					}
					it = hit + 1;
				}
			}
			return count;
		}

		bool MatchesAt(const Image& img, DWORD rva, const Pattern& pat)
		{
			return img.InExec(rva, static_cast<DWORD>(pat.Size())) && pat.MatchesAt(img.base + rva);
		}

		// ---------- cache file ----------

		struct Anchor
		{
			Pattern pat;		// pattern of the referencing function, from its start
			DWORD refOff = 0;	// offset of the 4-byte rel32/disp32 field within it
			DWORD insnEnd = 0;	// offset of the end of that instruction
		};

		struct Entry
		{
			size_t index = 0;
			std::string name;
			DWORD rva = 0;
			char kind = '0';	// 'C' code, 'D' data, '0' not present in this build
			Pattern self;		// code only: body pattern, always verified
			bool direct = false;// self pattern was unique in its build: usable as a locator
			std::vector<Anchor> anchors;
		};

		struct CacheFile
		{
			std::string module;
			std::string key;
			WORD ver[4]{};
			Source source = Source::None;
			std::map<size_t, Entry> entries;
		};

		fs::path CachePath(std::wstring_view dllName, const std::string& key)
		{
			return fs::path(CacheDir()) / (Lower(Narrow(dllName)) + "." + key + ".txt");
		}

		bool ReadCache(const fs::path& path, CacheFile& out, bool headerOnly = false) try
		{
			std::ifstream f(path);
			if (!f)
				return false;
			out = {};
			std::string line;
			if (!std::getline(f, line) || line != "DWMBGOC|" + std::to_string(kFormatVersion))
				return false;
			while (std::getline(f, line))
			{
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				auto p = Split(line, '|');
				if (p.empty())
					continue;
				if (p[0] == "module" && p.size() == 2) out.module = p[1];
				else if (p[0] == "build" && p.size() == 2) out.key = p[1];
				else if (p[0] == "version" && p.size() == 2)
				{
					unsigned a = 0, b = 0, c = 0, d = 0;
					if (sscanf_s(p[1].c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
						return false;
					out.ver[0] = WORD(a); out.ver[1] = WORD(b); out.ver[2] = WORD(c); out.ver[3] = WORD(d);
				}
				else if (p[0] == "source" && p.size() == 2)
					out.source = p[1] == "pdb" ? Source::Pdb : p[1] == "pattern" ? Source::Pattern : Source::None;
				else if (headerOnly)
					break;
				else if (p[0] == "E" && p.size() == 7)
				{
					Entry e;
					e.index = std::stoul(p[1]);
					e.name = p[2];
					e.rva = std::stoul(p[3], nullptr, 16);
					e.kind = p[4].empty() ? '0' : p[4][0];
					if (!p[5].empty() && !Pattern::Parse(p[5], e.self))
						return false;
					e.direct = p[6] == "1";
					out.entries[e.index] = std::move(e);
				}
				else if (p[0] == "A" && p.size() == 5)
				{
					auto it = out.entries.find(std::stoul(p[1]));
					if (it == out.entries.end())
						return false;
					Anchor a;
					a.refOff = std::stoul(p[2]);
					a.insnEnd = std::stoul(p[3]);
					if (!Pattern::Parse(p[4], a.pat) || a.refOff + 4 > a.pat.Size() || a.insnEnd > a.pat.Size())
						return false;
					it->second.anchors.push_back(std::move(a));
				}
			}
			return !out.key.empty() && out.source != Source::None;
		}
		catch (...)
		{
			return false; // corrupt file: ignore it
		}

		bool WriteCache(std::wstring_view dllName, const Image& img, const std::vector<Entry>& entries, Source source)
		{
			fs::create_directories(CacheDir());
			const fs::path path = CachePath(dllName, img.key);
			const fs::path tmp = path.wstring() + L".tmp";
			{
				std::ofstream f(tmp, std::ios::trunc);
				if (!f)
					return false;
				f << "DWMBGOC|" << kFormatVersion << "\n";
				f << "module|" << Lower(Narrow(dllName)) << "\n";
				f << "build|" << img.key << "\n";
				f << "version|" << img.VersionString() << "\n";
				f << "source|" << (source == Source::Pdb ? "pdb" : "pattern") << "\n";
				for (auto& e : entries)
				{
					char rva[16]{};
					sprintf_s(rva, "%X", e.rva);
					f << "E|" << e.index << "|" << e.name << "|" << rva << "|" << e.kind << "|"
						<< (e.kind == 'C' ? e.self.ToString() : "") << "|" << (e.direct ? 1 : 0) << "\n";
					for (auto& a : e.anchors)
						f << "A|" << e.index << "|" << a.refOff << "|" << a.insnEnd << "|" << a.pat.ToString() << "\n";
				}
				if (!f)
					return false;
			}
			std::error_code ec;
			fs::rename(tmp, path, ec);
			if (ec)
			{
				fs::remove(tmp, ec);
				return false;
			}

			// Keep only the newest few builds of this module.
			const std::string prefix = Lower(Narrow(dllName)) + ".";
			std::vector<std::pair<fs::file_time_type, fs::path>> files;
			for (auto& de : fs::directory_iterator(CacheDir(), ec))
			{
				const std::string fn = Lower(de.path().filename().string());
				if (fn.starts_with(prefix) && fn.ends_with(".txt"))
					files.emplace_back(de.last_write_time(ec), de.path());
			}
			std::ranges::sort(files, std::greater{}, &std::pair<fs::file_time_type, fs::path>::first);
			for (size_t i = kKeepBuilds; i < files.size(); ++i)
				fs::remove(files[i].second, ec);
			return true;
		}

		// ---------- building entries for the current build ----------

		std::vector<Entry> BuildEntries(const Image& img, const NameList& names, const std::vector<DWORD64>& offsets)
		{
			std::vector<Entry> entries;
			entries.reserve(names.size());

			// target RVA -> indices into `entries` that need anchors
			std::unordered_map<DWORD, std::vector<size_t>> needAnchor;

			for (auto& [index, name] : names)
			{
				Entry e;
				e.index = index;
				e.name = name;
				e.rva = index < offsets.size() ? static_cast<DWORD>(offsets[index]) : 0;
				if (e.rva == 0)
					e.kind = '0';
				else if (img.InExec(e.rva))
				{
					e.kind = 'C';
					MakePattern(img, e.rva, kMaxSelfLen, e.self);
					DWORD match = 0;
					e.direct = e.self.Fixed() >= kMinFixed && Find(img, e.self, match) == 1 && match == e.rva;
				}
				else if (img.InData(e.rva))
					e.kind = 'D';
				else
					e.kind = '0'; // outside any section: never trust it for matching

				if (e.kind == 'D' || (e.kind == 'C' && !e.direct))
					needAnchor[e.rva].push_back(entries.size());
				entries.push_back(std::move(e));
			}

			if (needAnchor.empty())
				return entries;

			// One pass over every function with unwind info, collecting references to the targets
			// near each function's start.
			struct Cand { DWORD begin, refOff, insnEnd; };
			std::unordered_map<DWORD, std::vector<Cand>> cands;
			for (size_t i = 0; i < img.pdataCount; ++i)
			{
				const DWORD b = img.pdata[i].BeginAddress, e = img.pdata[i].EndAddress;
				for (DWORD cur = b; cur < e && cur - b < kMaxAnchorRefEnd;)
				{
					hde64s hs{};
					if (!Decode(img, cur, hs))
						break;
					DWORD fieldOff = 0, target = 0;
					if (RefField(hs, cur, fieldOff, target) && target != b && needAnchor.contains(target))
						cands[target].push_back({ b, cur - b + fieldOff, cur - b + hs.len });
					cur += hs.len;
				}
			}

			for (auto& [target, idxs] : needAnchor)
			{
				auto it = cands.find(target);
				if (it == cands.end())
					continue;
				auto& list = it->second;
				std::ranges::sort(list, {}, &Cand::insnEnd);

				std::vector<Anchor> anchors;
				std::vector<DWORD> used;
				size_t tries = 0;
				for (auto& c : list)
				{
					if (anchors.size() >= kMaxAnchors || tries >= kAnchorTries)
						break;
					if (std::ranges::find(used, c.begin) != used.end())
						continue;
					++tries;
					Anchor a;
					a.refOff = c.refOff;
					a.insnEnd = c.insnEnd;
					if (!MakePattern(img, c.begin, c.insnEnd + kAnchorTail, a.pat) || a.pat.Size() < c.insnEnd || a.pat.Fixed() < kMinFixed)
						continue;
					DWORD match = 0;
					if (Find(img, a.pat, match) != 1 || match != c.begin)
						continue;
					used.push_back(c.begin);
					anchors.push_back(std::move(a));
				}
				for (size_t idx : idxs)
					entries[idx].anchors = anchors;
			}
			return entries;
		}
	}

	Source LoadExact(std::wstring_view dllName, const NameList& names, std::vector<DWORD64>& offsets) try
	{
		Image img;
		if (!img.Open(dllName))
			return Source::None;

		CacheFile cache;
		if (!ReadCache(CachePath(dllName, img.key), cache) || cache.key != img.key)
			return Source::None;

		std::vector<DWORD64> result(offsets.size(), 0);
		for (auto& [index, name] : names)
		{
			auto it = cache.entries.find(index);
			if (it == cache.entries.end() || it->second.name != name || index >= result.size())
				return Source::None; // the hook list changed since this build was cached
			result[index] = it->second.rva;
		}
		for (auto& [index, _] : names)
			offsets[index] = result[index];
		return cache.source;
	}
	catch (...)
	{
		return Source::None;
	}

	void Record(std::wstring_view dllName, const NameList& names, const std::vector<DWORD64>& offsets, Source source) try
	{
		Image img;
		if (!img.Open(dllName))
		{
			Log(dllName, "record: cannot map the DLL");
			return;
		}
		const auto start = std::chrono::steady_clock::now();
		auto entries = BuildEntries(img, names, offsets);

		size_t code = 0, data = 0, absent = 0, unlocatable = 0;
		for (auto& e : entries)
		{
			if (e.kind == 'C') ++code;
			else if (e.kind == 'D') ++data;
			else ++absent;
			if (e.kind != '0' && !e.direct && e.anchors.empty())
				++unlocatable;
		}
		const bool ok = WriteCache(dllName, img, entries, source);
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

		std::ostringstream msg;
		msg << "record " << img.VersionString() << " " << img.key << " from " << (source == Source::Pdb ? "pdb" : "pattern")
			<< ": " << code << " code, " << data << " data, " << absent << " absent, "
			<< unlocatable << " without a locator, " << ms << " ms" << (ok ? "" : " (WRITE FAILED)");
		Log(dllName, msg.str());
	}
	catch (const std::exception& ex)
	{
		Log(dllName, std::string("record failed: ") + ex.what());
	}
	catch (...)
	{
		Log(dllName, "record failed");
	}

	bool Derive(std::wstring_view dllName, const NameList& names, std::vector<DWORD64>& offsets) try
	{
		Image img;
		if (!img.Open(dllName))
			return false;

		// Previous builds of the same OS build number, newest first.
		std::vector<CacheFile> caches;
		{
			const std::string prefix = Lower(Narrow(dllName)) + ".";
			std::error_code ec;
			std::vector<std::pair<WORD, fs::path>> candidates;
			for (auto& de : fs::directory_iterator(CacheDir(), ec))
			{
				const std::string fn = Lower(de.path().filename().string());
				if (!fn.starts_with(prefix) || !fn.ends_with(".txt"))
					continue;
				CacheFile hdr;
				if (!ReadCache(de.path(), hdr, true) || hdr.key == img.key)
					continue;
				if (hdr.ver[0] != img.ver[0] || hdr.ver[1] != img.ver[1] || hdr.ver[2] != img.ver[2])
					continue;
				candidates.emplace_back(hdr.ver[3], de.path());
			}
			std::ranges::sort(candidates, std::greater{}, &std::pair<WORD, fs::path>::first);
			for (auto& [_, path] : candidates)
			{
				CacheFile c;
				if (ReadCache(path, c))
					caches.push_back(std::move(c));
			}
		}
		if (caches.empty())
		{
			Log(dllName, "derive " + img.VersionString() + ": no cached build with the same OS build number");
			return false;
		}

		std::vector<DWORD64> result(offsets.size(), 0);
		size_t viaDirect = 0, viaAnchor = 0, absent = 0;
		std::string failed;
		for (auto& [index, name] : names)
		{
			bool decided = false;
			for (auto& cache : caches)
			{
				auto it = cache.entries.find(index);
				if (it == cache.entries.end() || it->second.name != name)
					continue;
				const Entry& e = it->second;
				if (e.kind == '0')
				{
					decided = true; // not present in that build either
					++absent;
					break;
				}

				std::optional<DWORD> found;
				bool conflict = false, fromAnchor = false;
				auto consider = [&](DWORD rva, bool anchor)
				{
					const bool valid = e.kind == 'C' ? MatchesAt(img, rva, e.self) : img.InData(rva);
					if (!valid)
						return;
					if (found && *found != rva)
						conflict = true;
					else if (!found)
					{
						found = rva;
						fromAnchor = anchor;
					}
				};

				DWORD match = 0;
				if (e.kind == 'C' && e.direct && Find(img, e.self, match) == 1)
					consider(match, false);
				for (auto& a : e.anchors)
				{
					if (Find(img, a.pat, match) != 1)
						continue;
					const INT32 rel = *reinterpret_cast<const INT32*>(img.base + match + a.refOff);
					consider(static_cast<DWORD>(static_cast<INT64>(match) + a.insnEnd + rel), true);
				}

				if (conflict)
				{
					Log(dllName, "derive " + img.VersionString() + ": locators disagree for " + name + ", giving up");
					return false;
				}
				if (found)
				{
					result[index] = *found;
					fromAnchor ? ++viaAnchor : ++viaDirect;
					decided = true;
					break;
				}
			}
			if (!decided)
				failed += (failed.empty() ? "" : ", ") + name;
		}
		if (!failed.empty())
		{
			Log(dllName, "derive " + img.VersionString() + ": PDB needed, could not re-locate (changed in this build?): " + failed);
			return false;
		}

		for (auto& [index, _] : names)
			offsets[index] = result[index];

		std::ostringstream msg;
		msg << "derive " << img.VersionString() << " " << img.key << ": OK from " << caches.size() << " cached build(s), "
			<< viaDirect << " direct, " << viaAnchor << " via anchor, " << absent << " absent";
		Log(dllName, msg.str());

		Record(dllName, names, offsets, Source::Pattern);
		return true;
	}
	catch (const std::exception& ex)
	{
		Log(dllName, std::string("derive failed: ") + ex.what());
		return false;
	}
	catch (...)
	{
		return false;
	}

	bool LocalPdbExists(std::wstring_view dllName) try
	{
		Image img;
		if (!img.Open(dllName))
			return false;
		const std::wstring dir = Utils::GetCurrentDir() + L"\\data\\symbols\\" + img.pdbName + L"\\";
		const std::wstring keyHex(img.key.begin(), img.key.end());

		// SymbolDownloader writes the age in decimal; the symbol store layout uses hex. Accept either.
		std::wstring keyDec = keyHex.substr(0, 32);
		{
			const DWORD age = std::stoul(img.key.substr(32), nullptr, 16);
			keyDec += std::to_wstring(age);
		}
		std::error_code ec;
		return fs::exists(dir + keyHex + L"\\" + img.pdbName, ec) || fs::exists(dir + keyDec + L"\\" + img.pdbName, ec);
	}
	catch (...)
	{
		return false;
	}
}
