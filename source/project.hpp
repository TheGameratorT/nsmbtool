#pragma once

// Finding the project, and writing the file NCPatcher reads.

#include <filesystem>
#include <string>

namespace nsmb {

struct Project
{
	std::filesystem::path root;

	// Whether root was found by an existing nsmbref.lock. When it was not, the
	// root is where a lock *would* go -- next to ncpatcher.yaml -- and only
	// `use` may create one there.
	bool hasLock = false;

	[[nodiscard]] std::filesystem::path lockPath() const;
	[[nodiscard]] std::filesystem::path envPath() const;
};

// Walks up from `start` looking for nsmbref.lock, and failing that for an
// NCPatcher project file. Two passes rather than one combined walk: a lock
// anywhere above wins over an ncpatcher.yaml closer down, because the lock is
// the thing being looked for and the project file is only a guess at where one
// should live.
[[nodiscard]] Project findProject(const std::filesystem::path& start);

// Puts NSMBREF_ROOT into <project>/.ncpatcher.env, creating the file if it is
// not there.
//
// The file belongs to the project, not to this tool. Only the marked block is
// rewritten; every other line is carried across untouched, so a project may
// keep its own variables there -- NSMB_NITRO_ROOT, say, whose converted SDK
// headers are private and a property of the machine. The one thing outside the
// block that does not survive is another assignment of NSMBREF_ROOT, which
// would override the block, since the reader takes the last assignment of a
// name.
//
// Returns false if the file already said exactly this, so sync can be quiet
// about doing nothing.
bool writeEnvFile(const std::filesystem::path& file, const std::filesystem::path& referenceRoot,
	const std::string& repo, const std::string& rev);

// Whether git would carry .ncpatcher.env into a commit. The path it holds is
// machine-specific and absolute, so committing it hands every other clone a
// directory that does not exist.
enum class EnvFileStanding
{
	Ignored,     // or the project is not a git repository at all
	Tracked,     // already in the index, where .gitignore no longer applies
	Uncovered    // untracked and unignored
};

[[nodiscard]] EnvFileStanding envFileStanding(const std::filesystem::path& projectRoot);

} // namespace nsmb
