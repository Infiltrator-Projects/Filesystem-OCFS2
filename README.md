# Filesystem-OCFS2

Independent Oracle Cluster File System 2 provider for InfiltratorOS. Target: one native C/C++ engine and one distributable `OCFS2.so`.

**Status: documentation only; source extraction pending.** [Central source](https://github.com/Infiltrator-Projects/Filesystems/tree/101026021d9088d80b7f0f472d9061f0e2c688a3/native/filesystems/ocfs2). Editable Linux v6.12.107-derived source remains centrally. No independent driver or release exists.

## Common V2

Use only `infiltrator_filesystem_api_v2` from [Libraries](https://github.com/Infiltrator-Projects/Libraries). Own slot maps, system files, inodes, extent trees, allocation, refcounts, journal and cluster lock identities in the same engine for hosted and freestanding execution. No FUSE, separate per-format kernel/EFI driver or legacy ABI.

## Build and install

Planned: `/System/Filesystems/OCFS2.so` and `/System/Resources/Documentation/Filesystem-OCFS2.pdf`. Generic amd64 .deb, source-downloading CPU-native .run and GitHub-generated source ZIP only. No build or installer exists yet.

See [migration status](MIGRATION_STATUS.md) and [engineering philosophy](docs/PHILOSOPHY.md).
