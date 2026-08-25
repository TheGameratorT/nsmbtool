#pragma once

// `stamp` -- the build identifier the game shows on its crash screen.
//
// This is not code generation and has nothing to do with glue; it is here
// because it is the last thing the project's Python did that needed a program
// rather than a config key. The result is an ordinary file, fed back into the
// ROM through `files:` like any other asset.

#include <string>

namespace nsmb {

struct StampOptions
{
	// The global -C.
	std::string directory;

	// Where to write it. Resolved against the project root when relative.
	std::string out;
};

// `<short hash> <commit date>`, with the date in real UTC as `Z` claims.
//
// `seconds` is the committer date as a Unix timestamp, which is what makes this
// independent of the committer's timezone -- and of the machine's. The date git
// records carries an offset, and formatting those local fields while writing `Z`
// after them, as the script this replaces did, states a time that is not the one
// it means.
[[nodiscard]] std::string formatStamp(const std::string& shortHash, long long seconds);

int stampWrite(const StampOptions& options);

} // namespace nsmb
