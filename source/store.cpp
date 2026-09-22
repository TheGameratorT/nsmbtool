#include "store.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "except.hpp"
#include "unicode.hpp"
#include "lockfile.hpp"
#include "log.hpp"
#include "process.hpp"

namespace fs = std::filesystem;

namespace nsmb {

namespace {

std::string trimmed(std::string text)
{
	const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
	while (!text.empty() && isSpace(text.back()))
		text.pop_back();
	std::size_t start = 0;
	while (start < text.size() && isSpace(text[start]))
		start++;
	return text.substr(start);
}

const char* environment(const char* name)
{
	const char* value = std::getenv(name);
	return (value != nullptr && *value != '\0') ? value : nullptr;
}

// Best effort: a cleanup step whose failure means the thing it was cleaning up
// was already not there. Everything whose outcome matters checks the result.
void tryRun(const std::vector<std::string>& argv, const fs::path& workDir)
{
	(void)run(argv, workDir);
}

// git's own failure text, indented under ours. It is nearly always the more
// useful half -- "Repository not found", "could not resolve host" -- and
// swallowing it in favour of "git failed" is how a tool earns a reputation.
std::string gitSaid(const ProcessResult& result)
{
	std::string text = trimmed(result.err.empty() ? result.out : result.err);
	if (text.empty())
		return {};

	std::string out = "\n";
	std::size_t start = 0;
	while (start <= text.size())
	{
		const std::size_t end = text.find('\n', start);
		out += "       " + text.substr(start, end == std::string::npos ? end : end - start) + "\n";
		if (end == std::string::npos)
			break;
		start = end + 1;
	}
	out.pop_back();
	return out;
}

} // namespace

Store::Store(fs::path root) : m_root(std::move(root))
{
}

fs::path Store::defaultRoot()
{
	if (const char* configured = environment("NSMBTOOL_STORE"))
		return fs::path(configured);

#ifdef _WIN32
	if (const char* local = environment("LOCALAPPDATA"))
		return fs::path(local) / "nsmbtool";
	if (const char* profile = environment("USERPROFILE"))
		return fs::path(profile) / "AppData" / "Local" / "nsmbtool";
#else
	if (const char* xdg = environment("XDG_DATA_HOME"))
		return fs::path(xdg) / "nsmbtool";
	if (const char* home = environment("HOME"))
		return fs::path(home) / ".local" / "share" / "nsmbtool";
#endif

	throw nsmb::exception(
		"Could not work out where to keep the reference store: neither "
		ANSI_bCYAN "NSMBTOOL_STORE" ANSI_RESET " nor the usual home directory variables are set.");
}

fs::path Store::mirrorPath() const
{
	return m_root / "mirror.git";
}

fs::path Store::revisionPath(const std::string& rev) const
{
	return m_root / "reference" / rev;
}

fs::path Store::projectsFile() const
{
	return m_root / "projects.txt";
}

bool Store::has(const std::string& rev) const
{
	const fs::path path = revisionPath(rev);
	if (!fs::is_directory(path))
		return false;

	// A worktree always has a .git file pointing back at the mirror. Without
	// this, a bare directory would let rev-parse walk up and answer with an
	// enclosing repository's HEAD -- which happens the moment someone points
	// NSMBTOOL_STORE inside one.
	if (!fs::exists(path / ".git"))
		return false;

	const ProcessResult head = run({ "git", "rev-parse", "HEAD" }, path);
	return head.ok() && trimmed(head.out) == rev;
}

bool Store::mirrorHas(const std::string& rev) const
{
	if (!fs::is_directory(mirrorPath()))
		return false;

	return run({ "git", "cat-file", "-e", rev + "^{commit}" }, mirrorPath()).ok();
}

bool Store::ensureMirror(const std::string& repo, bool offline) const
{
	const fs::path mirror = mirrorPath();
	if (fs::is_directory(mirror))
	{
		// A mirror pointed at a different repository would quietly resolve
		// revisions from the wrong history, which is the one mistake here that
		// produces a plausible-looking wrong answer instead of an error.
		const ProcessResult url = run({ "git", "config", "--get", "remote.origin.url" }, mirror);
		if (url.ok() && trimmed(url.out) != repo)
		{
			throw nsmb::exception(
				"The store's mirror at " + pathToUtf8Generic(mirror) + " tracks\n"
				"       " + trimmed(url.out) + "\n"
				"       but this project's lock names\n"
				"       " + repo + "\n"
				"       Point NSMBTOOL_STORE somewhere else, or delete the mirror to re-clone it.");
		}
		return false;
	}

	if (offline)
		throw nsmb::exception("The reference store is empty and " ANSI_bCYAN "--offline" ANSI_RESET
			" was given, so there is nothing to sync from.");

	std::error_code error;
	fs::create_directories(m_root, error);
	if (error)
		throw nsmb::exception("Could not create the reference store at " + pathToUtf8Generic(m_root) + ".");

	log::info("Cloning " ANSI_bWHITE + repo + ANSI_RESET " into the store. This happens once.");

	// --mirror rather than --bare: it records a fetch refspec, so later updates
	// are a plain `git fetch` rather than a hand-written refspec every time.
	const ProcessResult clone = run(
		{ "git", "clone", "--mirror", "--progress", repo, pathToUtf8(mirror) },
		{}, Capture::Inherit);

	if (!clone.ok())
	{
		fs::remove_all(mirror, error);
		throw nsmb::exception("Could not clone " + repo + ".");
	}

	return true;
}

void Store::fetch(bool offline) const
{
	if (offline)
	{
		throw nsmb::exception(
			"The locked revision is not in the store, and " ANSI_bCYAN "--offline" ANSI_RESET
			" forbids fetching it.");
	}

	log::info("Fetching the reference.");
	const ProcessResult result = run({ "git", "fetch", "--prune", "--tags", "origin" },
		mirrorPath(), Capture::Inherit);
	if (!result.ok())
		throw nsmb::exception("Could not fetch the reference.");
}

void Store::ensure(const std::string& repo, const std::string& rev, bool offline)
{
	if (has(rev))
		return;

	(void)ensureMirror(repo, offline);

	if (!mirrorHas(rev))
	{
		fetch(offline);
		if (!mirrorHas(rev))
		{
			throw nsmb::exception(
				"The reference has no commit " ANSI_bWHITE + rev + ANSI_RESET ".\n"
				"       It was fetched and still is not there, so either the lock names a commit\n"
				"       that was never pushed, or one that has since been rewritten away.");
		}
	}

	const fs::path path = revisionPath(rev);

	// Anything already at the path failed has(), so it is either absent or
	// wreckage. Clearing it is safe in a way it would not be anywhere else: the
	// store holds nothing that is not reproducible from the mirror.
	std::error_code error;
	if (fs::exists(path))
	{
		tryRun({ "git", "worktree", "remove", "--force", pathToUtf8(path) }, mirrorPath());
		fs::remove_all(path, error);
	}

	fs::create_directories(path.parent_path(), error);
	if (error)
		throw nsmb::exception("Could not create " + pathToUtf8Generic(path.parent_path()) + ".");

	// Stale administrative entries left by a directory someone deleted by hand
	// make `worktree add` refuse the path.
	tryRun({ "git", "worktree", "prune" }, mirrorPath());

	const ProcessResult add = run(
		{ "git", "worktree", "add", "--detach", "--quiet", pathToUtf8(path), rev }, mirrorPath());

	if (!add.ok())
	{
		fs::remove_all(path, error);
		throw nsmb::exception("Could not check out " + rev + "." + gitSaid(add));
	}

	if (!has(rev))
	{
		fs::remove_all(path, error);
		throw nsmb::exception("Checked out " + rev + ", but the result is not at that commit.");
	}
}

std::string Store::resolve(const std::string& repo, const std::string& rev, bool offline)
{
	const bool freshlyCloned = ensureMirror(repo, offline);

	// A full hash already in the mirror needs no network at all, which is what
	// makes `use` on a hash a local operation.
	if (isFullObjectName(rev) && mirrorHas(rev))
		return rev;

	// Otherwise fetch, because a branch name has to mean its current tip rather
	// than whatever the mirror last saw -- unless the mirror is the clone this
	// call just made, which is already as current as it can be.
	if (!offline && !freshlyCloned)
		fetch(false);

	const ProcessResult result = run({ "git", "rev-parse", "--verify", "--quiet", rev + "^{commit}" },
		mirrorPath());

	if (!result.ok() || trimmed(result.out).empty())
	{
		throw nsmb::exception(
			"The reference has nothing called " ANSI_bWHITE + rev + ANSI_RESET ".\n"
			"       Give a branch, a tag or a commit hash that exists in\n"
			"       " + repo);
	}

	const std::string resolved = trimmed(result.out);
	if (!isFullObjectName(resolved))
		throw nsmb::exception("git resolved " + rev + " to \"" + resolved + "\", which is not a commit hash.");

	return resolved;
}

std::vector<std::string> Store::revisions() const
{
	std::vector<std::string> out;

	const fs::path dir = m_root / "reference";
	std::error_code error;
	if (!fs::is_directory(dir, error))
		return out;

	for (const fs::directory_entry& entry : fs::directory_iterator(dir, error))
	{
		if (!entry.is_directory(error))
			continue;

		const std::string name = pathToUtf8(entry.path().filename());
		if (isFullObjectName(name))
			out.push_back(name);
	}

	std::sort(out.begin(), out.end());
	return out;
}

std::vector<std::string> Store::localChanges(const std::string& rev) const
{
	std::vector<std::string> out;

	const ProcessResult status = run({ "git", "status", "--porcelain" }, revisionPath(rev));
	if (!status.ok())
		return out;

	std::istringstream stream(status.out);
	std::string line;
	while (std::getline(stream, line))
	{
		const std::string entry = trimmed(line);
		if (!entry.empty())
			out.push_back(entry);
	}
	return out;
}

void Store::remove(const std::string& rev)
{
	const fs::path path = revisionPath(rev);
	if (!fs::exists(path))
		return;

	// Through git first, so the mirror's administrative record goes with the
	// directory; the removal below is what catches a worktree git has already
	// lost track of.
	tryRun({ "git", "worktree", "remove", "--force", pathToUtf8(path) }, mirrorPath());

	std::error_code error;
	fs::remove_all(path, error);
	if (error)
		throw nsmb::exception("Could not remove " + pathToUtf8Generic(path) + ".");

	tryRun({ "git", "worktree", "prune" }, mirrorPath());
}

std::vector<fs::path> Store::projects() const
{
	std::vector<fs::path> out;

	std::ifstream stream(projectsFile());
	if (!stream.is_open())
		return out;

	std::string line;
	while (std::getline(stream, line))
	{
		const std::string entry = trimmed(line);
		if (entry.empty() || entry.front() == '#')
			continue;

		const fs::path path = utf8ToPath(entry);
		if (std::find(out.begin(), out.end(), path) == out.end())
			out.push_back(path);
	}
	return out;
}

void Store::rememberProject(const fs::path& projectRoot)
{
	std::vector<fs::path> known = projects();
	if (std::find(known.begin(), known.end(), projectRoot) != known.end())
		return;

	known.push_back(projectRoot);
	writeProjects(known);
}

void Store::writeProjects(const std::vector<fs::path>& list) const
{
	std::error_code error;
	fs::create_directories(m_root, error);

	std::ostringstream out;
	out << "# Projects that have synced against this store, so that `nsmbtool reference gc`\n"
	    << "# knows which revisions are still spoken for. Each project's revision is read from\n"
	    << "# its own nsmbref.lock, so nothing here can go stale by naming the wrong commit.\n";
	for (const fs::path& project : list)
		out << pathToUtf8Generic(project) << "\n";

	const std::string text = out.str();

	std::ofstream stream(projectsFile(), std::ios::binary | std::ios::trunc);
	if (!stream.is_open())
		throw nsmb::exception("Could not write " + pathToUtf8Generic(projectsFile()) + ".");
	stream.write(text.data(), std::streamsize(text.size()));
}

} // namespace nsmb
