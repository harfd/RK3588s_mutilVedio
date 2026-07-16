#pragma once

#include <string>
#include <vector>

std::string get_env_or_empty(const char *name);
std::string executable_dir();
std::vector<std::string> default_search_roots();
std::string resolve_existing_path(const std::vector<std::string> &relative_candidates,
                                  const char *env_var = nullptr);
std::string resolve_config_path(const std::string &filename, const char *env_var = nullptr);
