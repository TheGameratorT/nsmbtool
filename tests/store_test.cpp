// Tests for source/store.cpp -- the revision store.
//
// These drive the real git against a scratch repository. A mocked git would only
// prove the arguments were spelled the way the mock expected, and every failure
// this code actually has is in what git does: a worktree that outlives its
// directory, a mirror pointed at the wrong remote, a commit that is not there.
//
// Nothing here touches the network. The "remote" is a directory, which git
// clones exactly as it clones a URL.

#include "../source/store.hpp"
#include "../source/lockfile.hpp"
#include "../source/process.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static int g_failures = 0;

static void check(bool condition, const std::string& what)
{
	if (!condition)
	{
		std::cout << "FAIL: " << what << "\n";
		g_failures++;
	}
}

static void git(const fs::path& where, const std::vector<std::string>& arguments)
{
	std::vector<std::string> argv = { "git" };
	argv.insert(argv.end(), arguments.begin(), arguments.end());

	const nsmb::ProcessResult result = nsmb::run(argv, where);
	if (!result.ok())
	{
		std::cout << "FAIL: git";
		for (const std::string& argument : arguments)
			std::cout << " " << argument;
		std::cout << "\n" << result.err << "\n";
		g_failures++;
	}
}

static std::string commit(const fs::path& repo, const std::string& file, const std::string& text)
{
	std::ofstream stream(repo / file, std::ios::binary | std::ios::trunc);
	stream << text;
	stream.close();

	git(repo, { "add", file });
	git(repo, { "commit", "-q", "-m", "add " + file });

	const nsmb::ProcessResult head = nsmb::run({ "git", "rev-parse", "HEAD" }, repo);
	std::string sha = head.out;
	while (!sha.empty() && (sha.back() == '\n' || sha.back() == '\r'))
		sha.pop_back();
	return sha;
}

