#pragma once

// NCPatcher's ROM filesystem manifest, `ncpatcher.files/1`.
//
// Every file in the built ROM's table, not only the ones a module supplied --
// fid.hpp maps all of them. The ids are raw, as the ROM stores them; the game's
// file-id offset is applied here rather than there, because it is a fact about
// New Super Mario Bros. and not about the container.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nsmb {

struct ManifestFile
{
	std::uint32_t id = 0;
	std::string path;
};

struct Manifest
{
	// The variant the manifest was written for, when the project has variants.
	std::string variant;

	// Sorted by id, the way NCPatcher writes it.
	std::vector<ManifestFile> files;
};

[[nodiscard]] Manifest readManifest(const std::filesystem::path& file);

} // namespace nsmb
