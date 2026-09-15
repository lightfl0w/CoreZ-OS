#include "drivers/block/ide.h"

#include "ops/block_ops.h"
#include "drivers/driver_ops.h"

#include <stddef.h>

#include "kernel/asm_func.h"
#include "kernel/assert.h"
#include "drivers/char/console/io.h"
#include "kernel/init/pit/pit.h"
#include "arch/cpu.h"
#include "lib/str/str.h"
#include "libc/user/stdio.h"
#include "kernel/mm/pool/pool.h"

#define reg_data(channel) ((channel)->port_base + 0)
#define reg_error(channel) ((channel)->port_base + 1)
#define reg_sect_cnt(channel) ((channel)->port_base + 2)
#define reg_lba_l(channel) ((channel)->port_base + 3)
#define reg_lba_m(channel) ((channel)->port_base + 4)
#define reg_lba_h(channel) ((channel)->port_base + 5)
#define reg_dev(channel) ((channel)->port_base + 6)
#define reg_status(channel) ((channel)->port_base + 7)
#define reg_cmd(channel) (reg_status(channel))
#define reg_alt_status(channel) ((channel)->port_base + 0x206)
#define reg_ctl(channel) (reg_alt_status(channel))

#define BIT_ALT_STAT_BSY 0x80
#define BIT_ALT_STAT_DRDY 0x40
#define BIT_ALT_STAT_DRQ 0x8

#define BIT_DEV_MBS 0xa0
#define BIT_DEV_LBA 0x40
#define BIT_DEV_DEV 0x10

#define CMD_IDENTIFY 0xec
#define CMD_READ_SECTOR 0x20
#define CMD_WRITE_SECTOR 0x30

#define MAX_LBA_DEFAULT ((80 * 1024 * 1024 / 512) - 1)
#define MAX_LBA28 (0x0FFFFFFF)

uint8_t channel_cnt;
struct IDE_CHANNEL channels[2];

uint32_t ext_lba_base = 0;
uint8_t p_no = 0, l_no = 0;
struct LIST partition_list;

struct DISK_PART_ENTRY {
    uint8_t bootable;
    uint8_t start_head;
    uint8_t start_sec;
    uint8_t start_chs;
    uint8_t fs_type;
    uint8_t end_head;
    uint8_t end_sec;
    uint8_t end_chs;
    uint32_t start_lba;
    uint32_t sec_cnt;
} __attribute__((packed));

struct DISK_BOOT_SECTOR {
    uint8_t other[446];
    struct DISK_PART_ENTRY partition_table[4];
    uint16_t signature;
} __attribute__((packed));

_Static_assert(sizeof(struct DISK_PART_ENTRY) == 16,
               "partition_table_entry must be exactly 16 bytes (MBR spec)");
_Static_assert(sizeof(struct DISK_BOOT_SECTOR) == 512,
               "boot_sector must be exactly 512 bytes (one sector)");
_Static_assert(offsetof(struct DISK_BOOT_SECTOR, partition_table) == 446,
               "partition_table must start at offset 446 in boot_sector");

static void ide_panic(const char *msg) {
    set_text_color(12);
    kprintf("IDE PANIC: %s\n", msg);
    asm_cli();
    for (;;) {
        asm_hlt();
    }
}

static void select_disk(struct DISK *hd) {
    uint8_t reg_device = BIT_DEV_MBS | BIT_DEV_LBA;
    if (hd->dev_no == 1) {
        reg_device |= BIT_DEV_DEV;
    }
    outb(reg_dev(hd->my_channel), reg_device);
}

static void select_sector(struct DISK *hd, uint32_t lba, uint8_t sec_cnt) {
    ASSERT(lba <= hd->max_lba);
    struct IDE_CHANNEL *channel = hd->my_channel;
    outb(reg_sect_cnt(channel), sec_cnt);
    outb(reg_lba_l(channel), lba);
    outb(reg_lba_m(channel), lba >> 8);
    outb(reg_lba_h(channel), lba >> 16);

    outb(reg_dev(channel), BIT_DEV_MBS | BIT_DEV_LBA |
                               (hd->dev_no == 1 ? BIT_DEV_DEV : 0) |
                               (uint8_t)(lba >> 24));
}

static void wait_bsy_clear(struct IDE_CHANNEL *channel) {
    while (inb(reg_status(channel)) & BIT_ALT_STAT_BSY) {
    }
}

static void channel_send_cmd(struct IDE_CHANNEL *channel, uint8_t cmd) {
    wait_bsy_clear(channel);
    channel->expecting_intr = 1;
    outb(reg_cmd(channel), cmd);
}

static void read_from_sector(struct DISK *hd, void *buf, uint8_t sec_cnt) {
    uint32_t dwords = sec_cnt ? (uint32_t)sec_cnt << 7 : 256u << 7;
    cpu_ins(reg_data(hd->my_channel), buf, (int)dwords, 4);
}

