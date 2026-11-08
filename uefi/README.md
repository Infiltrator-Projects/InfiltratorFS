# Experimental native InfiltratorFS UEFI EFI filesystem driver

**Owner: InfiltratorFS.** This is the recovered GNU-EFI / FSW adapter from Boot and subsequently Filesystems, brought back under the only authoritative owner of the InfiltratorFS format. The driver is not a separate filesystem parser or plugin owned by Filesystems. It uses the *current* `include/infilfs/` and `src/` Format 0.19 portable reader from this repository at build time.

Historical basis: Filesystems commit `82c6dfbc919a9d42df82db745705f9993aee63d0` at `firmware/uefi/filesystems/`, with third-party licence text retained under `uefi/filesystems/`. An InfiltratorFS binary was once published from Boot v0.1.18, but hardware tests did **not** qualify it for normal use. That historical source depended on **Format 0.18**; this adapter now compiles against the repo's Format 0.19 sources instead of an obsolete vendored implementation.

## Demonstrated integration milestone — 8 November 2026

The [three-repository GitHub Actions qualification](https://github.com/Infiltrator-Projects/Kernel/actions/runs/42431603931) passed using this source revision (`c8f6431f2de34806d8614a241cab8db5a0daad48`), Boot `0.1.39` and the real InfiltratorOS `0.1.0-infiltrator` kernel. Under QEMU/OVMF, Boot loaded the kernel **and initramfs from an InfiltratorFS Format 0.19 volume**, and the resulting kernel loaded a matching native `infiltratorfs.ko`, mounted the same volume and performed read/write operations. The ESP contained Boot, its configuration and this EFI filesystem driver, **not** the kernel or initramfs.

This is an **end-to-end VM milestone**, not a physical-machine, Secure Boot, power-loss or full desktop installation qualification. In particular, the kernel-side reader and the UEFI reader are separate adapters with separate acceptance tests; the filesystem format remains owned here. The Kernel repository owns the pinned integration harness and its real execution logs. This repository independently tests the UEFI reader when its source changes on `main`.

To build the experimental standalone read-only `InfiltratorFS_x64.efi` without changing firmware, partitions or Boot settings:

```sh
git submodule update --init --recursive
sudo apt-get install -y build-essential gnu-efi
make -C uefi/filesystems -f Make.gnuefi ARCH=x86_64
```

Required acceptance before activation or publishing is independent PE/relocation verification, real Format 0.19 filesystem recognition, directory traversal, and complete kernel and initramfs reads with SHA-256 checking under OVMF. Include error propagation/oversize and wrong-version probes; verify inline, sparse, compressed and ordinary files, and make sure no write service is implemented. Only after these work should the driver be explicitly deployed beside OFS and tested on the physical Dell, leaving the normal Linux entry and GRUB recovery path untouched. See [issue #45](https://github.com/Infiltrator-Projects/InfiltratorFS/issues/45).

**The native driver has passed an end-to-end VM boot trial, but physical firmware compatibility and supported installation remain unqualified.** A green module compile alone is never sufficient to claim firmware or root-volume acceptance.
