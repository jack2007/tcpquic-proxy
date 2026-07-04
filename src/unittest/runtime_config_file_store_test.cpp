#include "runtime_config_file_store.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::filesystem::path TempPath(const std::string& name) {
    static unsigned counter = 0;
    return std::filesystem::temp_directory_path() /
           ("tcpquic-runtime-config-file-store-" + name + "-" + std::to_string(counter++) + ".json");
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

nlohmann::json ReadJson(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    nlohmann::json root;
    in >> root;
    return root;
}

void Cleanup(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove_all(path.string() + ".tmp", ec);
}

int TestPatchServerAclPreservesOtherFields() {
    const auto path = TempPath("preserve");
    Cleanup(path);
    WriteText(path, R"({
      "tls": {"cert": "certs/server.crt", "key": "certs/server.key"},
      "server": {
        "proto_listen": "0.0.0.0:4433",
        "allow_targets": ["10.0.0.0/8"],
        "deny_targets": []
      },
      "proto": {"profile": "max-throughput"}
    })");

    TqRuntimeConfigFileStore store(path.string());
    std::string err;
    if (!store.PatchServerAcl({"127.0.0.1/32"}, {"169.254.0.0/16"}, err)) return 1;

    const nlohmann::json root = ReadJson(path);
    if (root["server"]["allow_targets"] != nlohmann::json::array({"127.0.0.1/32"})) return 2;
    if (root["server"]["deny_targets"] != nlohmann::json::array({"169.254.0.0/16"})) return 3;
    if (root["server"]["proto_listen"] != "0.0.0.0:4433") return 4;
    if (root["tls"]["cert"] != "certs/server.crt") return 5;
    if (root["proto"]["profile"] != "max-throughput") return 6;
    Cleanup(path);
    return 0;
}

int TestLoadServerAclReadsCurrentFields() {
    const auto path = TempPath("load-acl");
    Cleanup(path);
    WriteText(path, R"({
      "tls": {"cert": "certs/server.crt"},
      "server": {
        "proto_listen": "0.0.0.0:4433",
        "allow_targets": ["10.0.0.0/8", "127.0.0.1/32"],
        "deny_targets": ["169.254.0.0/16"]
      }
    })");

    TqRuntimeConfigFileStore store(path.string());
    std::vector<std::string> allowTargets;
    std::vector<std::string> denyTargets;
    std::string err;
    if (!store.LoadServerAcl(allowTargets, denyTargets, err)) return 7;
    if (allowTargets != std::vector<std::string>({"10.0.0.0/8", "127.0.0.1/32"})) return 8;
    if (denyTargets != std::vector<std::string>({"169.254.0.0/16"})) return 9;
    Cleanup(path);
    return 0;
}

int TestLoadServerAclTreatsMissingFieldsAsEmpty() {
    const auto path = TempPath("load-missing");
    Cleanup(path);
    WriteText(path, R"({"server":{"proto_listen":"0.0.0.0:4433"}})");

    TqRuntimeConfigFileStore store(path.string());
    std::vector<std::string> allowTargets{"stale"};
    std::vector<std::string> denyTargets{"stale"};
    std::string err;
    if (!store.LoadServerAcl(allowTargets, denyTargets, err)) return 14;
    if (!allowTargets.empty()) return 15;
    if (!denyTargets.empty()) return 16;
    Cleanup(path);
    return 0;
}

int TestLoadServerAclAcceptsCommaSeparatedStrings() {
    const auto path = TempPath("load-string-list");
    Cleanup(path);
    WriteText(path, R"({
      "server": {
        "allow_targets": "10.0.0.0/8, 127.0.0.1/32",
        "deny_targets": "169.254.0.0/16,, 192.168.0.0/16 "
      }
    })");

    TqRuntimeConfigFileStore store(path.string());
    std::vector<std::string> allowTargets;
    std::vector<std::string> denyTargets;
    std::string err;
    if (!store.LoadServerAcl(allowTargets, denyTargets, err)) return 17;
    if (allowTargets != std::vector<std::string>({"10.0.0.0/8", "127.0.0.1/32"})) return 18;
    if (denyTargets != std::vector<std::string>({"169.254.0.0/16", "192.168.0.0/16"})) return 19;
    Cleanup(path);
    return 0;
}

int TestPatchServerAclCreatesServerObject() {
    const auto path = TempPath("create-server");
    Cleanup(path);
    WriteText(path, R"({"tls":{"cert":"server.crt"}})");

    TqRuntimeConfigFileStore store(path.string());
    std::string err;
    if (!store.PatchServerAcl({"0.0.0.0/0"}, {}, err)) return 10;

    const nlohmann::json root = ReadJson(path);
    if (!root.contains("server") || !root["server"].is_object()) return 11;
    if (root["server"]["allow_targets"] != nlohmann::json::array({"0.0.0.0/0"})) return 12;
    if (root["server"]["deny_targets"] != nlohmann::json::array()) return 13;
    Cleanup(path);
    return 0;
}

int TestRejectsMalformedJsonWithoutChangingFile() {
    const auto path = TempPath("malformed");
    Cleanup(path);
    const std::string original = R"({"server":)";
    WriteText(path, original);

    TqRuntimeConfigFileStore store(path.string());
    std::string err;
    if (store.PatchServerAcl({"127.0.0.1/32"}, {}, err)) return 20;
    if (ReadText(path) != original) return 21;
    if (err.find("parse") == std::string::npos && err.find("JSON") == std::string::npos &&
        err.find("malformed") == std::string::npos) {
        return 22;
    }
    Cleanup(path);
    return 0;
}

int TestRejectsNonObjectRootWithoutChangingFile() {
    const auto path = TempPath("array-root");
    Cleanup(path);
    const std::string original = R"(["not-object"])";
    WriteText(path, original);

    TqRuntimeConfigFileStore store(path.string());
    std::string err;
    if (store.PatchServerAcl({"127.0.0.1/32"}, {}, err)) return 30;
    if (ReadText(path) != original) return 31;
    if (err.find("object") == std::string::npos) return 32;
    Cleanup(path);
    return 0;
}

int TestTempWriteFailureLeavesOriginalFile() {
    const auto path = TempPath("temp-failure");
    Cleanup(path);
    const std::string original = R"({"server":{"allow_targets":["10.0.0.0/8"],"deny_targets":[]}})";
    WriteText(path, original);
    std::filesystem::create_directory(path.string() + ".tmp");

    TqRuntimeConfigFileStore store(path.string());
    std::string err;
    if (store.PatchServerAcl({"127.0.0.1/32"}, {}, err)) return 40;
    if (ReadText(path) != original) return 41;
    if (err.empty()) return 42;
    Cleanup(path);
    return 0;
}

} // namespace

int main() {
    if (int code = TestPatchServerAclPreservesOtherFields()) return code;
    if (int code = TestLoadServerAclReadsCurrentFields()) return code;
    if (int code = TestLoadServerAclTreatsMissingFieldsAsEmpty()) return code;
    if (int code = TestLoadServerAclAcceptsCommaSeparatedStrings()) return code;
    if (int code = TestPatchServerAclCreatesServerObject()) return code;
    if (int code = TestRejectsMalformedJsonWithoutChangingFile()) return code;
    if (int code = TestRejectsNonObjectRootWithoutChangingFile()) return code;
    if (int code = TestTempWriteFailureLeavesOriginalFile()) return code;
    return 0;
}
