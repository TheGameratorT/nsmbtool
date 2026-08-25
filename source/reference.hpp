#pragma once

// The `reference` subcommands.

#include <string>
#include <vector>

namespace nsmb {

struct ReferenceOptions
{
	// Where to start looking for the project. The global -C.
	std::string directory;

	// Refuse to touch the network. What a CI job or a packaging build wants: an
	// operation that would have fetched fails loudly instead of stalling on a
	// host it cannot reach.
	bool offline = false;

	// gc only: list what would go without removing it.
	bool dryRun = false;

	// use only.
	std::string repo;
};

int referenceList(const ReferenceOptions& options);
int referenceUse(const ReferenceOptions& options, const std::string& revision);
int referenceSync(const ReferenceOptions& options);
int referencePath(const ReferenceOptions& options, const std::string& revision);
int referenceGc(const ReferenceOptions& options);

} // namespace nsmb
