# Experimental native InfiltratorFS UEFI EFI filesystem driver

**Owner: InfiltratorFS.** This is the recovered GNU-EFI / FSW adapter from Boot and subsequently Filesystems, brought back under the only authoritative owner of the InfiltratorFS format. The driver is not a separate filesystem parser or plugin owned by Filesystems. It uses the *current* `include/infilfs/` and `src/` Format 0.19 portable reader from this repository at build time.

Historical basis: Filesystems commit `82c6dfbc919a9d42df82db745705f9993aee63d0` at `firmware/uefi/filesystems/`, with third-party licence text retained under `uefi/filesystems/`. An InfiltratorFS binary was once published from Boot v0.1.18, but hardware tests did **not** qualify it for normal use. That historical source depended on **Format 0.18**; this adapter now compiles against the repo's Format 0.19 sources instead of an obsolete vendored implementation.

To build the experimental standalone read-only `InfiltratorFS_x64.efi` without changing firmware, partitions or Boot settings:

```sh
git submodule update --init --recursive
sudo apt-get install -y build-essential gnu-efi
make -C uefi/filesystems -f Make.gnuefi ARCH=x86_64
```

Required acceptance before activation or publishing is independent PE/relocation verification, real Format 0.19 filesystem recognition, directory traversal, and complete kernel and initramfs reads with SHA-256 checking under OVMF. Include error propagation/oversize and wrong-version probes; verify inline, sparse, compressed and ordinary files, and make sure no write service is implemented. Only after these work should the driver be explicitly deployed beside OFS and tested on the physical Dell, leaving the normal Linux entry and GRUB recovery path untouched. See [issue #45](https://github.com/Infiltrator-Projects/InfiltratorFS/issues/45).

**This experimental source is not an end-to-end qualified EFI driver yet.** A green native `infiltratorfs.ko` test or EFI cross-compile alone cannot prove UEFI file access.
