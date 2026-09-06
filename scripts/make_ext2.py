import struct
import sys
from pathlib import Path

import make_fat

SECTOR = 512
TOTAL_SECTORS = 80 * 1024 * 1024 // SECTOR
P2_START = 18432

EXT2_SUPER_MAGIC = 0xEF53
BLOCK = 1024
SECT_PER_BLOCK = BLOCK // SECTOR
INODES_PER_GROUP = 1024
INODE_SIZE = 128
FIRST_DATA_BLOCK = 1

SUPER_BLK = 1
GDT_BLK = FIRST_DATA_BLOCK + 1
BLOCK_BITMAP_BLK = 3
INODE_BITMAP_BLK = 4
ITABLE_BLK = 5
ITABLE_BLOCKS = (INODES_PER_GROUP * INODE_SIZE + BLOCK - 1) // BLOCK
DATA_START = ITABLE_BLK + ITABLE_BLOCKS

FILES = [
    "prog_no_arg.elf", "prog_arg.elf", "cat.elf", "fork_demo.elf",
    "prog_pipe.elf", "font_demo.elf", "heap_demo.elf", "signal_demo.elf",
    "orphan.elf", "tls_test.elf", "sig_test.elf", "badptr_test.elf",
    "cow_stress.elf",
    "cwd_test.elf",
    "echocat.elf",
    "canary_test.elf",
    "font_subset.ttf", "nr_shell.elf", "ping.elf",
    "lc_demo.elf", "libc_testsuite.elf", "musl_demo.elf", "udp_echo.elf",
    "musl_abi_test.elf", "dev_demo.elf", "toybox"
]
ALIASES = {"forktest.elf": "fork_demo.elf"}


def part_entry(bootable, fs_type, start_lba, sec_cnt):
    return struct.pack("<BBBBBBBBII", bootable, 0, 0, 0, fs_type, 0, 0, 0,
                       start_lba & 0xFFFFFFFF, sec_cnt & 0xFFFFFFFF)


def make_mbr(boot_bin):
    p1 = part_entry(0x80, 0x0C, make_fat.PART_START,
                    make_fat.P1_TOTAL_SECTORS)
    p2 = part_entry(0x00, 0x83, P2_START,
                    TOTAL_SECTORS - P2_START)
    mbr = bytearray(boot_bin)
    if len(mbr) < SECTOR:
        mbr += b"\x00" * (SECTOR - len(mbr))
    mbr = mbr[:SECTOR]
    mbr[446:446 + 16] = p1
    mbr[446 + 16:446 + 32] = p2
    mbr[510] = 0x55
    mbr[511] = 0xAA
    return bytes(mbr)


def build_dirent_blocks(entries, block=BLOCK):
    out = bytearray()
    n = len(entries)
    for i, (ino, ftype, name) in enumerate(entries):
        nl = len(name)
        reclen = (8 + nl + 3) & ~3
        if len(out) % block + reclen > block:
            out += bytearray(block - len(out) % block)
        if i == n - 1:
            reclen = block - len(out) % block
        out += struct.pack("<IHBB", ino, reclen, nl, ftype)
        out += name.encode()
        out += bytearray((-len(out)) % 4)
    out += bytearray(block - len(out) % block)
    return bytes(out)


DEV_NODES = [("null", 1, 3), ("zero", 1, 5), ("tty", 5, 0),
             ("console", 5, 1)]


SYMLINKS = [("catlink", "/cat.elf"),
            ("longlink", "/cat.elf" + "/sub/dir/padding/xyz" * 3)]


def put_symlink(table, ino, target, block):
    off = (ino - 1) * INODE_SIZE
    struct.pack_into("<H", table, off + 0, 0xA1FF)
    struct.pack_into("<I", table, off + 4, len(target))
    if len(target) < 60:
        table[off + 40:off + 40 + len(target)] = target.encode()
    else:
        struct.pack_into("<I", table, off + 40, block)


def put_inode(table, ino, payload_len, blocks, is_dir, rdev=0):
    off = (ino - 1) * INODE_SIZE
    mode = 0x41ED if is_dir else (0x21B6 if rdev else 0x81A4)
    struct.pack_into("<H", table, off + 0, mode)
    struct.pack_into("<I", table, off + 4, payload_len)
    struct.pack_into("<H", table, off + 26, 2)
    for i in range(15):
        b = blocks[i] if i < len(blocks) else 0
        struct.pack_into("<I", table, off + 40 + 4 * i, b)
    if rdev:
        struct.pack_into("<I", table, off + 40, rdev)


