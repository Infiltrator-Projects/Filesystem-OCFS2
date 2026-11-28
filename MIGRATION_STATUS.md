# OCFS2 Common V2 migration

Native superblock, geometry, generation, optional metadata CRC and root/system-directory dinode recognition.

The native C implementation uses Common V2 bounded host I/O only. It is not a kernel, FUSE or external tool adapter. Test tools are independent oracles only. Constructed metadata fixtures, malformed-media rejection, hosted and import-free builds and sanitizers are defined in CI. Exact-source results establish the qualification boundary. Native namespace, allocation ownership beyond the decoded structures, journal or distributed-lock semantics, writes, repair, formatting and defragmentation remain outstanding. Runtime release is not yet authorized by these checks.

## Offline reader development

Native CORE mount/stat/list/read and VOLUME query now have a bounded local 4 KiB, single-slot implementation. It reconstructs allocation from native chain records and rejects dirty journal state. Independent local-image mount and populated payload qualification are required; no runtime release yet. Clustered ownership, recovery, indexed namespace, refcounts, xattrs, mutation and physical integration remain outstanding.

[Development qualification 46132443151](https://github.com/Infiltrator-Projects/Filesystem-OCFS2/actions/runs/46132443151) passed the independently formatted baseline namespace mount. Release CI must also pass the new populated structural payload, malformed-media and package tests. Independent populated-media interoperability is still outstanding.

### Mounted volume lifetime

The provider copies the caller's size-versioned host descriptor and assigns each mount an opaque generation. An operation reserves its mount until its callback returns. A concurrent unmount or second operation on that mount returns `IFS_FS_BUSY`. Independent mounts remain usable. Unmount removes the generation before releasing native state, so a closed handle returns `IFS_FS_INVALID` even after a new mount reuses the same memory. The Common V2 regression holds a directory read inside a host I/O callback while a second thread queries and unmounts both volumes.
