#pragma once

// Running git.
//
// This spawns with an argument vector rather than handing a string to a shell.
// That is not fastidiousness: the repository URL comes out of nsmbref.lock,
// which is a file in a repository someone else may have written, and a URL
// containing a semicolon would otherwise be a command. There is no shell here at
// all, so there is nothing for such a URL to escape into.

#include <filesystem>
#include <string>
#include <vector>

namespace nsmb {

struct ProcessResult
{
	int exitCode = -1;
	std::string out;
	std::string err;

	[[nodiscard]] bool ok() const { return exitCode == 0; }
};

enum class Capture
{
	// stdout and stderr are collected. For the short, quiet queries -- what SHA
	// does this name resolve to, is this worktree clean -- whose output is data.
	Output,

	// Both are left pointing at the terminal. For clone and fetch, whose output
	// is progress: a five-minute clone that prints nothing looks like a hang.
	Inherit
};

// Runs `argv[0]` with `argv` as its arguments, in `workDir` (empty for the
// current directory). Throws only if the program could not be started at all --
// a program that ran and failed comes back as a non-zero exitCode, because the
// caller usually has something more useful to say about it than the exit status.
[[nodiscard]] ProcessResult run(
	const std::vector<std::string>& argv,
	const std::filesystem::path& workDir = {},
	Capture capture = Capture::Output);

// Whether `git` can be found and answers `--version`. Checked once, up front,
// so that a machine without git says so rather than failing midway through a
// sync with an errno.
[[nodiscard]] bool gitAvailable();

} // namespace nsmb
