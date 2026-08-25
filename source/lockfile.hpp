#pragma once

// nsmbref.lock -- the committed statement of which code reference a project
// builds against.
//
// It exists because an environment variable cannot answer that question. A
// shell has one NSMBREF_ROOT and a machine has one shell profile, so two
// projects pinned to two revisions of the same reference are, today, simply not
// buildable in the same session: whichever the profile exports wins for both.
// Worse, the losing project still builds -- against the wrong headers, the wrong
// symbols and the wrong overlay catalog -- and says nothing.
//
// So the revision moves into the project, next to ncpatcher.yaml, under version
// control, where a diff shows it changing. What is deliberately *not* in here is
// NSMB_NITRO_ROOT: the converted Nitro SDK headers are private, cannot be
// fetched, and their location is a property of the machine rather than of the
// project. That stays an ordinary environment variable.

#include <filesystem>
#include <string>
#include <string_view>

namespace nsmb {

struct Lockfile
{
	// Looked for by walking up from the working directory, the way git finds
	// its own root.
	static constexpr std::string_view NAME = "nsmbref.lock";

	// The only version there is. A file declaring a higher one is an error
	// rather than a best effort: a lock this program does not fully understand
	// is a pin it cannot honour, and honouring it wrongly is the failure the
	// lock exists to prevent.
	static constexpr int VERSION = 1;

	static constexpr std::string_view DEFAULT_REPO =
		"https://github.com/MammaMiaTeam/NSMB-Code-Reference";

	std::string repo;
	std::string rev;   // full object name, never abbreviated

	// Where it was read from, for diagnostics. Not part of the file.
	std::filesystem::path file;

	[[nodiscard]] static Lockfile read(const std::filesystem::path& file);
	void write(const std::filesystem::path& file) const;
};

// A full object name: 40 hex digits, or 64 once a repository moves to SHA-256.
// Abbreviations are rejected wherever a lock is read or written, because the
// point of the pin is that it names exactly one commit forever, and an
// abbreviation only names one commit until the repository grows.
[[nodiscard]] bool isFullObjectName(std::string_view text);

} // namespace nsmb
