# -*- coding: utf-8 -*-
"""
修掉"MangoHud 一启用就刷 wine 报错"：

现象（用户日志）：
  sh: line 1: /data/data/com.termux/files/usr/glibc/bin/wine64: 没有那个文件或目录
  sh: line 1: /data/data/com.termux/files/usr/glibc/bin/wine: cannot execute binary file: 可执行文件格式错误
  sh: line 1: /data/.../wine-10.0-3proton-vanilla-wow64/bin/wine: cannot execute binary file
  sh: line 1: wine64: command not found

根因：overlay.cpp 的 wine 版本探测为了拿 `wine --version`，把一串候选路径拼成命令
丢给 exec()，而 exec() 是 popen → /bin/sh（Termux 下是 bionic sh）。
bionic sh 无法直接 exec glibc / x86_64 的 wine（需要 box64/FEX 或 proot 包装），
于是内核 ENOEXEC/ENOENT 被 sh 打成上面那几行。DXVK 路径不经过这段探测，
所以只在开启 MangoHud 时出现。

修法（三层，逐层收紧）：
  1) 路径名里就带版本（.../wine-10.0-3proton-vanilla-wow64/bin/wine）→ 直接解析成
     "wine-10.0"，**一个子进程都不开**；
  2) 剩余候选先做"本机能不能 exec"体检：ELF 架构 vs 宿主架构、PT_INTERP 解释器
     是否存在、PATH 里是否存在 —— 注定失败的直接跳过，不让 sh 有机会报错；
  3) 真需要跑的才跑，并且 `2>/dev/null` 收掉 stderr。
"""
import io

P = 'src/overlay.cpp'
s = io.open(P, encoding='utf-8').read()

def rep(s, old, new, tag, cnt=1):
    n = s.count(old)
    assert n == cnt, '%-32s expect %d got %d' % (tag, cnt, n)
    print('  %-32s x%d' % (tag, n))
    return s.replace(old, new)

# ---------- 1. 头文件 ----------
s = rep(s, '#include <chrono>\n#include <cstdlib>',
           '#include <chrono>\n#include <cstdlib>\n#include <cctype>\n#include <cstdio>',
           'includes')

