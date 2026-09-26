# Native Windows filesystem driver

The native Windows target is an InfiltratorFS filesystem driver (infiltratorfs-native.sys), not a ProjFS replacement layer and not a dependency on WinFsp, Dokan or FUSE.

## Architecture

Windows I/O Manager and Cache Manager semantics live in the kernel driver. The driver registers as a disk filesystem, recognises Format 0.18 volumes during IRP_MN_MOUNT_VOLUME, owns VPB/volume-device lifetime, FCB/CCB state, byte-range locking, oplock integration, cached/non-cached I/O, paging I/O and volume teardown.

The existing portable InfiltratorFS core remains the authoritative implementation of namespace, CoW, checksums, compression, snapshots, portable security, storage policy and crash-consistent publication. A companion native service links that same core. The kernel driver and service communicate through the private versioned protocol in infiltratorfs-native-protocol.h.

This split is deliberate:

- no third-party filesystem framework is introduced;
- Windows receives a real filesystem device and normal drive-letter/mount-manager integration;
- Cache Manager remains in the kernel where Windows requires it;
- the portable core is not forked into a second Windows-only filesystem implementation;
- raw storage stays below the filesystem namespace. The service performs storage callbacks through driver IOCTLs identified by mounted-volume ID, so it never recursively opens the mounted InfiltratorFS path.

## Request model

A filesystem IRP that needs portable-core semantics creates one native request and waits for a service response. Service worker threads issue IOCTL_INFILFS_NATIVE_WAIT_REQUEST; completion is returned with IOCTL_INFILFS_NATIVE_COMPLETE_REQUEST.

Large reads and writes are split into bounded chunks. Paging I/O uses the same request path but is marked explicitly so publication and write-through rules remain visible to the service.

Mounted raw storage is exported only through:

- IOCTL_INFILFS_NATIVE_RAW_READ
- IOCTL_INFILFS_NATIVE_RAW_WRITE
- IOCTL_INFILFS_NATIVE_RAW_FLUSH
- IOCTL_INFILFS_NATIVE_VOLUME_QUERY

The kernel validates the volume ID, bounds and read-only state before issuing any target-device I/O.

## Cache Manager contract

Each regular-file FCB owns an FSRTL_ADVANCED_FCB_HEADER, SECTION_OBJECT_POINTERS, main resource and paging-I/O resource. Cached opens initialize a Cache Manager map. Normal cached reads/writes use Cache Manager entry points; paging reads/writes are satisfied through the portable-core service. Flush/cleanup drains Cache Manager state before asking the core to publish its corresponding transaction.

The final driver must qualify:

1. mount/unmount and surprise-removal lifetime;
2. cached, non-cached and paging reads/writes;
3. memory-mapped read/write coherence;
4. directory enumeration and rename/delete/link semantics;
5. byte-range locks and oplocks;
6. Windows security descriptor mapping;
7. fsync/flush durability;
8. snapshot/reflink/storage-policy semantics through the shared core;
9. boot-time service ordering and recovery after service restart;
10. stress under Driver Verifier plus cross-platform Format 0.18 conformance.

The existing ProjFS bridge remains a useful driverless interoperability/recovery path, but it is not counted as the native-driver qualification.
