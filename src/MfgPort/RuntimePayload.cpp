#include "RuntimePayload.h"
#include "Sha256.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace xess_payload
{
	namespace
	{
		// MFG-Unlocker's pinned XeSS program manifest (BUILD.md). Another set is refused, its program keys mean
		// something else
		constexpr char ManifestSha256[] = "d74bd783d29280ed372653701ce9b0395362e381faace1213206e0bdd9bfc9e5";
		constexpr size_t ProgramCount = 54;
		constexpr size_t MaxProgramBytes = 1000000;

		std::vector<unsigned char> ReadFile(const std::filesystem::path& path)
		{
			std::ifstream file(path, std::ios::binary);

			if (!file)
				throw std::runtime_error("can't open " + path.string());

			std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});

			if (file.bad() || bytes.size() > MaxProgramBytes)
				throw std::runtime_error("can't read " + path.string());

			return bytes;
		}
	} // namespace

	std::string Hash(std::span<const unsigned char> bytes)
	{
		sha256_util::Digest digest{};

		if (!sha256_util::Compute(bytes.data(), bytes.size(), digest))
			throw std::runtime_error("hash failed");

		return sha256_util::LowerHex(digest);
	}

	Programs LoadPrograms(const std::filesystem::path& root)
	{
		const auto manifest = ReadFile(root / "programs.tsv");

		if (Hash(manifest) != ManifestSha256)
			throw std::runtime_error("programs.tsv is not the pinned XeSS program set");

		Programs programs;
		std::istringstream input(std::string(manifest.begin(), manifest.end()));
		std::string line;

		while (std::getline(input, line))
		{
			if (!line.empty() && line.back() == '\r')
				line.pop_back();

			if (line.empty())
				continue;

			std::istringstream row(line);
			std::string key, hash, extra;

			if (!(row >> key >> hash) || (row >> extra) || key.size() < 4 || key.size() > 24 || hash.size() != 64 ||
				!std::ranges::all_of(key, [](char c) { return c >= '0' && c <= '9'; }))
			{
				throw std::runtime_error("malformed programs.tsv row: " + line);
			}

			const auto path = root / (key + ".dxil");
			auto bytes = ReadFile(path);

			// A DXBC container states its own size at 24
			uint32_t size = 0;
			if (bytes.size() >= 28)
				memcpy(&size, bytes.data() + 24, sizeof(size));

			if (bytes.size() < 32 || memcmp(bytes.data(), "DXBC", 4) != 0 || size != bytes.size())
				throw std::runtime_error("not a DXIL program: " + path.string());

			if (Hash(bytes) != hash)
				throw std::runtime_error("hash mismatch: " + path.string());

			if (!programs.emplace(key, std::move(bytes)).second)
				throw std::runtime_error("duplicate program " + key);
		}

		if (programs.size() != ProgramCount)
			throw std::runtime_error("expected " + std::to_string(ProgramCount) + " programs, found " +
									 std::to_string(programs.size()));

		return programs;
	}

	std::vector<Replacement> LoadReplacements(const std::filesystem::path&) { return {}; }
} // namespace xess_payload
