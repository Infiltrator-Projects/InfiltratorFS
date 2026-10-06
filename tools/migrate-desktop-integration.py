#!/usr/bin/env python3
from pathlib import Path
import re


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


# ---------------------------------------------------------------------------
# packaging/build-linux-packages.sh
# ---------------------------------------------------------------------------
p = Path("packaging/build-linux-packages.sh")
s = p.read_text()

s = replace_once(
    s,
    'desktop_bundle_dir="${INFILTRATORFS_DESKTOP_BUNDLE_DIR:-}"\n',
    'integration_bundle="${INFILTRATORFS_OS_INTEGRATION_BUNDLE_DIR:-}"\n'
    'require_integration="${INFILTRATORFS_REQUIRE_OS_INTEGRATION:-0}"\n'
    'integration_enabled=0\n',
    "build-linux bundle variable",
)

s = replace_once(
    s,
    'case "$emit_run" in\n    0|1) ;;\n    *) echo "INFILTRATORFS_EMIT_RUN must be 0 or 1." >&2; exit 1 ;;\nesac\n'
    '[[ -f src/infiltratr-common/CMakeLists.txt ]] || {',
    'case "$emit_run" in\n    0|1) ;;\n    *) echo "INFILTRATORFS_EMIT_RUN must be 0 or 1." >&2; exit 1 ;;\nesac\n'
    'case "$require_integration" in\n    0|1) ;;\n    '    '* echo "INFILTRATORFS_REQUIRE_OS_INTEGRATION must be 0 or 1." >&2; exit 1 ;;\nesac\n'
    'if [[ -n "$integration_bundle" ]]; then\n'
    '    for required in gnome-disks libbd_fs.so manifest; do\n'
    '        [[ -s "$integration_bundle/$required" ]] || {\n'
    '            echo "Desktop integration bundle is missing $required." >&2\n'
    '            exit 1\n'
    '        }\n'
    '    done\n'
    '    integration_enabled=1\n'
    'elif [[ "$require_integration" = 1 ]]; then\n'
    '    echo "This package build requires the Ubuntu/Mint desktop integration bundle." >&2\n'
    '    exit 1\n'
    'fi\n'
    '[[ -f src/infiltratr-common/CMakeLists.txt ]] || {',
    "build-linux bundle validation",
)

s = replace_once(
    s,
    'install -m 0644 README.md "$package_root/usr/share/doc/infiltratorfs/README.md"\n'
    '# DKMS source must be self-contained.',
    'install -m 0644 README.md "$package_root/usr/share/doc/infiltratorfs/README.md"\n'
    'if [[ "$integration_enabled" = 1 ]]; then\n'
    '    install -d "$package_root/usr/lib/infiltratorfs/os-integration"\n'
    '    install -m 0755 "$integration_bundle/gnome-disks" "$package_root/usr/lib/infiltratorfs/os-integration/gnome-disks"\n'
    '    install -m 0644 "$integration_bundle/libbd_fs.so" "$package_root/usr/lib/infiltratorfs/os-integration/libbd_fs.so"\n'
    '    install -m 0644 "$integration_bundle/manifest" "$package_root/usr/lib/infiltratorfs/os-integration/manifest"\n'
    'fi\n'
    '# DKMS source must be self-contained.',
    "build-linux bundle install",
)

s = replace_once(
    s,
    'desktop_depends=", udisks2, infiltratorfs-desktop-integration (>= 1.0.0+ubuntu24.04.2)"\n'
    'desktop_identity="managed-packages"\n',
    'desktop_depends=""\n'
    'desktop_recommends=", udisks2"\n'
    'desktop_identity="core-only"\n'
    'if [[ "$integration_enabled" = 1 ]]; then\n'
    '    desktop_depends=", udisks2, gnome-disk-utility (>= 46~), gnome-disk-utility (<< 47~), libblockdev-fs3 (>= 3.1~), libblockdev-fs3 (<< 3.2~)"\n'
    '    desktop_recommends=""\n'
    '    desktop_identity="ubuntu24.04-mint22-bundled"\n'
    'fi\n',
    "build-linux package dependencies",
)

