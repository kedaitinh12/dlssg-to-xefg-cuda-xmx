#include "CudaKernels.h"
#include "Log.h"
#include "Util.h"
#include "CudaRoute/CudaRoute.h"
#include "MfgPort/MfgPortLog.h"
#include "MfgPort/RuntimePayload.h"

#include <array>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	// MFG-Unlocker's build of xess/cuda/xefg_cuda_layers.cu (generate_cuda_layers.py, PINNED_FATBIN): SASS for sm_75,
	// sm_86, sm_89 and sm_120 from nvcc 13.2. A different file is refused, the route's layer table is written for
	// exactly these kernels
	constexpr char FatbinSha256[] = "2ffae52e7878ae7e374ff953b864204f079c3199dc904b2ce7ecf8b37ee130f7";
	constexpr size_t FatbinBytes = 1122728;
	constexpr wchar_t FatbinName[] = L"xefg_cuda_layers.fatbin";

	// The kernel set that runs when nothing is asked for and its file is next to the dll: the variant build.yml compiles
	// with both changes (see src/CudaRoute/kernels). A missing or unusable file means the pinned build runs instead
	constexpr wchar_t AutoKernels[] = L"opt";

	// The kernels the route creates by name (kKernels in CudaRoute.cpp). A set that lacks one cannot be used
	constexpr const char* KernelNames[] = { "L_aa016078", "L_198d6bf8", "L_dcd8becf", "L_efbe3788", "L_50f0c205", "L_a3c3caa1",
		"L_9481d452", "L_06e97933", "L_3ed03daf", "L_748b0baf", "L_cb391582", "L_fc1b8a04", "L_3459485", "L_3452733",
		"L_flow16_acc17", "L_3453044" };

	struct State
	{
		std::once_flag once;
		bool ready = false; // the fatbin is found and pinned, and the route is not switched off
		bool ownTables = false; // also hook the game lists that keep a table of their own
		std::vector<unsigned char> fatbin;
	};

	// The route patches device and command list tables process wide, it lives as long as the process does
	State& Get()
	{
		static auto* state = new State;
		return *state;
	}

	void WriteLog(const char* stage, const std::string& fields) noexcept
	{
		try
		{
			mfg_port::Event(stage, fields);
		}
		catch (...)
		{
		}
	}

	// MFG-Unlocker's words for off and on, also what its OptiScaler.ini entries accept
	bool IsWord(const wchar_t* value, bool on)
	{
		static constexpr const wchar_t* Off[] = { L"off", L"0", L"false", L"no", L"disabled" };
		static constexpr const wchar_t* On[] = { L"on", L"1", L"true", L"yes", L"enabled" };

		for (const wchar_t* word : on ? On : Off)
		{
			if (_wcsicmp(value, word) == 0)
				return true;
		}

		return false;
	}

	bool IsOffWord(const wchar_t* value)
	{
		return IsWord(value, false);
	}

	// Environment variable first, then the entry in dlssg_to_xefg_intel_is_better.ini next to the dll. Empty if unset
	bool ReadSetting(const wchar_t* variable, const wchar_t* key, wchar_t (&value)[32])
	{
		value[0] = 0;

		if (GetEnvironmentVariableW(variable, value, ARRAYSIZE(value)) > 0)
			return true;

		value[0] = 0;
		const auto ini = ThisModulePath().replace_extension(L".ini");
		GetPrivateProfileStringW(L"XeFG", key, L"", value, ARRAYSIZE(value), ini.c_str());

		return value[0] != 0;
	}

	// Which kernel set: XeFGTensorCoresKernels in the ini, or the environment variable DLSSG_TO_XEFG_TENSOR_CORES_KERNELS.
	// "auto" (also when nothing is set) is the AutoKernels variant if its file is there; empty is the pinned build
	// (stock or pinned); any other plain name is the xefg_cuda_layers.<name>.fatbin the workflow built
	std::wstring RequestedKernels()
	{
		wchar_t value[32]{};

		if (!ReadSetting(L"DLSSG_TO_XEFG_TENSOR_CORES_KERNELS", L"XeFGTensorCoresKernels", value))
			return L"auto";

		std::wstring name = value;

		// The name goes into a file name: letters, digits, - and _ only
		for (const wchar_t c : name)
		{
			if (!iswalnum(c) && c != L'-' && c != L'_')
			{
				Log::Warn("XeFGTensorCoresKernels has to be a plain name, ignoring it");
				return L"auto";
			}
		}

		for (const wchar_t* pinned : { L"stock", L"pinned" })
		{
			if (_wcsicmp(name.c_str(), pinned) == 0)
				return {};
		}

		if (_wcsicmp(name.c_str(), L"auto") == 0)
			return L"auto";

		return name;
	}

	// Games whose command lists keep a table of their own (Agility SDK fast path lists). MFG-Unlocker never writes
	// those of a game's lists, because modules that validate them refuse the hooks, so this is opt-in
	bool OwnTablesOn()
	{
		wchar_t value[32]{};

		return ReadSetting(L"DLSSG_TO_XEFG_TENSOR_CORES_OWN_TABLES", L"XeFGTensorCoresOwnTables", value) && IsWord(value, true);
	}

	// [XeFG] XeFGTensorCores = off in dlssg_to_xefg_intel_is_better.ini next to the dll, or the environment variable
	// DLSSG_TO_XEFG_TENSOR_CORES set to off
	bool SwitchedOff(const char*& where)
	{
		wchar_t value[32]{};

		if (GetEnvironmentVariableW(L"DLSSG_TO_XEFG_TENSOR_CORES", value, ARRAYSIZE(value)) > 0 && IsOffWord(value))
		{
			where = "environment";
			return true;
		}

		value[0] = 0;
		const auto ini = ThisModulePath().replace_extension(L".ini");
		GetPrivateProfileStringW(L"XeFG", L"XeFGTensorCores", L"", value, ARRAYSIZE(value), ini.c_str());

		if (IsOffWord(value))
		{
			where = "ini";
			return true;
		}

		return false;
	}

	// Next to the programs, laid out the way the release zip has it or flat
	std::filesystem::path FindFatbin(const std::wstring& name)
	{
		const auto dll = ThisModulePath().parent_path();
		const auto root = dll / L"xefg_mfg_programs";

		for (const auto& candidate : { root / L"cuda" / name, root / name, dll / name })
		{
			std::error_code error;

			if (std::filesystem::is_regular_file(candidate, error))
				return candidate;
		}

		return {};
	}

	// The fatbin container's magic, and a size no kernel set of this kind comes near
	bool LooksLikeFatbin(const std::vector<unsigned char>& bytes)
	{
		return bytes.size() >= 16 && bytes.size() <= 16 * 1024 * 1024
			&& bytes[0] == 0x50 && bytes[1] == 0xed && bytes[2] == 0x55 && bytes[3] == 0xba;
	}

	// Every kernel the route asks for is in it by name (a kernel's name is a string of its cubins)
	bool HasKernels(const std::vector<unsigned char>& bytes)
	{
		const std::string_view data(reinterpret_cast<const char*>(bytes.data()), bytes.size());

		for (const char* name : KernelNames)
		{
			std::string needle(name);
			needle.push_back('\0');

			if (data.find(needle) == std::string_view::npos)
				return false;
		}

		return true;
	}

	std::vector<unsigned char> ReadFile(const std::filesystem::path& path)
	{
		std::ifstream file(path, std::ios::binary);

		if (!file)
			throw std::runtime_error("can't open " + path.string());

		std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});

		if (file.bad())
			throw std::runtime_error("can't read " + path.string());

		return bytes;
	}

	struct Candidate
	{
		std::filesystem::path path;
		bool pinned; // held to MFG-Unlocker's SHA-256 and size
		std::string label;
	};

	void Prepare(State& state)
	{
		xefg_cuda::SetLog(&WriteLog);

		try
		{
			const char* where = "";

			if (SwitchedOff(where))
			{
				Log::Info("Tensor cores switched off ({}), the ported model's layers run as D3D12 compute", where);
				xefg_cuda::Disable("switched_off");
				return;
			}

			// What to try, in order. A file named by DLSSG_TO_XEFG_TENSOR_CORES_FATBIN is tried alone, it is not held to
			// the pin and nothing replaces it. Otherwise the kernel set asked for (the AutoKernels variant when nothing is
			// asked for), then the pinned build: a variant that is missing or unusable falls back to it
			std::vector<Candidate> candidates;
			wchar_t custom[1024]{};
			const DWORD length = GetEnvironmentVariableW(L"DLSSG_TO_XEFG_TENSOR_CORES_FATBIN", custom, ARRAYSIZE(custom));

			if (length > 0 && length < ARRAYSIZE(custom))
				candidates.push_back({ custom, false, "file named by DLSSG_TO_XEFG_TENSOR_CORES_FATBIN" });
			else
			{
				std::wstring kernels = RequestedKernels();
				const bool automatic = kernels == L"auto";

				if (automatic)
					kernels = AutoKernels;

				if (!kernels.empty())
				{
					const std::wstring file = L"xefg_cuda_layers." + kernels + L".fatbin";
					const auto found = FindFatbin(file);

					if (!found.empty())
						candidates.push_back({ found, false, std::filesystem::path(kernels).string() + " kernel set" });
					else if (!automatic)
						Log::Warn("Kernel set {} is asked for, but there is no {} next to the dll. The pinned build is used",
							std::filesystem::path(kernels).string(), std::filesystem::path(file).string());
				}

				const auto pinnedPath = FindFatbin(FatbinName);

				if (!pinnedPath.empty())
					candidates.push_back({ pinnedPath, true, "pinned build" });
			}

			if (candidates.empty())
			{
				Log::Info("No {} in xefg_mfg_programs\\cuda, the ported model's layers run as D3D12 compute",
					std::filesystem::path(FatbinName).string());
				xefg_cuda::Disable("no_fatbin");
				return;
			}

			for (size_t i = 0; i < candidates.size(); ++i)
			{
				const auto& candidate = candidates[i];
				auto bytes = ReadFile(candidate.path);
				const auto hash = xess_payload::Hash(bytes);
				const bool usable = candidate.pinned ? (bytes.size() == FatbinBytes && hash == FatbinSha256)
					: (LooksLikeFatbin(bytes) && HasKernels(bytes));

				if (!usable)
				{
					Log::Warn("The {} {} is not {}{}", candidate.label, candidate.path.string(),
						candidate.pinned ? "MFG-Unlocker's pinned xefg_cuda_layers.fatbin" : "a CUDA fatbin with the route's kernels",
						i + 1 < candidates.size() ? ", trying the next one" : ", tensor cores stay off");
					continue;
				}

				if (candidate.pinned)
					Log::Info("Tensor cores use the {} ({})", candidate.label, candidate.path.string());
				else
					Log::Info("Tensor cores use the {} {} (SHA-256 {}), not MFG-Unlocker's pinned build. "
						"XeFGTensorCoresKernels = stock goes back to it", candidate.label, candidate.path.string(), hash);

				state.fatbin = std::move(bytes);
				state.ownTables = OwnTablesOn();
				state.ready = true;
				return;
			}

			xefg_cuda::Disable("fatbin_invalid");
		}
		catch (const std::exception& error)
		{
			Log::Warn("Tensor cores not available ({}), the ported model's layers run as D3D12 compute", error.what());
			xefg_cuda::Disable("error");
		}
	}

	std::mutex contextMutex;
	int contexts = 0;
} // namespace

