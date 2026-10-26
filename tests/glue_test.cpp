// Tests for source/glue.cpp -- the code generators.
//
// Two things here are not ordinary unit tests and are the reason this file
// exists at all.
//
// The hashes are a compatibility surface: a level stores the FNV-1a of
// `module.Object.Variant`, so a hash that changes silently invalidates every
// level already saved against it. The two values checked below are the ones the
// prototype had checked in, computed by an entirely separate implementation.
//
// The emitted text is compared literally rather than by parsing it, because the
// point of porting a generator is that the output does not move. A test that
// only checked "contains CoopFlagActor" would pass through a rewrite that
// changed the indentation of every generated header in the project.

#include "../source/glue.hpp"

#include <cstdio>
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

static void checkEqual(const std::string& got, const std::string& want, const std::string& what)
{
	if (got != want)
	{
		std::cout << "FAIL: " << what << "\n--- got ---\n" << got << "--- want ---\n" << want << "-----------\n";
		g_failures++;
	}
}

// The hash as the generators spell it, so the expected text below stays a
// statement about formatting rather than a second copy of the hash function.
static std::string hex(const char* key)
{
	char buffer[16];
	std::snprintf(buffer, sizeof(buffer), "0x%08x", nsmb::fnv1a(key));
	return buffer;
}

static fs::path g_dir;

static fs::path writeFile(const std::string& name, const std::string& text)
{
	const fs::path file = g_dir / name;
	fs::create_directories(file.parent_path());
	std::ofstream stream(file, std::ios::binary | std::ios::trunc);
	stream << text;
	return file;
}

// A graph with the shape the real project has: two modules, one of them with a
// component that is switched off, and objects spread across components so the
// ordering rule has something to do.
static const char* GRAPH = R"({
  "schema": "ncpatcher.modules/1",
  "dir": "modules",
  "modules": [
    {
      "key": "coop",
      "id": "Coop",
      "enabled": true,
      "extra": { "level-data": { "canFly": "flag", "canDie": "u32", "someArray": "u8[8]", "someVector": "u32[?]" } },
      "components": [
        {
          "name": "Vanilla",
          "enabled": true,
          "target": { "proc": "arm9", "overlay": null },
          "extra": { "objects": [
            { "name": "CoopFlagActor", "type": "actor", "header": "coop/actors/CoopFlagActor.hpp", "stage": ["Default", "Big"] },
            { "name": "CoopWorldUnlockSign", "type": "actor", "header": "coop/actors/CoopWorldUnlockSign.hpp" }
          ] }
        },
        {
          "name": "DesyncGuard",
          "enabled": true,
          "target": { "proc": "arm9", "overlay": 34 },
          "extra": { "objects": [
            { "name": "DesyncScene", "type": "scene", "header": "coop/scenes/DesyncScene.hpp" }
          ] }
        },
        {
          "name": "OffByDefault",
          "enabled": false,
          "target": { "proc": "arm9", "overlay": null },
          "extra": { "objects": [
            { "name": "NeverBuilt", "type": "actor", "header": "coop/actors/NeverBuilt.hpp" }
          ] }
        }
      ]
    },
    {
      "key": "dsimodewarn",
      "id": "DSiModeWarn",
      "enabled": true,
      "components": [
        {
          "name": "Vanilla",
          "enabled": true,
          "target": { "proc": "arm9", "overlay": null },
          "extra": { "objects": [
            { "name": "DSiModeScene", "type": "scene", "header": "dsimodewarn/DSiModeScene.hpp" }
          ] }
        }
      ]
    },
    { "key": "switched-off", "enabled": false }
  ]
})";

static const char* MANIFEST = R"({
  "schema": "ncpatcher.files/1",
  "variant": "en",
  "count": 4,
  "files": [
    { "id": 131, "path": "ARCHIVE/ARC0.narc", "size": 16 },
    { "id": 132, "path": "demo/boot.bin", "size": 16 },
    { "id": 2101, "path": "z_new/reserved", "size": 0, "action": "created" },
    { "id": 2102, "path": "z_new/coop/SE_VOC.nwav", "size": 4096, "action": "created" }
  ]
})";

static nsmb::GlueModel model()
{
	static const nsmb::Graph graph = nsmb::readGraph(writeFile("modules.json", GRAPH));
	return nsmb::buildModel(graph);
}

