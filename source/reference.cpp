#include "reference.hpp"

#include <algorithm>
#include <sstream>

#include "except.hpp"
#include "unicode.hpp"
#include "lockfile.hpp"
#include "log.hpp"
#include "process.hpp"
#include "project.hpp"
#include "store.hpp"

namespace fs = std::filesystem;

namespace nsmb {

namespace {

fs::path startDirectory(const ReferenceOptions& options)
{
	if (options.directory.empty())
		return fs::current_path();

	const fs::path directory(options.directory);
	if (!fs::is_directory(directory))
		throw nsmb::exception("Not a directory: " + pathToUtf8Generic(directory) + ".");
	return directory;
}

Project projectWithLock(const ReferenceOptions& options)
{
	const Project project = findProject(startDirectory(options));
	if (!project.hasLock)
	{
		throw nsmb::exception(
			"No " ANSI_bCYAN + std::string(Lockfile::NAME) + ANSI_RESET " here or above "
			+ pathToUtf8Generic(startDirectory(options)) + ".\n"
			"       Pin a revision first:  " ANSI_bCYAN "nsmbtool reference use <branch|tag|commit>" ANSI_RESET);
	}
	return project;
}

std::string abbreviate(const std::string& rev)
{
	return rev.substr(0, 10);
}

// The subject line and date of a checked-out revision, for `list`. Three
// near-identical hashes tell a person nothing; "Replace the symbol linker
// scripts with C files" tells them which one they meant.
std::string describeRevision(const Store& store, const std::string& rev)
{
	const ProcessResult result = run(
		{ "git", "log", "-1", "--format=%ad  %s", "--date=short", "HEAD" },
		store.revisionPath(rev));

	if (!result.ok())
		return {};

	std::string text = result.out;
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
		text.pop_back();
	return text;
}

// Every revision the machine still has a reason to keep, and the projects that
// give the reason. Registry entries whose project or lock has gone are dropped
// as a side effect -- gc is the only thing that ever prunes them, so it has to.
std::vector<std::pair<std::string, std::vector<fs::path>>> liveRevisions(
	Store& store, const Project& current, bool prune)
{
	std::vector<fs::path> known = store.projects();
	if (current.hasLock
	    && std::find(known.begin(), known.end(), current.root) == known.end())
	{
		known.push_back(current.root);
	}

	std::vector<fs::path> surviving;
	std::vector<std::pair<std::string, std::vector<fs::path>>> live;

	for (const fs::path& project : known)
	{
		const fs::path lockPath = project / Lockfile::NAME;
		if (!fs::is_regular_file(lockPath))
			continue;

		std::string rev;
		try
		{
			rev = Lockfile::read(lockPath).rev;
		}
		catch (const std::exception& e)
		{
			// A lock this tool cannot read is not a reason to delete the
			// revision someone is presumably still building against, so the
			// project stays registered and nothing here is claimed about it.
			log::warn(std::string(e.what()));
			surviving.push_back(project);
			continue;
		}

		surviving.push_back(project);

		const auto existing = std::find_if(live.begin(), live.end(),
			[&](const auto& entry) { return entry.first == rev; });
		if (existing != live.end())
			existing->second.push_back(project);
		else
			live.emplace_back(rev, std::vector<fs::path>{ project });
	}

	if (prune && surviving != known)
		store.writeProjects(surviving);

	return live;
}

void requireGit()
{
	if (!gitAvailable())
	{
		throw nsmb::exception(
			"git is not on PATH. nsmbtool manages the reference with it and has no other way in.");
	}
}

} // namespace

int referenceList(const ReferenceOptions& options)
{
	requireGit();

	Store store(Store::defaultRoot());
	const Project project = findProject(startDirectory(options));

	std::string lockedRev;
	if (project.hasLock)
		lockedRev = Lockfile::read(project.lockPath()).rev;

	const std::vector<std::string> installed = store.revisions();
	const auto live = liveRevisions(store, project, false);

	log::out(ANSI_bWHITE "Store" ANSI_RESET "  " + pathToUtf8Generic(store.root()));

	if (installed.empty())
	{
		log::out("  (empty)");
	}
	else
	{
		for (const std::string& rev : installed)
		{
			const bool current = rev == lockedRev;

			std::ostringstream line;
			line << (current ? ANSI_bGREEN "* " ANSI_RESET : "  ")
			     << (current ? ANSI_bWHITE : "") << abbreviate(rev) << ANSI_RESET;

			const std::string description = describeRevision(store, rev);
			if (!description.empty())
				line << "  " << description;

			const auto users = std::find_if(live.begin(), live.end(),
				[&](const auto& entry) { return entry.first == rev; });
			if (users != live.end())
				line << ANSI_CYAN "  (" << users->second.size()
				     << (users->second.size() == 1 ? " project)" : " projects)") << ANSI_RESET;

			log::out(line.str());
		}
	}

	if (!lockedRev.empty())
	{
		log::out("");
		log::out(ANSI_bWHITE "Locked" ANSI_RESET " " + abbreviate(lockedRev)
			+ "  " + pathToUtf8Generic(project.lockPath()));

		if (!store.has(lockedRev))
		{
			log::out("  " ANSI_bYELLOW "not in the store" ANSI_RESET
				" -- run " ANSI_bCYAN "nsmbtool reference sync" ANSI_RESET);
		}
	}

	// Unreferenced revisions are the whole subject of gc, so say how many there
	// are here rather than making someone run gc to find out.
	std::size_t unreferenced = 0;
	for (const std::string& rev : installed)
	{
		const bool spoken = std::any_of(live.begin(), live.end(),
			[&](const auto& entry) { return entry.first == rev; });
		if (!spoken)
			unreferenced++;
	}
	if (unreferenced > 0)
	{
		std::ostringstream oss;
		oss << unreferenced << (unreferenced == 1 ? " revision is" : " revisions are")
		    << " not named by any known project. " ANSI_bCYAN "nsmbtool reference gc" ANSI_RESET
		       " removes them.";
		log::out("");
		log::info(oss.str());
	}

	return 0;
}

int referenceUse(const ReferenceOptions& options, const std::string& revision)
{
	requireGit();

	const Project project = findProject(startDirectory(options));

	std::string repo(Lockfile::DEFAULT_REPO);
	std::string previous;
	if (project.hasLock)
	{
		const Lockfile existing = Lockfile::read(project.lockPath());
		repo = existing.repo;
		previous = existing.rev;
	}
	if (!options.repo.empty())
		repo = options.repo;

	Store store(Store::defaultRoot());
	const std::string resolved = store.resolve(repo, revision, options.offline);

	Lockfile lock;
	lock.repo = repo;
	lock.rev = resolved;
	lock.write(project.lockPath());

	if (!project.hasLock)
		log::info("Created " + pathToUtf8Generic(project.lockPath()) + ".");
	else if (previous == resolved)
		log::info("Already pinned to " ANSI_bWHITE + abbreviate(resolved) + ANSI_RESET ".");
	else
		log::info("Pinned " ANSI_bWHITE + abbreviate(previous) + ANSI_RESET " -> " ANSI_bWHITE
			+ abbreviate(resolved) + ANSI_RESET ".");

	// `use` syncs, because a lock naming a revision the machine does not have is
	// a state nobody wants to be left in by a command whose whole job was to
	// choose one.
	return referenceSync(options);
}

int referenceSync(const ReferenceOptions& options)
{
	requireGit();

	const Project project = projectWithLock(options);
	const Lockfile lock = Lockfile::read(project.lockPath());

	Store store(Store::defaultRoot());

	const bool alreadyThere = store.has(lock.rev);
	store.ensure(lock.repo, lock.rev, options.offline);

	const fs::path referenceRoot = store.revisionPath(lock.rev);

	const std::vector<std::string> changes = store.localChanges(lock.rev);
	if (!changes.empty())
	{
		std::ostringstream oss;
		oss << "The checked-out reference has uncommitted changes, so this build will not\n"
		       "       match " << abbreviate(lock.rev) << " despite the lock saying it does:";
		for (std::size_t i = 0; i < changes.size() && i < 5; i++)
			oss << "\n         " << changes[i];
		if (changes.size() > 5)
			oss << "\n         ... and " << (changes.size() - 5) << " more";
		oss << "\n       " << pathToUtf8Generic(referenceRoot);
		log::warn(oss.str());
	}

	const bool wrote = writeEnvFile(project.envPath(), referenceRoot, lock.repo, lock.rev);

	store.rememberProject(project.root);

	switch (envFileStanding(project.root))
	{
	case EnvFileStanding::Ignored:
		break;

	case EnvFileStanding::Tracked:
		log::warn(".ncpatcher.env is tracked by git. It names an absolute path into this\n"
		          "       machine's store, so every clone would get a directory that does not exist,\n"
		          "       and every sync that moved the reference would show up as a change.\n"
		          "       Run  " ANSI_bCYAN "git rm --cached .ncpatcher.env" ANSI_RESET "  and add it to .gitignore.");
		break;

	case EnvFileStanding::Uncovered:
		log::warn(".ncpatcher.env is not ignored by git. It names an absolute path into this\n"
		          "       machine's store, so committing it hands every other clone a directory that\n"
		          "       does not exist.\n"
		          "       Add it to .gitignore.");
		break;
	}

	if (alreadyThere && !wrote)
		log::info("Up to date at " ANSI_bWHITE + abbreviate(lock.rev) + ANSI_RESET ".");
	else
		log::info("Reference " ANSI_bWHITE + abbreviate(lock.rev) + ANSI_RESET " at "
			+ pathToUtf8Generic(referenceRoot));

	return 0;
}

int referencePath(const ReferenceOptions& options, const std::string& revision)
{
	Store store(Store::defaultRoot());

	std::string rev;
	if (revision.empty())
	{
		rev = Lockfile::read(projectWithLock(options).lockPath()).rev;
	}
	else if (isFullObjectName(revision))
	{
		rev = revision;
	}
	else
	{
		// Only against what is installed, and never over the network: `path` is
		// written inside `$(...)` in a build script, so it has to be fast and it
		// has to be safe to run with no connection.
		requireGit();
		for (const std::string& installed : store.revisions())
		{
			if (installed.starts_with(revision))
			{
				if (!rev.empty())
					throw nsmb::exception("\"" + revision + "\" matches more than one installed revision.");
				rev = installed;
			}
		}
		if (rev.empty())
			throw nsmb::exception("No installed revision matches \"" + revision + "\".");
	}

	if (!fs::is_directory(store.revisionPath(rev)))
	{
		throw nsmb::exception(
			"Revision " ANSI_bWHITE + abbreviate(rev) + ANSI_RESET " is not in the store.\n"
			"       Run " ANSI_bCYAN "nsmbtool reference sync" ANSI_RESET " to fetch it.");
	}

	// stdout, alone, unadorned: this is the one command whose output is meant to
	// be captured by a shell.
	std::cout << pathToUtf8Generic(store.revisionPath(rev)) << std::endl;
	return 0;
}

int referenceGc(const ReferenceOptions& options)
{
	requireGit();

	Store store(Store::defaultRoot());
	const Project project = findProject(startDirectory(options));

	const auto live = liveRevisions(store, project, !options.dryRun);

	std::vector<std::string> doomed;
	for (const std::string& rev : store.revisions())
	{
		const bool spoken = std::any_of(live.begin(), live.end(),
			[&](const auto& entry) { return entry.first == rev; });
		if (!spoken)
			doomed.push_back(rev);
	}

	if (doomed.empty())
	{
		log::info("Nothing to remove; every installed revision is named by a known project.");
		return 0;
	}

	for (const std::string& rev : doomed)
	{
		if (options.dryRun)
		{
			log::out("  would remove  " + abbreviate(rev) + "  "
				+ pathToUtf8Generic(store.revisionPath(rev)));
			continue;
		}

		store.remove(rev);
		log::info("Removed " ANSI_bWHITE + abbreviate(rev) + ANSI_RESET ".");
	}

	if (options.dryRun)
	{
		std::ostringstream oss;
		oss << doomed.size() << (doomed.size() == 1 ? " revision" : " revisions")
		    << " would be removed. The mirror keeps their history, so a later sync needs no network.";
		log::info(oss.str());
	}

	return 0;
}

} // namespace nsmb
