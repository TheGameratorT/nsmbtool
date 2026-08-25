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

// Writes <project>/.ncpatcher.env with NSMBREF_ROOT and nothing else.
//
// The file is generated in full every time and carries a header saying so.
// Nothing merges into it: it is owned by this tool, and a value someone added by
// hand would be silently dropped on the next sync, which is worse than never
// having accepted it. Machine-specific variables -- NSMB_NITRO_ROOT above all --
// belong in the shell profile, where they survive.
//
// Returns false if the file already said exactly this, so sync can be quiet
// about doing nothing.
bool writeEnvFile(const std::filesystem::path& file, const std::filesystem::path& referenceRoot,
	const std::string& repo, const std::string& rev);

// Whether git would carry .ncpatcher.env into a commit. The file is generated,
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