static void write_to_sector(struct DISK *hd, void *buf, uint8_t sec_cnt) {
    uint32_t words = sec_cnt ? (uint32_t)sec_cnt << 8 : 256 << 8;
    outsw(reg_data(hd->my_channel), buf, words);
}

static int busy_wait(struct DISK *hd) {
    struct IDE_CHANNEL *channel = hd->my_channel;
    uint32_t timeout = 30 * 1000 * 100;
    while (timeout--) {
        uint8_t st = inb(reg_status(channel));
        if (!(st & BIT_ALT_STAT_BSY)) {
            return (st & BIT_ALT_STAT_DRQ) != 0;
        }
    }
    return 0;
}

void ide_read(struct DISK *hd, uint32_t lba, void *buf, uint32_t sec_cnt) {
    ASSERT(lba <= hd->max_lba);
    ASSERT(sec_cnt > 0);
    lock_acquire(&hd->my_channel->lock);

    select_disk(hd);

    uint32_t secs_done = 0;
    while (secs_done < sec_cnt) {
        uint32_t chunk = sec_cnt - secs_done;
        if (chunk > 255) {
            chunk = 255;
        }
        select_sector(hd, lba + secs_done, (uint8_t)chunk);
        channel_send_cmd(hd->my_channel, CMD_READ_SECTOR);
        for (uint32_t i = 0; i < chunk; i++) {
            if (!busy_wait(hd)) {
                char error[64];
                sprintf(error, "%s read sector %d failed!!!!!!", hd->name,
                        (int)(lba + secs_done + i));
                ide_panic(error);
            }
            read_from_sector(hd, (char *)buf + (secs_done + i) * 512, 1);
        }
        secs_done += chunk;
    }
    lock_release(&hd->my_channel->lock);
}

void ide_write(struct DISK *hd, uint32_t lba, void *buf, uint32_t sec_cnt) {
    ASSERT(lba <= hd->max_lba);
    ASSERT(sec_cnt > 0);
    lock_acquire(&hd->my_channel->lock);

    select_disk(hd);

    uint32_t secs_done = 0;
    while (secs_done < sec_cnt) {
        uint32_t chunk = sec_cnt - secs_done;
        if (chunk > 255) {
            chunk = 255;
        }
        select_sector(hd, lba + secs_done, (uint8_t)chunk);
        channel_send_cmd(hd->my_channel, CMD_WRITE_SECTOR);
        for (uint32_t i = 0; i < chunk; i++) {
            if (!busy_wait(hd)) {
                char error[64];
                sprintf(error, "%s write sector %d failed!!!!!!", hd->name,
                        (int)(lba + secs_done + i));
                ide_panic(error);
            }
            write_to_sector(hd, (char *)buf + (secs_done + i) * 512, 1);
        }
        wait_bsy_clear(hd->my_channel);
        secs_done += chunk;
    }
    lock_release(&hd->my_channel->lock);
}

void intr_hd_handler(uint8_t irq_no) {
    ASSERT(irq_no == 0x2e || irq_no == 0x2f);
    uint32_t no = irq_no - 0x2e;
    struct IDE_CHANNEL *channel = &channels[no];
    ASSERT(channel->irq_no == irq_no);
    channel->expecting_intr = 0;
    inb(reg_status(channel));
}

static void swap_pairs_bytes(const char *dst, char *buf, uint32_t len) {
    uint8_t idx;
    for (idx = 0; idx < len; idx += 2) {
        buf[idx + 1] = *dst;
        dst++;
        buf[idx] = *dst;
        dst++;
    }
    buf[idx] = '\0';
}

static void identify_disk(struct DISK *hd) {
    char id_info[512];
    select_disk(hd);
    channel_send_cmd(hd->my_channel, CMD_IDENTIFY);

    if (!busy_wait(hd)) {
        char error[64];
        sprintf(error, "%s identify failed!!!!!!", hd->name);
        ide_panic(error);
    }
    read_from_sector(hd, id_info, 1);

    char buf[64];
    uint8_t sn_start = 10 * 2, sn_len = 20;
    uint8_t md_start = 27 * 2, md_len = 40;
    swap_pairs_bytes(&id_info[sn_start], buf, sn_len);
    kprintf("  disk %s info:\n", hd->name);
    kprintf("    SN: %s\n", buf);
    memset(buf, 0, sizeof(buf));
    swap_pairs_bytes(&id_info[md_start], buf, md_len);
    kprintf("    MODULE: %s\n", buf);
    uint32_t sector = *(uint32_t *)&id_info[60 * 2];
    kprintf("    SECTORS: %d\n", (int)sector);
    kprintf("    CAPACITY: %dMB\n", (int)(sector * 512 / 1024 / 1024));
    hd->max_lba = (sector > 1) ? sector - 1 : MAX_LBA_DEFAULT;
    if (hd->max_lba > MAX_LBA28) {
        hd->max_lba = MAX_LBA28;
    }
}

