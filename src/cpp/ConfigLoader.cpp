#include "PCH.h"
#include "ConfigLoader.h"

#include <c4/yml/yml.hpp>
#include <c4/yml/std/std.hpp>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

namespace EngineRelay::ConfigLoader {

    static constexpr const char* kConfigDir = "Data/SKSE/Plugins/EngineRelay";

    // ────────────────────────────────────────────────────────────────────────
    // Helpers shared by both parsers
    // ────────────────────────────────────────────────────────────────────────

    /// Trim leading/trailing ASCII whitespace from a string (in-place).
    static void Trim(std::string& s)
    {
        const auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) { s.clear(); return; }
        const auto end = s.find_last_not_of(" \t\r\n");
        s = s.substr(start, end - start + 1);
    }

    /// Parse a variable type name into VariableType. Returns false if unknown.
    static bool ParseVariableType(const std::string& typeName, VariableType& out)
    {
        if      (typeName == "Bool")  { out = VariableType::Bool;  return true; }
        else if (typeName == "Int8")  { out = VariableType::Int8;  return true; }
        else if (typeName == "Int16") { out = VariableType::Int16; return true; }
        else if (typeName == "Int32") { out = VariableType::Int32; return true; }
        else if (typeName == "Float") { out = VariableType::Float; return true; }
        return false;
    }

    // ────────────────────────────────────────────────────────────────────────
    // YAML loader (primary format)
    // ────────────────────────────────────────────────────────────────────────

    /// Read a scalar string child from a rapidyaml map node.
    static std::string YamlStr(const c4::yml::ConstNodeRef& node,
                                const char* key,
                                const char* fallback = "")
    {
        if (!node.readable() || !node.has_child(c4::to_csubstr(key)))
            return fallback;
        const auto child = node[c4::to_csubstr(key)];
        if (!child.has_val())
            return fallback;
        std::string result;
        c4::from_chars(child.val(), &result);
        return result;
    }

    static std::optional<Registration> LoadYaml(const std::filesystem::path& path)
    {
        std::ifstream file(path);
        if (!file.is_open()) {
            LOG_WARN("ConfigLoader: cannot open '{}'", path.string());
            return std::nullopt;
        }
        std::ostringstream ss;
        ss << file.rdbuf();
        std::string content = ss.str();

        if (content.empty()) {
            LOG_WARN("ConfigLoader: '{}' is empty — skipping.",
                path.filename().string());
            return std::nullopt;
        }

        c4::yml::Tree tree;
        try {
            tree = c4::yml::parse_in_place(c4::to_substr(content));
        } catch (const std::exception& e) {
            LOG_WARN("ConfigLoader: YAML parse error in '{}': {}",
                path.filename().string(), e.what());
            return std::nullopt;
        }

        const auto root = tree.rootref();

        Registration reg;
        reg.modName      = YamlStr(root, "modName");
        reg.behaviorPath = YamlStr(root, "behavior");
        reg.eventName    = YamlStr(root, "event");
        reg.graphName    = YamlStr(root, "graphName");
        reg.projectPath  = YamlStr(root, "projectPath");
        Trim(reg.modName);
        Trim(reg.behaviorPath);
        Trim(reg.eventName);
        Trim(reg.graphName);
        Trim(reg.projectPath);

        if (reg.modName.empty() || reg.behaviorPath.empty() ||
            reg.eventName.empty() || reg.graphName.empty())
        {
            LOG_WARN("ConfigLoader: '{}' missing required fields "
                     "(modName, behavior, event, graphName) — skipping.",
                path.filename().string());
            return std::nullopt;
        }

        // ── animations: YAML sequence ──
        if (root.readable() && root.has_child("animations")) {
            const auto animNode = root["animations"];
            if (animNode.is_seq()) {
                for (const auto& child : animNode) {
                    if (!child.has_val()) continue;
                    std::string anim;
                    c4::from_chars(child.val(), &anim);
                    Trim(anim);
                    if (!anim.empty())
                        reg.animations.push_back(std::move(anim));
                }
            } else {
                LOG_WARN("ConfigLoader: '{}' — 'animations' must be a YAML sequence.",
                    path.filename().string());
            }
        }

        // ── variables: YAML sequence of {name, type, value} maps ──
        if (root.readable() && root.has_child("variables")) {
            const auto varNode = root["variables"];
            if (varNode.is_seq()) {
                for (const auto& child : varNode) {
                    std::string varName  = YamlStr(child, "name");
                    std::string typeName = YamlStr(child, "type");
                    std::string valueStr = YamlStr(child, "value", "0");
                    Trim(varName);
                    Trim(typeName);
                    Trim(valueStr);

                    if (varName.empty() || typeName.empty()) {
                        LOG_WARN("ConfigLoader: '{}' — variable entry missing "
                                 "'name' or 'type' — skipping entry.",
                            path.filename().string());
                        continue;
                    }

                    Variable var;
                    var.name = varName;
                    if (!ParseVariableType(typeName, var.type)) {
                        LOG_WARN("ConfigLoader: '{}' — unknown variable type '{}' "
                                 "for '{}' — valid types: Bool, Int8, Int16, Int32, Float.",
                            path.filename().string(), typeName, varName);
                        continue;
                    }

                    try { var.initialValue = std::stoi(valueStr); }
                    catch (...) {
                        LOG_WARN("ConfigLoader: '{}' — invalid value '{}' for "
                                 "variable '{}' — defaulting to 0.",
                            path.filename().string(), valueStr, varName);
                        var.initialValue = 0;
                    }

                    reg.variables.push_back(std::move(var));
                }
            } else {
                LOG_WARN("ConfigLoader: '{}' — 'variables' must be a YAML sequence.",
                    path.filename().string());
            }
        }

        LOG_INFO("ConfigLoader: loaded '{}' — behavior='{}', event='{}', "
                 "{} animation(s), {} variable(s).",
            reg.modName, reg.behaviorPath, reg.eventName,
            reg.animations.size(), reg.variables.size());
        return reg;
    }

    // ────────────────────────────────────────────────────────────────────────
    // Public entry point
    // ────────────────────────────────────────────────────────────────────────

    std::vector<Registration> LoadConfigs()
    {
        std::vector<Registration> results;
        namespace fs = std::filesystem;

        if (!fs::exists(kConfigDir) || !fs::is_directory(kConfigDir)) {
            LOG_INFO("ConfigLoader: no config directory at '{}'", kConfigDir);
            return results;
        }

        for (const auto& dirEntry : fs::directory_iterator(kConfigDir)) {
            if (!dirEntry.is_regular_file()) continue;
            const auto ext = dirEntry.path().extension().string();

            if (ext != ".yml" && ext != ".yaml") continue;
            std::optional<Registration> reg = LoadYaml(dirEntry.path());

            if (reg.has_value())
                results.push_back(std::move(*reg));
        }

        LOG_INFO("ConfigLoader: {} config(s) loaded from '{}'.",
            results.size(), kConfigDir);
        return results;
    }

}
