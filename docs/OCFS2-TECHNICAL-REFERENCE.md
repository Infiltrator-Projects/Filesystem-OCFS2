# OCFS2: technical reference

## Origins and shared disk architecture

Oracle Cluster File System version 2 is a general-purpose shared-storage filesystem designed for simultaneous access by cooperating cluster nodes. It differs from the earlier OCFS format and entered Linux in the 2.6 era. Nodes coordinate caches, metadata and allocation through distributed locks and membership/fencing; each mount slot has its own journal and local allocation structures. Single-node local mode is a distinct admissible configuration, not a mechanism for bypassing the cluster's ownership rules. An offline reader must use a stable image, and an offline writer must ensure the filesystem is not mounted by any node.

## Blocks, clusters and discovery

All multibyte filesystem fields are little-endian; JBD2 log fields have their own big-endian encoding. OCFS2 uses filesystem blocks from 512 to 4096 bytes and allocation clusters from 4096 to 1048576 bytes, powers of two, with clusters at least as large as blocks. The primary superblock is filesystem block 2, not sector 2. For 4096-byte blocks this means byte 8192. Test candidate geometry and confirm the eight-byte OCFSV2 signature, self block number, generation, feature words and matching block-size exponent. Reserved front-of-volume blocks and optional backup superblocks are not ordinary free space. Do not assume that a signature at a guessed byte position establishes the entire geometry.

The retained header gives signatures OCFSV2, INODE01, EXBLK01, GROUP01 and REFCNT1 and block number 2 for the superblock. Its supported block range is 512-4096 bytes and cluster range 4096-1048576 bytes; verify the ratio and active feature flags, not just signatures.

## Superblock union fields

The superblock is a specialized dinode whose union starts at 0xC0. Offsets in this paragraph are relative to that union. Major/minor versions 0/2, mount counters 4/6, state 8, error policy 10, check interval 12, last check 16 (64-bit), creator OS 24, compatibility word 28, incompatibility 32, RO compatibility 36. Root dinode block at 40, system-directory dinode block at 48 (64-bit), block and cluster exponents 56/60, maximum slots 64 (16-bit), tuning flags 66, UUID hash 68, first cluster-group descriptor block 72. Label 80..143, UUID 144..159, cluster stack information from 160. Root/system pointers are physical filesystem-block addresses. Unknown incompatible features prohibit mount; unknown read-only-compatible features prohibit writes.

## Dinode layout and identity

Each dinode occupies one filesystem block. Signature INODE01 at 0, generation 8, suballocation slot/bit 12/14, high link count 16, inline xattr size 18, allocated cluster count 20, UID/GID 24/28, logical size 32 (64-bit), mode and low links 40/42, flags 44. Access/change/modify/deletion seconds at 48/56/64/72, self block 80, final extent leaf 88; filesystem generation 96, nanosecond times 100/104/108, attributes 112, orphan slot 116, dynamic features 118, external xattr 120. Integrity record at 128; directory-index root 136, refcount location 144, discontiguous suballocator location 152. Type-specific union at 184; principal data union at 192. Validate mode, generation, self address and applicable feature flags before decoding either union. An inode number is a physical dinode block, not an index into a global inode table.

## Extent record and extent block

The 16-byte list header contains depth, record capacity and next-free index at 0/2/4, then reserved fields; records begin at 16. Each 16-byte record contains logical cluster position at 0, either 32-bit internal coverage at 4 or 16-bit leaf length at 4 with flags at 7, and 64-bit physical block address at 8. The inode's extent list starts at 192. External extent blocks have EXBLK01 at 0, integrity record 8, suballocator fields 16/18, filesystem generation 20, self block 24, suballocator location 32, next leaf block 40 and list at 48. Validate depth, sorted key ranges, record count, generation and linked leaf coherence. Internal references name metadata blocks, not clusters of file payload. A leaf extent's physical start is in filesystem blocks, while logical position and length use clusters.

## Worked mapping and unrecorded storage

For 4096-byte blocks and 65536-byte clusters, each cluster contains 16 blocks. A leaf mapping logical clusters 20..23 to physical block 16000 covers 262144 bytes. Logical cluster 22 begins at physical block 16032, and byte 100 within it maps to partition offset 65667172. For any file offset, divide by cluster size, choose the covering extent, convert its relative cluster displacement to blocks and add the intra-cluster offset. An absent extent on a sparse file is a hole; an unwritten extent is allocated but reads as zero. A refcounted extent may legitimately share physical clusters. Never expose old storage bytes from an unwritten extent or declare all overlaps to be corruption without consulting the refcount tree.

## Chain allocator and group descriptors

