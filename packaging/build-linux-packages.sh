#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

build_dir="${1:-build}"
dist_dir="${2:-dist}"

version="$(sed -n 's/^project(InfiltratorFS VERSION \([^ ]*\) LANGUAGES C)$/\1/p' CMakeLists.txt)"
if [[ -z "$version" ]]; then
    echo "Could not determine InfiltratorFS version from CMakeLists.txt" >&2
    exit 1
fi

mkdir -p "$dist_dir"
architecture="$(dpkg --print-architecture 2>/dev/null || uname -m)"
deb_name="infiltratorfs_${version}_${architecture}.deb"
package_root="$(mktemp -d)"
payload="$(mktemp)"
trap 'rm -rf "$package_root" "$payload"' EXIT

install -d "$package_root/DEBIAN" "$package_root/usr/share/doc/infiltratorfs"
cmake --install "$build_dir" --prefix "$package_root/usr"
install -m 0644 LICENSE "$package_root/usr/share/doc/infiltratorfs/copyright"
install -m 0644 README.md "$package_root/usr/share/doc/infiltratorfs/README.md"
installed_size="$(du -sk "$package_root/usr" | cut -f1)"

cat > "$package_root/DEBIAN/control" <<EOF
Package: infiltratorfs
Version: ${version}
Section: utils
Priority: optional
Architecture: ${architecture}
Maintainer: The First Infiltrator
Depends: fuse3, libfuse3-3, policykit-1, util-linux, xdg-utils, zenity
Installed-Size: ${installed_size}
Homepage: https://github.com/The-First-Infiltrator/InfiltratorFS
Description: platform-neutral experimental filesystem tools
 InfiltratorFS formatter, inspector, scrubber, direct-image utility,
 Linux FUSE adapter and Linux Mint desktop manager.
 Use only with image files or disposable or backed-up media.
EOF

dpkg-deb --root-owner-group --build "$package_root" "$dist_dir/$deb_name"
dpkg-deb --contents "$dist_dir/$deb_name" > "$dist_dir/package-contents.txt"
grep -q 'usr/bin/infiltratorfs-manager$' "$dist_dir/package-contents.txt"
grep -q 'usr/lib/infiltratorfs/infiltratorfs-manager-helper$' "$dist_dir/package-contents.txt"
grep -q 'usr/share/applications/infiltratorfs-manager.desktop$' "$dist_dir/package-contents.txt"
test "$(dpkg-deb --field "$dist_dir/$deb_name" Version)" = "$version"
rm "$dist_dir/package-contents.txt"
sha256sum "$dist_dir/$deb_name" > "$dist_dir/$deb_name.sha256"

# The .run package is a genuine self-extracting Bash installer. Its payload is
# the complete checked-out release tree, including the pinned Common submodule,
# so the destination machine performs the actual native Linux compilation.
tar \
    --exclude='./.git' \
    --exclude='*/.git' \
    --exclude='./build' \
    --exclude='./dist' \
    -czf "$payload" .

run_name="infiltratorfs-${version}-linux-native.run"
cat > "$dist_dir/$run_name" <<'RUN_HEADER'
#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

self="$0"
marker='__INFILTRATORFS_PAYLOAD_BELOW__'
payload_start="$(awk -v marker="$marker" '$0 == marker { print NR + 1; exit }' "$self")"
if [[ -z "$payload_start" ]]; then
    echo "Installer payload marker not found." >&2
    exit 1
fi

verify_installer() {
    local header_tmp
    if [[ "$(head -n 1 "$self")" != '#!/usr/bin/env bash' ]]; then
        echo "Installer does not start with the expected Bash shebang." >&2
        return 1
    fi
    header_tmp="$(mktemp)"
    head -n "$((payload_start - 1))" "$self" > "$header_tmp"
    bash -n "$header_tmp"
    rm -f "$header_tmp"
    tail -n +"$payload_start" "$self" | gzip -t
}

verify_installer
if [[ "${1:-}" == "--verify" ]]; then
    echo "InfiltratorFS native Linux installer verified: Bash header and embedded source payload are intact."
    exit 0
fi

for command in cmake cc pkg-config tar gzip pkexec zenity; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Missing required command: $command" >&2
        echo "Install the Linux build/runtime dependencies, or use the .deb package." >&2
        exit 1
    fi
done
if ! pkg-config --exists fuse3; then
    echo "libfuse3 development files are required for the native Linux build." >&2
    echo "On Linux Mint/Ubuntu install libfuse3-dev and fuse3, or use the .deb package." >&2
    exit 1
fi

work="$(mktemp -d)"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT
mkdir -p "$work/source"
tail -n +"$payload_start" "$self" | gzip -dc | tar -xf - -C "$work/source"
cmake -S "$work/source" -B "$work/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$work/build" --parallel
ctest --test-dir "$work/build" --output-on-failure
cmake_path="$(command -v cmake)"
pkexec "$cmake_path" --install "$work/build" --prefix /usr
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$HOME/.local/share/applications" >/dev/null 2>&1 || true
fi
printf '\nInfiltratorFS installed successfully. Launch "InfiltratorFS Manager" from the application menu.\n'
exit 0
__INFILTRATORFS_PAYLOAD_BELOW__
RUN_HEADER
cat "$payload" >> "$dist_dir/$run_name"
chmod 0755 "$dist_dir/$run_name"

# Release construction must prove that the .run is actually a valid shell
# installer and that the embedded native-build source payload survived intact.
test "$(head -n 1 "$dist_dir/$run_name")" = '#!/usr/bin/env bash'
"$dist_dir/$run_name" --verify
sha256sum "$dist_dir/$run_name" > "$dist_dir/$run_name.sha256"

printf 'Built Linux release packages:\n  %s\n  %s\n' "$deb_name" "$run_name"
