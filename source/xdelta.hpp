#pragma once

// `xdelta` writes a binary patch between two ROMs.
//
// The patches are for distribution rather than for the ROM, and the hook that
// used to make them was a shell line: `mkdir -p` and `&&` do not exist in cmd,
// which is what a hook goes through on Windows. Creating the directory and
// spawning the encoder here replaces both with code that behaves the same on
// every platform, and leaves xdelta3 itself as the only thing to have on PATH.

#include <string>

namespace nsmb {

struct XdeltaOptions
{
	// The global -C.
	std::string directory;

	// The ROM the patch applies to (the clean one players already own).
	std::string source;

	// The ROM the patch produces (this build's output).
	std::string target;

	// Where the patch goes. Its directory is created when missing.
	std::string out;
};

int xdeltaWrite(const XdeltaOptions& options);

} // namespace nsmb