static bool rejectsGraph(const std::string& name, const std::string& text, std::string& message)
{
	try
	{
		(void)nsmb::buildModel(nsmb::readGraph(writeFile(name, text)));
		message.clear();
		return false;
	}
	catch (const std::exception& e)
	{
		message = e.what();
		return true;
	}
}

// Objects in a runtime module: their profiles are in the module, so the main
// binary's table holds a null for each, and the module carries the real ones.
static const char* RUNTIME_GRAPH = R"({
  "schema": "ncpatcher.modules/1",
  "modules": [ { "key": "coop", "id": "Coop", "enabled": true, "components": [
    { "name": "Bosses", "enabled": true,
      "target": { "proc": "arm9", "overlay": null, "runtime": "bosses" },
      "extra": { "objects": [
        { "name": "BossActor", "header": "coop/actors/BossActor.hpp", "stage": ["Default"] },
        { "name": "BossFireball", "header": "coop/actors/BossFireball.hpp" }
      ] } },
    { "name": "Vanilla", "enabled": true, "target": { "proc": "arm9", "overlay": null },
      "extra": { "objects": [
        { "name": "CoopFlagActor", "header": "coop/actors/CoopFlagActor.hpp", "stage": ["Default"] }
      ] } }
  ] } ]
})";

static const char* RUNTIME_MANIFEST = R"({
  "schema": "ncpatcher.files/1",
  "files": [
    { "id": 131, "path": "ARCHIVE/ARC0.narc" },
    { "id": 2101, "path": "z_new/ncprt/bosses.ncrt", "runtime-module": "bosses" }
  ]
})";

static std::string runtimeError(const std::string& name, const std::string& graph, const std::string& manifest)
{
	try
	{
		nsmb::GlueModel built = nsmb::buildModel(nsmb::readGraph(writeFile(name + ".json", graph)));
		nsmb::resolveRuntimeFiles(built, nsmb::readManifest(writeFile(name + "-files.json", manifest)));
		return {};
	}
	catch (const std::exception& e)
	{
		return e.what();
	}
}

