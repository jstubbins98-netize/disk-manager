#!/usr/bin/env bash
# Build as a normal user. Elevate only package installation and an explicitly
# approved system-wide installation. Never runs the application as root.
set -Eeuo pipefail

SOURCE_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SOURCE_DIR/build"
PREFIX="/usr/local"
INSTALL_DEPS=1
ASK_SYSTEM_INSTALL=1
JOBS="${JOBS:-4}"
PACKAGES=()
MISSING=()
MANAGER=""

usage() {
    cat <<'EOF'
Usage: ./build.sh [options]

Checks development/runtime dependencies, offers to install missing packages,
builds Disk Manager, runs its tests, then asks about system-wide installation.
Only an explicit "y" or "yes" authorizes each installation.

Supported automatic package installation:
  Debian/Ubuntu, Fedora, Arch/Manjaro, openSUSE.

Options:
  --no-install-deps       Do not install packages; check build tools and report
                          missing runtime tools. Useful with custom Qt/Nix setups.
  --no-system-install    Build/test only; do not offer a system-wide installation.
  --build-dir PATH       Use a separate build directory.
  --jobs NUMBER          Parallel compile jobs (default: 4, or JOBS environment).
  -h, --help             Show this help.

System-wide installation includes /usr/local/bin/diskmgr, the desktop launcher,
and its icon under /usr/local/share/icons/hicolor/512x512/apps. It may replace a previous
Disk Manager installation at that prefix. No disk operations are performed.
Run without sudo; the script asks for elevation only when installation needs it.
EOF
}
die() { printf 'Error: %s\n' "$*" >&2; exit 1; }
confirm() {
    local answer
    printf '%s [y/N] ' "$1"
    if ! IFS= read -r answer; then printf '\n'; return 1; fi
    case "${answer,,}" in y|yes) return 0 ;; *) return 1 ;; esac
}
as_root() {
    if (( EUID == 0 )); then
        "$@"
    else
        command -v sudo >/dev/null 2>&1 || die "sudo is required for this installation. Ask your administrator to install the listed packages."
        sudo -- "$@"
    fi
}
trap 'status=$?; printf "\nBuild/install stopped (exit %s, line %s). Fix the reported error and rerun this script.\n" "$status" "$LINENO" >&2; exit "$status"' ERR

