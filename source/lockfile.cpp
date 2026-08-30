#include "lockfile.hpp"

#include <cctype>
#include <fstream>
#include <sstream>

#include <yaml-cpp/yaml.h>

#include "except.hpp"
#include "unicode.hpp"
#include "log.hpp"

namespace fs = std::filesystem;

namespace nsmb {

namespace {

[[noreturn]] void fail(const fs::path& file, const std::string& what)
{
	throw nsmb::exception(pathToUtf8Generic(file) + ": " + what);
}

std::string requiredString(const YAML::Node& parent, const char* key, const fs::path& file)
{
	const YAML::Node node = parent[key];
	if (!node || node.IsNull())
		fail(file, std::string("Missing ") + ANSI_bCYAN + key + ANSI_RESET + ".");
	if (!node.IsScalar())
		fail(file, std::string(ANSI_bCYAN) + key + ANSI_RESET + " must be a string.");
	return node.Scalar();
}

} // namespace

bool isFullObjectName(std::string_view text)
{
	if (text.size() != 40 && text.size() != 64)
		return false;
	for (const char c : text)
	{
		if (!std::isxdigit(static_cast<unsigned char>(c)))
			return false;
	}
	return true;
}

Lockfile Lockfile::read(const fs::path& file)
{
	std::ifstream stream(file);
	if (!stream.is_open())
		fail(file, "Could not be opened.");

	YAML::Node root;
	try
	{
		root = YAML::Load(stream);
	}
	catch (const YAML::Exception& e)
	{
		fail(file, std::string("Is not valid YAML. ") + e.what());
	}

	if (!root || !root.IsMap())
		fail(file, "Should be a mapping with " ANSI_bCYAN "version" ANSI_RESET
		           " and " ANSI_bCYAN "reference" ANSI_RESET " keys.");

	const YAML::Node version = root["version"];
	if (!version || !version.IsScalar())
		fail(file, "Missing " ANSI_bCYAN "version" ANSI_RESET ".");

	int declared = 0;
	try
	{
		declared = version.as<int>();
	}
	catch (const YAML::Exception&)
	{
		fail(file, ANSI_bCYAN "version" ANSI_RESET " must be a number.");
	}

	if (declared > VERSION)
	{
		std::ostringstream oss;
		oss << "Declares version " << declared << ", but this nsmbtool only understands "
		    << VERSION << ". Upgrade nsmbtool rather than editing the lock.";
		fail(file, oss.str());
	}
	if (declared < 1)
		fail(file, ANSI_bCYAN "version" ANSI_RESET " must be at least 1.");

	const YAML::Node reference = root["reference"];
	if (!reference || !reference.IsMap())
		fail(file, "Missing the " ANSI_bCYAN "reference" ANSI_RESET " mapping.");

	Lockfile lock;
	lock.file = file;
	lock.repo = requiredString(reference, "repo", file);
	lock.rev = requiredString(reference, "rev", file);

	if (lock.repo.empty())
		fail(file, ANSI_bCYAN "reference.repo" ANSI_RESET " is empty.");

	if (!isFullObjectName(lock.rev))
	{
		fail(file, ANSI_bCYAN "reference.rev" ANSI_RESET " must be a full commit hash, not "
		           ANSI_bWHITE "\"" + lock.rev + "\"" ANSI_RESET ".\n"
		           "       Run " ANSI_bCYAN "nsmbtool reference use " + lock.rev + ANSI_RESET
		           " to resolve and record it.");
	}

	return lock;
}

void Lockfile::write(const fs::path& file) const
{
	if (!isFullObjectName(rev))
		throw nsmb::exception("Refusing to write a lock naming \"" + rev + "\", which is not a full commit hash.");

	// Written by hand rather than emitted. The comment is most of the value of
	// the file -- it is the only place the reader is told that the pin is the
	// point -- and an emitter would drop it on the next rewrite.
	std::ostringstream out;
	out << "# The NSMB code reference this project builds against.\n"
	    << "#\n"
	    << "# Commit this file. `nsmbtool reference sync` materialises the revision\n"
	    << "# below and puts NSMBREF_ROOT in .ncpatcher.env, which NCPatcher reads.\n"
	    << "# Change it with `nsmbtool reference use <branch|tag|commit>`.\n"
	    << "\n"
	    << "version: " << VERSION << "\n"
	    << "reference:\n"
	    << "  repo: " << repo << "\n"
	    << "  rev: " << rev << "\n";

	const std::string text = out.str();

	std::ofstream stream(file, std::ios::binary | std::ios::trunc);
	if (!stream.is_open())
		throw nsmb::exception("Could not write " + pathToUtf8Generic(file) + ".");
	stream.write(text.data(), std::streamsize(text.size()));
	if (!stream)
		throw nsmb::exception("Could not write " + pathToUtf8Generic(file) + ".");
}

} // namespace nsmb
