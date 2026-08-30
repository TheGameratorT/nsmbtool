// Tests for source/project.cpp -- finding the project and writing the file
// NCPatcher reads.
//
// .ncpatcher.env is a contract with another program, so its exact text matters:
// NCPatcher's reader trims, strips one layer of quotes and takes the last
// assignment of a name. What is checked here is that what this writes survives
// that reader unchanged, and that the lines it does not own survive the write.

#include "../source/project.hpp"
#include "../source/lockfile.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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

static void touch(const fs::path& file, const std::string& text = {})
{
	fs::create_directories(file.parent_path());
	std::ofstream stream(file, std::ios::binary | std::ios::trunc);
	stream << text;
}

static std::string read(const fs::path& file)
{
	std::ifstream stream(file, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

static bool contains(const std::string& text, std::string_view needle)
{
	return text.find(needle) != std::string::npos;
}

// How many assignments the file makes, comments and blank lines aside.
static std::size_t assignments(const std::string& text)
{
	std::size_t count = 0;
	std::istringstream stream(text);
	std::string line;
	while (std::getline(stream, line))
	{
		if (!line.empty() && line.front() != '#' && line.find('=') != std::string::npos)
			count++;
	}
	return count;
}

// The value NCPatcher's reader would end up with for `name`, applying the same
// trimming and unquoting.
static std::string envValueAsNcpatcherSeesIt(const std::string& text, const std::string& name)
{
	std::istringstream stream(text);
	std::string line;
	std::string found;
	while (std::getline(stream, line))
	{
		const auto trim = [](std::string s) {
			const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
			while (!s.empty() && space(s.front())) s.erase(s.begin());
			while (!s.empty() && space(s.back())) s.pop_back();
			return s;
		};

		const std::string trimmed = trim(line);
		if (trimmed.empty() || trimmed.front() == '#')
			continue;

		const std::size_t equals = trimmed.find('=');
		if (equals == std::string::npos)
			continue;
		if (trim(trimmed.substr(0, equals)) != name)
			continue;

		std::string value = trim(trimmed.substr(equals + 1));
		if (value.size() >= 2 && value.front() == value.back()
		    && (value.front() == '"' || value.front() == '\''))
		{
			value = value.substr(1, value.size() - 2);
		}
		found = value;
	}
	return found;
}

int main()
{
	const fs::path root = fs::temp_directory_path() / "nsmbtool-project-test";
	fs::remove_all(root);
	fs::create_directories(root);

	const std::string sha = "ac82391f0123456789abcdef0123456789abcdef";

	// A lock above wins over an ncpatcher.yaml below it. The lock is the thing
	// being looked for; the project file is only a guess at where one should go.
	{
		const fs::path project = root / "walk";
		touch(project / "nsmbref.lock",
			"version: 1\nreference:\n  repo: x\n  rev: " + sha + "\n");
		touch(project / "sub" / "ncpatcher.yaml");
		fs::create_directories(project / "sub" / "deep" / "deeper");

		const nsmb::Project found = nsmb::findProject(project / "sub" / "deep" / "deeper");
		check(found.hasLock, "a lock several directories up is found");
		check(found.root == project, "and its directory is the project root");
		check(found.lockPath() == project / "nsmbref.lock", "the lock path is derived from it");
		check(found.envPath() == project / ".ncpatcher.env", "so is the env path");
	}

	// With no lock anywhere, the project file says where one would go -- which
	// is what `use` needs in order to create the first one.
	{
		const fs::path project = root / "unpinned";
		touch(project / "ncpatcher.yaml");
		fs::create_directories(project / "modules" / "coop");

		const nsmb::Project found = nsmb::findProject(project / "modules" / "coop");
		check(!found.hasLock, "an unpinned project has no lock");
		check(found.root == project, "but ncpatcher.yaml still locates it");
	}

	{
		const fs::path nowhere = root / "nowhere" / "at" / "all";
		fs::create_directories(nowhere);
		const nsmb::Project found = nsmb::findProject(nowhere);
		check(!found.hasLock && found.root == nowhere,
			"with no marker at all the starting directory is the project");
	}

	// ncpatcher.json, for a project that has not migrated to v2 yet.
	{
		const fs::path project = root / "v1";
		touch(project / "ncpatcher.json", "{}");
		check(nsmb::findProject(project).root == project, "a v1 project file counts too");
	}

	{
		const fs::path project = root / "env";
		fs::create_directories(project);
		const fs::path file = project / ".ncpatcher.env";
		const fs::path reference = root / "store" / "reference" / sha;

		check(nsmb::writeEnvFile(file, reference, "https://example.invalid/r", sha),
			"the first write reports a change");
		check(!nsmb::writeEnvFile(file, reference, "https://example.invalid/r", sha),
			"writing the same thing again reports none");

		const std::string text = read(file);
		check(contains(text, "# >>> nsmbtool") && contains(text, "# <<< nsmbtool"),
			"the managed lines are marked as such");
		check(contains(text, sha), "the revision is recorded in a comment");

		check(envValueAsNcpatcherSeesIt(text, "NSMBREF_ROOT") == reference.generic_string(),
			"NCPatcher's reader recovers the reference path exactly");

		check(assignments(text) == 1, "exactly one variable is written");

		check(nsmb::writeEnvFile(file, root / "store" / "reference" / std::string(40, 'b'),
			"https://example.invalid/r", std::string(40, 'b')),
			"a different revision rewrites the block");
	}

	// The file belongs to the project. Whatever else is in it stays, and the
	// block goes where the hand-written assignment was rather than at the end.
	{
		const fs::path project = root / "shared";
		const fs::path file = project / ".ncpatcher.env";
		const fs::path reference = root / "store" / "reference" / sha;

		touch(file,
			"# The paths this machine builds with.\n"
			"NSMB_NITRO_ROOT=/opt/nitro\n"
			"\n"
			"NSMBREF_ROOT=/somewhere/stale\n"
			"\n"
			"OTHER=kept\n");

		nsmb::writeEnvFile(file, reference, "r", sha);
		const std::string text = read(file);

		check(contains(text, "# The paths this machine builds with."), "a comment of its own stays");
		check(envValueAsNcpatcherSeesIt(text, "NSMB_NITRO_ROOT") == "/opt/nitro",
			"and so does a variable of its own");
		check(envValueAsNcpatcherSeesIt(text, "OTHER") == "kept",
			"including one written below the block");
		check(envValueAsNcpatcherSeesIt(text, "NSMBREF_ROOT") == reference.generic_string(),
			"the stale assignment is the one that gave way");
		check(assignments(text) == 3, "and it left nothing behind");

		check(text.find("# >>> nsmbtool") < text.find("OTHER=kept"),
			"the block took the place of the assignment it replaced");
	}

	// An assignment left below the block would win over it, so sync takes it out.
	{
		const fs::path project = root / "shadowed";
		const fs::path file = project / ".ncpatcher.env";
		const fs::path reference = root / "store" / "reference" / sha;

		touch(file, "A=1\n");
		nsmb::writeEnvFile(file, reference, "r", sha);

		std::ofstream(file, std::ios::binary | std::ios::app) << "NSMBREF_ROOT=/hijacked\n";
		check(nsmb::writeEnvFile(file, reference, "r", sha), "the shadowing assignment is a change");

		const std::string text = read(file);
		check(envValueAsNcpatcherSeesIt(text, "NSMBREF_ROOT") == reference.generic_string(),
			"and the block wins after the sync");
		check(assignments(text) == 2, "because the duplicate is gone");
	}

	// Half a block is not something to guess at: closing it would mean deciding
	// how much of someone's file the block swallows.
	{
		const fs::path project = root / "truncated";
		const fs::path file = project / ".ncpatcher.env";

		touch(file, "# >>> nsmbtool\nNSMBREF_ROOT=/half/written\nKEEP=1\n");

		bool threw = false;
		try
		{
			nsmb::writeEnvFile(file, root / "store" / "reference" / sha, "r", sha);
		}
		catch (const std::exception&)
		{
			threw = true;
		}
		check(threw, "an unterminated block is refused");
		check(contains(read(file), "KEEP=1"), "and the file is left alone");
	}

	// A path with a trailing space would otherwise be trimmed away by the
	// reader on the other side.
	{
		const fs::path project = root / "spacey";
		fs::create_directories(project);
		const fs::path file = project / ".ncpatcher.env";
		const fs::path reference = root / "store " ;

		nsmb::writeEnvFile(file, reference, "r", sha);
		check(envValueAsNcpatcherSeesIt(read(file), "NSMBREF_ROOT") == reference.generic_string(),
			"a path with edge whitespace survives the round trip");
	}

	fs::remove_all(root);

	if (g_failures == 0)
		std::cout << "All project tests passed\n";
	return g_failures == 0 ? 0 : 1;
}