s = replace_once(
    s,
    'Depends: dkms, initramfs-tools, kmod, policykit-1, util-linux, xdg-utils, fontconfig, libssl3t64 | libssl3, libgtk-3-0t64 | libgtk-3-0, libglib2.0-0t64 | libglib2.0-0${desktop_depends}\n'
    'Recommends: linux-headers-generic, udev\n',
    'Depends: dkms, initramfs-tools, kmod, policykit-1, util-linux, xdg-utils, fontconfig, libssl3t64 | libssl3, libgtk-3-0t64 | libgtk-3-0, libglib2.0-0t64 | libglib2.0-0${desktop_depends}\n'
    'Conflicts: infiltratorfs-desktop-integration, infiltratorfs-gnome-disk-utility, infiltratorfs-libblockdev-fs3\n'
    'Replaces: infiltratorfs-desktop-integration, infiltratorfs-gnome-disk-utility, infiltratorfs-libblockdev-fs3\n'
    'Recommends: linux-headers-generic, udev${desktop_recommends}\n',
    "build-linux migration metadata",
)

start = s.index('test "$(dpkg-deb --field "$dist_dir/$deb_name" X-InfiltratorFS-Desktop-Integration)" = managed-packages')
end = s.index("if grep -q 'usr/bin/infilfs-fuse$'", start)
s = s[:start] + '''test "$(dpkg-deb --field "$dist_dir/$deb_name" X-InfiltratorFS-Desktop-Integration)" = "$desktop_identity"
if [[ "$integration_enabled" = 1 ]]; then
    for required in gnome-disks libbd_fs.so manifest; do
        grep -q "usr/lib/infiltratorfs/os-integration/${required}$" "$contents"
    done
else
    if grep -Eq 'usr/lib/infiltratorfs/os-integration/(gnome-disks|libbd_fs\\.so|manifest)$' "$contents"; then
        echo 'Core-only InfiltratorFS package unexpectedly contains a desktop integration bundle.' >&2
        exit 1
    fi
fi
''' + s[end:]

s = replace_once(
    s,
    '''grep -Eq '(^|, )infiltratorfs-desktop-integration([ ,]|$)' <<<"$depends" || {
    echo 'Core package must require the managed desktop integration package.' >&2
    exit 1
}
''',
    '''if [[ "$integration_enabled" = 1 ]]; then
    for dependency in udisks2 gnome-disk-utility libblockdev-fs3; do
        grep -Eq "(^|, )${dependency}([ ,|]|$)" <<<"$depends" || {
            echo "Integrated package must depend on distro-owned ${dependency}." >&2
            exit 1
        }
    done
fi
''',
    "build-linux managed dependency guard",
)

s = replace_once(
    s,
    "grep -Fq 'module loading is administratively disabled' <<<\"$postinst_text\"\n",
    "grep -Fq 'module loading is administratively disabled' <<<\"$postinst_text\"\n"
    "grep -Fq 'infiltratorfs-os-integration install' <<<\"$postinst_text\"\n",
    "build-linux postinst integration guard",
)
s = replace_once(
    s,
    "grep -Fq 'refusing to remove the filesystem package while / is mounted as InfiltratorFS' <<<\"$prerm_text\"\n",
    "grep -Fq 'refusing to remove the filesystem package while / is mounted as InfiltratorFS' <<<\"$prerm_text\"\n"
    "grep -Fq 'infiltratorfs-os-integration remove' <<<\"$prerm_text\"\n",
    "build-linux prerm integration guard",
)

start = s.index('bundle_work=""\nbundle_payload=""')
end = s.index('tar_args=(', start)
s = s[:start] + '''bundle_work=""
bundle_payload=""
if [[ -n "$integration_bundle" ]]; then
    [[ -d "$integration_bundle" ]] || {
        echo "Desktop integration bundle directory does not exist: $integration_bundle" >&2
        exit 1
    }
    bundle_work="$(mktemp -d)"
    bundle_payload="$bundle_work/infiltratorfs-os-integration-bundle.tar"
    for required in gnome-disks libbd_fs.so manifest; do
        [[ -s "$integration_bundle/$required" ]] || {
            echo "Desktop integration bundle is incomplete: $required" >&2
            exit 1
        }
    done
    tar -C "$integration_bundle" -cf "$bundle_payload" gnome-disks libbd_fs.so manifest
fi

''' + s[end:]

