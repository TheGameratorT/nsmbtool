#pragma once

// The revision store: one directory per commit, shared by every project on the
// machine.
//
// Three clones of the same reference sat on the machine this was written for,
// at 8.1, 11 and 7.4 MB of near-identical history, and none of them was named by
// anything but its path. The store replaces that with a single mirror holding
// the objects once, plus a checked-out worktree per revision that is addressed
// by its commit hash. Two projects on two revisions cost one history and two
// working trees.
//
// A worktree rather than an extracted archive for one reason worth stating: the
// checkout can be verified. `rev-parse HEAD` in a worktree answers what is
// actually there, so a sync interrupted halfway is detected and redone instead
// of being trusted because the directory exists.

#include <filesystem>
#include <string>
#include <vector>

namespace nsmb {

class Store
{
public:
	explicit Store(std::filesystem::path root);

	// $NSMBTOOL_STORE, else the platform's user data directory. The override
	// is what lets the tests run against a scratch store, and what lets a CI job
	// keep the store inside its own cache directory.
	[[nodiscard]] static std::filesystem::path defaultRoot();

	[[nodiscard]] const std::filesystem::path& root() const { return m_root; }
	[[nodiscard]] std::filesystem::path mirrorPath() const;
	[[nodiscard]] std::filesystem::path revisionPath(const std::string& rev) const;

	// Whether `rev` is checked out and is what it claims to be. A directory that
	// exists but does not answer with this hash is not present: it is wreckage
	// from an interrupted sync, and ensure() will replace it.
	[[nodiscard]] bool has(const std::string& rev) const;

	// Whether the mirror already holds the commit, which is what decides whether
	// sync needs the network at all.
	[[nodiscard]] bool mirrorHas(const std::string& rev) const;

	// Brings `rev` into the store, cloning or fetching only if it is not already
	// there. `offline` turns a needed fetch into an error instead.
	void ensure(const std::string& repo, const std::string& rev, bool offline);

	// Resolves a branch, tag or abbreviated hash to a full commit hash, fetching
	// first so that a branch name means its current tip rather than whatever the
	// mirror last saw.
	[[nodiscard]] std::string resolve(const std::string& repo, const std::string& rev, bool offline);

	// Every revision checked out in the store, sorted.
	[[nodiscard]] std::vector<std::string> revisions() const;

	// Uncommitted changes in a revision's worktree, as porcelain lines. A
	// reference is meant to be read-only; edits in it change build inputs
	// without changing the lock, which is the drift this whole design is
	// against, so callers say so out loud.
	[[nodiscard]] std::vector<std::string> localChanges(const std::string& rev) const;

	void remove(const std::string& rev);

	// The projects that have synced against this store, so gc knows which
	// revisions are still spoken for. Only paths are recorded; the revision is
	// re-read from each project's lock, so an entry cannot go stale by naming a
	// commit the project has since moved off.
	[[nodiscard]] std::vector<std::filesystem::path> projects() const;
	void rememberProject(const std::filesystem::path& projectRoot);
	void writeProjects(const std::vector<std::filesystem::path>& projects) const;

private:
	std::filesystem::path m_root;

	[[nodiscard]] std::filesystem::path projectsFile() const;
	// True when the mirror was created by this call, which means it is already
	// as current as the remote and a fetch on top of it would be pure latency.
	[[nodiscard]] bool ensureMirror(const std::string& repo, bool offline) const;
	void fetch(bool offline) const;
};

} // namespace nsmb