static void testRuntimeModules()
{
	nsmb::GlueModel built = nsmb::buildModel(nsmb::readGraph(writeFile("runtime.json", RUNTIME_GRAPH)));
	nsmb::resolveRuntimeFiles(built, nsmb::readManifest(writeFile("runtime-files.json", RUNTIME_MANIFEST)));

	check(built.runtimeModules.size() == 1 && built.runtimeModules[0].name == "bosses",
		"a runtime module holding objects is collected");
	check(built.runtimeModules[0].fileId == 2101, "and gets the file the manifest marks as its image");
	check(built.stageObjects.size() == 2 && built.stageObjects[0].runtime == std::optional<std::size_t>(0)
		&& !built.stageObjects[1].runtime.has_value(),
		"a stage object knows which module it needs, and one in main needs none");

	checkEqual(nsmb::emitExtendedProfiles(built),
		"#pragma once\n"
		"\n"
		"// Generated profile table for extended objects\n"
		"// Do not edit this file directly\n"
		"\n"
		"#include \"coop/actors/CoopFlagActor.hpp\"\n"
		"\n"
		"namespace Glue::Object {\n"
		"\n"
		"// Generated extended profile table\n"
		"// A row left null is an object in a runtime module, filled in while that\n"
		"// module is loaded.\n"
		"const ObjectProfile* mainExtPT[] = {\n"
		"    nullptr,  // BossActor (0x182) in rt:bosses\n"
		"    nullptr,  // BossFireball (0x183) in rt:bosses\n"
		"    &CoopFlagActor::Profile,  // CoopFlagActor (0x184)\n"
		"};\n"
		"\n"
		"constinit const ObjectProfile* const* currentExtPT = mainExtPT;\n"
		"\n"
		"}\n",
		"extended_profiles.hpp leaves runtime objects null and does not include them");

	const std::string stageObjects = nsmb::emitExtendedStageObjects(built);
	check(stageObjects.find("MODULE_OBJ(" + hex("coop.BossActor.Default")
		+ ", 386, 0, &BossActor::ObjectInfo_Default, \"coop.BossActor.Default\"),") != std::string::npos,
		"a runtime stage object names its module by index");
	check(stageObjects.find("#include \"coop/actors/BossActor.hpp\"") != std::string::npos,
		"and its header is still included, for the ObjectInfo the main binary keeps");

	checkEqual(nsmb::emitRuntimeModules(built),
		"#pragma once\n"
		"\n"
		"// Generated runtime module table\n"
		"// Do not edit this file directly\n"
		"\n"
		"namespace Glue::Runtime {\n"
		"\n"
		"// Indexed by a stage object's runtime field. The file id is the game's, as\n"
		"// \"\"fid gives it.\n"
		"constexpr ModuleFile runtimeModules[] = {\n"
		"    { 1970, \"bosses\" },\n"
		"};\n"
		"\n"
		"constexpr u32 runtimeModuleCount = 1;\n"
		"\n"
		"}\n",
		"generated/glue/runtime_modules.hpp");

	checkEqual(nsmb::emitRuntimeExports(built, 0),
		"#pragma once\n"
		"\n"
		"// Generated exports for runtime module: bosses\n"
		"// Do not edit this file directly\n"
		"\n"
		"#include \"glue/runtime.hpp\"\n"
		"#include \"objectids/coop.hpp\"\n"
		"#include \"coop/actors/BossActor.hpp\"\n"
		"#include \"coop/actors/BossFireball.hpp\"\n"
		"\n"
		"namespace Glue::Runtime {\n"
		"\n"
		"// What Glue puts in its profile table while this module is loaded.\n"
		"static const Entry moduleEntries[] = {\n"
		"    { ObjectID::Coop::BossActor, &BossActor::Profile },\n"
		"    { ObjectID::Coop::BossFireball, &BossFireball::Profile },\n"
		"};\n"
		"\n"
		"}\n"
		"\n"
		"ncprt_exports(const Glue::Runtime::Exports) = { 2, Glue::Runtime::moduleEntries };\n",
		"generated/glue/runtime/bosses.hpp");

	check(nsmb::emitStageObjectsJson(built).find("\"runtime\": \"bosses\"") != std::string::npos,
		"stageobjects.json names the runtime module a stage object needs");

	std::string graph = RUNTIME_GRAPH;
	const std::string actor = R"({ "name": "BossActor", "header")";
	graph.replace(graph.find(actor), actor.size(), R"({ "name": "BossActor", "type": "scene", "header")");
	check(runtimeError("rt-scene", graph, RUNTIME_MANIFEST).find("Coop::BossActor") != std::string::npos,
		"a scene in a runtime module is refused");

	graph = RUNTIME_GRAPH;
	const std::string stage = R"(, "stage": ["Default"] },
        { "name": "BossFireball")";
	graph.replace(graph.find(stage), stage.size(), R"( },
        { "name": "BossFireball")");
	check(runtimeError("rt-unplaced", graph, RUNTIME_MANIFEST).find("rt:bosses") != std::string::npos,
		"a runtime module with no stage object is refused");

	std::string manifest = RUNTIME_MANIFEST;
	const std::string tag = R"(, "runtime-module": "bosses")";
	manifest.erase(manifest.find(tag), tag.size());
	check(runtimeError("rt-nofile", RUNTIME_GRAPH, manifest).find("not written into the ROM") != std::string::npos,
		"a runtime module with no file is refused");

	manifest = RUNTIME_MANIFEST;
	const std::string last = R"("runtime-module": "bosses" })";
	manifest.insert(manifest.find(last) + last.size(),
		R"(,
    { "id": 2102, "path": "z_new/ncprt/bosses2.ncrt", "runtime-module": "bosses" })");
	check(runtimeError("rt-twofiles", RUNTIME_GRAPH, manifest).find("bosses2.ncrt") != std::string::npos,
		"a runtime module written to two files is refused");
}

int main()
{
	g_dir = fs::temp_directory_path() / "nsmbtool-glue-test";
	fs::remove_all(g_dir);
	fs::create_directories(g_dir);

	// --- the hashes, which are a compatibility surface -----------------------

	check(nsmb::fnv1a("coop.canFly") == 0xc8dfeb91u,
		"coop.canFly hashes to the value the prototype had checked in");
	check(nsmb::fnv1a("coop.CoopFlagActor.Default") == 0xf46a44b9u,
		"coop.CoopFlagActor.Default hashes to the value the prototype had checked in");
	check(nsmb::fnv1a("") == 2166136261u,
		"the empty string hashes to the FNV-1a offset basis");

	// --- level-data types ----------------------------------------------------

	{
		const nsmb::LevelDataType flag = nsmb::parseLevelDataType("flag");
		check(flag.kind == nsmb::LevelDataKind::Flag && flag.name.empty(),
			"flag has no type name");

		const nsmb::LevelDataType value = nsmb::parseLevelDataType("u32");
		check(value.kind == nsmb::LevelDataKind::Value && value.name == "u32",
			"a bare type is a value");

		const nsmb::LevelDataType array = nsmb::parseLevelDataType("u8[8]");
		check(array.kind == nsmb::LevelDataKind::Array && array.name == "u8"
			&& array.size.value_or(0) == 8, "u8[8] is an array of eight");

		const nsmb::LevelDataType sized = nsmb::parseLevelDataType("u32[?]");
		check(sized.kind == nsmb::LevelDataKind::SizedArray && sized.name == "u32"
			&& !sized.size.has_value(), "u32[?] is a sized array");

		check(nsmb::parseLevelDataType("  u16  ").name == "u16",
			"surrounding space is not part of the type");

		for (const char* bad : { "", "u8[]", "u8[x]", "u8[0]", "[8]", "u8[8", "u8]8[" })
		{
			bool threw = false;
			try { (void)nsmb::parseLevelDataType(bad); }
			catch (const std::exception&) { threw = true; }
			check(threw, std::string("the level-data type \"") + bad + "\" is refused");
		}
	}

	// --- object collection and ordering --------------------------------------

	const nsmb::GlueModel built = model();

	check(built.objects.size() == 4, "the disabled component's object is not collected");

	{
		// Sorted by (module, component, name), then numbered from 0x182. The
		// component sort is why DesyncScene comes before CoopFlagActor.
		const std::string order = built.objects[0].name + " " + built.objects[1].name + " "
			+ built.objects[2].name + " " + built.objects[3].name;
		checkEqual(order, "DesyncScene CoopFlagActor CoopWorldUnlockSign DSiModeScene",
			"objects sort by module, then component, then name");

		check(built.objects[0].id == 0x182 && built.objects[3].id == 0x185,
			"object ids run upward from 0x182");
	}

	// --- stage variants ------------------------------------------------------

	check(built.stageObjects.size() == 2, "one entry per stage: variant, and none for an object without");
	checkEqual(built.stageObjects[0].key, "coop.CoopFlagActor.Default", "the stage key is module.Object.Variant");
	checkEqual(built.stageObjects[0].info, "CoopFlagActor::ObjectInfo_Default", "the ObjectInfo symbol is derived from the variant");
	check(built.stageObjects[0].objectId == built.stageObjects[1].objectId,
		"both variants of one object spawn the same class");
	check(built.stageObjects[1].hash == 0x27e61112u,
		"coop.CoopFlagActor.Big hashes to the value the prototype had checked in");

	// --- level data ----------------------------------------------------------

	check(built.levelData.size() == 4, "every level-data key is collected");
	checkEqual(built.levelData[0].key, "coop.canDie", "level-data keys sort within their module");
	check(built.levelData[0].hash == nsmb::fnv1a("coop.canDie"), "the key is what is hashed");

	// --- the generated headers ----------------------------------------------

	checkEqual(nsmb::emitObjectIds(built, "Coop"),
		"#pragma once\n"
		"\n"
		"#include <cstdint>\n"
		"\n"
		"// Generated object IDs for module ID: Coop\n"
		"// Do not edit this file directly\n"
		"\n"
		"namespace ObjectID::Coop {\n"
		"\n"
		"    static constexpr std::uint16_t DesyncScene = 0x182;\n"
		"    static constexpr std::uint16_t CoopFlagActor = 0x183;\n"
		"    static constexpr std::uint16_t CoopWorldUnlockSign = 0x184;\n"
		"\n"
		"}\n",
		"objectids/coop.hpp");

	checkEqual(nsmb::emitObjectRegistry(built),
		"#pragma once\n"
		"\n"
		"#include <cstdint>\n"
		"\n"
		"// Generated object registry\n"
		"// Do not edit this file directly\n"
		"\n"
		"#include \"objectids/coop.hpp\"\n"
		"#include \"objectids/dsimodewarn.hpp\"\n"
		"\n"
		"namespace Game {\n"
		"    static constexpr std::uint16_t ExtendedObjectsStart = 0x182;\n"
		"    static constexpr std::uint16_t ExtendedObjectsCount = 4;\n"
		"}\n",
		"object_registry.hpp");

	checkEqual(nsmb::emitExtendedProfiles(built),
		"#pragma once\n"
		"\n"
		"// Generated profile table for extended objects\n"
		"// Do not edit this file directly\n"
		"\n"
		"#include \"coop/scenes/DesyncScene.hpp\"\n"
		"#include \"coop/actors/CoopFlagActor.hpp\"\n"
		"#include \"coop/actors/CoopWorldUnlockSign.hpp\"\n"
		"#include \"dsimodewarn/DSiModeScene.hpp\"\n"
		"\n"
		"namespace Glue::Object {\n"
		"\n"
		"// Generated extended profile table\n"
		"const ObjectProfile* mainExtPT[] = {\n"
		"    &DesyncScene::Profile,  // DesyncScene (0x182)\n"
		"    &CoopFlagActor::Profile,  // CoopFlagActor (0x183)\n"
		"    &CoopWorldUnlockSign::Profile,  // CoopWorldUnlockSign (0x184)\n"
		"    &DSiModeScene::Profile,  // DSiModeScene (0x185)\n"
		"};\n"
		"\n"
		"constinit const ObjectProfile* const* currentExtPT = mainExtPT;\n"
		"\n"
		"}\n",
		"generated/glue/extended_profiles.hpp");

	// DesyncScene is a scene whose component targets overlay 34, so it needs an
	// entry; DSiModeScene is a scene patched into main, which is resident
	// already and must not appear.
	checkEqual(nsmb::emitSceneRegistry(built),
		"#pragma once\n"
		"\n"
		"#include <nsmb_nitro.hpp>\n"
		"\n"
		"// Generated scene overlay registry\n"
		"// Do not edit this file directly\n"
		"\n"
		"#include \"objectids/coop.hpp\"\n"
		"\n"
		"NTR_INLINE u32 getSceneOverlayID(u16 sceneID, u32 defaultOverlayID) {\n"
		"\n"
		"    switch (sceneID) {\n"
		"        case ObjectID::Coop::DesyncScene: return 34;\n"
		"        default: return defaultOverlayID;\n"
		"    }\n"
		"\n"
		"}\n",
		"scene_overlay_registry.hpp");

	checkEqual(nsmb::emitExtendedStageObjects(built),
		"#pragma once\n"
		"\n"
		"// Generated extended stage object table\n"
		"// Do not edit this file directly\n"
		"\n"
		"#include \"coop/actors/CoopFlagActor.hpp\"\n"
		"\n"
		"namespace Glue::StageObject {\n"
		"\n"
		"// One entry per stage: variant. The hash of module.Object.Variant is what a\n"
		"// level stores; the 326 + n stage object ids are the editor's, per level, and\n"
		"// appear nowhere in this table.\n"
		"//\n"
		"// The id that does appear is the other space entirely: the 0x182 + n object id\n"
		"// naming the class this stage object spawns. Two stage: variants of one object\n"
		"// share it and differ only in their ObjectInfo.\n"
		"ModuleStageObjectInfo moduleStageObjectInfos[] = {\n"
		"    MODULE_OBJ(0xf46a44b9, 387, Glue::Runtime::None, &CoopFlagActor::ObjectInfo_Default, \"coop.CoopFlagActor.Default\"),\n"
		"    MODULE_OBJ(0x27e61112, 387, Glue::Runtime::None, &CoopFlagActor::ObjectInfo_Big, \"coop.CoopFlagActor.Big\"),\n"
		"};\n"
		"\n"
		"constexpr u32 moduleStageObjectCount = 2;\n"
		"\n"
		"}\n",
		"generated/glue/extended_stageobjects.hpp");

	// The flag is absent: it has no payload to return, so there is no getter to
	// specialise, and hasFlag needs no table.
	checkEqual(nsmb::emitLevelDataGetters(built),
		"#pragma once\n"
		"\n"
		"// Generated glue level block attributes\n"
		"// Do not edit this file directly\n"
		"\n"
		"// Template specializations for level data types\n"
		"template<> struct GetterGetDataType<\"coop.canDie\"> { using type = u32; };\n"
		"template<> struct GetterGetDataType<\"coop.someArray\"> { using type = u8*; };\n"
		"template<> struct GetterGetDataType<\"coop.someVector\"> { using type = SizedArray<u32>; };\n"
		"\n"
		"// Overload for get that deduces type from hash\n"
		"template<CTString HashStr>\n"
		"NTR_INLINE std::optional<GetterDataType<HashStr>> get() {\n"
		"\t// coop.canDie\n"
		"\tif constexpr (Hash(HashStr.data).value == " + hex("coop.canDie") + ") {\n"
		"\t\treturn getValue<u32>(" + hex("coop.canDie") + ");\n"
		"\t}\n"
		"\t// coop.someArray\n"
		"\tif constexpr (Hash(HashStr.data).value == " + hex("coop.someArray") + ") {\n"
		"\t\treturn getArray<u8>(" + hex("coop.someArray") + ");\n"
		"\t}\n"
		"\t// coop.someVector\n"
		"\tif constexpr (Hash(HashStr.data).value == " + hex("coop.someVector") + ") {\n"
		"\t\treturn getSizedArray<u32>(" + hex("coop.someVector") + ");\n"
		"\t}\n"
		"\treturn {};\n"
		"}\n",
		"generated/glue/level_data_getters.hpp");

	// --- file ids ------------------------------------------------------------

	{
		const nsmb::Manifest manifest = nsmb::readManifest(writeFile("files.json", MANIFEST));
		checkEqual(nsmb::emitFileIds(manifest),
			"#pragma once\n"
			"\n"
			"#include <cstdint>\n"
			"#include <string_view>\n"
			"\n"
			"consteval std::uint16_t operator\"\"fid(const char* str, std::size_t len) {\n"
			"    std::string_view path{str, len};\n"
			"\n"
			"    if (path == \"ARCHIVE/ARC0.narc\") return 0;\n"
			"    if (path == \"demo/boot.bin\") return 1;\n"
			"    if (path == \"reserved\") return 1970;\n"
			"    if (path == \"coop/SE_VOC.nwav\") return 1971;\n"
			"}\n",
			"fid.hpp drops the z_new/ prefix and subtracts the 131 overlays");

		check(manifest.variant == "en", "the manifest's variant is read");
	}

	{
		// A z_new/ addition that collides with a vanilla path once the prefix
		// is dropped would give one literal two meanings.
		const fs::path file = writeFile("collide.json", R"({
  "schema": "ncpatcher.files/1",
  "files": [
    { "id": 200, "path": "coop/x.bin" },
    { "id": 900, "path": "z_new/coop/x.bin" }
  ]
})");
		bool threw = false;
		std::string message;
		try { (void)nsmb::emitFileIds(nsmb::readManifest(file)); }
		catch (const std::exception& e) { threw = true; message = e.what(); }
		check(threw && message.find("coop/x.bin") != std::string::npos,
			"two files that share a name once z_new/ is dropped are refused");
	}

	// --- the contracts -------------------------------------------------------

	{
		const std::string json = nsmb::emitStageObjectsJson(built);
		check(json.find("\"schema\": \"nsmbtool.stageobjects/1\"") != std::string::npos,
			"stageobjects.json names its schema");
		check(json.find("\"stageObjectIdBase\": 326") != std::string::npos,
			"stageobjects.json states the base the editor allocates from");
		check(json.find("\"objectId\": 387") != std::string::npos,
			"stageobjects.json carries the object id, so the editor knows what spawns");
		check(json.find("\"runtime\": null") != std::string::npos,
			"stageobjects.json says an object in the main binary needs no runtime module");
		check(json.find("\"stageObjectId\"") == std::string::npos,
			"stageobjects.json carries no stage object id -- those are the editor's, per level");
	}

	{
		const std::string json = nsmb::emitLevelDataJson(built);
		check(json.find("\"schema\": \"nsmbtool.leveldata/1\"") != std::string::npos,
			"level_data.json names its schema");
		check(json.find("\"dataType\": \"flag\"") != std::string::npos,
			"level_data.json keeps the flags the C++ header drops");
		check(json.find("\"arraySize\": 8") != std::string::npos,
			"level_data.json carries the arity of a fixed array");
	}

	// --- refusals ------------------------------------------------------------

	{
		std::string message;
		check(rejectsGraph("dup-object.json", R"({
  "schema": "ncpatcher.modules/1",
  "modules": [ { "key": "coop", "id": "Coop", "enabled": true, "components": [
    { "name": "A", "enabled": true, "extra": { "objects": [ { "name": "Thing", "header": "a.hpp" } ] } },
    { "name": "B", "enabled": true, "extra": { "objects": [ { "name": "Thing", "header": "b.hpp" } ] } }
  ] } ]
})", message) && message.find("Coop::Thing") != std::string::npos,
			"one object name declared by two components is refused");

		check(rejectsGraph("dup-stage.json", R"({
  "schema": "ncpatcher.modules/1",
  "modules": [ { "key": "coop", "id": "Coop", "enabled": true, "components": [
    { "name": "A", "enabled": true, "extra": { "objects": [
      { "name": "Thing", "header": "a.hpp", "stage": ["Big", "Big"] } ] } }
  ] } ]
})", message) && message.find("Big") != std::string::npos,
			"one stage variant listed twice is refused");

		check(rejectsGraph("bad-type.json", R"({
  "schema": "ncpatcher.modules/1",
  "modules": [ { "key": "coop", "id": "Coop", "enabled": true,
    "extra": { "level-data": { "oops": "u8[]" } }, "components": [] } ]
})", message) && message.find("oops") != std::string::npos,
			"a level-data type with empty brackets names the key it came from");

		check(rejectsGraph("bad-obj-type.json", R"({
  "schema": "ncpatcher.modules/1",
  "modules": [ { "key": "coop", "id": "Coop", "enabled": true, "components": [
    { "name": "A", "enabled": true, "extra": { "objects": [
      { "name": "Thing", "type": "sprite", "header": "a.hpp" } ] } }
  ] } ]
})", message) && message.find("sprite") != std::string::npos,
			"an object type that is neither actor nor scene is refused");

		check(rejectsGraph("no-header.json", R"({
  "schema": "ncpatcher.modules/1",
  "modules": [ { "key": "coop", "id": "Coop", "enabled": true, "components": [
    { "name": "A", "enabled": true, "extra": { "objects": [ { "name": "Thing" } ] } }
  ] } ]
})", message) && message.find("header") != std::string::npos,
			"an object with no header is refused");

		check(rejectsGraph("wrong-schema.json",
			R"({ "schema": "ncpatcher.modules/2", "modules": [] })", message)
			&& message.find("ncpatcher.modules/1") != std::string::npos,
			"a schema this build does not know is refused by name");
	}

	// A graph with nothing in it still has to produce compilable headers, since
	// the module source includes them unconditionally.
	{
		const nsmb::GlueModel empty = nsmb::buildModel(
			nsmb::readGraph(writeFile("empty.json",
				R"({ "schema": "ncpatcher.modules/1", "modules": [] })")));

		check(nsmb::emitSceneRegistry(empty).find("#define GLUE_OBJECT_NO_SCENE_IN_OVERLAY\n")
			!= std::string::npos, "no scenes in overlays leaves a define rather than an empty switch");
		check(nsmb::emitExtendedStageObjects(empty).find("#define GLUE_STAGEOBJECT_NONE\n")
			!= std::string::npos, "no stage objects leaves a define rather than a zero-length array");
		check(nsmb::emitObjectRegistry(empty).find("ExtendedObjectsCount = 0;")
			!= std::string::npos, "an empty registry still counts");
		check(nsmb::emitLevelDataGetters(empty).find("\treturn {};\n}\n")
			!= std::string::npos, "an empty getter still returns nothing");
		check(nsmb::emitRuntimeModules(empty).find("#define GLUE_RUNTIME_NONE\n")
			!= std::string::npos, "no runtime modules leaves a define rather than a zero-length array");
	}

	testRuntimeModules();

	fs::remove_all(g_dir);

	if (g_failures == 0)
		std::cout << "glue_test: all checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
