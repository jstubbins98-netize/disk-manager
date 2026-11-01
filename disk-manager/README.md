# Disk Manager for Linux

A native **C++20 / Qt6 Widgets** application for managing drives, analyzing disk
space, and creating backups. It works with the disks and folders on the Linux
computer where it is running.

**Never test formatting on a disk containing data you need.** Hardware-changing
operations have not been exercised on physical devices in this development
environment. Use a disposable external drive and keep a separate backup.

## Build and launch

### Automatic dependency check, build, and optional installation

From the repository root:

```sh
./build.sh
```

Or from this standalone native project folder:

```sh
./build.sh
```

The script detects Debian/Ubuntu, Fedora, Arch/Manjaro, and openSUSE, lists
missing build and runtime packages, and asks before installing them with your
distribution's package manager. On Arch this uses a full `pacman -Syu` upgrade
to avoid an unsupported partial upgrade; the script warns before asking.
It configures a Release build, compiles, runs the core tests, and then asks:

```text
Install Disk Manager system-wide now? [y/N]
```

Only `y` or `yes` installs system-wide. The default is **no**, including when
standard input is closed. A declined dependency-install prompt stops the build
rather than silently installing anything.

If you answer yes to the final prompt, CMake installs:

- `/usr/local/bin/diskmgr`
- `/usr/local/share/applications/io.diskmanager.desktop`
- `/usr/local/share/icons/hicolor/512x512/apps/io.diskmanager.png`
- `/usr/local/share/doc/DiskManager/README.md`

This may replace a previous installation at those paths. Compilation and tests
run as the user who launched the script; `sudo` is used only for approved
package installation, system-wide installation, and desktop/icon cache refresh.
Run the script **without sudo**. It never launches the app as root or starts,
enables, or reconfigures disk services.

The supplied drive picture is preserved in `logo/HDD_logo.png`. The launcher
uses a 512×512 copy, `logo/io.diskmanager.png`, installed in the standard hicolor
icon theme. Reinstall to apply this icon to an existing installation.

Useful options:

```sh
# Build/test only, without the system-wide installation question.
./build.sh --no-system-install

# Dependencies already provided by a custom toolchain (including Nix).
# Runtime tools are still checked and missing features are reported.
./build.sh --no-install-deps --no-system-install

# Limit resource usage or choose a separate build directory.
./build.sh --jobs 2 --build-dir /tmp/diskmanager-build
./build.sh --help
```

Nix environments and unrecognized distributions require manually provided
dependencies and `--no-install-deps`; the script never tries apt inside a
detected Nix toolchain. Custom Qt installations can set `CMAKE_PREFIX_PATH`.
Older distribution releases whose repositories do not provide Qt 6.4+ or a
suitable C++20 compiler will fail CMake's requirements; use a newer distribution
or provide a supported toolchain. No third-party repositories are added.

### Manual dependency installation and build

Dependencies: C++20 compiler (GCC 12+ or recent Clang), CMake 3.20+, Qt 6.4+
Core/Widgets/Concurrent/D-Bus development packages, and rsync 3.x.
Device operations need UDisks2, a running system D-Bus, and your desktop's polkit
authentication agent. The corresponding filesystem tools must be installed for
formatting.

On Ubuntu 24.04 / Debian 12 or later:

```sh
sudo apt install build-essential cmake ninja-build pkg-config qt6-base-dev \
    libgl-dev rsync udisks2 polkitd psmisc \
    e2fsprogs exfatprogs ntfs-3g dosfstools btrfs-progs xfsprogs
```

On Fedora:

```sh
sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config qt6-qtbase-devel \
    rsync udisks2 polkit psmisc e2fsprogs exfatprogs \
    ntfs-3g dosfstools btrfs-progs xfsprogs
```

