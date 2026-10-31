#pragma once

// Ported from MFG-Unlocker (source/native/xess/port/shader_port.h), MIT licensed, see NOTICE.md.
//
// XeFG decodes its compute programs through one cache function, called from one site. That call is redirected here so
// the program keys the ported MFG model asks for come from the pinned program set; every other key is decoded by XeFG
// itself, unchanged. Install before any context and keep it for the process lifetime.
//
// Everything is checked before the site is written: the module must be one of the known instruction families, the
// call must reach XeFG's decoder, and its first bytes and the page's ownership must match.
#include "MfgPortLog.h"
#include "RuntimeLayout.h"
#include "RuntimePayload.h"

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

class ShaderPort
{
	using Decode = void (*)(void*, const void*, void*);
	using Resize = void (*)(void*, size_t);

	inline static ShaderPort* instance{};

	uintptr_t base{};
	unsigned char* site{};
	void* relay{};
	LONG original{}, replacement{};
	DWORD protection{};
	Decode decode{};
	Resize resize{};
	bool active{};
	std::atomic<unsigned> callers{};
	std::map<std::string, std::vector<unsigned char>> programs;

	std::mutex reportedMutex;
	std::set<std::string> reported;

	static void read(uintptr_t p, void* out, size_t count)
	{
		SIZE_T got{};

		if (!p || count > 1000000 || !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(p), out, count, &got) ||
			got != count)
		{
			throw std::runtime_error("shader_port_read");
		}
	}

	template <class T> static T get(uintptr_t p)
	{
		T out{};
		read(p, &out, sizeof(out));
		return out;
	}

	static std::string string(uintptr_t p)
	{
		const auto size = get<size_t>(p + 16), capacity = get<size_t>(p + 24);

		if (!size || size > 128 || capacity < size || capacity > 65536)
			throw std::runtime_error("shader_port_string");

		std::string out(size, '\0');
		read(capacity >= 16 ? get<uintptr_t>(p) : p, out.data(), size);

		for (auto c : out)
			if (c < ' ' || c > '~')
				throw std::runtime_error("shader_port_string_ascii");

		return out;
	}

	static void dispatch(void* store, const void* keyObject, void* output)
	{
		auto self = instance;
		if (!self)
			TerminateProcess(GetCurrentProcess(), 100);

		struct Call
		{
			std::atomic<unsigned>& count;
			explicit Call(std::atomic<unsigned>& c) : count(c) { ++count; }
			~Call() { --count; }
		} call(self->callers);

		const auto key = string(reinterpret_cast<uintptr_t>(keyObject));

		for (auto c : key)
			if (c < '0' || c > '9')
				throw std::runtime_error("shader_port_key");

		const auto found = self->programs.find(key);

		// Not ours, XeFG's own decoder runs as always
		if (found == self->programs.end())
		{
			self->decode(store, keyObject, output);
			return;
		}

		const auto& bytes = found->second;
		self->resize(output, bytes.size());

		auto start = get<uintptr_t>(reinterpret_cast<uintptr_t>(output));
		auto end = get<uintptr_t>(reinterpret_cast<uintptr_t>(output) + 8);
		auto cap = get<uintptr_t>(reinterpret_cast<uintptr_t>(output) + 16);

		if (!start || end - start != bytes.size() || cap < end)
			throw std::runtime_error("shader_port_output_allocation");

		memcpy(reinterpret_cast<void*>(start), bytes.data(), bytes.size());

		{
			std::scoped_lock lock(self->reportedMutex);

			if (self->reported.size() < 128 && self->reported.insert(key).second)
				mfg_port::Event("shader_supplied", "\"key\":\"" + key + "\",\"bytes\":" + std::to_string(bytes.size()));
		}
	}

	void change(LONG from, LONG to)
	{
		DWORD old{}, unused{};

		if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old))
			throw std::runtime_error("shader_port_protection");

		LONG prior = InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(site + 1), to, from);
		bool flushed = FlushInstructionCache(GetCurrentProcess(), site, 5) != FALSE;
		bool restored = VirtualProtect(site, 5, old, &unused) != FALSE;

		MEMORY_BASIC_INFORMATION state{};

		// A half applied site is not something to keep running on (same as MFG-Unlocker)
		if (prior != from || !flushed || !restored || old != protection ||
			VirtualQuery(site, &state, sizeof(state)) != sizeof(state) || state.Protect != protection ||
			get<LONG>(reinterpret_cast<uintptr_t>(site + 1)) != to)
		{
			TerminateProcess(GetCurrentProcess(), 101);
		}
	}