def build(build_dir, out, smoke=False):
    bd = Path(build_dir)
    boot_bin = (bd / "boot.bin").read_bytes()
    loader_bin = (bd / "loader.bin").read_bytes()
    kernel_bin = (bd / "kernel.bin").read_bytes()

    names = list(FILES)
    names += [a for a in ALIASES if (bd / a).exists()]

    pre = {}
    for name in names:
        src = bd / (ALIASES.get(name, name))
        if src.exists():
            pre[name] = src.read_bytes()
    names = [n for n in names if n in pre]
    if smoke:
        pre["autoexec"] = (b"dev_demo.elf\nmusl_abi_test.elf\nfork_demo.elf\n"
                   b"toybox echo TOYBOX_ECHO_OK\ntoybox ls /\n")
        names.append("autoexec")

    ino_map = {}
    next_ino = 3
    dir_entries = [(2, 2, "."), (2, 2, "..")]
    for name in names:
        ino_map[name] = next_ino
        dir_entries.append((next_ino, 1, name))
        next_ino += 1

    link_ino = next_ino
    next_ino += len(SYMLINKS)
    link_blk_list = []
    for i, (_, tgt) in enumerate(SYMLINKS):
        dir_entries.append((link_ino + i, 7, SYMLINKS[i][0]))
        link_blk_list.append(0)

    dev_ino = next_ino
    next_ino += 1 + len(DEV_NODES)
    dev_entries = [(dev_ino, 2, "."), (dev_ino, 2, "..")]
    for i, (name, maj, mnr) in enumerate(DEV_NODES):
        dev_entries.append((dev_ino + 1 + i, 3, name))
    dir_entries.append((dev_ino, 2, "dev"))

    used_inodes = next_ino - 1

    root_block = DATA_START
    root_dir = build_dirent_blocks(dir_entries)
    n_root_blks = len(root_dir) // BLOCK
    dev_dir = build_dirent_blocks(dev_entries)

    cur_block = DATA_START + n_root_blks
    file_ptrs = {}
    var_blocks = []
    var_indirect = []
    for name in names:
        payload = pre[name]
        nblk = (len(payload) + BLOCK - 1) // BLOCK
        ptrs = [0] * 15
        if nblk <= 12:
            blocks = list(range(cur_block, cur_block + nblk))
            cur_block += nblk
            ptrs[0:nblk] = blocks
        else:
            blocks = list(range(cur_block, cur_block + nblk))
            cur_block += nblk
            ptrs[0:12] = blocks[0:12]
            n_single = min(nblk - 12, 256)
            n_double = nblk - 12 - n_single
            if n_single:
                sing = cur_block
                cur_block += 1
                ptrs[12] = sing
                var_indirect.append((sing, blocks[12:12 + n_single]))
            if n_double:
                dbl = cur_block
                cur_block += 1
                n_sub = (n_double + 255) // 256
                subs = list(range(cur_block, cur_block + n_sub))
                cur_block += n_sub
                ptrs[13] = dbl
                var_indirect.append((dbl, subs))
                for k, sb in enumerate(subs):
                    lo = 12 + n_single + k * 256
                    var_indirect.append(
                        (sb, blocks[lo:lo + 256]))
        file_ptrs[name] = ptrs
        var_blocks.append((blocks, payload))

    for i, (_, tgt) in enumerate(SYMLINKS):
        if len(tgt) >= 60:
            link_blk_list[i] = cur_block
            cur_block += 1

    dev_dir_block = cur_block
    cur_block += 1

    itable = bytearray(ITABLE_BLOCKS * BLOCK)
    put_inode(itable, 2, len(root_dir),
              [root_block + i for i in range(n_root_blks)], True)
    for i, (_, tgt) in enumerate(SYMLINKS):
        put_symlink(itable, link_ino + i, tgt, link_blk_list[i])
    put_inode(itable, dev_ino, BLOCK, [dev_dir_block], True)
    for i, (_, maj, mnr) in enumerate(DEV_NODES):
        put_inode(itable, dev_ino + 1 + i, 0, [], False,
                  rdev=(maj << 8) | mnr)
    for name in names:
        put_inode(itable, ino_map[name], len(pre[name]),
                  file_ptrs[name], False)

    used_blocks = set(range(0, DATA_START))
    for i in range(n_root_blks):
        used_blocks.add(root_block + i)
    for blocks, _ in var_blocks:
        used_blocks.update(blocks)
    for iblk, _ in var_indirect:
        used_blocks.add(iblk)
    used_blocks.update(b for b in link_blk_list if b)
    used_blocks.add(dev_dir_block)
    total_blocks = (TOTAL_SECTORS - P2_START) // SECT_PER_BLOCK
    free_blocks = total_blocks - len(used_blocks)

    bm_len = (total_blocks + 7) // 8
    block_bitmap = bytearray(bm_len)
    for b in used_blocks:
        block_bitmap[b >> 3] |= 0x80 >> (b & 7)

    im_len = (used_inodes + 7) // 8
    inode_bitmap = bytearray(im_len)
    for i in range(1, used_inodes + 1):
        inode_bitmap[(i - 1) >> 3] |= 0x80 >> ((i - 1) & 7)

    gdt = bytearray(BLOCK)
    struct.pack_into("<III", gdt, 0,
                     BLOCK_BITMAP_BLK, INODE_BITMAP_BLK, ITABLE_BLK)

    sb = bytearray(BLOCK)
    struct.pack_into("<IIIIIIIIIII", sb, 0,
                     INODES_PER_GROUP, total_blocks, 0, free_blocks,
                     INODES_PER_GROUP - used_inodes, FIRST_DATA_BLOCK,
                     0, 0, total_blocks, total_blocks, INODES_PER_GROUP)
    struct.pack_into("<IIHHH", sb, 44, 0, 0, 0, 0, 0) 
    struct.pack_into("<H", sb, 56, EXT2_SUPER_MAGIC)

    mbr = make_mbr(boot_bin)

    fat_img = bd / "fat32.img"
    make_fat.fat32(build_dir, str(fat_img))
    fat_data = fat_img.read_bytes()

    base = P2_START
    with open(out, "wb") as f:
        f.truncate(TOTAL_SECTORS * SECTOR)
        f.seek(0)
        f.write(mbr)
        f.seek(make_fat.PART_START * SECTOR)
        f.write(fat_data)
        f.seek(base * SECTOR + SUPER_BLK * BLOCK)
        f.write(bytes(sb))
        f.seek(base * SECTOR + GDT_BLK * BLOCK)
        f.write(bytes(gdt))
        f.seek(base * SECTOR + BLOCK_BITMAP_BLK * BLOCK)
        f.write(bytes(block_bitmap))
        f.seek(base * SECTOR + INODE_BITMAP_BLK * BLOCK)
        f.write(bytes(inode_bitmap))
        f.seek(base * SECTOR + ITABLE_BLK * BLOCK)
        f.write(bytes(itable))
        f.seek(base * SECTOR + root_block * BLOCK)
        f.write(root_dir)
        f.seek(base * SECTOR + dev_dir_block * BLOCK)
        f.write(dev_dir)
        for i, (_, tgt) in enumerate(SYMLINKS):
            if link_blk_list[i]:
                f.seek(base * SECTOR + link_blk_list[i] * BLOCK)
                f.write(tgt.encode())
        for iblk, data in var_indirect:
            idx = bytearray(BLOCK)
            for j, b in enumerate(data):
                struct.pack_into("<I", idx, 4 * j, b)
            f.seek(base * SECTOR + iblk * BLOCK)
            f.write(bytes(idx))
        for blocks, payload in var_blocks:
            f.seek(base * SECTOR + blocks[0] * BLOCK)
            f.write(payload)

    print(f"OK: {out} ({TOTAL_SECTORS * SECTOR // 1024 // 1024}MB)")
    print(f"  P1 @{make_fat.PART_START} FAT32({make_fat.P1_TOTAL_SECTORS}sec) "
          f"[bootable] -> {fat_img}")
    print(f"  P2 @{P2_START} ext2: {len(names)} files, {free_blocks} free blocks")


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--smoke"]
    arg1 = args[0] if len(args) > 0 else "build"
    arg2 = args[1] if len(args) > 1 else "test_hd.img"
    build(arg1, arg2, smoke="--smoke" in sys.argv)