pattern = re.compile(
    r'''    if \[\[ -f "\$verify_root/infiltratorfs-desktop-integration-bundle\.tar" \]\]; then\n.*?    fi\n    grep -Fq 'bash "\$ROOT/packaging/build-linux-packages\.sh"' ''',
    re.S,
)
replacement = '''    if [[ -f "$verify_root/infiltratorfs-os-integration-bundle.tar" ]]; then
        bundle_verify="$verify_root/.desktop-bundle-verify"
        mkdir -p "$bundle_verify"
        tar -xf "$verify_root/infiltratorfs-os-integration-bundle.tar" -C "$bundle_verify"
        for required in gnome-disks libbd_fs.so manifest; do
            test -s "$bundle_verify/$required"
        done
    fi
    grep -Fq 'bash "$ROOT/packaging/build-linux-packages.sh"' '''
s, count = pattern.subn(replacement, s, count=1)
if count != 1:
    raise SystemExit(f"build-linux installer bundle verifier: expected 1 match, found {count}")

s = replace_once(
    s,
    'packaging/build-linux-packages.sh packaging/infiltratorfs-os-integration \\\n',
    'packaging/build-linux-packages.sh packaging/infiltratorfs-os-integration \\\n        packaging/build-noble-desktop-integration.sh \\\n',
    "build-linux installer required files",
)
p.write_text(s)

# ---------------------------------------------------------------------------
# support/installer/bootstrap.sh
# ---------------------------------------------------------------------------
p = Path("support/installer/bootstrap.sh")
s = p.read_text()
s = replace_once(
    s,
    'DESKTOP_BUNDLE="$ROOT/infiltratorfs-desktop-integration-bundle.tar"',
    'DESKTOP_BUNDLE="$ROOT/infiltratorfs-os-integration-bundle.tar"',
    "bootstrap bundle name",
)

pattern = re.compile(r'prepare_desktop_bundle\(\) \{.*?\n\}\n\ndesktop_bundle_host_supported\(\)', re.S)
replacement = '''prepare_desktop_bundle() {
    local destination="$1" required
    [[ -f "$DESKTOP_BUNDLE" ]] || return 1
    rm -rf "$destination"
    mkdir -p "$destination"
    tar -xf "$DESKTOP_BUNDLE" -C "$destination"
    for required in gnome-disks libbd_fs.so manifest; do
        [[ -s "$destination/$required" ]] || {
            echo "Bundled desktop integration is incomplete: $required" >&2
            return 1
        }
    done
    [[ "$(sed -n 's/^target=//p' "$destination/manifest")" = ubuntu-24.04-linuxmint-22.x ]] || {
        echo 'Bundled desktop integration target is invalid.' >&2
        return 1
    }
}

desktop_bundle_host_supported()'''
s, count = pattern.subn(replacement, s, count=1)
if count != 1:
    raise SystemExit(f"bootstrap prepare bundle: expected 1 match, found {count}")

pattern = re.compile(r'verify_desktop_integration\(\) \{.*?\n\}\n\nprint_package_commands\(\)', re.S)
replacement = '''verify_desktop_integration() {
    local owner
    [[ -x /usr/lib/infiltratorfs/infiltratorfs-os-integration ]] || {
        echo 'InfiltratorFS desktop integration helper is not installed.' >&2
        return 1
    }
    /usr/lib/infiltratorfs/infiltratorfs-os-integration verify
    owner="$(dpkg-query -S /usr/bin/gnome-disks 2>/dev/null | head -n1 | cut -d: -f1 || true)"
    [[ "$owner" = gnome-disk-utility ]] || {
        echo "GNOME Disks must remain owned by the distro package; found ${owner:-unknown}." >&2
        return 1
    }
}

print_package_commands()'''
s, count = pattern.subn(replacement, s, count=1)
if count != 1:
    raise SystemExit(f"bootstrap verify integration: expected 1 match, found {count}")

s = replace_once(
    s,
    "        printf 'The release installer contains the ABI-matched GNOME Disks/libblockdev integration bundle.\\n'\n"
    "    else\n"
    "        printf 'Desktop integration will be resolved through APT as a required package.\\n'\n",
    "        printf 'The release installer contains the ABI-matched in-place GNOME Disks/libblockdev integration bundle.\\n'\n"
    "    else\n"
    "        printf 'No desktop integration bundle is present; core filesystem support will still install.\\n'\n",
    "bootstrap dry-run description",
)

