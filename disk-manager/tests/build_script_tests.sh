#!/usr/bin/env bash
# Installer control-flow tests. Every package-manager/elevated command is mocked.
set -euo pipefail
SOURCE_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
TEMP="$(mktemp -d)"
trap 'rm -rf -- "$TEMP"' EXIT
MOCK_BIN="$TEMP/bin"
mkdir -p "$MOCK_BIN"
export MOCK_LOG="$TEMP/commands" MOCK_STATE="$TEMP/installed"
export IN_NIX_SHELL=""

cat > "$MOCK_BIN/mock-tool" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
tool="${0##*/}"
printf '%s' "$tool" >> "$MOCK_LOG"
printf ' <%s>' "$@" >> "$MOCK_LOG"
printf '\n' >> "$MOCK_LOG"
case "$tool" in
    uname) printf 'Linux\n' ;;
    sudo)
        [[ "${1:-}" != -- ]] || shift
        exec "$@"
        ;;
    dpkg-query)
        if [[ "${MOCK_MISSING:-0}" == 1 && ! -e "$MOCK_STATE" && "${*: -1}" == rsync ]]; then
            exit 1
        fi
        printf 'install ok installed'
        ;;
    apt-get)
        if [[ "${1:-}" == install ]]; then touch "$MOCK_STATE"; fi
        ;;
    cmake)
        if [[ "${1:-}" == --build && "${MOCK_FAIL_BUILD:-0}" == 1 ]]; then exit 42; fi
        ;;
esac
MOCK
chmod +x "$MOCK_BIN/mock-tool"
for tool in uname sudo dpkg-query apt-get cmake ctest ninja pkg-config c++ \
            lsblk sync rsync udisksctl dbus-send fuser mkfs.ext4 mkfs.exfat \
            mkfs.ntfs mkfs.vfat mkfs.btrfs mkfs.xfs systemctl update-desktop-database gtk-update-icon-cache; do
    ln -s mock-tool "$MOCK_BIN/$tool"
done
export PATH="$MOCK_BIN:$PATH"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
contains() { grep -q -- "$1" "$MOCK_LOG" || fail "Expected command: $1"; }
absent() { if grep -q -- "$1" "$MOCK_LOG"; then fail "Unexpected command: $1"; fi; }
reset_log() { : > "$MOCK_LOG"; rm -f "$MOCK_STATE"; }

# Rejecting installation must still configure/build/test, without elevation.
reset_log
printf 'no\n' | bash "$SOURCE_DIR/build.sh" --no-install-deps \
    --build-dir "$TEMP/build with spaces" > "$TEMP/no.out"
contains 'cmake <-S>'
contains 'cmake <--build>'
contains 'ctest <--test-dir>'
absent 'cmake <--install>'
absent '^sudo'

# EOF also defaults to no; no unattended system-wide installation.
reset_log
bash "$SOURCE_DIR/build.sh" --no-install-deps < /dev/null > "$TEMP/eof.out"
absent 'cmake <--install>'

# Only yes after a successful build installs through CMake under /usr/local.
reset_log
printf 'YES\n' | bash "$SOURCE_DIR/build.sh" --no-install-deps > "$TEMP/yes.out"
contains 'cmake <--install>'
contains '<--prefix> </usr/local>'
contains 'gtk-update-icon-cache <--force> <--ignore-theme-index> </usr/local/share/icons/hicolor>'

# A compiler failure stops before tests or installation, preserving its exit code.
reset_log
if MOCK_FAIL_BUILD=1 bash "$SOURCE_DIR/build.sh" --no-install-deps \
    --no-system-install > "$TEMP/failure.out" 2>&1; then
    fail "Failed build reported success"
else
    status=$?
    [[ "$status" == 42 ]] || fail "Build failure status was not preserved"
fi
absent '^ctest'
absent 'cmake <--install>'

# Invalid inputs fail without even configuring.
reset_log
if bash "$SOURCE_DIR/build.sh" --jobs 0 > "$TEMP/bad.out" 2>&1; then
    fail "Accepted zero compile jobs"
fi
absent '^cmake'

# Exercise Debian's package path on Debian/Ubuntu hosts, without real apt/sudo.
if [[ -r /etc/os-release ]]; then
    # shellcheck source=/dev/null
    . /etc/os-release
    case " ${ID:-} ${ID_LIKE:-} " in
        *" ubuntu "*|*" debian "*)
            reset_log
            if printf 'no\n' | MOCK_MISSING=1 bash "$SOURCE_DIR/build.sh" \
                --no-system-install > "$TEMP/decline-deps.out" 2>&1; then
                fail "Built after missing dependency installation was declined"
            fi
            absent '^apt-get'
            absent 'cmake <-S>'
            reset_log
            printf 'yes\n' | MOCK_MISSING=1 bash "$SOURCE_DIR/build.sh" \
                --no-system-install > "$TEMP/install-deps.out"
            contains 'apt-get <update>'
            contains 'apt-get <install> <-y> <--> <rsync>'
            contains 'cmake <--build>'
            absent 'cmake <--install>'
            ;;
    esac
fi
printf 'PASS: build helper prompts, EOF, dependency installation, paths, failures, and system-install consent\n'