From this folder:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./build/diskmgr
```

For manual system-wide installation:

```sh
sudo cmake --install build --prefix /usr/local
```

Alternatively, install just for your user, without administrator privileges:

```sh
cmake --install build --prefix "$HOME/.local"
```

Ensure `~/.local/bin` is in your PATH. **Run the GUI as your ordinary user, not
with sudo.** UDisks2 asks polkit for authorization when required; your desktop
policy may also prompt for mounting or unmounting.

This repository also contains an unrelated initial web scaffold. The C++ program
is independent of pnpm, the API server, and the canvas. A browser cannot access
your laptop's USB disks: build and run this application on that laptop.

## What works

### Devices

- Real `lsblk` discovery of disks and partitions; capacity, label, filesystem,
  mount points, and protection state.
- UDisks2 mount (read/write or read-only), sync then unmount, and physical drive
  power-off after all its volumes unmount.
- Unmount errors include `fuser` output where available. No forced unmount.
- SMART failure indicator through UDisks2 when supported by the drive/bridge.
- Format an **existing, unmounted external partition** as ext4, exFAT, NTFS,
  FAT32, Btrfs, or XFS. Missing formatter tools cause explicit UDisks2 errors.
- Quick format or optional partition zero-fill, with a clear sanitization warning.
- Summary naming the exact partition/capacity, typed label/name confirmation,
  fresh identity validation, and normal desktop authorization.
- Internal disks, system mounts, swap, read-only disks, and unsupported
  encrypted/LVM/RAID device stacks are protected. Protection applies to the
  whole physical disk when any of its volumes is protected.

**Formatting permanently erases data on the selected partition.** Zero-fill is
not guaranteed secure sanitization of SSDs, flash, remapped sectors, or drives
with controller caches. There is no claim of certified secure erase.

### Space

- Cancellable background folder scans; no symlink traversal or mount crossing.
- Filesystem used/available/reserved figures and file-type totals.
- Size-sorted file list with last-access dates; files older than 180 days are
  highlighted. Access time is only a hint, not evidence a file is unused.
- Clickable one-level folder rectangle map and folder ring. These intentionally
  do not claim to be recursive squarified treemaps or multilevel sunbursts.
- Same-size grouping followed by streaming SHA-256 duplicate comparison.
  Existing hard links are not counted as reclaimable duplicate copies.
- GiB, GB, and percentage display for scanned file sizes.
- Successful-scan history, persisted atomically and limited to 365 entries.
- Cleanup suggestions and individually reviewed move-to-Trash actions.
  No permanent-delete fallback.

Sizes are logical file bytes rather than allocated blocks. Hidden files are
included. Errors and cancelled scans are explicitly identified as incomplete.
The GUI lists the 10,000 largest files; the scanner keeps complete metadata for
duplicate detection and backup verification. Very large scans consume memory
proportional to the number of files. Folder totals refer to immediate children;
click to analyze a deeper level.

### Backup

- Home, Documents & Media (Documents/Pictures/Videos/Music), readable full-system,
  or custom folder source; literal-name/path exclusions.
- Destination selection from mounted external volumes.
- Mandatory complete source scan and conservative destination-space check.
  The check includes the full logical source size, 4 KiB per regular file,
  and a 64 MiB margin. It deliberately does **not** assume compression or
  hard-link savings. Large files are suggested for exclusion on failure.
- Source/destination overlap protection. For `/`, a destination inside the
  source is only accepted if it is on a different filesystem.
- A second scan and space/volume check immediately before copying.
- Full rsync copies and hard-linked incremental/snapshot versions. The latter
  two modes use the same versioning engine in this release: a complete
  browsable tree with unchanged files linked to the previous compatible backup.
- Incremental mode explicitly rejects known non-Linux filesystems. Failed
  hard-link creation on any other unsupported filesystem is an error.
- rsync progress output (percentage, rate, elapsed/remaining estimates, current
  file, and final statistics), pause/resume and cancel **during copying**.
  Preparation scans and post-copy verification must finish before closing.
- Optional SHA-256 comparison of every scanned regular file after copying.
- Atomic completion manifests and reports. A failed/cancelled copy is marked
  incomplete, is never a restore candidate, and is never a hard-link base.
- Restore everything or one file to an **empty** destination directory, without
  overwriting an existing user tree.
- Daily/weekly **user-session systemd timers**, with configuration and volume
  UUID checks. Missing/unmounted/different external drives fail the run safely.
- Review old versions individually and move them to Trash; newest completed
  version is protected. There is no automatic retention deletion.
- An explicit local-destination opt-in for development/testing.

Backups are file-level, not bootable images or atomic filesystem snapshots.
Live files can change while copying; a verification mismatch fails the backup.
No files are deleted from the source. The backup command does not use `--delete`
or `--inplace`. Ownership is not preserved because the app is not root.
ACLs, extended attributes, sparse layout, sockets, device nodes, and files on
other mounted filesystems are not guaranteed preserved. Relative symlinks that
stay inside the source are copied by rsync; unsafe external symlinks are skipped.
On FAT/exFAT/NTFS destinations, full copies disable Unix permission preservation,
hard links, and symlinks, and allow two-second timestamp precision. They are
portable file copies, not faithful Linux filesystem replicas.
Verification covers scanned regular files, not symlinks or empty directories.
rsync nonzero exits (including partial transfers) are failures.

The Full system preset is limited to files your user can read on the root
filesystem and excludes `/dev`, `/proc`, `/sys`, `/run`, `/tmp`, `/mnt`, `/media`,
and `/lost+found`. An unreadable file blocks the run rather than producing a
misleading complete backup. This is **not** privileged full-system recovery.

### Backup layout

```text
<destination>/DiskManager/
  .lock
  <UTC timestamp>-<unique id>/
    manifest.json
    data/