old_build = '''rm -rf "$PACKAGE_DIR"
INFILTRATORFS_PACKAGE_VERSION="$NATIVE_PACKAGE_VERSION" \\
INFILTRATORFS_BUILD_IDENTITY=native-local \\
INFILTRATORFS_EMIT_RUN=0 \\
    bash "$ROOT/packaging/build-linux-packages.sh" "$BUILD_DIR" "$PACKAGE_DIR"
'''
new_build = '''desktop_dir="$BUILD_DIR/desktop-integration"
integration_enabled=0
if [[ -f "$DESKTOP_BUNDLE" ]]; then
    desktop_bundle_host_supported || {
        echo 'The bundled desktop integration is qualified only for Ubuntu 24.04 and Linux Mint 22.x.' >&2
        exit 1
    }
    prepare_desktop_bundle "$desktop_dir"
    integration_enabled=1
fi

rm -rf "$PACKAGE_DIR"
if [[ "$integration_enabled" = 1 ]]; then
    INFILTRATORFS_PACKAGE_VERSION="$NATIVE_PACKAGE_VERSION" \\
    INFILTRATORFS_BUILD_IDENTITY=native-local \\
    INFILTRATORFS_EMIT_RUN=0 \\
    INFILTRATORFS_OS_INTEGRATION_BUNDLE_DIR="$desktop_dir" \\
    INFILTRATORFS_REQUIRE_OS_INTEGRATION=1 \\
        bash "$ROOT/packaging/build-linux-packages.sh" "$BUILD_DIR" "$PACKAGE_DIR"
else
    INFILTRATORFS_PACKAGE_VERSION="$NATIVE_PACKAGE_VERSION" \\
    INFILTRATORFS_BUILD_IDENTITY=native-local \\
    INFILTRATORFS_EMIT_RUN=0 \\
        bash "$ROOT/packaging/build-linux-packages.sh" "$BUILD_DIR" "$PACKAGE_DIR"
fi
'''
s = replace_once(s, old_build, new_build, "bootstrap package build")

pattern = re.compile(
    r'desktop_dir="\$BUILD_DIR/desktop-integration"\ndeclare -a desktop_debs=\(\).*?\n\)\nrefresh_desktop_storage\nverify_desktop_integration',
    re.S,
)
replacement = '''(
    cd "$PACKAGE_DIR"
    run_as_root apt-get install -y "./$(basename "$PACKAGE")"
)
refresh_desktop_storage
if [[ "$integration_enabled" = 1 ]]; then
    verify_desktop_integration
fi'''
s, count = pattern.subn(replacement, s, count=1)
if count != 1:
    raise SystemExit(f"bootstrap obsolete package install block: expected 1 match, found {count}")

s = s.replace(
    'Desktop integration: managed GNOME Disks / libblockdev packages verified',
    'Desktop integration: single-package in-place GNOME Disks / libblockdev integration',
)
p.write_text(s)

