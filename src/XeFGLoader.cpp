#include "XeFG.h"
#include "Log.h"
#include "Util.h"

#include <Psapi.h>

#include <vector>

namespace
{
	constexpr wchar_t XeFGDllName[] = L"libxess_fg.dll";

	std::filesystem::path ModulePath(HMODULE module)
	{
		wchar_t path[MAX_PATH]{};
		GetModuleFileNameW(module, path, MAX_PATH);
		return path;
	}

	// Every libxess_fg.dll in the process, the game and Streamline plugins can bring their own copies
	std::vector<HMODULE> LoadedXeFGModules()
	{
		std::vector<HMODULE> modules(1024);
		DWORD needed = 0;

		if (!EnumProcessModules(GetCurrentProcess(), modules.data(), (DWORD)(modules.size() * sizeof(HMODULE)), &needed))
			return {};

		modules.resize(std::min<size_t>(modules.size(), needed / sizeof(HMODULE)));
		std::erase_if(modules,
			[](HMODULE m) { return _wcsicmp(ModulePath(m).filename().c_str(), XeFGDllName) != 0; });

		return modules;
	}

	bool TryModule(HMODULE module)
	{
		Log::Info("Trying {}", ModulePath(module).string());

		if (XeFG::Resolve(module))
			return true;

		Log::Warn("Incompatible libxess_fg.dll");
		return false;
	}

	// OptiScaler keeps libxess_fg.dll in its own folder, while this dll sits in custom\<name>\dlssg below it. Look
	// next to this dll first, then up to three folders up to cover the OptiScaler install layout
	std::vector<std::filesystem::path> CandidatePaths()
	{
		auto here = ThisModulePath();
		std::vector<std::filesystem::path> paths{ here.replace_filename(XeFGDllName) };

		auto directory = here.parent_path();
		for (int i = 0; i < 3 && directory.has_parent_path(); i++)
		{
			directory = directory.parent_path();
			paths.push_back(directory / XeFGDllName);
		}

		return paths;
	}

	bool Search()
	{
		for (HMODULE module : LoadedXeFGModules())
		{
			if (!TryModule(module))
				continue;

			// Keeps it loaded even if its owner unloads it
			HMODULE pinned = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(module), &pinned);

			return true;
		}

		for (const auto& path : CandidatePaths())
		{
			if (!std::filesystem::exists(path))
				continue;

			HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

			if (!module)
			{
				Log::Warn("{} can't be loaded", path.string());
				continue;
			}

			if (TryModule(module))
				return true;

			FreeLibrary(module);
		}

		Log::Error("No compatible libxess_fg.dll is loaded and none was found next to this dll or in its parent folders");
		return false;
	}
} // namespace

bool XeFG::Load()
{
	static const bool loaded = Search();
	return loaded;
}
