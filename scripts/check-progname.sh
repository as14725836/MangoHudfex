#!/usr/bin/env bash
# check-progname.sh —— 诊断 HUD“程序”行取名字
# 用法（Termux 里、游戏正在跑的时候）：
#   bash scripts/check-progname.sh
set -u
echo '== wine/程序相关进程 =='
ps -ef 2>/dev/null | grep -iE 'wine|exe' | grep -v grep | head -40
echo
echo '== 各进程 comm / cmdline ==' 
for p in $(ps -eo pid 2>/dev/null | tail -n +2); do
    c=$(cat /proc/$p/comm 2>/dev/null || true)
    case "$c" in
        *wine*|*Wine*|*exe*|*EXE*|*Exe*|*fex*|*FEX*|*box64*)
            echo "[$p] comm=$c"
            strings /proc/$p/cmdline 2>/dev/null | head -4
            echo
            ;;
    esac
done | head -100

echo
echo '== 各进程 maps 里的 exe（磁盘真实文件名）=='
for p in $(ps -eo pid 2>/dev/null | tail -n +2); do
    c=$(cat /proc/$p/comm 2>/dev/null || true)
    case "$c" in
        *wine*|*Wine*|*exe*|*EXE*|*Exe*|*fex*|*FEX*|*box64*)
            echo "[$p] comm=$c"
            awk '{ for (i=1;i<=5;i++) sub(/^[^ ]+ +/,""); print }' /proc/$p/maps 2>/dev/null | grep -i '\.exe' | sort -u | head -10
            echo
            ;;
    esac
done | head -120
echo '== wine 配置 =='
cat "${PREFIX:-/data/data/com.termux/files/usr}/glibc/opt/conf/wine_path.conf" 2>/dev/null | head -5
echo '== 完（把以上输出发给 AI）==' 
