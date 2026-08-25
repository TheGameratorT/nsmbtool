// Tests for source/project.cpp -- finding the project and writing the file
// NCPatcher reads.
//
// The generated .ncpatcher.env is a contract with another program, so its exact
// text matters: NCPatcher's reader trims, strips one layer of quotes and takes
// the last assignment of a name. What is checked here is that what this writes
// survives that reader unchanged.

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
		check(contains(text, "Do not edit, and do not commit"), "the file says it is generated");
		check(contains(text, "NSMB_NITRO_ROOT"), "and says why the SDK path is not in it");
		check(contains(text, sha), "the revision is recorded in a comment");

		check(envValueAsNcpatcherSeesIt(text, "NSMBREF_ROOT") == reference.generic_string(),
			"NCPatcher's reader recovers the reference path exactly");

		// Nothing but NSMBREF_ROOT: the file is regenerated wholesale, so a
		// second variable here would be a promise this cannot keep.
		std::size_t assignments = 0;
		std::istringstream stream(text);
		std::string line;
		while (std::getline(stream, line))
		{
			if (!line.empty() && line.front() != '#' && line.find('=') != std::string::npos)
				assignments++;
		}
		check(assignments == 1, "exactly one variable is written");

		check(nsmb::writeEnvFile(file, root / "store" / "reference" / std::string(40, 'b'),
			"https://example.invalid/r", std::string(40, 'b')),
			"a different revision rewrites the file");
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
