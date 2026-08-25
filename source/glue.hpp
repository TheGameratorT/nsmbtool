#pragma once

// The glue code generators.
//
// NCPatcher resolves the module graph and the ROM's file table and writes them
// out; everything below turns those two dumps into the headers the game's code
// is compiled against, plus the two JSON contracts a level editor would read.
//
// The knowledge that makes this game-specific lives in three constants, and
// nowhere else. None of them may be derived from a dump: they are facts about
// how New Super Mario Bros. is written, not about whatever ROM is loaded.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "graph.hpp"
#include "manifest.hpp"

namespace nsmb {

// The first object id past the game's own object table. Custom objects are
// allocated upward from here, and the generated headers emit the literal so the
// C++ side keeps seeing the number it always has.
inline constexpr std::uint16_t OBJECT_ID_BASE = 0x182;

// Where the vanilla stage-object table ends. Editors allocate `325 + n` per
// level; nothing here does, which is why this constant is only documentation
// for the contract and never an index.
inline constexpr std::uint16_t STAGE_OBJECT_ID_BASE = 325;

// The game subtracts this from every file id it is handed, because the ROM's
// table counts the 131 overlays before the NitroFS files begin. It cannot be
// read off the manifest: a `create`-mode region that adds overlay 131 changes
// the ROM's overlay count while every existing file id must stay exactly where
// it is, so the number the code was written against is the only correct one.
inline constexpr std::uint32_t FILE_ID_OFFSET = 131;

// FNV-1a, 32-bit. Must keep agreeing with Glue::hash in the game's hash.hpp
// byte for byte -- a level stores the hash, so changing it invalidates levels
// rather than failing a build.
[[nodiscard]] std::uint32_t fnv1a(std::string_view text);

enum class LevelDataKind
{
	Flag,       // present or absent; no payload, and no C++ getter
	Value,      // u32
	Array,      // u8[8]
	SizedArray  // u32[?] -- a count followed by that many elements
};

struct LevelDataType
{
	LevelDataKind kind = LevelDataKind::Flag;

	// The element or value type. Empty for a flag, which has neither.
	std::string name;

	// Set only for Array.
	std::optional<int> size;
};

// Throws on anything that is not one of the four forms.
[[nodiscard]] LevelDataType parseLevelDataType(std::string_view text);

// One custom object, with the id it was allocated.
struct ObjectEntry
{
	std::string module;     // Coop
	std::string component;  // Vanilla
	std::string name;       // CoopFlagActor
	std::string type;       // actor | scene
	std::string header;
	std::optional<int> overlay;
	std::vector<std::string> stage;
	std::uint16_t id = 0;
};

// One placeable entry: an object as it can be dropped into a level. An object
// with `stage: [Default, Big]` is one class and two of these.
struct StageObjectEntry
{
	std::string key;   // coop.CoopFlagActor.Default -- the identity a level stores
	std::uint32_t hash = 0;
	std::uint16_t objectId = 0;
	std::string info;  // CoopFlagActor::ObjectInfo_Default
	std::string header;

	std::string module, component, object, variant;
};

struct LevelDataEntry
{
	std::string key;   // coop.canFly
	std::uint32_t hash = 0;
	LevelDataType type;
	std::string module;
};

// Everything the emitters need, resolved and ordered.
struct GlueModel
{
	std::vector<ObjectEntry> objects;
	std::vector<StageObjectEntry> stageObjects;
	std::vector<LevelDataEntry> levelData;

	// The module ids that own at least one object, in the order the object list
	// first mentions them. What the registry's #includes are generated from.
	[[nodiscard]] std::vector<std::string> objectModules() const;
};

// Collects, validates and orders. The sort on (module, component, name) is for
// reproducible builds and readable diffs -- object ids are compile-time
// constants that only have to agree with themselves within one build, so
// renumbering them invalidates nothing.
[[nodiscard]] GlueModel buildModel(const Graph& graph);

// Each emitter returns a whole file. They are separate from writing so that a
// test can compare text without a filesystem, and so that an unchanged file is
// left alone -- fid.hpp is included nearly everywhere, and rewriting it on
// every build would rebuild the project on every build.
[[nodiscard]] std::string emitObjectIds(const GlueModel& model, const std::string& moduleId);
[[nodiscard]] std::string emitObjectRegistry(const GlueModel& model);
[[nodiscard]] std::string emitExtendedProfiles(const GlueModel& model);
[[nodiscard]] std::string emitSceneRegistry(const GlueModel& model);
[[nodiscard]] std::string emitLevelDataGetters(const GlueModel& model);
[[nodiscard]] std::string emitExtendedStageObjects(const GlueModel& model);
[[nodiscard]] std::string emitFileIds(const Manifest& manifest);
[[nodiscard]] std::string emitLevelDataJson(const GlueModel& model);
[[nodiscard]] std::string emitStageObjectsJson(const GlueModel& model);

struct GlueOptions
{
	// The global -C.
	std::string directory;

	// All three are resolved against the project root when relative.
	std::string graph;
	std::string manifest;
	std::string out;
};

int glueGenerate(const GlueOptions& options);

} // namespace nsmb
