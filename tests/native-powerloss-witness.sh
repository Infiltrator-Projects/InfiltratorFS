#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Independent durable witness for a bounded physical power-interruption exercise.
# Never formats, unmounts, reboots or removes existing user data.
set -euo pipefail
export LC_ALL=C
die() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
[[ $# == 4 ]] || die 'usage: bash tests/native-powerloss-witness.sh write|verify NATIVE_MOUNT WITNESS_DIRECTORY SESSION'
mode=$1
target=$(realpath -e -- "$2")
witness=$(realpath -e -- "$3")
session=$4
[[ $mode == write || $mode == verify ]] || die 'mode must be write or verify'
[[ $session =~ ^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$ ]] || die 'invalid session'
[[ -d $target && -d $witness && $target != / ]] || die 'dedicated target and witness directories required'
[[ $(findmnt -rn -M "$target" -o FSTYPE) == infiltratorfs ]] || die 'target must be an exact native InfiltratorFS mount'
target_dev=$(findmnt -rn -M "$target" -o MAJ:MIN)
witness_dev=$(findmnt -rn -T "$witness" -o MAJ:MIN)
root_dev=$(findmnt -rn -M / -o MAJ:MIN)
[[ -n $target_dev && -n $witness_dev && $target_dev != "$root_dev" && $target_dev != "$witness_dev" ]] || die 'separate non-root native target and witness device required'
physical_parent() {
    local dev type parent
    dev=$(readlink -f -- "/dev/block/$1")
    [[ -b $dev ]] || return 1
    type=$(lsblk -ndo TYPE "$dev")
    case $type in
        disk) printf '%s\n' "$dev" ;;
        part)
            parent=$(lsblk -ndo PKNAME "$dev")
            [[ -n $parent && $(lsblk -ndo TYPE "/dev/$parent") == disk ]] || return 1
            printf '/dev/%s\n' "$parent" ;;
        *) return 1 ;;
    esac
}
target_disk=$(physical_parent "$target_dev") || die 'target is not a direct disk or partition'
witness_disk=$(physical_parent "$witness_dev") || die 'witness is not a direct disk or partition'
[[ $target_disk != "$witness_disk" ]] || die 'witness must be on a different disk'
data="$target/.infilfs-powerloss-$session"
ledger="$witness/infilfs-powerloss-$session"
boot=$(cat /proc/sys/kernel/random/boot_id)
if [[ $mode == write ]]; then
    [[ ! -e $data && ! -L $data && ! -e $ledger && ! -L $ledger ]] || die 'session already exists'
    source=$(git -C "$(dirname "$0")/.." rev-parse HEAD)
    mkdir -- "$data" "$ledger"
    printf '%s\n' "$boot" > "$ledger/boot-id"
    printf '%s\n' "$(uname -r)" > "$ledger/kernel"
    printf '%s\n' "$source" > "$ledger/source"
    sync -f "$ledger"
    sync -f "$data"
    input=$(mktemp /dev/shm/infilfs-witness.XXXXXX)
    trap 'rm -f -- "$input"' EXIT
    printf 'Writing 4 MiB batches, at most 256 (1 GiB); only synced payloads enter the independent witness.\n'
    for ((i=1; i<=256; i++)); do
        name=$(printf 'payload-%04d.bin' "$i")
        dd if=/dev/urandom of="$input" bs=1M count=4 status=none
        hash=$(sha256sum -- "$input" | cut -d' ' -f1)
        # Generate and hash bytes independently of the filesystem under test.
        cp -- "$input" "$data/$name"
        # syncfs covers payload and directory metadata before witness publication.
        sync -f "$data"
        printf '%s  %s\n' "$hash" "$name" > "$ledger/.pending"
        mv -- "$ledger/.pending" "$ledger/$name.sha256"
        sync -f "$ledger"
        printf 'Witness durable: %s\n' "$name"
    done
    printf 'Writer completed normally; this alone is NOT power-loss qualification.\n'
else
    [[ -d $data && ! -L $data && -d $ledger && ! -L $ledger ]] || die 'session directories missing'
    [[ $(cat "$ledger/boot-id") != "$boot" ]] || die 'verification requires a different boot instance'
    shopt -s nullglob
    expected=1
    for record in "$ledger"/payload-*.bin.sha256; do
        name=$(printf 'payload-%04d.bin' "$expected")
        [[ $(basename "$record") == "$name.sha256" ]] || die 'non-contiguous witness sequence'
        [[ -f $record && ! -L $record ]] || die 'invalid witness record'
        read -r hash recorded extra < "$record"
        [[ $hash =~ ^[0-9a-f]{64}$ && $recorded == "$name" && -z $extra ]] || die 'malformed witness'
        [[ -f $data/$name && ! -L $data/$name && $(stat -c %s "$data/$name") == 4194304 ]] || die "missing or truncated durable file: $name"
        actual=$(sha256sum -- "$data/$name" | cut -d' ' -f1)
        [[ $actual == "$hash" ]] || die "durable data mismatch: $name"
        expected=$((expected+1))
    done
    ((expected > 1)) || die 'no committed witness records'
    printf 'PASS: %d independently witnessed durable payloads survived a different boot.\n' "$((expected-1))"
    printf 'Recorded source: %s\n' "$(cat "$ledger/source")"
    printf 'Still required: documented abrupt power interruption, offline structural check and deep scrub, repeated failure points and hardware identity.\n'
fi