```

Do not edit files inside older hard-linked backup versions: edits can affect
the same inode in another version. Use the Restore action to create independent
copies first. Do not unplug a drive during a backup or format operation.

## Command-line mode

```sh
diskmgr --help
diskmgr --list
diskmgr --scan "$HOME/Documents"

# Inspect the required space without copying.
diskmgr --check --source "$HOME" --dest "/media/$USER/Backup"

# Full verified backup to a mounted external volume.
diskmgr --backup --source "$HOME" --dest "/media/$USER/Backup" \
    --exclude node_modules --exclude .cache

# A hard-linked version; requires a compatible filesystem.
diskmgr --backup --source "$HOME" --dest "/media/$USER/Backup" \
    --mode incremental

# Deliberate local-only test; source/destination must not overlap.
diskmgr --backup --source /tmp/test-source --dest /tmp/test-destination \
    --allow-local

# A configuration written by the GUI scheduler.
diskmgr --backup-config /path/to/scheduled-backup.json
```

There is no destructive formatting CLI. Exclusions are literal names or paths,
not wildcard expressions. Absolute subfolder exclusions are translated to the
same source-relative rules for both scanning and rsync.

Configuration/history uses Qt's per-user application data directory
(`~/.local/share/DiskManager/DiskManager` on a standard Linux desktop).
Backup reports are stored on the destination beside the copied files.

### Manage a schedule

Use Backup → Check space → Schedule. Only one saved schedule is supported;
changing it replaces the previous configuration.

```sh
systemctl --user list-timers diskmanager-backup.timer
journalctl --user -u diskmanager-backup.service
systemctl --user disable --now diskmanager-backup.timer
```

The user session must be running. Timers do not mount an absent drive or elevate
privileges. Recheck settings before scheduling. The executable must remain at
the installed path used when the timer was created.

## Roadmap

The following features are not currently implemented:

- GPT/MBR creation and multiple-partition editing.
- Custom mount points and automatic mounting on plug-in.
- True multilevel sunburst / recursive squarified treemap visualizations,
  scheduled space snapshots, and a trend graph (scan history is a table).
- Compressed/encrypted repositories (LUKS, Borg, or Restic), privileged
  full-system recovery, and multiple custom source roots in one job.
- Plug-in backup triggers, automatic retention deletion, desktop notifications,
  and network destinations.
- SMART self-tests, temperature history, and comprehensive health diagnostics.

## Development verification

`core-tests` uses synthetic block-device JSON and temporary folders. It checks
system-disk protection, whole-disk refusal, D-Bus object naming, exclusion and
symlink behavior, SHA-256 duplicates, cancellation, source/destination overlap,
local-destination opt-in, and space margins. It **never** mounts, unmounts,
formats, or powers down a real disk.

For headless visual inspection of the actual Qt UI:

```sh
./build/diskmgr --screenshot generated-artifacts/screenshots
```

The `offscreen` Qt platform plugin must be available. This captures the real
native window, not a web mockup.
