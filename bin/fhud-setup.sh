#!/usr/bin/env bash
OS_RELEASE_FILES=("/etc/os-release" "/usr/lib/os-release")
XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
MANGOHUD_CONFIG_DIR="$XDG_CONFIG_HOME/MangoHud"
SU_CMD=$(command -v sudo || command -v doas || echo)
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# doas requires a double dash if the command it runs will include any dashes,
# so append a double dash to the command
[[ $SU_CMD == *doas ]] && SU_CMD="$SU_CMD -- "

# Correctly identify the os-release file.
for os_release in ${OS_RELEASE_FILES[@]} ; do
    if [[ ! -e "${os_release}" ]]; then
        continue
    fi
    DISTRO=$(sed -rn 's/^ID(_LIKE)*=(.+)/\2/p' ${os_release} | sed 's/"//g')
done

mangohud_usage() {
    echo 'Accepted arguments: "install", "uninstall".'
}

mangohud_config() {
    mkdir -p "${MANGOHUD_CONFIG_DIR}"
    echo You can use the example configuration file from
    echo /usr/share/doc/mangohud/MangoHud.conf.example
    echo as a starting point by copying it to
    echo ${MANGOHUD_CONFIG_DIR}/MangoHud.conf
    echo
}

mangohud_uninstall() {
    [ "$UID" -eq 0 ] || exec $SU_CMD bash "$0" uninstall
    rm -rfv "/usr/lib/fhud"
    rm -fv "/usr/share/vulkan/implicit_layer.d/FHUD.x86.json"
    rm -fv "/usr/share/vulkan/implicit_layer.d/FHUD.x86_64.json"
    rm -fv "/usr/share/vulkan/implicit_layer.d/FHUD.json"
    rm -frv "/usr/share/doc/mangohud"
    rm -fv "/usr/share/man/man1/mangohud.1"
    rm -fv "/usr/bin/fhud"
    rm -fv "/usr/bin/fhudplot"
    rm -fv "/usr/bin/fhud.x86"
}

mangohud_install() {
    rm -rf "$HOME/.local/share/MangoHud/"
    rm -f "$HOME/.local/share/vulkan/implicit_layer.d/"{mangohud32.json,mangohud64.json}

    [ "$UID" -eq 0 ] || mangohud_config

    if [ "$UID" -ne 0 ]; then
        if [ ! -f "$SCRIPT_DIR/MangoHud-package.tar" ]; then
            echo "Error: MangoHud-package.tar was not found next to fhud-setup.sh."
            exit 1
        fi

        tar xf "$SCRIPT_DIR/MangoHud-package.tar" || {
            echo "Error: failed to unpack MangoHud-package.tar."
            exit 1
        }

        exec $SU_CMD bash "$SCRIPT_DIR/fhud-setup.sh" install
    fi

    mangohud_uninstall

    DEFAULTLIB=lib32
    for i in $DISTRO; do
        case $i in
            *arch*)
            DEFAULTLIB=lib64
            ;;
        esac
    done

    echo DEFAULTLIB: $DEFAULTLIB
    /usr/bin/install -Dvm644 ./usr/lib/fhud/lib64/libFHUD.so /usr/lib/fhud/lib64/libFHUD.so
    /usr/bin/install -Dvm644 ./usr/lib/fhud/lib32/libFHUD.so /usr/lib/fhud/lib32/libFHUD.so
    /usr/bin/install -Dvm644 ./usr/lib/fhud/lib64/libFHUD_opengl.so /usr/lib/fhud/lib64/libFHUD_opengl.so
    /usr/bin/install -Dvm644 ./usr/lib/fhud/lib32/libFHUD_opengl.so /usr/lib/fhud/lib32/libFHUD_opengl.so
    /usr/bin/install -Dvm644 ./usr/lib/fhud/lib64/libFHUD_shim.so /usr/lib/fhud/lib64/libFHUD_shim.so
    /usr/bin/install -Dvm644 ./usr/lib/fhud/lib32/libFHUD_shim.so /usr/lib/fhud/lib32/libFHUD_shim.so
    /usr/bin/install -Dvm644 ./usr/share/vulkan/implicit_layer.d/FHUD.x86_64.json /usr/share/vulkan/implicit_layer.d/FHUD.x86_64.json
    /usr/bin/install -Dvm644 ./usr/share/vulkan/implicit_layer.d/FHUD.x86.json /usr/share/vulkan/implicit_layer.d/FHUD.x86.json
    /usr/bin/install -Dvm644 ./usr/share/man/man1/mangohud.1 /usr/share/man/man1/mangohud.1
    /usr/bin/install -Dvm644 ./usr/share/doc/mangohud/MangoHud.conf.example /usr/share/doc/mangohud/MangoHud.conf.example
    /usr/bin/install -vm755  ./usr/bin/fhud /usr/bin/fhud
    /usr/bin/install -vm755  ./usr/bin/fhud-server /usr/bin/fhud-server
    /usr/bin/install -vm755  ./usr/bin/fhudplot /usr/bin/fhudplot

    ln -sv $DEFAULTLIB /usr/lib/fhud/lib

    # FIXME get the triplet somehow
    ln -sv lib64 /usr/lib/fhud/x86_64
    ln -sv lib64 /usr/lib/fhud/x86_64-linux-gnu
    ln -sv . /usr/lib/fhud/lib64/x86_64
    ln -sv . /usr/lib/fhud/lib64/x86_64-linux-gnu

    ln -sv lib32 /usr/lib/fhud/i686
    ln -sv lib32 /usr/lib/fhud/i386-linux-gnu
    ln -sv lib32 /usr/lib/fhud/i686-linux-gnu

    mkdir -p /usr/lib/fhud/tls
    ln -sv ../lib64 /usr/lib/fhud/tls/x86_64
    ln -sv ../lib32 /usr/lib/fhud/tls/i686

    # Some distros search in $prefix/x86_64-linux-gnu/tls/x86_64 etc instead
    if [ ! -e /usr/lib/fhud/lib/i386-linux-gnu ]; then
        ln -sv ../lib32 /usr/lib/fhud/lib/i386-linux-gnu
    fi
    if [ ! -e /usr/lib/fhud/lib/i686-linux-gnu ]; then
        ln -sv ../lib32 /usr/lib/fhud/lib/i686-linux-gnu
    fi
    if [ ! -e /usr/lib/fhud/lib/x86_64-linux-gnu ]; then
        ln -sv ../lib64 /usr/lib/fhud/lib/x86_64-linux-gnu
    fi

    # $LIB can be "lib/tls/x86_64"?
    ln -sv ../tls /usr/lib/fhud/lib/tls

    # Let's not clean up because this can be destructive
    # rm -rf ./usr

    echo "MangoHud Installed"
}

for a in $@; do
    case $a in
        "install") mangohud_install;;
        "uninstall") mangohud_uninstall;;
        *)
            echo "Unrecognized command argument: $a"
            mangohud_usage
    esac
done

if [ -z $@ ]; then
    mangohud_usage
fi
