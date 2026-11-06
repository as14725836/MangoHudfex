#include "file_utils.h"
#include <filesystem.h>
#include <string>
#include <vector>
#include <utility>
#include <fstream>
#include <spdlog/spdlog.h>
#include "hud_elements.h"
#include "overlay.h"

namespace fs = ghc::filesystem;
class WineSync {
    private:
        // 顺序很重要：命中哪个就先报哪个。旧代码用 unordered_map 迭代，
        // 顺序不确定，同时加载了 esync + fsync 时报出来的可能是随机的。
        inline static const std::vector<std::pair<std::string, std::string>> methods {
            {"ntsync", "NTsync"},
            {"fsync", "Fsync"},
            {"esync", "Esync"},
            {"winesync", "Wserver"},
        };

        // 必须显式初始化：旧代码是裸的 `pid_t pid;`，第一次 set_pid() 时
        // 拿它跟 _pid 比较/传给 lib_loaded()，读到的是栈上垃圾值。
        pid_t pid = -1;
        std::string method = "NONE";
        // 默认 false：rpcss（wineserver）等非游戏进程也会加载本层，
        // 探测不到就直接不显示，不要先假定“在 wine 里”。
        bool inside_wine = false;
        bool probed = false;

    public:
        void determine_sync_variant() {
#ifdef __linux__
            // 判断是否在 wine 里。上游只认 wine-preloader / wine64-preloader，
            // 但这在 Termux + box64 / FEX-Emu 下永远不成立（进程名是 box64/fex），
            // 而且 wine 10 的 wow64 模式不再有 preloader。这里改用更宽的判定：
            // 只要 exe 路径或进程名里带 wine 就算。
            std::string wineProcess = get_exe_path();
            if (wineProcess.empty())
                wineProcess = read_proc_comm(pid);

            const std::string lower = to_lower(wineProcess);
            inside_wine = lower.find("wine") != std::string::npos;

            // Termux 下 /proc/self/exe 可能是 box64 / fex，不带 "wine"；
            // 再看一次 cmdline（cmdline 里通常会有 wine 的绝对路径）。
            if (!inside_wine) {
                std::ifstream cmdline("/proc/"
                                      + std::string(pid > 0 ? std::to_string(pid) : "self")
                                      + "/cmdline", std::ios::binary);
                std::string tok;
                char ch;
                while (cmdline.get(ch)) {
                    if (ch == '\0') {
                        if (to_lower(tok).find("wine") != std::string::npos) {
                            inside_wine = true;
                            break;
                        }
                        tok.clear();
                    } else {
                        tok += ch;
                    }
                }
                if (!inside_wine && to_lower(tok).find("wine") != std::string::npos)
                    inside_wine = true;
            }
            if (!inside_wine)
                return;

            for (auto& [key, val] : methods) {
                if (lib_loaded(key, pid)) {
                    method = key;
                    break;
                }
            }

            SPDLOG_DEBUG("Wine sync method: {}", method);
#endif
        }

        bool valid() {
            return inside_wine;
        }

        // return sync method as display name
        const char* get_method() {
            // 不用 at()：一旦 method 不是预期键（例如将来新增后拼错），
            // at() 抛出的 std::out_of_range 会直接从 HUD 绘制路径逃出去。
            for (auto& [key, val] : methods) {
                if (key == method)
                    return val.c_str();
            }
            return "NONE";
        }

        void set_pid(pid_t _pid) {
            if (probed && _pid == pid)
                return;

            pid = _pid;
            probed = true;
            determine_sync_variant();
        }
};

extern std::unique_ptr<WineSync> winesync_ptr;
