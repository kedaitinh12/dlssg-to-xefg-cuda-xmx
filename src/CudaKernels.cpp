#include "CudaKernels.h"
#include "Log.h"
#include "Util.h"
#include "CudaRoute/CudaRoute.h"
#include "MfgPort/MfgPortLog.h"
#include "MfgPort/RuntimePayload.h"

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	// MFG-Unlocker's build of xess/cuda/xefg_cuda_layers.cu (generate_cuda_layers.py, PINNED_FATBIN): SASS for sm_75,
	// sm_86, sm_89 and sm_120 from nvcc 13.2. A different file is refused, the route's layer table is written for
	// exactly these kernels
	constexpr char FatbinSha256[] = "2ffae52e7878ae7e374ff953b864204f079c3199dc904b2ce7ecf8b37ee130f7";
	constexpr size_t FatbinBytes = 1122728;
	constexpr wchar_t FatbinName[] = L"xefg_cuda_layers.fatbin";

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
	std::filesystem::path FindFatbin()
	{
		const auto dll = ThisModulePath().parent_path();
		const auto root = dll / L"xefg_mfg_programs";

		for (const auto& candidate : { root / L"cuda" / FatbinName, root / FatbinName, dll / FatbinName })
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

			// Trying kernels of your own: DLSSG_TO_XEFG_TENSOR_CORES_FATBIN names the file, which is then not held to the pin
			std::filesystem::path path;
			bool pinned = true;
			wchar_t custom[1024]{};
			const DWORD length = GetEnvironmentVariableW(L"DLSSG_TO_XEFG_TENSOR_CORES_FATBIN", custom, ARRAYSIZE(custom));

			if (length > 0 && length < ARRAYSIZE(custom))
			{
				path = custom;
				pinned = false;
			}
			else
				path = FindFatbin();

			if (path.empty())
			{
				Log::Info("No {} in xefg_mfg_programs\\cuda, the ported model's layers run as D3D12 compute",
					std::filesystem::path(FatbinName).string());
				xefg_cuda::Disable("no_fatbin");
				return;
			}

			auto bytes = ReadFile(path);
			const auto hash = xess_payload::Hash(bytes);

			if (pinned ? (bytes.size() != FatbinBytes || hash != FatbinSha256) : !LooksLikeFatbin(bytes))
			{
				Log::Warn("{} is not {}, tensor cores stay off", path.string(),
					pinned ? "MFG-Unlocker's pinned xefg_cuda_layers.fatbin" : "a CUDA fatbin");
				xefg_cuda::Disable(pinned ? "fatbin_not_pinned" : "fatbin_invalid");
				return;
			}

			if (!pinned)
				Log::Warn("Tensor cores use {} (SHA-256 {}), not MFG-Unlocker's pinned build. Its kernels must keep the names, "
					"block size and parameters of the pinned ones", path.string(), hash);

			state.fatbin = std::move(bytes);
			state.ownTables = OwnTablesOn();
			state.ready = true;
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