int main()
{
	if (!nsmb::gitAvailable())
	{
		std::cout << "git is not available; skipping store tests\n";
		return 0;
	}

	const fs::path root = fs::temp_directory_path() / "nsmbtool-store-test";
	fs::remove_all(root);
	fs::create_directories(root);

	// A source repository standing in for the reference.
	const fs::path source = root / "source";
	fs::create_directories(source);
	git(source, { "init", "-q", "-b", "main" });
	git(source, { "config", "user.email", "test@example.invalid" });
	git(source, { "config", "user.name", "Test" });

	const std::string first = commit(source, "one.txt", "one\n");
	const std::string second = commit(source, "two.txt", "two\n");
	git(source, { "tag", "v1" });

	check(nsmb::isFullObjectName(first) && nsmb::isFullObjectName(second) && first != second,
		"the scratch repository has two distinct commits");

	const std::string repo = source.generic_string();
	nsmb::Store store(root / "store");

	check(store.revisions().empty(), "a fresh store holds nothing");
	check(!store.has(first), "and has no revision");
	check(!store.mirrorHas(first), "and no mirror");

	// An empty store with --offline has nowhere to get anything from, and says
	// so rather than failing somewhere inside git.
	{
		bool refused = false;
		try { store.ensure(repo, first, true); }
		catch (const std::exception&) { refused = true; }
		check(refused, "offline against an empty store is refused");
	}

	store.ensure(repo, first, false);
	check(store.has(first), "ensure checks out the requested commit");
	check(fs::is_regular_file(store.revisionPath(first) / "one.txt"),
		"the worktree holds that commit's content");
	check(!fs::exists(store.revisionPath(first) / "two.txt"),
		"and not a later commit's");

	// The second time it is already there, so nothing needs the network -- which
	// is what makes sync cheap enough to put in a build script.
	store.ensure(repo, first, true);
	check(store.has(first), "a second ensure is offline and idempotent");

	store.ensure(repo, second, false);
	check(store.has(second) && store.has(first), "two revisions coexist");

	{
		const std::vector<std::string> installed = store.revisions();
		check(installed.size() == 2, "both revisions are listed");
		check(std::is_sorted(installed.begin(), installed.end()), "the listing is sorted");
	}

	// One mirror behind both, which is the entire point of the store.
	check(fs::is_directory(store.mirrorPath()), "there is a single mirror");

	{
		check(store.resolve(repo, "v1", false) == second, "a tag resolves to its commit");
		check(store.resolve(repo, "main", false) == second, "a branch resolves to its tip");
		check(store.resolve(repo, first.substr(0, 8), false) == first,
			"an abbreviation resolves to the full name");
		check(store.resolve(repo, first, true) == first,
			"a full hash already in the mirror resolves offline");

		bool refused = false;
		try { (void)store.resolve(repo, "no-such-branch", false); }
		catch (const std::exception&) { refused = true; }
		check(refused, "a name that does not exist is refused");
	}

	// A commit that was never pushed cannot be honoured, and the message has to
	// say that rather than leaving an empty directory behind.
	{
		bool refused = false;
		std::string message;
		try { store.ensure(repo, std::string(40, 'a'), false); }
		catch (const std::exception& e) { refused = true; message = e.what(); }
		check(refused, "a commit the reference does not have is refused");
		check(message.find("no commit") != std::string::npos, "and is described as missing");
		check(!fs::exists(store.revisionPath(std::string(40, 'a'))),
			"and leaves nothing behind");
	}

	// Wreckage from an interrupted sync: the directory is there and the content
	// is not. It must not be mistaken for a checkout.
	{
		const fs::path path = store.revisionPath(first);
		(void)nsmb::run({ "git", "worktree", "remove", "--force", path.string() }, store.mirrorPath());
		fs::create_directories(path);
		std::ofstream(path / "leftover.txt") << "junk\n";

		check(!store.has(first), "a directory that is not a checkout does not count as one");

		store.ensure(repo, first, true);
		check(store.has(first), "and ensure replaces it, without the network");
		check(!fs::exists(path / "leftover.txt"), "the wreckage is gone");
	}

	// Editing the reference changes build inputs without changing the lock,
	// which is the drift the whole design is against.
	{
		check(store.localChanges(first).empty(), "a fresh checkout is clean");

		std::ofstream(store.revisionPath(first) / "one.txt", std::ios::trunc) << "edited\n";
		check(!store.localChanges(first).empty(), "an edit in the reference is noticed");

		store.remove(first);
		store.ensure(repo, first, true);
		check(store.localChanges(first).empty(), "and a re-checkout is clean again");
	}

	// A mirror tracking a different repository would resolve revisions out of
	// the wrong history: the one mistake here that yields a plausible answer.
	{
		bool refused = false;
		std::string message;
		try { store.ensure("https://example.invalid/other", first, true); }
		catch (const std::exception& e) { refused = true; message = e.what(); }
		check(!refused, "an already-present revision is returned before the remote is looked at");

		try { store.ensure("https://example.invalid/other", std::string(40, 'c'), true); }
		catch (const std::exception& e) { refused = true; message = e.what(); }
		check(refused && message.find("tracks") != std::string::npos,
			"a mirror of a different repository is refused by name");
	}

	{
		store.remove(second);
		check(!store.has(second), "a removed revision is gone");
		check(!fs::exists(store.revisionPath(second)), "and so is its directory");
		check(store.mirrorHas(second), "but the mirror keeps its history, so a re-sync is offline");

		store.ensure(repo, second, true);
		check(store.has(second), "which it is");

		store.remove(second);
		store.remove(second);
		check(!store.has(second), "removing twice is not an error");
	}

	// A store placed inside somebody else's repository. Without the .git guard,
	// rev-parse in an empty revision directory walks up and answers with the
	// enclosing HEAD, and a checkout that is not there looks present.
	{
		const fs::path nested = root / "nested";
		fs::create_directories(nested);
		git(nested, { "init", "-q", "-b", "main" });
		git(nested, { "config", "user.email", "test@example.invalid" });
		git(nested, { "config", "user.name", "Test" });
		const std::string enclosing = commit(nested, "host.txt", "host\n");

		nsmb::Store inner(nested / "store");
		fs::create_directories(inner.revisionPath(enclosing));
		check(!inner.has(enclosing),
			"an empty directory inside another repository is not a checkout");
	}

	{
		const fs::path projectA = root / "projects" / "a";
		const fs::path projectB = root / "projects" / "b";
		fs::create_directories(projectA);
		fs::create_directories(projectB);

		check(store.projects().empty(), "no projects are registered yet");

		store.rememberProject(projectA);
		store.rememberProject(projectB);
		store.rememberProject(projectA);

		const std::vector<fs::path> known = store.projects();
		check(known.size() == 2, "each project is registered once");
		check(std::find(known.begin(), known.end(), projectA) != known.end()
			&& std::find(known.begin(), known.end(), projectB) != known.end(),
			"and both are there");

		store.writeProjects({ projectB });
		check(store.projects() == std::vector<fs::path>{ projectB },
			"the registry can be rewritten, which is how gc prunes it");
	}

	fs::remove_all(root);

	if (g_failures == 0)
		std::cout << "All store tests passed\n";
	return g_failures == 0 ? 0 : 1;
}
