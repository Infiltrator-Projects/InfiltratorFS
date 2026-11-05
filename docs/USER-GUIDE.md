# InfiltratorFS

## A filesystem built from first principles

InfiltratorFS is an independently designed filesystem with a portable on-disk format and native operating-system adapters. Its Linux package includes a kernel driver, filesystem tools and the InfiltratorFS Manager.

The current filesystem remains pre-1.0. Use disposable or backed-up media, and check the project's current qualification evidence before relying on a feature for important data. Formatting a partition destroys its existing contents.

## How it is organised

The portable format owns the meaning of files, directories, allocation, transactions, snapshots and integrity. Platform adapters connect that format to the host operating system. Linux uses the native infiltratorfs.ko driver; the Manager and command-line tools provide separate administration interfaces.

Format 0.19 uses copy-on-write transactions, checksummed checkpoints and recovery locators. It includes sparse files, shared extents and reflinks, symbolic and hard links, named read-only snapshots, and per-extent compression. Metadata uses CRC64-ECMA and logical file data uses SHA-256 integrity checks.

## Installation and the Linux driver

The Debian package installs the Manager and tools, and supplies the source for the native Linux kernel module. DKMS builds and installs infiltratorfs.ko for the target kernel; matching kernel headers are required. The package also prepares initramfs integration when the host supports it.

The hardware-native installer obtains the exact released source and builds on the target machine. The released Debian package can be used when an offline package is required. Release information and package assets are maintained on the project's GitHub Releases page.

## Using the Manager

The Linux Manager provides a graphical interface for working with selected non-system partitions. Check the device and partition identity carefully before any operation. Formatting removes the existing data on the selected partition; maintain a separate verified backup when the data matters.

On Linux Mint, the project's Nemo Format action is designed for an existing partition. The system's whole-device Mintstick formatter has different behaviour. Follow the on-screen device and partition information rather than assuming the two actions are interchangeable.

## Mounting a volume

The Linux driver can be loaded and a suitable volume mounted with the native mount helper. Replace the example device and mount point with the correct values for the target system.

    sudo modprobe infiltratorfs
    sudo mount -t infiltratorfs -o rw /dev/<partition> /mnt/infiltratorfs
    findmnt -T /mnt/infiltratorfs -o SOURCE,FSTYPE,OPTIONS

The reported filesystem type should be infiltratorfs.

## Checking and scrubbing

The normal fsck.infiltratorfs check is structural: it checks checkpoints, allocation, objects, namespaces and checksum metadata. It does not read all file data or recompute every payload checksum.

The optional --scrub mode checks data integrity more deeply and may take much longer. It is a mode of fsck.infiltratorfs, not a separate executable. FSCK-SCRUB-SEPARATION.md defines these semantics.

## Snapshots and maintenance

The filesystem supports retained generations and named read-only snapshots. Linux tools include fragmentation metrics, bounded online defragmentation, online grow and bounded shrink. Check feature prerequisites in the project documentation before maintenance.

Snapshots are not a substitute for a separate backup. Qualification applies to particular source revisions and evidence classes; older tests do not prove every later release or physical-media scenario.

## Platform boundaries

Linux is the most complete mounted adapter. Windows includes image/raw-partition tooling and a driverless Explorer projection. A native Windows driver exists in source, but mounted Windows and related qualification remain outstanding.

Development-format compatibility before version 1.0 is not promised. Test media may require reformatting after an on-disk-format change.

## Installed files and project information

The Manager and tools are provided by the InfiltratorFS package. The Linux kernel module is installed through DKMS. This guide is installed as /System/Resources/Documentation/InfiltratorFS.pdf.

The project README gives the product overview. ON_DISK_FORMAT.md, ROADMAP.md and QUALIFICATION.md govern the format, feature status and qualification evidence.

Project: https://github.com/Infiltrator-Projects/InfiltratorFS
