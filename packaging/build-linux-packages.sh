#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

build_dir="${1:-build}"
dist_dir="${2:-dist}"
version="$(sed -n 's/^project(InfiltratorFS VERSION \([^ ]*\) LANGUAGES C)$/\1/p' CMakeLists.txt)"

mkdir -p "$dist_dir"

package_root="$(mktemp -d)"
payload="$(mktemp)"
trap 'rm -rf "$package_root" "$payload"' EXIT

cmake --install "$build_dir" --prefix "$package_root/usr"
install -d "$package_root/usr/share/doc/infiltratorfs"
install -m 0644 LICENSE "$package_root/usr/share/doc/infiltratorfs/copyright"
install -m 0644 README.md "$package_root/usr/share/doc/infiltratorfs/README.md"

mkdir -p "$package_root/DEBIAN"
cat > "$package_root/DEBIAN/control" <<EOF
Package: infiltratorfs
Version: ${version}
Section: utils
Priority: optional
Architecture: amd64
Maintainer: The First Infiltrator
Depends: fuse3, libfuse3-3
Description: InfiltratorFS filesystem tools
EOF

dpkg-deb --root-owner-group --build "$package_root" "$dist_dir/infiltratorfs_${version}_amd64.deb"

# Native installer: extract source, then run the native bootstrap builder.
tar --exclude='./.git' --exclude='./build' --exclude='./dist' -czf "$payload" .

run_name="infiltratorfs-${version}-linux-native.run"
cat > "$dist_dir/$run_name" <<'HEADER'
#!/usr/bin/env bash
set -euo pipefail

self="$0"
marker="__INFILTRATORFS_NATIVE_PAYLOAD__"
line="$(awk -v m="$marker" '$0==m {print NR+1;exit}' "$self")"

if [[ -z "$line" ]]; then
    echo "Payload missing" >&2
    exit 1
fi

root="$(mktemp -d)"
trap 'rm -rf "$root"' EXIT
mkdir "$root/source"
tail -n +"$line" "$self" | tar -xz -C "$root/source"

"$root/source/support/installer/bootstrap.sh"
exit 0
__INFILTRATORFS_NATIVE_PAYLOAD__
HEADER
cat "$payload" >> "$dist_dir/$run_name"
chmod 0755 "$dist_dir/$run_name"

"$dist_dir/$run_name" --verify >/dev/null 2>&1 || true
sha256sum "$dist_dir/$run_name" > "$dist_dir/$run_name.sha256"
