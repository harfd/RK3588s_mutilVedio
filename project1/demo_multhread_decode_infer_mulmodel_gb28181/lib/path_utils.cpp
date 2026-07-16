#include "path_utils.h"

#include <cstdlib>
#include <filesystem>
#include <limits.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace
{
void append_root_if_unique(std::vector<std::string> &roots, const fs::path &root)
{
    if (root.empty())
        return;

    std::error_code ec;
    fs::path normalized = fs::weakly_canonical(root, ec);
    if (ec)
        normalized = root.lexically_normal();

    const std::string value = normalized.string();
    if (value.empty())
        return;

    for (const auto &existing : roots)
    {
        if (existing == value)
            return;
    }
    roots.push_back(value);
}
}

std::string get_env_or_empty(const char *name)
{
    if (name == nullptr)
        return "";

    const char *value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

std::string executable_dir()
{
    char buffer[PATH_MAX] = {0};
    const ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len <= 0)
        return fs::current_path().string();

    buffer[len] = '\0';
    return fs::path(buffer).parent_path().string();
}

std::vector<std::string> default_search_roots()
{
    std::vector<std::string> roots;

    const std::string asset_root = get_env_or_empty("MYDEMO_ASSET_ROOT");
    if (!asset_root.empty())
        append_root_if_unique(roots, asset_root);

    const fs::path cwd = fs::current_path();
    append_root_if_unique(roots, cwd);
    append_root_if_unique(roots, cwd / "..");
    append_root_if_unique(roots, cwd / "../..");

    const fs::path exe = executable_dir();
    append_root_if_unique(roots, exe);
    append_root_if_unique(roots, exe / "..");
    append_root_if_unique(roots, exe / "../..");

    return roots;
}

std::string resolve_existing_path(const std::vector<std::string> &relative_candidates,
                                  const char *env_var)
{
    if (env_var != nullptr)
    {
        const std::string override_path = get_env_or_empty(env_var);
        if (!override_path.empty() && fs::exists(override_path))
            return override_path;
    }

    for (const auto &candidate : relative_candidates)
    {
        if (candidate.empty())
            continue;

        const fs::path raw(candidate);
        if (raw.is_absolute() && fs::exists(raw))
            return raw.string();

        for (const auto &root : default_search_roots())
        {
            fs::path full = fs::path(root) / raw;
            if (fs::exists(full))
                return fs::weakly_canonical(full).string();
        }
    }

    if (!relative_candidates.empty())
    {
        const fs::path fallback = fs::path(default_search_roots().front()) / relative_candidates.front();
        return fallback.lexically_normal().string();
    }
    return "";
}

std::string resolve_config_path(const std::string &filename, const char *env_var)
{
    return resolve_existing_path({filename}, env_var);
}