public:
	bool hasProgram(const char* key) const { return programs.contains(key); }

	ShaderPort(HMODULE module, const std::filesystem::path& programsRoot)
	{
		const auto& layout = xess_layout::Get(module);
		base = reinterpret_cast<uintptr_t>(module);
		site = reinterpret_cast<unsigned char*>(base + layout.Rva(0x1ad217));

		MEMORY_BASIC_INFORMATION info{};
		original = get<LONG>(reinterpret_cast<uintptr_t>(site + 1));

		const unsigned char decodeExpected[]{ 0x48, 0x89, 0x5c, 0x24, 0x20, 0x55, 0x56, 0x57, 0x48, 0x81, 0xec, 0xb0, 0, 0, 0 };
		const unsigned char resizeExpected[]{ 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x4c, 0x8b, 0x49, 0x08 };

		if (instance || *site != 0xe8 || reinterpret_cast<uintptr_t>(site + 5) + original != base + layout.Rva(0x11230) ||
			memcmp(reinterpret_cast<void*>(base + layout.Rva(0x19fda0)), decodeExpected, sizeof(decodeExpected)) ||
			memcmp(reinterpret_cast<void*>(base + layout.Rva(0x1a09c0)), resizeExpected, sizeof(resizeExpected)) ||
			VirtualQuery(site, &info, sizeof(info)) != sizeof(info) || info.AllocationBase != module ||
			info.Type != MEM_IMAGE || info.Protect != PAGE_EXECUTE_READ)
		{
			throw std::runtime_error("shader_port_identity");
		}

		protection = info.Protect;
		decode = reinterpret_cast<Decode>(base + layout.Rva(0x19fda0));
		resize = reinterpret_cast<Resize>(base + layout.Rva(0x1a09c0));
		programs = xess_payload::LoadPrograms(programsRoot);

		// The jump to dispatch has to be reachable from the call site with a rel32
		const auto callEnd = reinterpret_cast<uintptr_t>(site + 5);
		const uintptr_t low = callEnd > 0x7ffe0000 ? callEnd - 0x7ffe0000 : 0x10000;
		const uintptr_t high = callEnd + 0x7ffe0000;

		for (uintptr_t p = (low + 0xffff) & ~uintptr_t(0xffff); p < high;)
		{
			MEMORY_BASIC_INFORMATION region{};

			if (VirtualQuery(reinterpret_cast<void*>(p), &region, sizeof(region)) != sizeof(region))
				break;

			auto end = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;

			if (region.State == MEM_FREE && end - p >= 0x10000)
			{
				relay = VirtualAlloc(reinterpret_cast<void*>(p), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

				if (relay)
					break;
			}

			if (end <= p)
				break;

			p = (end + 0xffff) & ~uintptr_t(0xffff);
		}

		if (!relay)
			throw std::runtime_error("shader_port_relay_allocation");

		unsigned char stub[14]{ 0xff, 0x25, 0, 0, 0, 0 };
		const auto destination = reinterpret_cast<uintptr_t>(&dispatch);
		memcpy(stub + 6, &destination, 8);
		memcpy(relay, stub, sizeof(stub));

		DWORD prior{};

		if (!VirtualProtect(relay, 0x1000, PAGE_EXECUTE_READ, &prior) ||
			!FlushInstructionCache(GetCurrentProcess(), relay, sizeof(stub)))
		{
			VirtualFree(relay, 0, MEM_RELEASE);
			relay = nullptr;
			throw std::runtime_error("shader_port_relay_protection");
		}

		const auto distance = reinterpret_cast<intptr_t>(relay) - reinterpret_cast<intptr_t>(site + 5);

		if (distance < INT32_MIN || distance > INT32_MAX)
		{
			VirtualFree(relay, 0, MEM_RELEASE);
			relay = nullptr;
			throw std::runtime_error("shader_port_relay_range");
		}

		replacement = LONG(distance);
		instance = this;
		change(original, replacement);
		active = true;

		mfg_port::Event("shader_port_applied", "\"programs\":" + std::to_string(programs.size()));
	}

	~ShaderPort()
	{
		if (!active)
			return;

		if (callers.load() != 0)
			TerminateProcess(GetCurrentProcess(), 102);

		change(replacement, original);
		instance = nullptr;

		if (!VirtualFree(relay, 0, MEM_RELEASE))
			TerminateProcess(GetCurrentProcess(), 103);

		mfg_port::Event("shader_port_restored");
	}

	ShaderPort(const ShaderPort&) = delete;
	ShaderPort& operator=(const ShaderPort&) = delete;
};
