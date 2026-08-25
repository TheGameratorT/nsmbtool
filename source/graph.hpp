#pragma once

// NCPatcher's resolved module graph, as much of it as the generators need.
//
// The dump is `ncpatcher.modules/1`, which NCPatcher writes as JSON. JSON is a
// subset of YAML 1.2, so yaml-cpp reads it without a second parser and without
// the two programs sharing a library -- which is the whole point of the split.
//
// Only the parts this tool acts on are modelled. Everything NCPatcher passes
// through untouched (`objects:`, `level-data:`) arrives under `extra`, because
// NCPatcher deliberately has no opinion about what those mean.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nsmb {

// One `objects:` entry in a component's `extra`.
struct ObjectDef
{
	std::string name;    // CoopFlagActor
	std::string type;    // actor | scene
	std::string header;  // coop/actors/CoopFlagActor.hpp

	// The `stage:` variants, in declaration order. Each one becomes a separate
	// placeable entry with its own ObjectInfo -- one actor class, several
	// geometries. Empty means the object cannot be placed in a level at all,
	// which is the normal case for a scene.
	std::vector<std::string> stage;
};

struct ComponentDef
{
	std::string name;
	bool enabled = true;

	// The overlay the component's code is patched into, when it has one. A
	// scene object declared here is reachable only once that overlay is
	// resident, which is what the scene registry exists to say.
	std::optional<int> overlay;

	std::vector<ObjectDef> objects;
};

// One `level-data:` entry, before the module prefix is applied.
struct LevelDataDef
{
	std::string key;   // canFly
	std::string type;  // flag | u32 | u8[8] | u32[?]
};

struct ModuleDef
{
	std::string key;   // the directory name, and what the project lists
	std::string id;    // Coop -- what generated identifiers are named after
	bool enabled = false;

	std::vector<ComponentDef> components;
	std::vector<LevelDataDef> levelData;
};

struct Graph
{
	std::vector<ModuleDef> modules;
};

// Reads and validates a module dump. Throws with the file named on anything it
// cannot make sense of, including a schema it does not recognise.
[[nodiscard]] Graph readGraph(const std::filesystem::path& file);

} // namespace nsmb
