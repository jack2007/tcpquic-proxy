#pragma once

#include <string>
#include <vector>

class TqRuntimeConfigFileStore {
public:
    explicit TqRuntimeConfigFileStore(std::string path);

    bool LoadServerAcl(
        std::vector<std::string>& allowTargets,
        std::vector<std::string>& denyTargets,
        std::string& err) const;

    bool PatchServerAcl(
        const std::vector<std::string>& allowTargets,
        const std::vector<std::string>& denyTargets,
        std::string& err) const;

private:
    std::string Path;
};
