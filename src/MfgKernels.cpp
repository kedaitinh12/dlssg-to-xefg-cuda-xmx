#include "MfgKernels.h"
#include "Log.h"
#include "Util.h"
#include "MfgPort/ModelPort.h"
#include "MfgPort/RuntimePayload.h"
#include "MfgPort/RuntimeLayout.h"
#include "MfgPort/ShaderPort.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace
{
	// The 16 reconstruction program pairs the ported model needs from the payload (MFG-Unlocker, xess_mod.cpp)
	constexpr const char* ReconstructionPrograms[] = {
		"3518243144275597942266", "3518243145375109570477", "3519166589661394643131", "3519166588561883014920",
		"3515873413746975655382", "3515873414846487283593", "35115408591383412559519", "35115408590283900931308",
		"35118300971948070613363", "35118300970848558985152", "351747859676298538780", "351747860775810166991",
		"3517111067725881270903", "3517111066626369642692", "35110677435103722760616", "35110677436203234388827",
	};

	// The port patches the library's code and tables process wide, so it lives as long as the process does
	struct State
	{
		std::once_flag once;
		bool active = false;
		std::unique_ptr<ShaderPort> shaders;
		std::unique_ptr<ModelPort> model;
	};

	State& Get()
	{
		static auto* state = new State;
		return *state;
	}

	// next to the dll, either laid out flat or as MFG-Unlocker's build input has it
	std::filesystem::path FindPrograms()
	{
		auto root = ThisModulePath().parent_path() / L"xefg_mfg_programs";

		if (!std::filesystem::exists(root / "programs.tsv") && std::filesystem::exists(root / "programs" / "programs.tsv"))
			root /= "programs";

		return root;
	}

	void TryInstall(HMODULE module, State& state)
	{
		try
		{
			const auto root = FindPrograms();

			if (!std::filesystem::exists(root / "programs.tsv"))
			{
				Log::Info("No ported XMX MFG program set in {}, XeFG keeps its DP4a model for every generated frame",
					root.string());
				return;
			}

			// Decoder first: the model port makes XeFG ask for the ported programs right afterwards
			auto shaders = std::make_unique<ShaderPort>(module, root);

			for (const char* key : ReconstructionPrograms)
			{
				if (!shaders->hasProgram(key))
					throw std::runtime_error(std::string("missing reconstruction program ") + key);
			}

			auto model = std::make_unique<ModelPort>(module, true, true);

			state.shaders = std::move(shaders);
			state.model = std::move(model);
			state.active = true;

			Log::Info("Ported XMX MFG model installed, one optical flow field per generated batch");
		}
		catch (const std::exception& error)
		{
			Log::Warn("Ported XMX MFG model not installed ({}), XeFG keeps its DP4a model", error.what());
		}
	}
} // namespace

bool MfgKernels::Install(HMODULE module)
{
	auto& state = Get();

	std::call_once(state.once, [module, &state] { TryInstall(module, state); });

	return state.active;
}