# ---------- 2. 插入辅助函数 ----------
old_wine_hdr = '// Get WINE version\n'
helpers = r'''// ============================================================================
// 本机可执行性判断 + 从路径读版本（Termux / glibc 移植补充）
//
// 为什么要这些：下面的 wine 版本探测会把候选路径拼成 "<cand> --version" 交给
// exec()，而 exec() 是 popen → 走 /bin/sh。在 Termux 里那是 bionic sh，它直接
// exec glibc / x86_64 的 wine 必然失败，并把
//   sh: line 1: /.../wine: cannot execute binary file: 可执行文件格式错误
// 打进游戏日志。所以这里先判断"跑不跑得起来"，跑不起来的候选干脆别交出去。
// ============================================================================

/** 读 ELF 头的 e_machine；非 ELF（脚本等）返回 0 */
static unsigned short elf_machine_of(const std::string& path)
{
   FILE* f = fopen(path.c_str(), "rb");
   if (!f)
      return 0;
   unsigned char h[20] = {0};
   const size_t n = fread(h, 1, sizeof(h), f);
   fclose(f);
   if (n < 20 || h[0] != 0x7F || h[1] != 'E' || h[2] != 'L' || h[3] != 'F')
      return 0;
   return static_cast<unsigned short>(h[18] | (h[19] << 8));   // 小端
}

/** 读 ELF 的 PT_INTERP（动态解释器路径）；静态或非 ELF 返回空 */
static std::string elf_interp_of(const std::string& path)
{
   FILE* f = fopen(path.c_str(), "rb");
   if (!f)
      return {};
   // 只处理 ELF64：e_phoff@0x20(8) e_phentsize@0x36(2) e_phnum@0x38(2)
   unsigned char eh[64] = {0};
   if (fread(eh, 1, sizeof(eh), f) < 64 || eh[4] != 2 /*ELFCLASS64*/) {
      fclose(f);
      return {};
   }
   auto rd64 = [&](int off) -> unsigned long long {
      unsigned long long v = 0;
      for (int i = 7; i >= 0; --i)
         v = (v << 8) | eh[off + i];
      return v;
   };
   auto rd16 = [&](int off) -> unsigned { return static_cast<unsigned>(eh[off] | (eh[off + 1] << 8)); };
   const unsigned long long phoff = rd64(0x20);
   const unsigned phentsize = rd16(0x36);
   const unsigned phnum = rd16(0x38);
   std::string out;
   for (unsigned i = 0; i < phnum && out.empty(); ++i) {
      unsigned char ph[64] = {0};
      if (fseek(f, static_cast<long>(phoff + static_cast<unsigned long long>(i) * phentsize), SEEK_SET) != 0)
         break;
      if (fread(ph, 1, sizeof(ph), f) < 56)
         break;
      auto rd32 = [&](int off) -> unsigned { return static_cast<unsigned>(ph[off] | (ph[off+1] << 8) | (ph[off+2] << 16) | (ph[off+3] << 24)); };
      auto rd64p = [&](int off) -> unsigned long long {
         unsigned long long v = 0;
         for (int k = 7; k >= 0; --k)
            v = (v << 8) | ph[off + k];
         return v;
      };
      if (rd32(0) != 3 /*PT_INTERP*/)
         continue;
      const unsigned long long p_off = rd64p(8);
      const unsigned long long p_filesz = rd64p(32);
      if (p_filesz == 0 || p_filesz > 4096)
         continue;
      std::vector<char> buf(static_cast<size_t>(p_filesz), 0);
      if (fseek(f, static_cast<long>(p_off), SEEK_SET) == 0 &&
          fread(buf.data(), 1, static_cast<size_t>(p_filesz), f) == p_filesz)
         out.assign(buf.data());   // NUL 结尾，遇到 '\0' 自然截断
   }
   fclose(f);
   return out;
}

/** 宿主架构的 e_machine；取不到（未知平台）返回 0 = 不做架构过滤 */
static unsigned short host_elf_machine()
{
#if defined(__aarch64__)
   return 183;   // EM_AARCH64
#elif defined(__x86_64__)
   return 62;    // EM_X86_64
#elif defined(__i386__)
   return 3;     // EM_386
#elif defined(__arm__)
   return 40;    // EM_ARM
#else
   return 0;
#endif
}

/**
 * 这个候选在当前环境里跑得起来吗？
 *   非绝对路径 → 必须能在 PATH 里找到（否则 sh 会报 command not found）
 *   非 ELF（脚本）→ 允许
 *   ELF → 架构要一致，且动态解释器要存在（glibc 链接的二进制在 bionic 域里解释器不存在）
 */
static bool host_can_exec(const std::string& cand)
{
   std::string path = cand;
   if (path.find('/') == std::string::npos) {
      const char* pe = getenv("PATH");
      if (!pe)
         return false;
      std::stringstream ss(pe);
      std::string dir;
      bool found = false;
      while (std::getline(ss, dir, ':')) {
         if (dir.empty())
            continue;
         const std::string full = dir + "/" + path;
         if (access(full.c_str(), X_OK) == 0) {
            path = full;
            found = true;
            break;
         }
      }
      if (!found)
         return false;
   }
   if (access(path.c_str(), X_OK) != 0)
      return false;

   const unsigned short host = host_elf_machine();
   const unsigned short m = elf_machine_of(path);
   if (host != 0 && m != 0 && m != host)
      return false;        // 需要 box64 / FEX-Emu 才能跑，直接 exec 必失败

   if (m != 0) {
      const std::string interp = elf_interp_of(path);
      if (!interp.empty() && access(interp.c_str(), X_OK) != 0)
         return false;     // 解释器不在：说明它是另一个域（glibc/proot）的二进制
   }
   return true;
}

/** 从路径名里读版本：.../wine-10.0-3proton-vanilla-wow64/bin/wine → "wine-10.0" */
static std::string wine_version_from_path(const std::string& p)
{
   static const std::string key = "wine-";
   size_t at = p.find(key);
   while (at != std::string::npos) {
      size_t i = at + key.size();
      size_t j = i;
      bool digit = false;
      while (j < p.size() && (std::isdigit(static_cast<unsigned char>(p[j])) || p[j] == '.')) {
         if (std::isdigit(static_cast<unsigned char>(p[j])))
            digit = true;
         ++j;
      }
      if (digit)
         return "wine-" + p.substr(i, j - i);
      at = p.find(key, at + key.size());
   }
   return {};
}

'''
s = rep(s, old_wine_hdr, helpers + old_wine_hdr, 'insert helpers')

