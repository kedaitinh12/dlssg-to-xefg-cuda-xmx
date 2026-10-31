#pragma once

// Ported from MFG-Unlocker (source/native/xess/runtime_payload.h), MIT licensed, see NOTICE.md.
//
// MFG-Unlocker embeds its ported programs as RCDATA resources. They are a separately prepared, hash-pinned build
// input, so here they are read from a folder next to the dll instead: programs.tsv plus one <key>.dxil per row.
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace xess_payload
{
	// Program key (a decimal number XeFG's model refers to) to decoded DXIL
	using Programs = std::map<std::string, std::vector<unsigned char>>;

	// Lowercase SHA-256 hex, the form the pinned manifest uses
	std::string Hash(std::span<const unsigned char> bytes);

	// Every program the pinned manifest lists, verified against it. Throws std::runtime_error with the reason
	Programs LoadPrograms(const std::filesystem::path& root);

	// MFG-Unlocker also replaces a few of Intel's own programs with byte-identical faster ones. Those are not part of
	// the pinned program set, so the port runs on Intel's own copies for those stages
	struct Replacement
	{
		std::string name, nativeHash;
		size_t nativeBytes{};
		std::vector<unsigned char> bytes;
	};

	std::vector<Replacement> LoadReplacements(const std::filesystem::path& root);
} // namespace xess_payload