while (( $# )); do
    case "$1" in
        --no-install-deps) INSTALL_DEPS=0; shift ;;
        --no-system-install) ASK_SYSTEM_INSTALL=0; shift ;;
        --build-dir)
            (( $# >= 2 )) || die "--build-dir requires a path."
            [[ -n "$2" && "$2" != -* ]] || die "--build-dir requires a path."
            BUILD_DIR="$2"; shift 2 ;;
        --jobs)
            (( $# >= 2 )) || die "--jobs requires a number."
            JOBS="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "Unknown option: $1 (use --help)." ;;
    esac
done
[[ "$(uname -s)" == Linux ]] || die "Disk Manager requires Linux."
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die "The job count must be a positive integer."
[[ "$BUILD_DIR" = /* ]] || BUILD_DIR="$PWD/$BUILD_DIR"

detect_distribution() {
    # A Nix toolchain may coexist with an Ubuntu host whose apt is not usable.
    # Never try to mutate that host with a distribution package manager.
    local cmake_path
    cmake_path="$(command -v cmake || true)"
    if [[ -d /nix/store && ( "$cmake_path" == /nix/store/* || -n "${IN_NIX_SHELL:-}" ) ]]; then
        MANAGER=nix
        return
    fi
    [[ -r /etc/os-release ]] || return
    local ID="" ID_LIKE=""
    # shellcheck source=/dev/null
    . /etc/os-release
    case " $ID $ID_LIKE " in
        *" debian "*|*" ubuntu "*)
            MANAGER=apt
            PACKAGES=(build-essential cmake ninja-build pkg-config qt6-base-dev
                libgl-dev util-linux rsync udisks2 dbus polkitd psmisc
                e2fsprogs exfatprogs ntfs-3g dosfstools btrfs-progs xfsprogs systemd)
            ;;
        *" fedora "*)
            MANAGER=dnf
            PACKAGES=(gcc-c++ cmake ninja-build pkgconf-pkg-config qt6-qtbase-devel
                libglvnd-devel util-linux rsync udisks2 dbus polkit psmisc
                e2fsprogs exfatprogs ntfs-3g dosfstools btrfs-progs xfsprogs systemd)
            ;;
        *" arch "*|*" manjaro "*)
            MANAGER=pacman
            PACKAGES=(base-devel cmake ninja pkgconf qt6-base libglvnd util-linux
                rsync udisks2 dbus polkit psmisc e2fsprogs exfatprogs ntfs-3g
                dosfstools btrfs-progs xfsprogs systemd)
            ;;
        *" opensuse "*|*" opensuse-"*|*" suse "*)
            MANAGER=zypper
            PACKAGES=(gcc-c++ cmake ninja pkgconf-pkg-config qt6-base-devel
                libglvnd-devel util-linux rsync udisks2 dbus-1 polkit psmisc
                e2fsprogs exfatprogs ntfs-3g dosfstools btrfsprogs xfsprogs systemd)
            ;;
    esac
}
package_installed() {
    local status
    case "$MANAGER" in
        apt)
            status="$(dpkg-query -W -f='${Status}' "$1" 2>/dev/null || true)"
            [[ "$status" == "install ok installed" ]]
            ;;
        dnf|zypper) rpm -q "$1" >/dev/null 2>&1 ;;
        pacman)
            # Arch's base-devel is a package on current releases. Do not infer
            # it is installed merely from one compiler being present.
            pacman -Q "$1" >/dev/null 2>&1
            ;;
        *) return 1 ;;
    esac
}
install_missing() {
    case "$MANAGER" in
        apt)
            command -v apt-get >/dev/null || die "apt-get not found."
            as_root apt-get update
            as_root apt-get install -y -- "${MISSING[@]}"
            ;;
        dnf) as_root dnf install -y -- "${MISSING[@]}" ;;
        # Refresh and upgrade together: partial upgrades are unsupported on Arch.
        pacman) as_root pacman -Syu --needed -- "${MISSING[@]}" ;;
        zypper) as_root zypper --non-interactive install -- "${MISSING[@]}" ;;
        *) die "Automatic dependency installation is not supported on this system." ;;
    esac
}
check_dependencies() {
    detect_distribution
    if (( INSTALL_DEPS )); then
        case "$MANAGER" in
            nix)
                die "Nix-based toolchain detected. Provide dependencies in your Nix environment, then run ./build.sh --no-install-deps. See README.md for the package list."
                ;;
            "") die "Unsupported distribution. Install the dependencies in README.md manually, then use --no-install-deps." ;;
        esac
        local package
        for package in "${PACKAGES[@]}"; do
            if ! package_installed "$package"; then MISSING+=("$package"); fi
        done
        if (( ${#MISSING[@]} )); then
            printf '\nMissing packages (%s):\n  %s\n\n' "$MANAGER" "${MISSING[*]}"
            if [[ "$MANAGER" == pacman ]]; then
                printf 'Arch requires a full synchronized system upgrade (pacman -Syu) with this installation.\n'
            fi
            confirm "Install these dependencies using administrator privileges?" ||
                die "Dependency installation declined. Nothing was installed. Install them manually or use --no-install-deps."
            install_missing
            for package in "${MISSING[@]}"; do
                package_installed "$package" || die "Package still missing after installation: $package"
            done
        else
            printf 'All required distribution packages are installed.\n'
        fi
    else
        printf 'Automatic dependency installation disabled; checking the available toolchain.\n'
    fi

    local tool
    for tool in cmake ctest ninja pkg-config; do
        command -v "$tool" >/dev/null 2>&1 || die "Missing build tool: $tool. Install it and rerun."
    done
    if [[ -n "${CXX:-}" ]]; then
        command -v "$CXX" >/dev/null 2>&1 || die "CXX must name an available C++20 compiler executable."
    elif ! command -v c++ >/dev/null 2>&1 && ! command -v g++ >/dev/null 2>&1 && ! command -v clang++ >/dev/null 2>&1; then
        die "No C++ compiler found. Install GCC 12+ or a recent Clang."
    fi
    # Custom Qt installations may have CMake configs without pkg-config files;
    # the subsequent required CMake find_package is the final authority.
    if ! pkg-config --atleast-version=6.4 Qt6Core Qt6Widgets Qt6Concurrent Qt6DBus; then
        printf 'Qt6 >= 6.4 was not found with pkg-config. Trying CMake discovery (set CMAKE_PREFIX_PATH for a custom Qt installation).\n'
    fi
    local runtime_missing=()
    for tool in lsblk sync rsync udisksctl dbus-send fuser \
                mkfs.ext4 mkfs.exfat mkfs.ntfs mkfs.vfat mkfs.btrfs mkfs.xfs systemctl; do
        if ! command -v "$tool" >/dev/null 2>&1 &&
           [[ ! -x "/usr/sbin/$tool" && ! -x "/sbin/$tool" ]]; then
            runtime_missing+=("$tool")
        fi
    done
    if (( ${#runtime_missing[@]} )); then
        printf '\nRuntime tools missing or not on PATH: %s\n' "${runtime_missing[*]}" >&2
        printf 'Related device, formatting, backup, or scheduling features will not work until these are installed.\n' >&2
        if (( INSTALL_DEPS )); then
            die "Dependencies were installed but runtime tools are unavailable. Check your PATH (including /usr/sbin and /sbin)."
        fi
    fi
    printf '\nDevice operations also need a running system D-Bus, UDisks2, and your desktop polkit authentication agent.\n'
    printf 'This script does not start services, change authorization rules, or run disk operations.\n'
}

check_dependencies
printf '\nConfiguring %s\n' "$SOURCE_DIR"
cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
printf '\nCompiling with %s parallel jobs…\n' "$JOBS"
cmake --build "$BUILD_DIR" --parallel "$JOBS"
printf '\nRunning tests…\n'
ctest --test-dir "$BUILD_DIR" --output-on-failure

printf '\nBuild succeeded. Run as your ordinary desktop user:\n  %q\n' "$BUILD_DIR/diskmgr"
if (( ASK_SYSTEM_INSTALL )); then
    printf '\nSystem-wide installation will write to:\n'
    printf '  %s/bin/diskmgr\n  %s/share/applications/io.diskmanager.desktop\n  %s/share/icons/hicolor/512x512/apps/io.diskmanager.png\n  %s/share/doc/DiskManager/\n' "$PREFIX" "$PREFIX" "$PREFIX" "$PREFIX"
    printf 'An existing Disk Manager installation at this prefix may be replaced.\n'
    if confirm "Install Disk Manager system-wide now?"; then
        CMAKE_BIN="$(command -v cmake)"
        as_root "$CMAKE_BIN" --install "$BUILD_DIR" --prefix "$PREFIX"
        if command -v update-desktop-database >/dev/null 2>&1; then
            if ! as_root "$(command -v update-desktop-database)" "$PREFIX/share/applications"; then
                printf 'Warning: installation completed, but refreshing the desktop database failed.\n' >&2
            fi
        fi
        if command -v gtk-update-icon-cache >/dev/null 2>&1; then
            if ! as_root "$(command -v gtk-update-icon-cache)" --force --ignore-theme-index "$PREFIX/share/icons/hicolor"; then
                printf 'Warning: installation completed, but refreshing the icon cache failed. Sign out and back in if the old icon remains.\n' >&2
            fi
        fi
        printf '\nInstalled system-wide. Launch "diskmgr" or find Disk Manager in your application menu.\n'
        printf 'Run the application without sudo.\n'
    else
        printf '\nSystem-wide installation skipped. The compiled program remains in %s.\n' "$BUILD_DIR"
    fi
fi