The global cluster bitmap and suballocators use chain lists. A chain-list header contains clusters per group, bits per cluster, capacity and next-free record at offsets 0/2/4/6, followed by reserved bytes and sixteen-byte chain records from 16. Each record gives free bits, total bits and first group block at 0/4/8. A GROUP01 descriptor has bitmap bytes 8, represented bits 10, free bits 12, chain index 14, generation 16, contiguous-free hint 20, next group 24, parent dinode 32, self block 40, integrity 48 and bitmap at 64. Set bits identify occupied allocation units. Check chains for cycles, range overlap and consistent counters. In discontiguous metadata groups a flat extent list at 0x140 maps group positions; ordinary cluster groups remain contiguous. Interpret the group bit unit according to its owning allocator.

## System namespace and local allocation

The system directory contains the global bitmap, slot map, journal objects, local allocation, inode/extent suballocators, orphan directories, truncate logs and optional quota files. Identify objects by their native directory entries and validate their system type; do not guess physical locations from a different formatted image. Per-slot local allocation can reserve a window from the global allocator, then distribute its bits to local allocations. Account for these reserved windows before computing genuinely free capacity. Per-slot truncate logs defer reclamation; pending orphan and truncate state must be resolved under native recovery rules. A free-looking bit in one allocator does not override ownership established by its parent.

## Directory entries and index

An ordinary entry begins with 64-bit inode block at 0, 16-bit record length at 8, one-byte name length at 10 and type at 11; name begins at 12. Records are four-byte aligned and stay within their native payload. Inode zero is unused. A 64-byte trailer on applicable directory blocks includes compatibility record fields, DIRTRL1 signature at 16, self block at 40, parent dinode at 48 and integrity record at 56. Indexed directories maintain a separate hash tree; a sixteen-byte index entry stores major and minor hash at 0/4 and physical directory-entry block at 8. Follow the index to candidate records but compare exact names. Validate index owner, directory trailer, link targets and dot/dot-dot. Inline directories have their own capacity based on dinode layout and reserved xattr bytes.

## Integrity and shared extents

The optional metadata integrity record contains CRC32 at 0, 16-bit Hamming ECC at 4 and reserved at 6. Compute checks according to the native block-check algorithm with the integrity field treated as specified; CRC verifies the protected block while ECC can locate certain single-bit errors. Correction must be verified again and never silently written to an unstable source. Refcount records are sixteen bytes: 64-bit physical cluster position, 32-bit cluster count, 32-bit refcount. A reflinked extent's ownership is governed by this tree, not a duplicate-allocation prohibition. On write, allocate a private replacement where required and transactionally adjust both the file extent and shared refcount. Free only when all native references are gone.

## Journal and cluster recovery

Each slot has a JBD2 journal mapped through its system dinode. Journal fields use big-endian format. The superblock, slot map and journal recovery generation must identify the actual owner. Recovery may replay only validated committed transactions and handle native orphan, local-allocation and truncate cleanup. Cluster mode requires membership and fencing to ensure a failed node cannot resume writing during recovery. Offline exclusive ownership still requires replay or a verified clean state before mutation. A file relocation updates extent mappings, allocation bits and refcounts under the appropriate locks and transaction; a source cluster must remain owned until all required references are safely retired. An independent defragmenter cannot treat the disk as a simple flat bitmap.

## Implementation and qualification boundary

The independent provider uses Common V2: https://github.com/Infiltrator-Projects/Libraries/blob/main/docs/FILESYSTEM_COMMON_ABI_V2.md . Negotiate actual operation masks and media profiles. Distinguish unsupported, corrupt, I/O and stale-handle results. Require bounded host I/O, exclusive unmounted ownership and durable flush before mutation. Test Media and Defragmenter consume the same engine, not separate format parsers. Hosted and freestanding binaries, relocation, recovery, BOOT, kernel integration and packages require separate exact-source evidence. Current support belongs to the repository README and qualification records. The intended package paths are /System/Filesystems/ and /System/Resources/Documentation/; the reference is not an installation or release-status claim.

## Formatting and independent acceptance

A formatter establishes geometry, primary and applicable backups, system and root directories, global and slot-specific allocators, journals, slot map and feature flags as a coherent initial state. Test different block/cluster ratios, extent-depth changes, discontiguous metadata groups, inline data, unwritten extents, refcount transitions, hash collisions and interrupted journal/slot recovery. Compare independently created mkfs.ocfs2 images, debugfs.ocfs2 mappings and fsck.ocfs2 results. Test actual cluster fencing separately from single-host I/O. Source: https://github.com/torvalds/linux/blob/v6.12/fs/ocfs2/ocfs2_fs.h ; algorithms in fs/ocfs2/alloc.c, suballoc.c, dir.c, refcounttree.c, journal.c and blockcheck.c. Oracle design/administration: https://docs.oracle.com/en/operating-systems/oracle-linux/9/ocfs2/ . Qualify checksum/ECC, slot journals, refcount trees, lock ownership, fencing and formatter rules independently for each admitted feature profile. No independent provider qualification is implied.



Retained source: https://github.com/Infiltrator-Projects/Filesystems/blob/101026021d9088d80b7f0f472d9061f0e2c688a3/native/filesystems/ocfs2/linux/ocfs2_fs.h (blob c93689b568).
