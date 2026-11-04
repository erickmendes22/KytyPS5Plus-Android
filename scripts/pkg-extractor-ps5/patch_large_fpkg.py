#!/usr/bin/env python3
"""Fix false 'metadata too large' failures for large PS5 packages.

The NAPS file-offset table records file *starts* and sparse boundaries,
not the end of the final file. A gigabyte-sized final file therefore
must not be mistaken for gigabytes of PFS metadata.

Probe for an inner PFS superblock backward in bounded pieces; after
finding it, read only the bytes needed for the inode table. Directory
blocks are already read on demand by InnerImage.walk_dir.
"""
from pathlib import Path
import sys
src=Path(sys.argv[1])/"src/inner.rs"
text=src.read_text()
old = '''/// The superblock, inodes and directories after the last file. Real images
/// keep a few MiB there; a larger tail means a corrupt layout.
const MAX_META_TAIL: u64 = 1 << 30;'''
new = '''/// Never allocate the entire interval after the last NAPS file offset:
/// that offset marks a file *start*, and its payload may be many GiB.
const META_SCAN_CHUNK: u64 = 8 << 20;
/// Hard cap only on the actual superblock + inode table allocation.
const MAX_INODE_TABLE_BYTES: u64 = 256 << 20;'''
assert text.count(old)==1, "Cannot locate previous metadata memory guard"
text=text.replace(old,new,1)
old = '''        // Metadata follows the last file boundary below the mount size.
        let meta_base = self.boundaries.iter().copied().filter(|&o| o > 0 && o < self.mount).max().unwrap_or(0);
        let first = self.blocks.partition_point(|b| b.logical + b.uncomp as u64 <= meta_base);
        let tail_base = self.blocks.get(first).ok_or_else(|| Error::format("no blocks after the last file"))?.logical;
        let tail_len = self.mount - tail_base;
        if tail_len > MAX_META_TAIL {
            return Err(Error::format(format!(
                "{tail_len:#x} bytes after the last file boundary at {tail_base:#x}; the inner metadata should be far smaller"
            )));
        }
        let mut tail = vec![0u8; tail_len as usize];
        self.read_at(tail_base, &mut tail)?;
        let sb_off = find_superblock(&tail, self.mount)
            .ok_or_else(|| Error::format(format!("no inner superblock covering {:#x} bytes after logical {tail_base:#x}", self.mount)))?;
        let tree = Tree::parse(&tail, sb_off, tail_base)?;'''
new = '''        // NAPS records the start of each file. The final file can be >1 GiB,
        // so scanning/allocating everything since that point is not sound.
        let meta_base = self.boundaries.iter().copied().filter(|&o| o > 0 && o < self.mount).max().unwrap_or(0);
        let first = self.blocks.partition_point(|b| b.logical + b.uncomp as u64 <= meta_base);
        let lower = self.blocks.get(first).ok_or_else(|| Error::format("no blocks after the last file"))?.logical;
        let mut end = self.mount;
        let sb_logical = loop {
            let start = meta_probe_start(lower, end);
            let n = usize::try_from(end - start).map_err(|_| Error::format("metadata probe range overflow"))?;
            let mut probe = vec![0u8; n];
            self.read_at(start, &mut probe)?;
            if let Some(sb_off) = find_superblock(&probe, self.mount) {
                break start + sb_off as u64;
            }
            if start == lower {
                return Err(Error::format(format!(
                    "no inner PFS superblock from {lower:#x} to {:#x} ({} MiB scanned)",
                    self.mount, (self.mount - lower) >> 20
                )));
            }
            end = start;
        };
        // The PFS tree parser needs the superblock and inodes, not the
        // full logical remainder of the image. It reads directory contents
        // lazily via self.read_at() if they are not within this small buffer.
        let mut sb = [0u8; 0x40];
        self.read_at(sb_logical, &mut sb)?;
        let block_size = u64::from(le32(&sb, 0x20)?);
        let inode_count = le64(&sb, 0x30)?;
        if block_size == 0 || !block_size.is_multiple_of(0x1000) ||
           inode_count == 0 || inode_count > 1_000_000 ||
           block_size < INNER_INODE_SIZE as u64 {
            return Err(Error::format(format!(
                "inner superblock: block size {block_size:#x}, {inode_count} inodes"
            )));
        }
        let inodes_per_block = block_size / INNER_INODE_SIZE as u64;
        let inode_blocks = inode_count.div_ceil(inodes_per_block);
        let table_len = (1 + inode_blocks).checked_mul(block_size)
            .ok_or_else(|| Error::format("inner inode table size overflow"))?;
        if table_len > MAX_INODE_TABLE_BYTES || sb_logical.checked_add(table_len).is_none_or(|v| v > self.mount) {
            return Err(Error::format(format!(
                "inner inode table needs {table_len:#x} bytes at {sb_logical:#x}; outside safe image bounds"
            )));
        }
        let mut tail = vec![0u8; table_len as usize];
        self.read_at(sb_logical, &mut tail)?;
        let tree = Tree::parse(&tail, 0, sb_logical)?;'''
assert text.count(old)==1, "Cannot find exact parsing block"
text=text.replace(old,new,1)
before='''/// Requires the superblock to cover exactly the NAPS mount size, so the
/// outer image's superblock can never be taken for the inner one.'''
helper='''/// Return a bounded, nonoverlapping scan range aligned to the same
/// 64 KiB grid used by find_superblock. The lower bound is inclusive.
fn meta_probe_start(lower: u64, end: u64) -> u64 {
    let tentative = end.saturating_sub(META_SCAN_CHUNK).max(lower);
    lower + ((tentative - lower) / 0x10000) * 0x10000
}

'''
assert text.count(before)==1
text=text.replace(before,helper+before,1)
mark='''    #[test]
    fn the_superblock_must_match_the_mount_size() {'''
test='''    #[test]
    fn metadata_scan_handles_a_last_file_larger_than_one_gib() {
        let lower = 0x1487e40000u64;
        let mount = lower + 0x4fea0000u64; // User's >1 GiB false alarm
        let mut end = mount;
        let mut ranges = 0usize;
        while end > lower {
            let start = meta_probe_start(lower, end);
            assert!(start < end);
            assert!(end - start <= META_SCAN_CHUNK + 0x10000);
            assert_eq!((start - lower) % 0x10000, 0);
            end = start;
            ranges += 1;
        }
        assert_eq!(end, lower);
        assert!(ranges > 100);
    }

'''
assert text.count(mark)==1
text=text.replace(mark,test+mark,1)
src.write_text(text)
print("Large PS5 FPKG metadata scanner integrated")
