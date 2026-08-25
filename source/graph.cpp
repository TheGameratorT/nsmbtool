#include "graph.hpp"

#include <fstream>

#include <yaml-cpp/yaml.h>

#include "except.hpp"
#include "log.hpp"
#include "unicode.hpp"

namespace fs = std::filesystem;

namespace nsmb {

namespace {

constexpr std::string_view SCHEMA = "ncpatcher.modules/1";

[[noreturn]] void fail(const fs::path& file, const std::string& what)
{
	throw nsmb::exception(pathToUtf8Generic(file) + ": " + what);
}

// A YAML key that may hold one item or a list of them, as one list.
//
// `stage: Default` and `stage: [Default]` mean the same thing, and a config
// language that insists on the brackets for the common case is one people get
// wrong.
std::vector<YAML::Node> asList(const YAML::Node& node)
{
	if (!node || node.IsNull())
		return {};
	if (!node.IsSequence())
		return { node };

	std::vector<YAML::Node> out;
	for (const YAML::Node& item : node)
		out.push_back(item);
	return out;
}

// yaml-cpp throws rather than answering when a missing key is indexed again,
// so a nested lookup has to be spelled out step by step. `extra` is absent from
// most modules and most components, which makes this the common path.
YAML::Node child(const YAML::Node& parent, const char* key)
{
	if (!parent || !parent.IsMap())
		return YAML::Node(YAML::NodeType::Undefined);
	return parent[key];
}

std::string requiredString(const YAML::Node& parent, const char* key,
	const fs::path& file, const std::string& where)
{
	const YAML::Node node = parent[key];
	if (!node || node.IsNull() || !node.IsScalar() || node.Scalar().empty())
		fail(file, where + " has no " + ANSI_bCYAN + key + ANSI_RESET + ".");
	return node.Scalar();
}

ObjectDef readObject(const YAML::Node& node, const fs::path& file, const std::string& where)
{
	if (!node.IsMap())
		fail(file, where + " has an " + ANSI_bCYAN "objects" ANSI_RESET " entry that is not a mapping.");

	ObjectDef object;
	object.name = requiredString(node, "name", file, where + "'s object");

	const std::string what = where + "'s object " ANSI_bWHITE + object.name + ANSI_RESET;

	const YAML::Node type = node["type"];
	object.type = (type && type.IsScalar()) ? type.Scalar() : "actor";
	if (object.type != "actor" && object.type != "scene")
	{
		fail(file, what + " has type " ANSI_bWHITE + object.type + ANSI_RESET ", which is neither "
			ANSI_bCYAN "actor" ANSI_RESET " nor " ANSI_bCYAN "scene" ANSI_RESET ".");
	}

	object.header = requiredString(node, "header", file, what);

	for (const YAML::Node& variant : asList(node["stage"]))
	{
		if (!variant.IsScalar() || variant.Scalar().empty())
			fail(file, what + " has an empty " ANSI_bCYAN "stage" ANSI_RESET " variant.");
		object.stage.push_back(variant.Scalar());
	}

	return object;
}

} // namespace

Graph readGraph(const fs::path& file)
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
		fail(file, "Is not a module dump.");

	const YAML::Node schema = root["schema"];
	if (!schema || !schema.IsScalar() || schema.Scalar() != SCHEMA)
	{
		fail(file, "Is not " ANSI_bWHITE + std::string(SCHEMA) + ANSI_RESET ".\n"
			"       Regenerate it with a matching NCPatcher, or point "
			ANSI_bCYAN "--graph" ANSI_RESET " at the right file.");
	}

	Graph graph;

	for (const YAML::Node& moduleNode : asList(root["modules"]))
	{
		if (!moduleNode.IsMap())
			fail(file, "Has a module that is not a mapping.");

		ModuleDef module;
		module.key = requiredString(moduleNode, "key", file, "A module");

		const YAML::Node enabled = moduleNode["enabled"];
		module.enabled = enabled && enabled.IsScalar() && enabled.Scalar() == "true";

		// A disabled module was never read past its name, so there is nothing
		// below to look at. Keeping it in the list is still worth doing: it is
		// how a generator can say why a module contributed nothing.
		if (!module.enabled)
		{
			graph.modules.push_back(std::move(module));
			continue;
		}

		module.id = requiredString(moduleNode, "id", file,
			"Module " ANSI_bWHITE + module.key + ANSI_RESET);

		const std::string moduleWhere = "Module " ANSI_bWHITE + module.id + ANSI_RESET;

		const YAML::Node levelData = child(child(moduleNode, "extra"), "level-data");
		if (levelData && !levelData.IsNull())
		{
			if (!levelData.IsMap())
			{
				fail(file, moduleWhere + "'s " ANSI_bCYAN "level-data" ANSI_RESET
					" is not a mapping of key to type.");
			}

			for (auto it = levelData.begin(); it != levelData.end(); ++it)
			{
				LevelDataDef entry;
				entry.key = it->first.Scalar();
				if (!it->second.IsScalar() || it->second.Scalar().empty())
				{
					fail(file, moduleWhere + "'s " ANSI_bCYAN "level-data" ANSI_RESET " key "
						ANSI_bWHITE + entry.key + ANSI_RESET " has no type.");
				}
				entry.type = it->second.Scalar();
				module.levelData.push_back(std::move(entry));
			}
		}

		for (const YAML::Node& componentNode : asList(moduleNode["components"]))
		{
			if (!componentNode.IsMap())
				fail(file, moduleWhere + " has a component that is not a mapping.");

			ComponentDef component;
			component.name = requiredString(componentNode, "name", file,
				moduleWhere + "'s component");

			const YAML::Node componentEnabled = componentNode["enabled"];
			component.enabled = !componentEnabled || !componentEnabled.IsScalar()
				|| componentEnabled.Scalar() == "true";

			const YAML::Node overlay = child(child(componentNode, "target"), "overlay");
			if (overlay && overlay.IsScalar())
			{
				try
				{
					component.overlay = std::stoi(overlay.Scalar());
				}
				catch (const std::exception&)
				{
					fail(file, moduleWhere + "'s component " ANSI_bWHITE + component.name
						+ ANSI_RESET " has an overlay that is not a number.");
				}
			}

			const std::string where = moduleWhere + "'s component "
				ANSI_bWHITE + component.name + ANSI_RESET;

			for (const YAML::Node& objectNode : asList(child(child(componentNode, "extra"), "objects")))
				component.objects.push_back(readObject(objectNode, file, where));

			module.components.push_back(std::move(component));
		}

		graph.modules.push_back(std::move(module));
	}

	return graph;
}

} // namespace nsmb
