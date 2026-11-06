#include "file_utils.h"
#include "string_utils.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <fstream>
#include <cstring>
#include <string>
#include <spdlog/spdlog.h>

#ifndef PROCDIR
#define PROCDIR "/proc"
#endif

std::string read_line(const std::string& filename)
{
    std::string line;
    std::ifstream file(filename);
    if (file.fail()){
        return line;
    }
    std::getline(file, line);
    return line;
}

std::string get_basename(const std::string&& path)
{
    auto npos = path.find_last_of("/\\");
    if (npos == std::string::npos)
        return path;

    if (npos < path.size() - 1)
        return path.substr(npos + 1);
    return path;
}

#ifdef __linux__
std::vector<std::string> ls(const char* root, const char* prefix, LS_FLAGS flags)
{
    std::vector<std::string> list;
    struct dirent* dp;

    // 在 Android Proot 环境下，直接 opendir 受限目录可能会崩，先检查权限
    if (access(root, R_OK | X_OK) != 0) {
        return list; // 如果没权限，直接返回空列表
    }

    DIR* dirp = opendir(root);
    if (!dirp) {
        //SPDLOG_ERROR("Error opening directory '{}': {}", root, strerror(errno));
        return list;
    }

    while ((dp = readdir(dirp))) {
        if ((prefix && !starts_with(dp->d_name, prefix))
            || !strcmp(dp->d_name, ".")
            || !strcmp(dp->d_name, ".."))
            continue;

        switch (dp->d_type) {
        case DT_LNK: {
            struct stat s;
            std::string path(root);
            if (path.back() != '/')
                path += "/";
            path += dp->d_name;

            if (stat(path.c_str(), &s))
                continue;

            if (((flags & LS_DIRS) && S_ISDIR(s.st_mode))
                || ((flags & LS_FILES) && S_ISREG(s.st_mode))) {
                list.push_back(dp->d_name);
            }
            break;
        }
        case DT_DIR:
            if (flags & LS_DIRS)
                list.push_back(dp->d_name);
            break;
        case DT_REG:
            if (flags & LS_FILES)
                list.push_back(dp->d_name);
            break;
        }
    }

    closedir(dirp);
    return list;
}

bool file_exists(const std::string& path)
{
    struct stat s;
    return !stat(path.c_str(), &s) && !S_ISDIR(s.st_mode);
}

bool dir_exists(const std::string& path)
{
    struct stat s;
    return !stat(path.c_str(), &s) && S_ISDIR(s.st_mode);
}

std::string read_symlink(const char * link)
{
    char result[PATH_MAX] {};
    ssize_t count = readlink(link, result, PATH_MAX);
    return std::string(result, (count > 0) ? count : 0);
}

std::string read_symlink(const std::string&& link)
{
    return read_symlink(link.c_str());
}

std::string get_exe_path()
{
    return read_symlink(PROCDIR "/self/exe");
}

std::string read_proc_comm(pid_t pid)
{
    std::string path = PROCDIR "/";
    path += (pid > 0) ? std::to_string(pid) : std::string("self");
    path += "/comm";
    return read_line(path);
}

std::string get_wine_exe_name(bool keep_ext)
{
    const std::string exe_path = get_exe_path();
    // 上游只认 preloader 名字。Termux + box64 / FEX-Emu 下进程是 box64/fex，
    // wine 10 的 wow64 模式也不再提供 preloader —— 那样 wine-<name>.conf
    // 会彻底失效。这里放宽：exe 路径带 wine，或 comm/cmdline 里能看到
    // 一个 .exe 参数，就当作在 wine 里。
    const bool preloader_name = ends_with(exe_path, "wine-preloader")
                             || ends_with(exe_path, "wine64-preloader");
    const bool looks_like_wine = preloader_name
                              || to_lower(exe_path).find("wine") != std::string::npos;

    std::string line = read_line(PROCDIR "/self/comm"); // 内核只保留 15 字符
    if (ends_with(line, ".exe", true))
    {
        auto dot = keep_ext ? std::string::npos : line.find_last_of('.');
        return line.substr(0, dot);
    }

    // comm 被截断时（长 exe 名）退到 cmdline 里找 .exe 参数。
    if (!looks_like_wine)
        return std::string();

    std::ifstream cmdline(PROCDIR "/self/cmdline");
    // Iterate over arguments (separated by NUL byte).
    while (std::getline(cmdline, line, '\0')) {
        // 只认真正的 .exe 参数，不要拿 wine 自身路径当程序名。
        if (ends_with(line, ".exe", true)) {
            auto slash = line.find_last_of("/\\");
            const std::string base = (slash == std::string::npos) ? line : line.substr(slash + 1);
            auto dot = keep_ext ? std::string::npos : base.find_last_of('.');
            return base.substr(0, dot);
        }
        // 兼容非 .exe 的 Windows 程序（.bat / 原生 ELF 等）的绝对路径参数：
        // 但要排除 wine 自己的二进制，否则会把 wine64 当成程序名。
        if (!preloader_name && !line.empty() && line[0] == '/'
            && to_lower(line).find("wine") == std::string::npos) {
            auto slash = line.find_last_of('/');
            if (slash != std::string::npos && slash + 1 < line.size()) {
                const std::string base = line.substr(slash + 1);
                auto dot = keep_ext ? std::string::npos : base.find_last_of('.');
                return base.substr(0, dot);
            }
        }
    }
    return std::string();
}

std::string get_home_dir()
{
    std::string path;
    const char* p = getenv("HOME");

    if (p)
        path = p;
    return path;
}

std::string get_data_dir()
{
    const char* p = getenv("XDG_DATA_HOME");
    if (p)
        return p;

    std::string path = get_home_dir();
    if (!path.empty())
        path += "/.local/share";
    return path;
}

std::string get_config_dir()
{
    const char* p = getenv("XDG_CONFIG_HOME");
    if (p)
        return p;

    std::string path = get_home_dir();
    if (!path.empty())
        path += "/.config";
    return path;
}

bool lib_loaded(const std::string& lib, pid_t pid) {

   std::string who = pid != -1 ? std::to_string(pid) : "self";
   auto paths = { fs::path("/proc") / who / "map_files",
            fs::path("/proc") / who / "fd" };
    for (auto& path : paths) {
        if (dir_exists(path.string())) {
            std::error_code ec;
            for (auto& p : fs::directory_iterator(path, ec)) {
                if (ec) {
                    SPDLOG_DEBUG("Unable to iterate {}: {}", path.string(), ec.message());
                    break;
                }
                auto file = p.path().string();
                auto sym = read_symlink(file.c_str());
                if (to_lower(sym).find(lib) != std::string::npos) {
                    return true;
                }
            }
        } else {
            SPDLOG_DEBUG("tried to access path that doesn't exist {}", path.string());
        }
    }
    return false;
}

std::string remove_parentheses(const std::string& text) {
    // Remove parentheses and text between them
    std::regex pattern("\\([^)]*\\)");
    return std::regex_replace(text, pattern, "");
}

std::string to_lower(const std::string& str) {
    std::string lowered = str;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lowered;
}

#endif // __linux__
