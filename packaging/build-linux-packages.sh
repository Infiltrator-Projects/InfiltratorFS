#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -Eeuo pipefail

build_dir="${1:-build}"
dist_dir="${2:-dist}"
version="$(sed -n 's/^project(InfiltratorFS VERSION \([^ ]*\) LANGUAGES C)$/\1/p' CMakeLists.txt)"

if [[ -z "$version" ]]; then
    echo "Could not determine InfiltratorFS version from CMakeLists.txt" >&2
    exit 1
fi
if [[ ! -f src/infiltratr-common/CMakeLists.txt ]]; then
    echo "The pinned Infiltratr Common submodule is not initialised." >&2
    echo "Run: git submodule update --init --recursive" >&2
    exit 1
fi

mkdir -p "$dist_dir"
architecture="$(dpkg --print-architecture 2>/dev/null || true)"
if [[ -z "$architecture" ]]; then
    echo "Could not determine the Debian package architecture" >&2
    exit 1
fi

deb_name="infiltratorfs_${version}_${architecture}.deb"
run_name="infiltratorfs-${version}-linux-native.run"
package_root="$(mktemp -d)"
payload="$(mktemp)"
trap 'rm -rf "$package_root" "$payload"' EXIT

cmake --install "$build_dir" --prefix "$package_root/usr"
install -d "$package_root/usr/share/doc/infiltratorfs"
install -m 0644 LICENSE "$package_root/usr/share/doc/infiltratorfs/copyright"
install -m 0644 README.md "$package_root/usr/share/doc/infiltratorfs/README.md"

# Ship the native read-only kernel adapter as DKMS source rather than as a
# kernel-version-specific binary. The installed host therefore builds only the
# out-of-tree module against its own kernel headers; Linux itself is untouched.
dkms_root="$package_root/usr/src/infiltratorfs-$version"
install -d "$dkms_root"
install -m 0644 kernel/Makefile "$dkms_root/Makefile"
install -m 0644 kernel/infiltratorfs.c "$dkms_root/infiltratorfs.c"
install -m 0644 kernel/infiltratorfs_format.h "$dkms_root/infiltratorfs_format.h"
install -m 0644 kernel/infiltratorfs_rw.inc "$dkms_root/infiltratorfs_rw.inc"
cat > "$dkms_root/dkms.conf" <<EOF
PACKAGE_NAME="infiltratorfs"
PACKAGE_VERSION="${version}"
BUILT_MODULE_NAME[0]="infiltratorfs"
DEST_MODULE_LOCATION[0]="/updates/dkms"
AUTOINSTALL="yes"
MAKE[0]="make KDIR=/lib/modules/\${kernelver}/build"
CLEAN="make KDIR=/lib/modules/\${kernelver}/build clean"
EOF

install -d "$package_root/DEBIAN"
installed_size="$(du -sk "$package_root/usr" | cut -f1)"
cat > "$package_root/DEBIAN/control" <<EOF
Package: infiltratorfs
Version: ${version}
Section: utils
Priority: optional
Architecture: ${architecture}
Maintainer: The First Infiltrator
Depends: dkms, kmod, fuse3, libfuse3-3, policykit-1, util-linux, xdg-utils, zenity
Recommends: linux-headers-generic
Installed-Size: ${installed_size}
Homepage: https://github.com/The-First-Infiltrator/InfiltratorFS
Description: platform-neutral experimental filesystem tools
 InfiltratorFS formatter, inspector, scrubber, forensic scanner,
 standard mount/fsck helpers, direct-image utility, Linux FUSE adapter,
 native Linux kernel module via DKMS with initial read-write support and Linux Mint desktop manager.
 Use only with image files or disposable or backed-up media.
EOF

cat > "$package_root/DEBIAN/preinst" <<EOF
#!/bin/sh
set -e
module='infiltratorfs'
version='${version}'