static void partition_scan(struct DISK *hd, uint32_t ext_lba) {
    struct DISK_BOOT_SECTOR *bs = (struct DISK_BOOT_SECTOR *)get_kernel_pages(1);
    if (bs == NULL) {
        return;
    }
    ide_read(hd, ext_lba, bs, 1);
    uint8_t part_idx = 0;
    struct DISK_PART_ENTRY *p = bs->partition_table;

    while (part_idx++ < 4) {
        if (p->fs_type == 0x5) {
            if (ext_lba == 0) {
                ext_lba_base = p->start_lba;
                partition_scan(hd, p->start_lba);
            } else {
                partition_scan(hd, p->start_lba + ext_lba_base);
            }
        } else if (p->fs_type != 0) {
            if (ext_lba == 0) {
                hd->prim_parts[p_no].start_lba = ext_lba + p->start_lba;
                hd->prim_parts[p_no].sec_cnt = p->sec_cnt;
                hd->prim_parts[p_no].my_disk = hd;
                list_append(&partition_list, &hd->prim_parts[p_no].part_tag);
                sprintf(hd->prim_parts[p_no].name, "%s%d", hd->name, p_no + 1);
                p_no++;
                ASSERT(p_no < 4);
            } else {
                hd->logic_parts[l_no].start_lba = ext_lba + p->start_lba;
                hd->logic_parts[l_no].sec_cnt = p->sec_cnt;
                hd->logic_parts[l_no].my_disk = hd;
                list_append(&partition_list, &hd->logic_parts[l_no].part_tag);
                sprintf(hd->logic_parts[l_no].name, "%s%d", hd->name, l_no + 5);
                l_no++;
                if (l_no >= 8) {
                    free_kernel_page((uint32_t)bs);
                    return;
                }
            }
        }
        p++;
    }
    free_kernel_page((uint32_t)bs);
}

static void print_partition_info(void) {
    struct LIST_ELEM *e = partition_list.head.next;
    while (e != &partition_list.tail) {
        struct DISK_PARTITION *part = list_entry(e, struct DISK_PARTITION, part_tag);
        kprintf("    %s start_lba:0x%x, sec_cnt:0x%x\n", part->name,
                part->start_lba, part->sec_cnt);
        e = e->next;
    }
}

static int block_read(void *dev, uint32_t lba, void *buf, uint32_t count) {
    ide_read((struct DISK *)dev, lba, buf, count);
    return 0;
}

static int block_write(void *dev, uint32_t lba, const void *buf,
                       uint32_t count) {
    ide_write((struct DISK *)dev, lba, buf, count);
    return 0;
}

const struct BLOCK_OPS BLOCK = {
    .read_sectors = block_read,
    .write_sectors = block_write,
};

static int ide_drv_init(void) {
    ide_init();
    return 0;
}

DRIVER_REGISTER("ide", 20, ide_drv_init);

void ide_init(void) {
    kprintf("ide_init start\n");

    uint8_t hd_cnt = *((uint8_t *)(0x475));
    if (hd_cnt == 0) {
        kprintf("  no hard disk detected (0x475=0), skip ide_init\n");
        return;
    }

    channel_cnt = (uint8_t)DIV_ROUND_UP(hd_cnt, 2);
    struct IDE_CHANNEL *channel;
    uint8_t channel_no = 0, dev_no = 0;
    uint8_t global_dev = 0;
    list_init(&partition_list);

    while (channel_no < channel_cnt) {
        channel = &channels[channel_no];
        sprintf(channel->name, "ide%d", channel_no);
        if (channel_no == 0) {
            channel->port_base = 0x1f0;
            channel->irq_no = 0x20 + 14;
        } else {
            channel->port_base = 0x170;
            channel->irq_no = 0x20 + 15;
        }
        channel->expecting_intr = 0;
        lock_init(&channel->lock);
        sema_init(&channel->disk_done, 0);

        while (dev_no < 2 && global_dev < hd_cnt) {
            struct DISK *hd = &channel->devices[dev_no];
            hd->my_channel = channel;
            hd->dev_no = dev_no;
            sprintf(hd->name, "sd%c", 'a' + channel_no * 2 + dev_no);
            identify_disk(hd);
            p_no = 0;
            l_no = 0;
            ext_lba_base = 0;
            partition_scan(hd, 0);
            dev_no++;
            global_dev++;
        }
        dev_no = 0;
        channel_no++;
    }
    kprintf("\n  all partition info\n");
    print_partition_info();
    kprintf("ide_init done\n");
}
