#include "runtime_config_file_store.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace {

bool ReadJsonFile(const std::string& path, nlohmann::json& root, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "failed to open runtime config file: " + path;
        return false;
    }

    try {
        root = nlohmann::json::parse(std::string(
            std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()));
    } catch (const nlohmann::json::exception& ex) {
        err = "malformed runtime config JSON: " + std::string(ex.what());
        return false;
    }

    if (!root.is_object()) {
        err = "runtime config root must be a JSON object";
        return false;
    }
    return true;
}

nlohmann::json StringArrayJson(const std::vector<std::string>& values) {
    nlohmann::json array = nlohmann::json::array();
    for (const auto& value : values) {
        array.push_back(value);
    }
    return array;
}

void SplitCommaList(const std::string& value, std::vector<std::string>& out) {
    out.clear();
    size_t start = 0;
    while (start <= value.size()) {
        size_t comma = value.find(',', start);
        if (comma == std::string::npos) {
            comma = value.size();
        }
        const std::string item = value.substr(start, comma - start);
        const size_t begin = item.find_first_not_of(" \t");
        const size_t end = item.find_last_not_of(" \t");
        if (begin != std::string::npos) {
            out.push_back(item.substr(begin, end - begin + 1));
        }
        start = comma + 1;
    }
}

bool ReadStringList(const nlohmann::json& value, std::vector<std::string>& out) {
    if (value.is_string()) {
        SplitCommaList(value.get<std::string>(), out);
        return true;
    }
    if (!value.is_array()) {
        return false;
    }
    std::vector<std::string> next;
    next.reserve(value.size());
    for (const auto& item : value) {
        if (!item.is_string()) {
            return false;
        }
        next.push_back(item.get<std::string>());
    }
    out = std::move(next);
    return true;
}

bool WriteTextFileAtomically(const std::string& path, const std::string& body, std::string& err) {
    std::error_code ec;
    const std::filesystem::path target(path);
    const std::filesystem::path parent = target.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            err = "failed to create runtime config directory: " + ec.message();
            return false;
        }
    }

    const std::filesystem::path tmp = target.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "failed to open runtime config temp file: " + tmp.string();
            return false;
        }
        out << body;
        out.close();
        if (!out) {
            err = "failed to write runtime config temp file: " + tmp.string();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }

    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        err = "failed to publish runtime config file: " + ec.message();
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace

TqRuntimeConfigFileStore::TqRuntimeConfigFileStore(std::string path) : Path(std::move(path)) {}

bool TqRuntimeConfigFileStore::LoadServerAcl(
    std::vector<std::string>& allowTargets,
    std::vector<std::string>& denyTargets,
    std::string& err) const {
    err.clear();
    allowTargets.clear();
    denyTargets.clear();
    if (Path.empty()) {
        err = "runtime config path is empty";
        return false;
    }

    nlohmann::json root;
    if (!ReadJsonFile(Path, root, err)) {
        return false;
    }

    if (!root.contains("server") || root["server"].is_null()) {
        return true;
    }
    if (!root["server"].is_object()) {
        err = "runtime config server must be a JSON object";
        return false;
    }

    const nlohmann::json& server = root["server"];
    if (server.contains("allow_targets") && !ReadStringList(server["allow_targets"], allowTargets)) {
        err = "invalid server.allow_targets";
        return false;
    }
    if (server.contains("deny_targets") && !ReadStringList(server["deny_targets"], denyTargets)) {
        err = "invalid server.deny_targets";
        return false;
    }
    return true;
}

bool TqRuntimeConfigFileStore::PatchServerAcl(
    const std::vector<std::string>& allowTargets,
    const std::vector<std::string>& denyTargets,
    std::string& err) const {
    err.clear();
    if (Path.empty()) {
        err = "runtime config path is empty";
        return false;
    }

    nlohmann::json root;
    if (!ReadJsonFile(Path, root, err)) {
        return false;
    }

    if (!root.contains("server") || root["server"].is_null()) {
        root["server"] = nlohmann::json::object();
    }
    if (!root["server"].is_object()) {
        err = "runtime config server must be a JSON object";
        return false;
    }

    root["server"]["allow_targets"] = StringArrayJson(allowTargets);
    root["server"]["deny_targets"] = StringArrayJson(denyTargets);
    return WriteTextFileAtomically(Path, root.dump(2) + "\n", err);
}
