#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:-build}"
module="${2:-kernel/infiltratorfs.ko}"
root="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d)"
image="$work/root-metadata.img"
mountpoint="$work/mnt"
src="$work/source"
loop=""

cleanup() {
    set +e
    sync
    mountpoint -q "$mountpoint" && umount "$mountpoint"
    [[ -n "$loop" ]] && losetup -d "$loop" 2>/dev/null
    rmmod infiltratorfs 2>/dev/null
    rm -rf "$work"
}
trap cleanup EXIT

for cmd in setfacl getfacl setfattr getfattr setcap getcap rsync losetup mount umount; do
    command -v "$cmd" >/dev/null
done
[[ $EUID -eq 0 ]] || { echo "must run as root" >&2; exit 2; }

mkdir -p "$mountpoint" "$src"
truncate -s 2G "$image"
"$build_dir/mkfs.infilfs" "$image" >/dev/null
loop="$(losetup --find --show "$image")"
insmod "$module"
mount -t infiltratorfs "$loop" "$mountpoint"

mkdir "$mountpoint/meta"
touch "$mountpoint/meta/owner"
chown 12345:23456 "$mountpoint/meta/owner"
chmod 0640 "$mountpoint/meta/owner"
[[ "$(stat -c '%u:%g:%a' "$mountpoint/meta/owner")" == "12345:23456:640" ]]

cp /bin/true "$mountpoint/meta/capability"
setcap cap_net_bind_service=ep "$mountpoint/meta/capability"
getcap "$mountpoint/meta/capability" | grep -Fq cap_net_bind_service

setfattr -n user.infiltratorfs -v root-metadata "$mountpoint/meta/owner"
[[ "$(getfattr --only-values -n user.infiltratorfs "$mountpoint/meta/owner")" == "root-metadata" ]]
setfattr -n trusted.infiltratorfs -v trusted-value "$mountpoint/meta/owner"
[[ "$(getfattr --only-values -n trusted.infiltratorfs "$mountpoint/meta/owner")" == "trusted-value" ]]

touch "$mountpoint/meta/setuid"
chmod 4755 "$mountpoint/meta/setuid"
[[ "$(stat -c '%a' "$mountpoint/meta/setuid")" == "4755" ]]
mkdir "$mountpoint/meta/setgid" "$mountpoint/meta/sticky"
chmod 2755 "$mountpoint/meta/setgid"
chmod 1777 "$mountpoint/meta/sticky"
[[ "$(stat -c '%a' "$mountpoint/meta/setgid")" == "2755" ]]
[[ "$(stat -c '%a' "$mountpoint/meta/sticky")" == "1777" ]]

mkfifo "$mountpoint/meta/fifo"
mknod "$mountpoint/meta/char-null" c 1 3
[[ -p "$mountpoint/meta/fifo" && -c "$mountpoint/meta/char-null" ]]
python3 - "$mountpoint/meta/socket" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX)
s.bind(sys.argv[1])
s.close()
PY
[[ -S "$mountpoint/meta/socket" ]]
rm "$mountpoint/meta/socket"

printf 'hardlink-data' > "$mountpoint/meta/hard-a"
ln "$mountpoint/meta/hard-a" "$mountpoint/meta/hard-b"
[[ "$(stat -c %i "$mountpoint/meta/hard-a")" == "$(stat -c %i "$mountpoint/meta/hard-b")" ]]
ln -s hard-a "$mountpoint/meta/symlink"
[[ "$(readlink "$mountpoint/meta/symlink")" == hard-a ]]

python3 - "$mountpoint/meta/owner" <<'PY'
import os, sys
at = 1700000000123456789
mt = 1700000000987654321
os.utime(sys.argv[1], ns=(at, mt))
st = os.stat(sys.argv[1])
assert st.st_atime_ns == at, (st.st_atime_ns, at)
assert st.st_mtime_ns == mt, (st.st_mtime_ns, mt)
PY

setfacl -m u:12345:r-- "$mountpoint/meta/owner"
getfacl -n "$mountpoint/meta/owner" | grep -Eq '^user:12345:r--$'

mkdir -p "$src/tree"
printf 'clone-data' > "$src/tree/file"
ln "$src/tree/file" "$src/tree/file-hard"
ln -s file "$src/tree/file-link"
truncate -s 16M "$src/tree/sparse"
printf x | dd of="$src/tree/sparse" bs=1 seek=$((8*1024*1024)) conv=notrunc status=none
chown 12345:23456 "$src/tree/file" "$src/tree/file-hard"
chmod 0640 "$src/tree/file"
setfacl -m u:12345:r-- "$src/tree/file"
setfattr -n user.clone -v yes "$src/tree/file"
rsync -aHAXS --numeric-ids "$src/tree/" "$mountpoint/clone/"
[[ "$(stat -c '%u:%g:%a' "$mountpoint/clone/file")" == "12345:23456:640" ]]
[[ "$(stat -c %i "$mountpoint/clone/file")" == "$(stat -c %i "$mountpoint/clone/file-hard")" ]]
[[ "$(readlink "$mountpoint/clone/file-link")" == file ]]
getfacl -n "$mountpoint/clone/file" | grep -Eq '^user:12345:r--$'
[[ "$(getfattr --only-values -n user.clone "$mountpoint/clone/file")" == yes ]]

cc -O2 -Wall -Wextra "$root/tests/native-linux-api.c" -o "$work/native-linux-api"
mkdir "$mountpoint/meta/api"
"$work/native-linux-api" "$mountpoint/meta/api"
sync
umount "$mountpoint"
mount -t infiltratorfs "$loop" "$mountpoint"

[[ "$(stat -c '%u:%g:%a' "$mountpoint/meta/owner")" == "12345:23456:640" ]]
getcap "$mountpoint/meta/capability" | grep -Fq cap_net_bind_service
[[ "$(getfattr --only-values -n trusted.infiltratorfs "$mountpoint/meta/owner")" == "trusted-value" ]]
[[ -p "$mountpoint/meta/fifo" && -c "$mountpoint/meta/char-null" ]]
[[ "$(stat -c %i "$mountpoint/meta/hard-a")" == "$(stat -c %i "$mountpoint/meta/hard-b")" ]]
[[ "$(readlink "$mountpoint/meta/symlink")" == hard-a ]]
getfacl -n "$mountpoint/meta/owner" | grep -Eq '^user:12345:r--$'
[[ "$(cat "$mountpoint/meta/api/linked-tmpfile")" == anonymous-data ]]
[[ "$(cat "$mountpoint/meta/api/exchange-a")" == bravo ]]
sync
umount "$mountpoint"

"$build_dir/infilfs-scrub" "$loop" | tee "$work/scrub.txt"
grep -Fq 'Result:              CLEAN' "$work/scrub.txt"
echo 'Native Linux full root metadata qualification: PASS'
