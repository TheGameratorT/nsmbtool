// Tests for source/lockfile.cpp -- nsmbref.lock.
//
// The lock's whole value is that it names exactly one commit, so most of what is
// checked here is refusal: an abbreviated hash, a version from the future, a
// missing field. A lock that is accepted loosely is a pin that does not pin.

#include "../source/lockfile.hpp"

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

static fs::path g_dir;

static fs::path writeLock(const std::string& name, const std::string& text)
{
	const fs::path file = g_dir / name;
	std::ofstream stream(file, std::ios::binary | std::ios::trunc);
	stream << text;
	return file;
}

static bool rejects(const std::string& name, const std::string& text, std::string& message)
{
	try
	{
		(void)nsmb::Lockfile::read(writeLock(name, text));
		message.clear();
		return false;
	}
	catch (const std::exception& e)
	{
		message = e.what();
		return true;
	}
}

static bool contains(const std::string& text, std::string_view needle)
{
	return text.find(needle) != std::string::npos;
}

int main()
{
	g_dir = fs::temp_directory_path() / "nsmbtool-lockfile-test";
	fs::remove_all(g_dir);
	fs::create_directories(g_dir);

	const std::string sha = "ac82391f0123456789abcdef0123456789abcdef";

	check(nsmb::isFullObjectName(sha), "a 40-hex name is full");
	check(nsmb::isFullObjectName(std::string(64, 'a')), "a 64-hex name is full, for SHA-256");
	check(!nsmb::isFullObjectName("ac82391"), "an abbreviation is not");
	check(!nsmb::isFullObjectName("main"), "a branch name is not");
	check(!nsmb::isFullObjectName(std::string(39, 'a') + "g"), "a non-hex digit disqualifies");

	{
		const fs::path file = writeLock("good.lock",
			"version: 1\n"
			"reference:\n"
			"  repo: https://example.invalid/reference\n"
			"  rev: " + sha + "\n");

		const nsmb::Lockfile lock = nsmb::Lockfile::read(file);
		check(lock.repo == "https://example.invalid/reference", "the repo is read");
		check(lock.rev == sha, "the revision is read");
		check(lock.file == file, "the lock remembers where it came from");
	}

	// Written and read back, because the writer is hand-rolled rather than an
	// emitter and a stray indent would only show up here.
	{
		nsmb::Lockfile lock;
		lock.repo = std::string(nsmb::Lockfile::DEFAULT_REPO);
		lock.rev = sha;

		const fs::path file = g_dir / "round.lock";
		lock.write(file);

		const nsmb::Lockfile back = nsmb::Lockfile::read(file);
		check(back.repo == lock.repo && back.rev == lock.rev, "a written lock reads back");

		std::ifstream stream(file);
		const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
		check(contains(text, "# The NSMB code reference"), "the header comment is written");
		check(contains(text, "nsmbtool reference use"), "the comment says how to change it");
		check(text.back() == '\n', "the file ends with a newline");
	}

	{
		nsmb::Lockfile lock;
		lock.repo = "https://example.invalid/reference";
		lock.rev = "ac82391";

		bool refused = false;
		try { lock.write(g_dir / "short.lock"); }
		catch (const std::exception&) { refused = true; }
		check(refused, "writing an abbreviated revision is refused");
	}

	std::string message;

	check(rejects("future.lock",
		"version: 2\nreference:\n  repo: x\n  rev: " + sha + "\n", message)
		&& contains(message, "version 2"), "a newer version is refused by number");
	check(contains(message, "Upgrade nsmbtool"), "and says what to do about it");

	check(rejects("short.read.lock",
		"version: 1\nreference:\n  repo: x\n  rev: ac82391\n", message)
		&& contains(message, "full commit hash"), "an abbreviated revision is refused");
	check(contains(message, "reference use ac82391"), "and offers the command that resolves it");

	check(rejects("norev.lock",
		"version: 1\nreference:\n  repo: x\n", message)
		&& contains(message, "rev"), "a missing revision is refused");

	check(rejects("norepo.lock",
		"version: 1\nreference:\n  rev: " + sha + "\n", message)
		&& contains(message, "repo"), "a missing repo is refused");

	check(rejects("noref.lock", "version: 1\n", message)
		&& contains(message, "reference"), "a missing reference mapping is refused");

	check(rejects("noversion.lock",
		"reference:\n  repo: x\n  rev: " + sha + "\n", message)
		&& contains(message, "version"), "a missing version is refused");

	check(rejects("zero.lock",
		"version: 0\nreference:\n  repo: x\n  rev: " + sha + "\n", message),
		"version 0 is refused");

	check(rejects("scalar.lock", "just a string\n", message), "a non-mapping is refused");
	check(rejects("broken.lock", "version: 1\n  bad indent: [\n", message),
		"unparseable YAML is refused");

	// The path is in every message, because a project can have a lock above it
	// that it never looked at.
	check(contains(message, "broken.lock"), "the failing file is named");

	fs::remove_all(g_dir);

	if (g_failures == 0)
		std::cout << "All lockfile tests passed\n";
	return g_failures == 0 ? 0 : 1;
}
