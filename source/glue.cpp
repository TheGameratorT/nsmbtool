#include "glue.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "except.hpp"
#include "log.hpp"
#include "unicode.hpp"

namespace fs = std::filesystem;

namespace nsmb {

namespace {

std::string hex32(std::uint32_t value)
{
	char buffer[16];
	std::snprintf(buffer, sizeof(buffer), "0x%08x", value);
	return buffer;
}

std::string hexId(std::uint16_t value)
{
	char buffer[16];
	std::snprintf(buffer, sizeof(buffer), "0x%X", value);
	return buffer;
}

std::string lower(std::string text)
{
	for (char& c : text)
		c = char(std::tolower(static_cast<unsigned char>(c)));
	return text;
}

// JSON string escaping. Nothing in a module graph has any business containing a
// quote or a control character, but a contract file that silently produces
// invalid JSON when one does is worse than one that escapes it.
std::string jsonString(std::string_view text)
{
	std::string out = "\"";
	for (const char c : text)
	{
		switch (c)
		{
		case '"':  out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (static_cast<unsigned char>(c) < 0x20)
			{
				char buffer[8];
				std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
				out += buffer;
			}
			else
			{
				out += c;
			}
			break;
		}
	}
	out += '"';
	return out;
}

// Writes only when the bytes differ.
//
// `glue` runs from a post-files hook, so it runs on every build. fid.hpp is
// included by most of the project; rewriting it unconditionally would give the
// compiler a newer timestamp than every object file and rebuild everything,
// every time, for no change.
bool writeIfChanged(const fs::path& file, const std::string& content)
{
	std::ifstream existing(file, std::ios::binary);
	if (existing.is_open())
	{
		const std::string previous(
			(std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
		if (previous == content)
			return false;
	}
	existing.close();

	std::error_code error;
	fs::create_directories(file.parent_path(), error);

	std::ofstream stream(file, std::ios::binary | std::ios::trunc);
	if (!stream.is_open())
		throw nsmb::exception("Could not write " + pathToUtf8Generic(file) + ".");

	stream << content;
	if (!stream)
		throw nsmb::exception("Could not write " + pathToUtf8Generic(file) + ".");

	return true;
}

} // namespace

std::uint32_t fnv1a(std::string_view text)
{
	std::uint32_t hash = 2166136261u;
	for (const char c : text)
	{
		hash ^= static_cast<unsigned char>(c);
		hash *= 16777619u;
	}
	return hash;
}

LevelDataType parseLevelDataType(std::string_view text)
{
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
		text.remove_prefix(1);
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
		text.remove_suffix(1);

	if (text.empty())
		throw nsmb::exception("A level-data type cannot be empty.");

	if (text == "flag")
		return LevelDataType{ LevelDataKind::Flag, {}, {} };

	const std::size_t open = text.find('[');
	if (open == std::string_view::npos)
	{
		if (text.find(']') != std::string_view::npos)
		{
			throw nsmb::exception("The level-data type " ANSI_bWHITE + std::string(text)
				+ ANSI_RESET " closes a bracket it never opened.");
		}
		return LevelDataType{ LevelDataKind::Value, std::string(text), {} };
	}

	if (text.back() != ']')
	{
		throw nsmb::exception("The level-data type " ANSI_bWHITE + std::string(text)
			+ ANSI_RESET " does not end in " ANSI_bCYAN "]" ANSI_RESET ".");
	}

	const std::string name(text.substr(0, open));
	const std::string_view extent = text.substr(open + 1, text.size() - open - 2);

	if (name.empty())
	{
		throw nsmb::exception("The level-data type " ANSI_bWHITE + std::string(text)
			+ ANSI_RESET " has no element type.");
	}

	if (extent.empty())
	{
		throw nsmb::exception("The level-data type " ANSI_bWHITE + std::string(text)
			+ ANSI_RESET " has empty brackets.\n"
			"       Give a size (" ANSI_bCYAN "u8[8]" ANSI_RESET ") or "
			ANSI_bCYAN "?" ANSI_RESET " for one the level carries a count for ("
			ANSI_bCYAN "u32[?]" ANSI_RESET ").");
	}

	if (extent == "?")
		return LevelDataType{ LevelDataKind::SizedArray, name, {} };

	int size = 0;
	for (const char c : extent)
	{
		if (!std::isdigit(static_cast<unsigned char>(c)))
		{
			throw nsmb::exception("The level-data type " ANSI_bWHITE + std::string(text)
				+ ANSI_RESET " has a size that is neither a number nor "
				ANSI_bCYAN "?" ANSI_RESET ".");
		}
		size = size * 10 + (c - '0');
		if (size > 0xFFFF)
		{
			throw nsmb::exception("The level-data type " ANSI_bWHITE + std::string(text)
				+ ANSI_RESET " is longer than a level could hold.");
		}
	}

	if (size == 0)
	{
		throw nsmb::exception("The level-data type " ANSI_bWHITE + std::string(text)
			+ ANSI_RESET " has no elements.");
	}

	return LevelDataType{ LevelDataKind::Array, name, size };
}

std::vector<std::string> GlueModel::objectModules() const
{
	std::vector<std::string> out;
	for (const ObjectEntry& object : objects)
	{
		if (std::find(out.begin(), out.end(), object.module) == out.end())
			out.push_back(object.module);
	}
	return out;
}

GlueModel buildModel(const Graph& graph)
{
	GlueModel model;

	for (const ModuleDef& module : graph.modules)
	{
		if (!module.enabled)
			continue;

		for (const ComponentDef& component : module.components)
		{
			if (!component.enabled)
				continue;

			for (const ObjectDef& object : component.objects)
			{
				ObjectEntry entry;
				entry.module = module.id;
				entry.component = component.name;
				entry.name = object.name;
				entry.type = object.type;
				entry.header = object.header;
				entry.overlay = component.overlay;
				entry.stage = object.stage;
				model.objects.push_back(std::move(entry));
			}
		}
	}

	std::sort(model.objects.begin(), model.objects.end(),
		[](const ObjectEntry& left, const ObjectEntry& right) {
			return std::tie(left.module, left.component, left.name)
			     < std::tie(right.module, right.component, right.name);
		});

	// The identifier is ObjectID::<module>::<name>, so two objects of one name
	// in one module would generate the same constant twice -- with different
	// values, since each still gets its own id.
	std::map<std::string, std::string> seen;
	std::uint16_t next = OBJECT_ID_BASE;
	for (ObjectEntry& object : model.objects)
	{
		const std::string identity = object.module + "::" + object.name;
		const auto found = seen.find(identity);
		if (found != seen.end())
		{
			throw nsmb::exception("Two components declare the object " ANSI_bWHITE
				+ identity + ANSI_RESET ": " ANSI_bWHITE + found->second + ANSI_RESET
				" and " ANSI_bWHITE + object.component + ANSI_RESET ".");
		}
		seen.emplace(identity, object.component);

		object.id = next++;
	}

	// One entry per stage: variant, in object order and then declaration order,
	// so the table reads down the same way the module files do.
	std::map<std::uint32_t, std::string> hashes;
	for (const ObjectEntry& object : model.objects)
	{
		std::set<std::string> variants;
		for (const std::string& variant : object.stage)
		{
			if (!variants.insert(variant).second)
			{
				throw nsmb::exception("The object " ANSI_bWHITE + object.module + "::"
					+ object.name + ANSI_RESET " lists the stage variant " ANSI_bWHITE
					+ variant + ANSI_RESET " twice.");
			}

			StageObjectEntry entry;
			entry.key = lower(object.module) + "." + object.name + "." + variant;
			entry.hash = fnv1a(entry.key);
			entry.objectId = object.id;
			entry.info = object.name + "::ObjectInfo_" + variant;
			entry.header = object.header;
			entry.module = object.module;
			entry.component = object.component;
			entry.object = object.name;
			entry.variant = variant;

			const auto clash = hashes.find(entry.hash);
			if (clash != hashes.end())
			{
				throw nsmb::exception("The stage objects " ANSI_bWHITE + clash->second
					+ ANSI_RESET " and " ANSI_bWHITE + entry.key + ANSI_RESET
					" hash to the same value.\n"
					"       A level stores the hash, so one of them has to be renamed.");
			}
			hashes.emplace(entry.hash, entry.key);

			model.stageObjects.push_back(std::move(entry));
		}
	}

	for (const ModuleDef& module : graph.modules)
	{
		if (!module.enabled)
			continue;

		for (const LevelDataDef& declared : module.levelData)
		{
			LevelDataEntry entry;
			entry.key = lower(module.id) + "." + declared.key;
			entry.hash = fnv1a(entry.key);
			entry.module = module.id;

			try
			{
				entry.type = parseLevelDataType(declared.type);
			}
			catch (const nsmb::exception& e)
			{
				throw nsmb::exception("Module " ANSI_bWHITE + module.id + ANSI_RESET
					"'s level-data key " ANSI_bWHITE + declared.key + ANSI_RESET ": "
					+ e.what());
			}

			model.levelData.push_back(std::move(entry));
		}
	}

	std::sort(model.levelData.begin(), model.levelData.end(),
		[](const LevelDataEntry& left, const LevelDataEntry& right) {
			return std::tie(left.module, left.key) < std::tie(right.module, right.key);
		});

	std::map<std::uint32_t, std::string> dataHashes;
	for (const LevelDataEntry& entry : model.levelData)
	{
		const auto clash = dataHashes.find(entry.hash);
		if (clash != dataHashes.end())
		{
			// Same spelling means two modules share an id, which is a project
			// mistake rather than a hash accident; either way one has to change.
			throw nsmb::exception("The level-data keys " ANSI_bWHITE + clash->second
				+ ANSI_RESET " and " ANSI_bWHITE + entry.key + ANSI_RESET
				+ " hash to the same value.\n"
				  "       A level stores the hash, so one of them has to be renamed.");
		}
		dataHashes.emplace(entry.hash, entry.key);
	}

	return model;
}

std::string emitObjectIds(const GlueModel& model, const std::string& moduleId)
{
	std::ostringstream out;
	out << "#pragma once\n"
	       "\n"
	       "#include <cstdint>\n"
	       "\n"
	       "// Generated object IDs for module ID: " << moduleId << "\n"
	       "// Do not edit this file directly\n"
	       "\n"
	       "namespace ObjectID::" << moduleId << " {\n"
	       "\n";

	for (const ObjectEntry& object : model.objects)
	{
		if (object.module == moduleId)
			out << "    static constexpr std::uint16_t " << object.name << " = " << hexId(object.id) << ";\n";
	}

	out << "\n}\n";
	return out.str();
}

std::string emitObjectRegistry(const GlueModel& model)
{
	std::ostringstream out;
	out << "#pragma once\n"
	       "\n"
	       "#include <cstdint>\n"
	       "\n"
	       "// Generated object registry\n"
	       "// Do not edit this file directly\n"
	       "\n";

	for (const std::string& moduleId : model.objectModules())
		out << "#include \"objectids/" << lower(moduleId) << ".hpp\"\n";

	out << "\n"
	       "namespace Game {\n"
	       "    static constexpr std::uint16_t ExtendedObjectsStart = " << hexId(OBJECT_ID_BASE) << ";\n"
	       "    static constexpr std::uint16_t ExtendedObjectsCount = " << model.objects.size() << ";\n"
	       "}\n";
	return out.str();
}

std::string emitExtendedProfiles(const GlueModel& model)
{
	std::ostringstream out;
	out << "#pragma once\n"
	       "\n"
	       "// Generated profile table for extended objects\n"
	       "// Do not edit this file directly\n"
	       "\n";

	std::vector<std::string> included;
	for (const ObjectEntry& object : model.objects)
	{
		if (std::find(included.begin(), included.end(), object.header) != included.end())
			continue;
		included.push_back(object.header);
		out << "#include \"" << object.header << "\"\n";
	}

	out << "\n"
	       "namespace Glue::Object {\n"
	       "\n"
	       "// Generated extended profile table\n"
	       "const ObjectProfile* mainExtPT[] = {\n";

	for (const ObjectEntry& object : model.objects)
	{
		out << "    &" << object.name << "::Profile,  // " << object.name
		    << " (" << hexId(object.id) << ")\n";
	}

	out << "};\n"
	       "\n"
	       "constinit const ObjectProfile* const* currentExtPT = mainExtPT;\n"
	       "\n"
	       "}\n";
	return out.str();
}

std::string emitSceneRegistry(const GlueModel& model)
{
	// A scene only needs an entry if its code lives in an overlay: the registry
	// exists to tell the game which overlay to make resident before the scene
	// can run, and a scene patched into main is already there.
	std::vector<const ObjectEntry*> scenes;
	for (const ObjectEntry& object : model.objects)
	{
		if (object.type == "scene" && object.overlay.has_value())
			scenes.push_back(&object);
	}

	std::ostringstream out;
	out << "#pragma once\n"
	       "\n"
	       "#include <nsmb_nitro.hpp>\n"
	       "\n"
	       "// Generated scene overlay registry\n"
	       "// Do not edit this file directly\n"
	       "\n";

	if (scenes.empty())
	{
		out << "#define GLUE_OBJECT_NO_SCENE_IN_OVERLAY\n";
		return out.str();
	}

	std::vector<std::string> modules;
	for (const ObjectEntry* scene : scenes)
	{
		if (std::find(modules.begin(), modules.end(), scene->module) == modules.end())
			modules.push_back(scene->module);
	}
	std::sort(modules.begin(), modules.end());

	for (const std::string& moduleId : modules)
		out << "#include \"objectids/" << lower(moduleId) << ".hpp\"\n";

	out << "\n"
	       "NTR_INLINE u32 getSceneOverlayID(u16 sceneID, u32 defaultOverlayID) {\n"
	       "\n"
	       "    switch (sceneID) {\n";

	for (const ObjectEntry* scene : scenes)
	{
		out << "        case ObjectID::" << scene->module << "::" << scene->name
		    << ": return " << *scene->overlay << ";\n";
	}

	out << "        default: return defaultOverlayID;\n"
	       "    }\n"
	       "\n"
	       "}\n";
	return out.str();
}

std::string emitLevelDataGetters(const GlueModel& model)
{
	// Flags carry no payload, so there is nothing to return and no getter to
	// specialise. They exist only in the contract, where an editor uses them to
	// offer a checkbox, and at runtime through hasFlag, which needs no table.
	std::vector<const LevelDataEntry*> typed;
	for (const LevelDataEntry& entry : model.levelData)
	{
		if (entry.type.kind != LevelDataKind::Flag)
			typed.push_back(&entry);
	}

	const auto cppType = [](const LevelDataEntry& entry) -> std::string {
		switch (entry.type.kind)
		{
		case LevelDataKind::Value:      return entry.type.name;
		case LevelDataKind::Array:      return entry.type.name + "*";
		case LevelDataKind::SizedArray: return "SizedArray<" + entry.type.name + ">";
		default:                        return {};
		}
	};

	const auto getter = [](const LevelDataEntry& entry) -> std::string {
		switch (entry.type.kind)
		{
		case LevelDataKind::Value:      return "getValue<" + entry.type.name + ">";
		case LevelDataKind::Array:      return "getArray<" + entry.type.name + ">";
		case LevelDataKind::SizedArray: return "getSizedArray<" + entry.type.name + ">";
		default:                        return {};
		}
	};

	std::ostringstream out;
	out << "#pragma once\n"
	       "\n"
	       "// Generated glue level block attributes\n"
	       "// Do not edit this file directly\n"
	       "\n"
	       "// Template specializations for level data types\n";

	for (const LevelDataEntry* entry : typed)
	{
		out << "template<> struct GetterGetDataType<\"" << entry->key << "\"> { using type = "
		    << cppType(*entry) << "; };\n";
	}

	out << "\n"
	       "// Overload for get that deduces type from hash\n"
	       "template<CTString HashStr>\n"
	       "NTR_INLINE std::optional<GetterDataType<HashStr>> get() {\n";

	for (const LevelDataEntry* entry : typed)
	{
		out << "\t// " << entry->key << "\n"
		       "\tif constexpr (Hash(HashStr.data).value == " << hex32(entry->hash) << ") {\n"
		       "\t\treturn " << getter(*entry) << "(" << hex32(entry->hash) << ");\n"
		       "\t}\n";
	}

	out << "\treturn {};\n"
	       "}\n";
	return out.str();
}

std::string emitExtendedStageObjects(const GlueModel& model)
{
	std::ostringstream out;
	out << "#pragma once\n"
	       "\n"
	       "// Generated extended stage object table\n"
	       "// Do not edit this file directly\n"
	       "\n";

	if (model.stageObjects.empty())
	{
		out << "// No module declares a stage: variant, so there is no table: a zero-length\n"
		       "// array is not valid C++, and an empty one would be dead weight in the ROM.\n"
		       "#define GLUE_STAGEOBJECT_NONE\n";
		return out.str();
	}

	std::vector<std::string> included;
	for (const StageObjectEntry& entry : model.stageObjects)
	{
		if (std::find(included.begin(), included.end(), entry.header) != included.end())
			continue;
		included.push_back(entry.header);
		out << "#include \"" << entry.header << "\"\n";
	}

	out << "\n"
	       "namespace Glue::StageObject {\n"
	       "\n"
	       "// One entry per stage: variant. The hash of module.Object.Variant is what a\n"
	       "// level stores; the 326 + n stage object ids are the editor's, per level, and\n"
	       "// appear nowhere in this table.\n"
	       "//\n"
	       "// The id that does appear is the other space entirely: the 0x182 + n object id\n"
	       "// naming the class this stage object spawns. Two stage: variants of one object\n"
	       "// share it and differ only in their ObjectInfo.\n"
	       "ModuleStageObjectInfo moduleStageObjectInfos[] = {\n";

	for (const StageObjectEntry& entry : model.stageObjects)
	{
		out << "    MODULE_OBJ(" << hex32(entry.hash) << ", " << entry.objectId
		    << ", &" << entry.info << ", \"" << entry.key << "\"),\n";
	}

	out << "};\n"
	       "\n"
	       "constexpr u32 moduleStageObjectCount = " << model.stageObjects.size() << ";\n"
	       "\n"
	       "}\n";
	return out.str();
}

std::string emitFileIds(const Manifest& manifest)
{
	// z_new/ is where NCPatcher puts files a module added, because the ROM's
	// file table only grows at the end and a directory that sorts last is the
	// only place a new name can go without renumbering. The code says
	// "coop/x.nwav"fid and should not have to know that.
	std::map<std::string, const ManifestFile*> byName;
	for (const ManifestFile& file : manifest.files)
	{
		std::string name = file.path;
		if (name.starts_with("z_new/"))
			name = name.substr(6);

		const auto clash = byName.find(name);
		if (clash != byName.end())
		{
			throw nsmb::exception("The ROM files " ANSI_bWHITE + clash->second->path
				+ ANSI_RESET " and " ANSI_bWHITE + file.path + ANSI_RESET
				" are both named " ANSI_bWHITE + name + ANSI_RESET " once z_new/ is dropped.\n"
				"       One of them has to be renamed: a file id literal can only mean one file.");
		}

		if (file.id < FILE_ID_OFFSET)
		{
			throw nsmb::exception("The ROM file " ANSI_bWHITE + file.path + ANSI_RESET
				" has id " + std::to_string(file.id) + ", which is below the "
				+ std::to_string(FILE_ID_OFFSET) + " overlays the game offsets by.");
		}

		byName.emplace(std::move(name), &file);
	}

	std::vector<const ManifestFile*> sorted;
	sorted.reserve(byName.size());
	for (const auto& [name, file] : byName)
		sorted.push_back(file);
	std::sort(sorted.begin(), sorted.end(),
		[](const ManifestFile* left, const ManifestFile* right) { return left->id < right->id; });

	std::ostringstream out;
	out << "#pragma once\n"
	       "\n"
	       "#include <cstdint>\n"
	       "#include <string_view>\n"
	       "\n"
	       "consteval std::uint16_t operator\"\"fid(const char* str, std::size_t len) {\n"
	       "    std::string_view path{str, len};\n"
	       "\n";

	// No fallthrough return, deliberately: a name that is not in the ROM falls
	// off the end of a consteval function, which is a compile error naming the
	// call site. A sentinel would be a wrong file id at runtime instead.
	for (const ManifestFile* file : sorted)
	{
		std::string name = file->path;
		if (name.starts_with("z_new/"))
			name = name.substr(6);
		out << "    if (path == \"" << name << "\") return " << (file->id - FILE_ID_OFFSET) << ";\n";
	}

	out << "}\n";
	return out.str();
}

std::string emitLevelDataJson(const GlueModel& model)
{
	const auto kindName = [](LevelDataKind kind) -> const char* {
		switch (kind)
		{
		case LevelDataKind::Flag:       return "flag";
		case LevelDataKind::Value:      return "value";
		case LevelDataKind::Array:      return "array";
		case LevelDataKind::SizedArray: return "sizedArray";
		}
		return "flag";
	};

	std::ostringstream out;
	out << "{\n"
	       "  \"schema\": \"nsmbtool.leveldata/1\",\n"
	       "  \"levelData\": [\n";

	for (std::size_t i = 0; i < model.levelData.size(); i++)
	{
		const LevelDataEntry& entry = model.levelData[i];
		out << "    {\n"
		       "      \"key\": " << jsonString(entry.key) << ",\n"
		       "      \"module\": " << jsonString(entry.module) << ",\n"
		       "      \"dataType\": " << jsonString(kindName(entry.type.kind)) << ",\n"
		       "      \"hash\": " << jsonString(hex32(entry.hash));
		if (!entry.type.name.empty())
			out << ",\n      \"typeName\": " << jsonString(entry.type.name);
		if (entry.type.size.has_value())
			out << ",\n      \"arraySize\": " << *entry.type.size;
		out << "\n    }" << (i + 1 == model.levelData.size() ? "" : ",") << "\n";
	}

	out << "  ]\n"
	       "}\n";
	return out.str();
}

std::string emitStageObjectsJson(const GlueModel& model)
{
	std::ostringstream out;
	out << "{\n"
	       "  \"schema\": \"nsmbtool.stageobjects/1\",\n"
	       "  \"stageObjectIdBase\": " << STAGE_OBJECT_ID_BASE << ",\n"
	       "  \"stageObjects\": [\n";

	for (std::size_t i = 0; i < model.stageObjects.size(); i++)
	{
		const StageObjectEntry& entry = model.stageObjects[i];
		out << "    {\n"
		       "      \"name\": " << jsonString(entry.key) << ",\n"
		       "      \"hash\": " << jsonString(hex32(entry.hash)) << ",\n"
		       "      \"module\": " << jsonString(entry.module) << ",\n"
		       "      \"component\": " << jsonString(entry.component) << ",\n"
		       "      \"object\": " << jsonString(entry.object) << ",\n"
		       "      \"variant\": " << jsonString(entry.variant) << ",\n"
		       "      \"objectId\": " << entry.objectId << ",\n"
		       "      \"info\": " << jsonString(entry.info) << ",\n"
		       "      \"header\": " << jsonString(entry.header) << "\n"
		       "    }" << (i + 1 == model.stageObjects.size() ? "" : ",") << "\n";
	}

	out << "  ]\n"
	       "}\n";
	return out.str();
}

int glueGenerate(const GlueOptions& options)
{
	const fs::path start = options.directory.empty()
		? fs::current_path() : utf8ToPath(options.directory);

	if (!fs::is_directory(start))
		throw nsmb::exception(pathToUtf8Generic(start) + " is not a directory.");

	const auto resolve = [&](const std::string& given) {
		const fs::path path = utf8ToPath(given);
		return path.is_absolute() ? path : start / path;
	};

	const fs::path graphFile = resolve(options.graph);
	const fs::path manifestFile = resolve(options.manifest);
	const fs::path outDir = resolve(options.out);

	if (!fs::exists(graphFile))
	{
		throw nsmb::exception("No module dump at " ANSI_bWHITE + pathToUtf8Generic(graphFile)
			+ ANSI_RESET ".\n"
			"       NCPatcher writes it where " ANSI_bCYAN "modules.dump" ANSI_RESET
			" points; pass " ANSI_bCYAN "--graph" ANSI_RESET " if it is elsewhere.");
	}

	if (!fs::exists(manifestFile))
	{
		throw nsmb::exception("No file manifest at " ANSI_bWHITE + pathToUtf8Generic(manifestFile)
			+ ANSI_RESET ".\n"
			"       NCPatcher writes it where " ANSI_bCYAN "files.dump" ANSI_RESET
			" points, after insertion; pass " ANSI_bCYAN "--manifest" ANSI_RESET
			" if it is elsewhere.");
	}

	const Graph graph = readGraph(graphFile);
	const Manifest manifest = readManifest(manifestFile);
	const GlueModel model = buildModel(graph);

	// include/ is what the project puts on the compiler's include path, and
	// generated/glue/ sits under it because glue/level.hpp includes its getters
	// by that relative path.
	const fs::path include = outDir / "include";
	const fs::path generated = include / "generated" / "glue";

	std::size_t changed = 0;

	for (const std::string& moduleId : model.objectModules())
	{
		changed += writeIfChanged(
			include / "objectids" / (lower(moduleId) + ".hpp"), emitObjectIds(model, moduleId)) ? 1 : 0;
	}

	changed += writeIfChanged(include / "object_registry.hpp", emitObjectRegistry(model)) ? 1 : 0;
	changed += writeIfChanged(include / "scene_overlay_registry.hpp", emitSceneRegistry(model)) ? 1 : 0;
	changed += writeIfChanged(include / "fid.hpp", emitFileIds(manifest)) ? 1 : 0;
	changed += writeIfChanged(generated / "extended_profiles.hpp", emitExtendedProfiles(model)) ? 1 : 0;
	changed += writeIfChanged(generated / "level_data_getters.hpp", emitLevelDataGetters(model)) ? 1 : 0;
	changed += writeIfChanged(generated / "extended_stageobjects.hpp", emitExtendedStageObjects(model)) ? 1 : 0;
	changed += writeIfChanged(outDir / "level_data.json", emitLevelDataJson(model)) ? 1 : 0;
	changed += writeIfChanged(outDir / "stageobjects.json", emitStageObjectsJson(model)) ? 1 : 0;

	std::ostringstream summary;
	summary << model.objects.size() << " object" << (model.objects.size() == 1 ? "" : "s") << ", "
	        << model.stageObjects.size() << " stage object"
	        << (model.stageObjects.size() == 1 ? "" : "s") << ", "
	        << model.levelData.size() << " level-data key"
	        << (model.levelData.size() == 1 ? "" : "s") << ", "
	        << manifest.files.size() << " file id"
	        << (manifest.files.size() == 1 ? "" : "s");
	if (!manifest.variant.empty())
		summary << " for " ANSI_bWHITE << manifest.variant << ANSI_RESET;
	summary << ".";

	log::info(summary.str());

	if (changed == 0)
		log::info("Everything in " ANSI_bWHITE + pathToUtf8Generic(outDir) + ANSI_RESET " was already up to date.");
	else
		log::info("Wrote " + std::to_string(changed) + " file" + (changed == 1 ? "" : "s")
			+ " to " ANSI_bWHITE + pathToUtf8Generic(outDir) + ANSI_RESET ".");

	return 0;
}

} // namespace nsmb