bool CudaKernels::Install(ID3D12Device* device, HMODULE provider)
{
	auto& state = Get();

	std::call_once(state.once, [&state] { Prepare(state); });

	if (!state.ready)
		return false;

	std::string reason;
	const bool active = xefg_cuda::Install(device, provider, state.fatbin, reason);

	// The route logged why when it first gave up, this is for the context that is created right now
	if (!active)
		Log::Debug("Tensor cores not active for this context ({})", reason);

	return active;
}

void CudaKernels::Adopt(ID3D12GraphicsCommandList* cmdList)
{
	if (xefg_cuda::Active())
		xefg_cuda::AdoptRecordingList(cmdList, Get().ownTables);
}

void CudaKernels::Executed()
{
	static std::atomic<bool> reported;

	if (reported.load(std::memory_order_relaxed) || !xefg_cuda::Launched())
		return;

	if (reported.exchange(true))
		return;

	const auto status = xefg_cuda::Snapshot();
	Log::Info("Tensor cores are running: compute capability {}.{}, {} layer programs recognized, {} launches, {} fallbacks",
		status.major, status.minor, status.layers, status.launches, status.fallbacks);
}

void CudaKernels::ContextCreated()
{
	std::scoped_lock lock(contextMutex);
	contexts++;
}

void CudaKernels::ContextDestroyed()
{
	std::scoped_lock lock(contextMutex);

	if (--contexts > 0)
		return;

	contexts = 0;

	if (!xefg_cuda::Active())
		return;

	const auto status = xefg_cuda::Snapshot();
	Log::Info("Tensor cores: {} launches, {} fallbacks over this context's life", status.launches, status.fallbacks);

	// XeFG released the destroyed contexts' pipeline states, the route holds on to them until it is told
	xefg_cuda::ForgetLayers();
}
