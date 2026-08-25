// Tests for source/process.cpp -- spawning git.
//
// This is the riskiest code in the tool, because it is the only part with two
// completely separate implementations. The Windows half is invisible to a native
// build on anything else, so it is easy to break and never find out.
//
// What is checked is the part that is genuinely hard: an argument the machine's
// ANSI code page cannot spell. Windows has a narrow and a wide version of every
// API, and the narrow half of CreateProcess would mangle "José" -- on the one
// machine where the user cannot rename themselves out of the problem.
//
// The child is asked to *create* something named after the argument rather than
// echo it back, because a console echoes through the console code page, which
// cannot spell these characters even when it received them perfectly. A
// directory on disk is the unambiguous evidence.

#include "../source/process.hpp"
#include "../source/unicode.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

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

// Making a directory, spelled for whichever shell is present.
static std::vector<std::string> makeDirectory(const std::string& name)
{
#ifdef _WIN32
	return { "cmd.exe", "/c", "mkdir", name };
#else
	return { "/bin/mkdir", name };
#endif
}

static std::vector<std::string> echoText(const std::string& text)
{
#ifdef _WIN32
	return { "cmd.exe", "/c", "echo", text };
#else
	return { "/bin/echo", text };
#endif
}

int main()
{
	const fs::path base = fs::temp_directory_path() / "nsmbtool-process-test";
	fs::remove_all(base);
	fs::create_directories(base);

	// "José-文字": a Latin-1 character the OEM code page mangles, and two
	// that no single-byte code page can hold at all.
	const std::string tricky = "Jos\xc3\xa9-\xe6\x96\x87\xe5\xad\x97";
	const fs::path expected = base / nsmb::utf8ToPath(tricky);

	{
		const nsmb::ProcessResult result = nsmb::run(makeDirectory(tricky), base);
		check(result.ok(), "a child runs with a non-ASCII argument");
		check(fs::is_directory(expected), "and receives it byte for byte");
	}

	// The working directory travels by a different route from the arguments --
	// on Windows it is already UTF-16 and needs no conversion at all -- so it is
	// worth its own check.
	{
		const nsmb::ProcessResult result = nsmb::run(makeDirectory("marker"), expected);
		check(result.ok(), "a child runs in a non-ASCII working directory");
		check(fs::is_directory(expected / "marker"), "and it is the right one");
	}

	// Quoting is hand-rolled on Windows, against the rules CommandLineToArgvW
	// undoes on the other side.
	{
		const nsmb::ProcessResult result = nsmb::run(makeDirectory("two words"), base);
		check(result.ok() && fs::is_directory(base / "two words"),
			"an argument containing a space stays one argument");
	}

#ifdef _WIN32
	// The rule the hand-rolled quoting exists for: backslashes are only special
	// where they run up against the closing quote, and a directory argument
	// ending in a separator is exactly where that happens. Get it wrong and the
	// backslash escapes the quote instead of standing for itself, and the rest
	// of the command line is swallowed into the argument.
	{
		const nsmb::ProcessResult result = nsmb::run(
			{ "cmd.exe", "/c", "mkdir", (base / "trailing sep").string() + "\\" });
		check(result.ok() && fs::is_directory(base / "trailing sep"),
			"an argument ending in a backslash does not escape its own closing quote");
	}
#else
	// A quote is a perfectly ordinary character in a POSIX filename, and there
	// is no command line to re-parse -- exec takes the vector as it stands.
	{
		const nsmb::ProcessResult result = nsmb::run(makeDirectory("quote\"inside"), base);
		check(result.ok() && fs::is_directory(base / "quote\"inside"),
			"an argument containing a quote survives, quoting rules or not");
	}
#endif

	{
		const nsmb::ProcessResult result = nsmb::run(echoText("plain-ascii"));
		check(result.ok(), "output capture: the child succeeds");
		check(result.out.find("plain-ascii") != std::string::npos, "and its stdout comes back");
	}

	// A program that ran and failed is a result, not an exception: the caller
	// almost always has something more useful to say than the exit status.
	{
		const nsmb::ProcessResult result = nsmb::run(makeDirectory(""));
		check(!result.ok(), "a child that fails reports a non-zero exit code");
	}

	// A program that could not be started at all is the exception, because there
	// is no exit code to report and no output to explain it.
	{
		bool threw = false;
		try { (void)nsmb::run({ "definitely-not-a-program-anywhere" }); }
		catch (const std::exception&) { threw = true; }
		check(threw, "a program that does not exist throws");
	}

	{
		bool threw = false;
		try { (void)nsmb::run({}); }
		catch (const std::exception&) { threw = true; }
		check(threw, "an empty command throws");
	}

	// Inherit mode writes to this test's own stdout, so there is nothing to
	// compare; what matters is that it still waits and still reports.
	{
		const nsmb::ProcessResult result = nsmb::run(makeDirectory("inherited"), base,
			nsmb::Capture::Inherit);
		check(result.ok() && fs::is_directory(base / "inherited"),
			"an uncaptured child still runs and is still waited for");
		check(result.out.empty(), "and captures nothing");
	}

	fs::remove_all(base);

	if (g_failures == 0)
		std::cout << "All process tests passed\n";
	return g_failures == 0 ? 0 : 1;
}