# ---------- 3. preloader 分支 ----------
old_pre = '''          else {
             char *dir = dirname((char*)wineProcess.c_str());
             stringstream findVersion;
             if (preloader == "wine-preloader")
                findVersion << "\\"" << dir << "/wine\\" --version";
             else
                findVersion << "\\"" << dir << "/wine64\\" --version";
             const char *wine_env = getenv("WINELOADERNOEXEC");
             if (wine_env)
                unsetenv("WINELOADERNOEXEC");
             wineVersion = exec(findVersion.str());
             trim(wineVersion);
             SPDLOG_DEBUG("WINE version: {}", wineVersion);
             if (wine_env)
                setenv("WINELOADERNOEXEC", wine_env, 1);
          }'''
new_pre = '''          else {
             char *dir = dirname((char*)wineProcess.c_str());
             const std::string cand =
                std::string(dir) + (preloader == "wine-preloader" ? "/wine" : "/wine64");
             // 路径名里带版本就先取它：零子进程，也不会踩到跨域 exec 的报错
             wineVersion = wine_version_from_path(wineProcess);
             if (wineVersion.empty() && host_can_exec(cand)) {
                stringstream findVersion;
                findVersion << "\\"" << cand << "\\" --version 2>/dev/null";
                const char *wine_env = getenv("WINELOADERNOEXEC");
                if (wine_env)
                   unsetenv("WINELOADERNOEXEC");
                wineVersion = exec(findVersion.str());
                trim(wineVersion);
                if (wine_env)
                   setenv("WINELOADERNOEXEC", wine_env, 1);
             }
             SPDLOG_DEBUG("WINE version: {}", wineVersion);
          }'''
s = rep(s, old_pre, new_pre, 'preloader branch')

# ---------- 4. 兜底探测循环 ----------
old_loop = '''            const char *wine_env = getenv("WINELOADERNOEXEC");
            if (wine_env)
               unsetenv("WINELOADERNOEXEC");
            for (const std::string& cand : cands) {
               std::stringstream findVersion;
               findVersion << "\\"" << cand << "\\" --version";
               std::string v = exec(findVersion.str());
               trim(v);
               if (!v.empty() && v.find("not found") == std::string::npos) {
                  wineVersion = v;
                  break;
               }
            }
            if (wine_env)
               setenv("WINELOADERNOEXEC", wine_env, 1);
            SPDLOG_DEBUG("WINE version (fallback): {}", wineVersion);'''
new_loop = '''            const char *wine_env = getenv("WINELOADERNOEXEC");
            bool ran_any = false;
            for (const std::string& cand : cands) {
               // 1) 路径名里就带版本号（.../wine-10.0-3proton-*/bin/wine）→ 直接解析。
               //    Termux + box64/FEX 环境基本都在这一步命中，且不 fork 任何进程。
               const std::string pv = wine_version_from_path(cand);
               if (!pv.empty()) {
                  wineVersion = pv;
                  break;
               }
               // 2) 本机注定跑不起来的候选跳过：否则 bionic sh 会往日志里刷
               //    "cannot execute binary file: 可执行文件格式错误" / "command not found"
               if (!host_can_exec(cand))
                  continue;
               // 3) 真探测：stdout 收版本，stderr 丢弃
               std::stringstream findVersion;
               findVersion << "\\"" << cand << "\\" --version 2>/dev/null";
               if (!ran_any) {
                  ran_any = true;
                  if (wine_env)
                     unsetenv("WINELOADERNOEXEC");
               }
               std::string v = exec(findVersion.str());
               trim(v);
               if (!v.empty() && v.find("not found") == std::string::npos) {
                  wineVersion = v;
                  break;
               }
            }
            if (ran_any && wine_env)
               setenv("WINELOADERNOEXEC", wine_env, 1);
            SPDLOG_DEBUG("WINE version (fallback): {}", wineVersion);'''
s = rep(s, old_loop, new_loop, 'fallback loop')

# <vector> 用于 elf_interp_of 的缓冲区
if '#include <vector>' not in s:
    s = rep(s, '#include <cstdio>', '#include <cstdio>\n#include <vector>', 'vector include')

io.open(P, 'w', encoding='utf-8').write(s)
print('OK: wine 探测补丁就位')