# Remove older registrations before this package is unpacked/configured. A
# broken old DKMS source must not poison future kernel-header postinst hooks.
if command -v dkms >/dev/null 2>&1; then
    dkms status -m "\$module" 2>/dev/null | while IFS= read -r line; do
        case "\$line" in
            "\$module"/*) ;;
            *) continue ;;
        esac
        head="\${line%%,*}"
        head="\${head%%:*}"
        old_version="\${head#\${module}/}"
        if [ -n "\$old_version" ] && [ "\$old_version" != "\$version" ]; then
            echo "InfiltratorFS: removing stale DKMS registration \$old_version before upgrade."
            dkms remove -m "\$module" -v "\$old_version" --all || true
            rm -rf "/usr/src/\$module-\$old_version" "/var/lib/dkms/\$module/\$old_version" || true
        fi
    done
fi
exit 0
EOF
chmod 0755 "$package_root/DEBIAN/preinst"

cat > "$package_root/DEBIAN/postinst" <<EOF
#!/bin/sh
set -e
module='infiltratorfs'
version='${version}'
kernel="\$(uname -r)"

if command -v dkms >/dev/null 2>&1; then
    if ! dkms status -m "\$module" -v "\$version" 2>/dev/null | grep -q .; then
        dkms add -m "\$module" -v "\$version"
    fi
    if [ -f "/lib/modules/\$kernel/build/Makefile" ]; then
        if dkms build -m "\$module" -v "\$version" -k "\$kernel" && \
           dkms install -m "\$module" -v "\$version" -k "\$kernel" --force; then
            :
        else
            echo "InfiltratorFS: WARNING: native kernel module build failed for \$kernel." >&2
            echo "The userspace tools and FUSE adapter are installed and remain usable." >&2
            echo "After installing compatible headers/source, retry: sudo dkms autoinstall" >&2
        fi
    else
        echo "InfiltratorFS: kernel headers for \$kernel are not installed." >&2
        echo "Install linux-headers-\$kernel, then run: sudo dkms autoinstall" >&2
    fi
fi
if command -v depmod >/dev/null 2>&1; then
    depmod -a || true
fi
exit 0
EOF
chmod 0755 "$package_root/DEBIAN/postinst"

cat > "$package_root/DEBIAN/prerm" <<EOF
#!/bin/sh
set -e
if [ "\${1:-}" = remove ] && command -v dkms >/dev/null 2>&1; then
    dkms remove -m infiltratorfs -v '${version}' --all || true
fi
exit 0
EOF
chmod 0755 "$package_root/DEBIAN/prerm"

cat > "$package_root/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if command -v depmod >/dev/null 2>&1; then
    depmod -a || true
fi
exit 0
EOF
chmod 0755 "$package_root/DEBIAN/postrm"

dpkg-deb --root-owner-group --build "$package_root" "$dist_dir/$deb_name"
dpkg-deb --contents "$dist_dir/$deb_name" > "$dist_dir/package-contents.txt"
grep -q 'usr/bin/infiltratorfs-manager$' "$dist_dir/package-contents.txt"
grep -q 'usr/bin/infilfs-forensic$' "$dist_dir/package-contents.txt"
grep -q 'usr/sbin/mount.infiltratorfs$' "$dist_dir/package-contents.txt"
grep -q 'usr/sbin/fsck.infiltratorfs$' "$dist_dir/package-contents.txt"
grep -q 'usr/lib/infiltratorfs/infiltratorfs-manager-helper$' "$dist_dir/package-contents.txt"
grep -q 'usr/share/applications/infiltratorfs-manager.desktop$' "$dist_dir/package-contents.txt"
grep -q "usr/src/infiltratorfs-${version}/dkms.conf$" "$dist_dir/package-contents.txt"
grep -q "usr/src/infiltratorfs-${version}/infiltratorfs.c$" "$dist_dir/package-contents.txt"
grep -q "usr/src/infiltratorfs-${version}/infiltratorfs_format.h$" "$dist_dir/package-contents.txt"
grep -q "usr/src/infiltratorfs-${version}/infiltratorfs_rw.inc$" "$dist_dir/package-contents.txt"
test "$(dpkg-deb --field "$dist_dir/$deb_name" Version)" = "$version"
dpkg-deb --ctrl-tarfile "$dist_dir/$deb_name" | tar -xOf - ./preinst | \
    grep -Fq 'removing stale DKMS registration'
for dependency in dkms kmod fuse3 libfuse3-3 policykit-1 util-linux xdg-utils zenity; do
    dpkg-deb --field "$dist_dir/$deb_name" Depends |
        grep -Eq "(^|, )${dependency}([ ,]|$)"
done
rm -f "$dist_dir/package-contents.txt"

# The native installer contains the complete tested source tree, including the
# pinned Common submodule. The target machine performs the real native build.
source_epoch="$(git log -1 --format=%ct 2>/dev/null || date +%s)"
tar \
    --sort=name \
    --mtime="@${source_epoch}" \
    --owner=0 \
    --group=0 \
    --numeric-owner \
    --exclude='./.git' \
    --exclude='*/.git' \
    --exclude='./build*' \
    --exclude='./dist' \
    -czf "$payload" .

