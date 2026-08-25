#include "manifest.hpp"

#include <algorithm>
#include <fstream>

#include <yaml-cpp/yaml.h>

#include "except.hpp"
#include "log.hpp"
#include "unicode.hpp"

namespace fs = std::filesystem;

namespace nsmb {

namespace {

constexpr std::string_view SCHEMA = "ncpatcher.files/1";

[[noreturn]] void fail(const fs::path& file, const std::string& what)
{
	throw nsmb::exception(pathToUtf8Generic(file) + ": " + what);
}

} // namespace

Manifest readManifest(const fs::path& file)
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
		fail(file, std::string("Could not be read: ") + e.what() + ".");
	}

	if (!root.IsMap())
		fail(file, "Is not a file manifest.");

	const YAML::Node schema = root["schema"];
	if (!schema || !schema.IsScalar() || schema.Scalar() != SCHEMA)
	{
		fail(file, "Is not " ANSI_bWHITE + std::string(SCHEMA) + ANSI_RESET ".\n"
			"       Regenerate it with a matching NCPatcher, or point "
			ANSI_bCYAN "--manifest" ANSI_RESET " at the right file.");
	}

	Manifest manifest;

	const YAML::Node variant = root["variant"];
	if (variant && variant.IsScalar())
		manifest.variant = variant.Scalar();

	const YAML::Node files = root["files"];
	if (!files || !files.IsSequence())
		fail(file, "Has no " ANSI_bCYAN "files" ANSI_RESET " list.");

	manifest.files.reserve(files.size());
	for (const YAML::Node& node : files)
	{
		if (!node.IsMap())
			fail(file, "Has a file entry that is not a mapping.");

		ManifestFile entry;

		const YAML::Node path = node["path"];
		if (!path || !path.IsScalar() || path.Scalar().empty())
			fail(file, "Has a file entry with no " ANSI_bCYAN "path" ANSI_RESET ".");
		entry.path = path.Scalar();

		const YAML::Node id = node["id"];
		if (!id || !id.IsScalar())
		{
			fail(file, "File " ANSI_bWHITE + entry.path + ANSI_RESET
				" has no " ANSI_bCYAN "id" ANSI_RESET ".");
		}

		try
		{
			const unsigned long long value = std::stoull(id.Scalar());
			if (value > 0xFFFFFFFFull)
				throw std::out_of_range("id");
			entry.id = std::uint32_t(value);
		}
		catch (const std::exception&)
		{
			fail(file, "File " ANSI_bWHITE + entry.path + ANSI_RESET " has id "
				ANSI_bWHITE + id.Scalar() + ANSI_RESET ", which is not a file id.");
		}

		manifest.files.push_back(std::move(entry));
	}

	// NCPatcher writes them in id order already. Sorting anyway costs nothing
	// and means the generators do not have to trust that it always will.
	std::sort(manifest.files.begin(), manifest.files.end(),
		[](const ManifestFile& left, const ManifestFile& right) { return left.id < right.id; });

	return manifest;
}

} // namespace nsmb
