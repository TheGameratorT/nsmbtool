// nsmbtool -- the NSMB-specific half of the toolchain.
//
// NCPatcher stays game-agnostic: it knows DS containers, module graphs and file
// ids, and nothing about New Super Mario Bros. Everything that is knowledge of
// *this game* lives here instead, and the two communicate through the JSON
// dumps NCPatcher writes rather than through a shared library. That is why this
// is a separate program with a separate repository and no build-order
// relationship to the patcher.
//
// Today it manages the code reference. The glue code generators come next, and
// they will read `ncpatcher.modules/1` and `ncpatcher.files/1`.

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

#include "except.hpp"
#include "log.hpp"
#include "reference.hpp"
#include "unicode.hpp"

namespace {

constexpr const char* VERSION = "0.1.0";

void printUsage()
{
	std::cout << nsmb::log::paint(
ANSI_bWHITE "nsmbtool" ANSI_RESET " " ANSI_CYAN "<command>" ANSI_RESET " [options]\n"
"\n"
ANSI_bWHITE "Commands\n" ANSI_RESET
"  " ANSI_bCYAN "reference list" ANSI_RESET "              Revisions in the store, and which this project uses\n"
"  " ANSI_bCYAN "reference use" ANSI_RESET " <rev>         Pin a branch, tag or commit, then sync\n"
"  " ANSI_bCYAN "reference sync" ANSI_RESET "              Materialise the locked revision and write .ncpatcher.env\n"
"  " ANSI_bCYAN "reference path" ANSI_RESET " [<rev>]      Print a revision's directory\n"
"  " ANSI_bCYAN "reference gc" ANSI_RESET "                Remove revisions no known project names\n"
"\n"
ANSI_bWHITE "Options\n" ANSI_RESET
"  " ANSI_bCYAN "-C" ANSI_RESET " <dir>                    Act as if started in <dir>\n"
"  " ANSI_bCYAN "--offline" ANSI_RESET "                   Fail rather than reach the network\n"
"  " ANSI_bCYAN "--repo" ANSI_RESET " <url>                With " ANSI_bCYAN "use" ANSI_RESET ", change which repository is pinned\n"
"  " ANSI_bCYAN "--dry-run" ANSI_RESET "                   With " ANSI_bCYAN "gc" ANSI_RESET ", list what would go and remove nothing\n"
"  " ANSI_bCYAN "--no-color" ANSI_RESET "                  Never write escape sequences\n"
"  " ANSI_bCYAN "-h" ANSI_RESET ", " ANSI_bCYAN "--help" ANSI_RESET ", " ANSI_bCYAN "--version" ANSI_RESET "\n"
"\n"
ANSI_bWHITE "Environment\n" ANSI_RESET
"  " ANSI_bCYAN "NSMBTOOL_STORE" ANSI_RESET "           Where revisions are kept; defaults to the user data directory\n");
}

// The arguments as UTF-8.
//
// On Windows the char** main gets has already been through the ANSI code page,
// so a path argument naming a directory the code page cannot spell arrives
// mangled. GetCommandLineW is the same command line before that happened.
std::vector<std::string> commandLineArguments(int argc, char** argv)
{
#ifdef _WIN32
	(void)argc;
	(void)argv;

	int count = 0;
	LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
	if (wide == nullptr)
		return {};

	std::vector<std::string> args;
	args.reserve(std::size_t(count > 0 ? count - 1 : 0));
	for (int i = 1; i < count; i++)
		args.push_back(nsmb::toUtf8(wide[i]));

	LocalFree(wide);
	return args;
#else
	return std::vector<std::string>(argv + 1, argv + argc);
#endif
}

[[noreturn]] void usageError(const std::string& what)
{
	nsmb::log::error(what);
	std::cerr << "\n";
	printUsage();
	std::exit(2);
}

// Whether escape sequences are worth writing. NO_COLOR is honoured because it
// is the one convention every tool that grew a --no-color flag eventually
// agreed on.
bool shouldColor()
{
	if (std::getenv("NO_COLOR") != nullptr)
		return false;

#ifdef _WIN32
	if (_isatty(_fileno(stdout)) == 0)
		return false;

	// Consoles since Windows 10 understand the sequences, but only once asked.
	const HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD mode = 0;
	if (console == INVALID_HANDLE_VALUE || !GetConsoleMode(console, &mode))
		return false;
	return SetConsoleMode(console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
	return isatty(fileno(stdout)) != 0;
#endif
}

} // namespace

int main(int argc, char** argv)
{
	nsmb::log::setColorEnabled(shouldColor());

	std::vector<std::string> args = commandLineArguments(argc, argv);

	nsmb::ReferenceOptions options;
	std::vector<std::string> positional;

	for (std::size_t i = 0; i < args.size(); i++)
	{
		const std::string& argument = args[i];

		const auto valueOf = [&](const char* name) -> std::string {
			if (i + 1 >= args.size())
				usageError(std::string(name) + " needs a value.");
			return args[++i];
		};

		if (argument == "-h" || argument == "--help")
		{
			printUsage();
			return 0;
		}
		else if (argument == "--version")
		{
			std::cout << "nsmbtool " << VERSION << std::endl;
			return 0;
		}
		else if (argument == "--no-color" || argument == "--no-colour")
		{
			nsmb::log::setColorEnabled(false);
		}
		else if (argument == "--offline")
		{
			options.offline = true;
		}
		else if (argument == "--dry-run")
		{
			options.dryRun = true;
		}
		else if (argument == "-C")
		{
			options.directory = valueOf("-C");
		}
		else if (argument == "--repo")
		{
			options.repo = valueOf("--repo");
		}
		else if (argument.starts_with("-") && argument != "-")
		{
			usageError("Unknown option " ANSI_bWHITE + argument + ANSI_RESET ".");
		}
		else
		{
			positional.push_back(argument);
		}
	}

	if (positional.empty())
	{
		printUsage();
		return 2;
	}

	try
	{
		// `ref` because this one gets typed a lot and the long form is the
		// documented spelling, not a test of anyone's patience.
		if (positional[0] != "reference" && positional[0] != "ref")
			usageError("Unknown command " ANSI_bWHITE + positional[0] + ANSI_RESET ".");

		if (positional.size() < 2)
			usageError(ANSI_bCYAN "reference" ANSI_RESET " needs a subcommand.");

		const std::string& subcommand = positional[1];
		const std::size_t extra = positional.size() - 2;

		if (subcommand == "list")
		{
			if (extra != 0)
				usageError(ANSI_bCYAN "reference list" ANSI_RESET " takes no arguments.");
			return nsmb::referenceList(options);
		}
		if (subcommand == "use")
		{
			if (extra != 1)
				usageError(ANSI_bCYAN "reference use" ANSI_RESET " needs exactly one revision.");
			return nsmb::referenceUse(options, positional[2]);
		}
		if (subcommand == "sync")
		{
			if (extra != 0)
				usageError(ANSI_bCYAN "reference sync" ANSI_RESET " takes no arguments.");
			return nsmb::referenceSync(options);
		}
		if (subcommand == "path")
		{
			if (extra > 1)
				usageError(ANSI_bCYAN "reference path" ANSI_RESET " takes at most one revision.");
			return nsmb::referencePath(options, extra == 1 ? positional[2] : std::string());
		}
		if (subcommand == "gc")
		{
			if (extra != 0)
				usageError(ANSI_bCYAN "reference gc" ANSI_RESET " takes no arguments.");
			return nsmb::referenceGc(options);
		}

		usageError("Unknown subcommand " ANSI_bWHITE + subcommand + ANSI_RESET ".");
	}
	catch (const std::exception& e)
	{
		nsmb::log::error(e.what());
		return 1;
	}
}