# ---------------------------------------------------------------------------
# tests/desktop-integration.sh: keep current functional probes but replace the
# obsolete package-ownership contract with the single-package contract.
# ---------------------------------------------------------------------------
p = Path("tests/desktop-integration.sh")
s = p.read_text()
s = s.replace(
    'noble_bundle_builder="$repo_root/packaging/build-noble-desktop-packages.sh"',
    'noble_bundle_builder="$repo_root/packaging/build-noble-desktop-integration.sh"',
)
start = s.index('# Public Ubuntu 24.04 / Linux Mint 22.x desktop integration is delivered')
end = s.index('# Mintstick/Nemo\'s USB Stick Formatter is intentionally NOT an InfiltratorFS', start)
block = '''# Public Ubuntu 24.04 / Linux Mint 22.x integration remains inside the one
# InfiltratorFS package.  The distro-owned GNOME Disks and libblockdev packages
# stay installed; only their exact ABI-matched binaries are diverted while
# InfiltratorFS is installed, and uninstall restores the originals.
for integration_file in "$os_helper" "$mint_guard" "$noble_libblockdev_patch" \\
                        "$noble_gnome_patch" "$noble_bundle_builder"; do
    test -s "$integration_file"
done
test ! -e "$repo_root/packaging/build-noble-desktop-packages.sh"
bash -n "$os_helper"
bash -n "$noble_bundle_builder"
git apply --numstat "$noble_libblockdev_patch" >/dev/null
git apply --numstat "$noble_gnome_patch" >/dev/null
python3 - "$mint_guard" <<'PY'
import ast
import pathlib
import sys
ast.parse(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
PY
grep -Fq 'dpkg-divert --package "$OWNER" --add --rename' "$os_helper"
grep -Fq 'dpkg-divert --package "$OWNER" --remove --rename' "$os_helper"
grep -Fq 'restart_udisks' "$os_helper"
grep -Fq 'require_udisks_formatter' "$os_helper"
grep -Fq 'org.freedesktop.UDisks2.Manager.CanFormat infiltratorfs' "$os_helper"
grep -Fq 'libblockdev-fs3' "$os_helper"
grep -Fq 'InfiltratorFS formatter service is unavailable' "$noble_gnome_patch"
grep -Fq 'src/disks/gducreateotherpage.c' "$noble_gnome_patch"
grep -Fq '{"infiltratorfs", N_("InfiltratorFS' "$noble_gnome_patch"
grep -Fq 'src/disks/gduwindow.c' "$noble_gnome_patch"
grep -Fq 'src/disks/gduvolumegrid.c' "$noble_gnome_patch"
grep -Fq 'InfiltratorFS (format %s)' "$noble_gnome_patch"
grep -Fq 'BD_FS_TECH_INFILTRATORFS' "$noble_libblockdev_patch"

# The core package may carry migration Conflicts/Replaces for the three retired
# package names, but it must never depend on, build, bundle, install or publish
# those packages again.
grep -Fq 'INFILTRATORFS_OS_INTEGRATION_BUNDLE_DIR' "$repo_root/packaging/build-linux-packages.sh"
grep -Fq 'ubuntu24.04-mint22-bundled' "$repo_root/packaging/build-linux-packages.sh"
grep -Fq 'gnome-disk-utility (>= 46~)' "$repo_root/packaging/build-linux-packages.sh"
grep -Fq 'libblockdev-fs3 (>= 3.1~)' "$repo_root/packaging/build-linux-packages.sh"
grep -Fq 'infiltratorfs-os-integration-bundle.tar' "$repo_root/packaging/build-linux-packages.sh"
if grep -Fq 'INFILTRATORFS_DESKTOP_BUNDLE_DIR' "$repo_root/packaging/build-linux-packages.sh"; then
    echo 'desktop-integration: obsolete managed-package bundle variable returned' >&2
    exit 1
fi
for retired_pattern in \\
    'infiltratorfs-libblockdev-fs3_*.deb' \\
    'infiltratorfs-gnome-disk-utility_*.deb' \\
    'infiltratorfs-desktop-integration_*.deb'; do
    if grep -Fq "$retired_pattern" "$repo_root/packaging/build-linux-packages.sh" || \\
       grep -Fq "$retired_pattern" "$bootstrap"; then
        echo "desktop-integration: retired replacement package path returned: $retired_pattern" >&2
        exit 1
    fi
done

'''
s = s[:start] + block + s[end:]
p.write_text(s)

# ---------------------------------------------------------------------------
# tests/ci-duration-policy.py: release artifact policy follows the raw bundle.
# ---------------------------------------------------------------------------
p = Path("tests/ci-duration-policy.py")
s = p.read_text()
s = s.replace(
    "    'infiltratorfs-desktop-integration.manifest',\n"
    "    'infiltratorfs-desktop-integration-bundle.tar',\n"
    "    'managed-packages',\n",
    "    'infiltratorfs-os-integration-bundle.tar',\n"
    "    'ubuntu24.04-mint22-bundled',\n"
    "    'INFILTRATORFS_OS_INTEGRATION_BUNDLE_DIR',\n",
)
s = s.replace(
    "    'INFILTRATORFS_OS_INTEGRATION_BUNDLE_DIR',\n"
    "    'INFILTRATORFS_REQUIRE_OS_INTEGRATION',\n",
    "    'INFILTRATORFS_DESKTOP_BUNDLE_DIR',\n"
    "    'managed-packages',\n",
)
p.write_text(s)

print('desktop integration migration staged')