cat > "$dist_dir/$run_name" <<'RUN_HEADER'
#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -Eeuo pipefail

self="$0"
marker='__INFILTRATORFS_NATIVE_PAYLOAD__'
payload_start="$(awk -v marker="$marker" '$0 == marker { print NR + 1; exit }' "$self")"

if [[ -z "$payload_start" ]]; then
    echo "Installer payload marker not found." >&2
    exit 1
fi

verify_installer() {
    local first_line header_tmp verify_root verify_ok
    first_line="$(head -n 1 "$self")"
    if [[ "$first_line" != '#!/usr/bin/env bash' ]]; then
        echo "Installer does not start with the expected Bash shebang." >&2
        return 1
    fi

    header_tmp="$(mktemp)"
    head -n "$((payload_start - 1))" "$self" > "$header_tmp"
    if ! bash -n "$header_tmp"; then
        rm -f "$header_tmp"
        return 1
    fi
    rm -f "$header_tmp"

    if ! tail -n +"$payload_start" "$self" | gzip -t; then
        echo "Embedded source payload is not a valid gzip stream." >&2
        return 1
    fi

    verify_root="$(mktemp -d)"
    verify_ok=1
    if ! tail -n +"$payload_start" "$self" |
        tar --no-same-owner -xzf - -C "$verify_root"; then
        verify_ok=0
    fi
    for required in CMakeLists.txt README.md support/installer/bootstrap.sh \
        src/infiltratr-common/CMakeLists.txt kernel/Makefile \
        kernel/infiltratorfs.c kernel/infiltratorfs_format.h; do
        if [[ ! -f "$verify_root/$required" ]]; then
            echo "Embedded source payload is missing $required." >&2
            verify_ok=0
        fi
    done
    if [[ ! -x "$verify_root/support/installer/bootstrap.sh" ]]; then
        echo "Embedded native bootstrap is not executable." >&2
        verify_ok=0
    elif ! bash -n "$verify_root/support/installer/bootstrap.sh"; then
        verify_ok=0
    fi
    rm -rf "$verify_root"
    [[ "$verify_ok" -eq 1 ]]
}

usage() {
    cat <<'USAGE'
Usage: infiltratorfs-<version>-linux-native.run [--verify|--dry-run]

  --verify   Check the installer header and embedded source payload only.
  --dry-run  Report requirements and commands without changing the system.
USAGE
}

verify_installer

case "${1:-}" in
    --verify)
        echo "InfiltratorFS native installer verified: header, payload and bootstrap are intact."
        exit 0
        ;;
    --dry-run|'')
        ;;
    --help|-h)
        usage
        exit 0
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/source"
tail -n +"$payload_start" "$self" |
    tar --no-same-owner -xzf - -C "$work/source"

if [[ "${1:-}" == '--dry-run' ]]; then
    "$work/source/support/installer/bootstrap.sh" --dry-run
else
    "$work/source/support/installer/bootstrap.sh"
fi
exit 0
__INFILTRATORFS_NATIVE_PAYLOAD__
RUN_HEADER
cat "$payload" >> "$dist_dir/$run_name"
chmod 0755 "$dist_dir/$run_name"

# Release construction must fail if the self-extractor or embedded bootstrap is
# malformed. This validation never compiles, installs or modifies the system.
test "$(head -n 1 "$dist_dir/$run_name")" = '#!/usr/bin/env bash'
"$dist_dir/$run_name" --verify
"$dist_dir/$run_name" --dry-run > "$dist_dir/native-installer-dry-run.txt"
grep -Fq 'Dry run only; no packages will be installed' \
    "$dist_dir/native-installer-dry-run.txt"
grep -Fq 'Older InfiltratorFS DKMS registrations would be removed' \
    "$dist_dir/native-installer-dry-run.txt"
grep -Fq 'Native build commands:' "$dist_dir/native-installer-dry-run.txt"
grep -Fq 'Native kernel module commands:' "$dist_dir/native-installer-dry-run.txt"
rm -f "$dist_dir/native-installer-dry-run.txt"

(
    cd "$dist_dir"
    sha256sum "$deb_name" > "$deb_name.sha256"
    sha256sum "$run_name" > "$run_name.sha256"
)

printf 'Built Linux release packages:\n  %s\n  %s\n' "$deb_name" "$run_name"
