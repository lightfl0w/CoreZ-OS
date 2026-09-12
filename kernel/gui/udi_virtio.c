#include "kernel/gui/udi.h"
#include "kernel/gui/vgpu_pci.h"

#include "drivers/char/console/io.h"
#include "kernel/init/pit/pit.h"
#include "kernel/mm/pool/pool.h"
#include "lib/str/str.h"

#define VIRTIO_GPU_VENDOR 0x1AF4
#define VIRTIO_GPU_DEVICE 0x1050

#define VGPU_CTRL_VQ 0

enum {
    VGPU_RESP_OK_NODATA = 0x1100,
    VGPU_RESP_OK_DATA = 0x1101,
    VGPU_RESP_ERR_UNSPEC = 0x1200,
    VGPU_RESP_ERR_OUT_OF_MEMORY = 0x1201,
    VGPU_RESP_ERR_INVALID_SCANOUT_ID = 0x1202,
    VGPU_RESP_ERR_INVALID_RESOURCE_ID = 0x1203,
    VGPU_RESP_ERR_INVALID_CONTEXT_ID = 0x1204,
    VGPU_RESP_ERR_INVALID_PARAMETER = 0x1205,
};

enum {
    VGPU_CMD_GET_DISPLAY_INFO = 0x0100,
    VGPU_CMD_RESOURCE_CREATE_2D = 0x0101,
    VGPU_CMD_RESOURCE_UNREF = 0x0102,
    VGPU_CMD_SET_SCANOUT = 0x0103,
    VGPU_CMD_RESOURCE_FLUSH = 0x0104,
    VGPU_CMD_TRANSFER_TO_HOST_2D = 0x0105,
    VGPU_CMD_ATTACH_BACKING = 0x0106,
    VGPU_CMD_DETACH_BACKING = 0x0107,
};

enum {
    VGPU_FORMAT_B8G8R8A8_UNORM = 1,
};

struct vgpu_rect {
    uint32_t x, y, w, h;
};

struct vgpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
} __attribute__((packed));

struct vgpu_ctrl_resp {
    struct vgpu_ctrl_hdr hdr;
} __attribute__((packed));

struct vgpu_resource_create_2d {
    struct vgpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct vgpu_resource_unref {
    struct vgpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct vgpu_set_scanout {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct vgpu_resource_flush {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct vgpu_transfer_to_host_2d {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct vgpu_attach_backing {
    struct vgpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed));

struct vgpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct vgpu_vq {
    volatile struct {
        uint64_t addr;
        uint32_t len;
        uint16_t flags;
        uint16_t next;
    } *desc;
    volatile struct {
        uint16_t flags;
        uint16_t idx;
        uint16_t ring[];
    } *avail;
    volatile struct {
        uint16_t flags;
        uint16_t idx;
        volatile struct {
            uint32_t idx;
            uint32_t len;
        } ring[];
    } *used;
    uint16_t size;
    uint16_t last_used;
    uint16_t free_head;
    uint16_t free_count;
};

static struct vgpu_dev {
    volatile uint8_t *common;
    volatile uint8_t *notify;
    uint32_t notify_mult;
    uint16_t notify_off;
    uint32_t queue_size;
    struct vgpu_vq ctrlq;
    uint64_t qdesc_phys;
    uint32_t ready;
    uint32_t next_resource;
} vg;

static void vg_cfg_write32(uint32_t off, uint32_t v) {
    vg_mmio_write32(vg.common, off, v);
}

static uint32_t vg_cfg_read32(uint32_t off) {
    return vg_mmio_read32(vg.common, off);
}

static uint16_t vg_cfg_read16(uint32_t off) {
    return (uint16_t)vg_mmio_read32(vg.common, off);
}

static void vg_cfg_write16(uint32_t off, uint16_t v) {
    vg_mmio_write16(vg.common, off, v);
}

static void vg_cfg_write8(uint32_t off, uint8_t v) {
    vg_mmio_write8(vg.common, off, v);
}

static int vg_parse_caps(uint8_t bus, uint8_t dev) {
    uint32_t status = pci_read32(bus, dev, 0x06);
    if (!(status & 0x00100000u))
        return -1;
    uint8_t cap_ptr = (uint8_t)(pci_read32(bus, dev, 0x34) & 0xFF);
    int found = 0;
    uint32_t common_off = 0, notify_off = 0, notify_mult = 0;
    uint32_t common_len = 0, notify_len = 0;
    uint8_t common_bar = 0, notify_bar = 0;
    uint64_t bars[6] = {0};

    for (int guard = 0; guard < 16 && cap_ptr; guard++) {
        uint32_t c0 = pci_read32(bus, dev, cap_ptr);
        uint8_t cap_vndr = c0 & 0xFF;
        uint8_t cap_next = (c0 >> 8) & 0xFF;
        uint8_t cap_len = (c0 >> 16) & 0xFF;
        uint8_t cfg_type = (c0 >> 24) & 0xFF;
        if (cap_vndr != 0x09) {
            cap_ptr = cap_next;
            continue;
        }
        uint32_t c1 = pci_read32(bus, dev, (uint32_t)(cap_ptr + 4));
        uint8_t bar = (c1 & 0xFF);
        uint32_t offset = pci_read32(bus, dev, (uint32_t)(cap_ptr + 8));
        uint32_t length = pci_read32(bus, dev, (uint32_t)(cap_ptr + 12));
        if (bar < 6) {
            if (bars[bar] == 0) {
                uint32_t lo = pci_read32(bus, dev, 0x10 + bar * 4);
                uint32_t hi = pci_read32(bus, dev, 0x10 + bar * 4 + 4);
                uint64_t base = (lo & ~0xFu) | ((uint64_t)hi << 32);
                if (lo & 1)
                    base = lo & ~0x3u;
                bars[bar] = base;
            }
            if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG) {
                common_off = offset;
                common_len = length;
                common_bar = bar;
                found |= 1;
            } else if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                notify_off = offset;
                notify_mult = length;
                notify_len = 4096;
                notify_bar = bar;
                found |= 2;
            }
        }
        (void)cap_len;
        cap_ptr = cap_next;
    }
    if (found != 3 || bars[common_bar] == 0 || bars[notify_bar] == 0)
        return -1;

    vg.common = (volatile uint8_t *)ioremap(
        (uint32_t)(bars[common_bar] + common_off),
        (common_len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    vg.notify = (volatile uint8_t *)ioremap(
        (uint32_t)(bars[notify_bar] + notify_off),
        (notify_len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    if (vg.common == 0 || vg.notify == 0)
        return -1;
    vg.notify_mult = notify_mult;
    vg.notify_off = (uint16_t)notify_off;
    return 0;
}

/*
 * KASLR-safe virtual -> physical translation for DMA.
 * Walks the current CR3 page tables (kernel mappings are present in
 * every address space), handling 4KB and 2MB leaf entries.
 */
static uint64_t vg_v2p(const void *v) {
    uint64_t va = (uint64_t)(uintptr_t)v;
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t e = ((uint64_t *)phys_to_virt(cr3 & ~0xFFFull))[(va >> 39) & 0x1FF];
    if (!(e & 1))
        return 0;
    e = ((uint64_t *)phys_to_virt(PTE_PHYS(e)))[(va >> 30) & 0x1FF];
    if (!(e & 1))
        return 0;
    e = ((uint64_t *)phys_to_virt(PTE_PHYS(e)))[(va >> 21) & 0x1FF];
    if (!(e & 1))
        return 0;
    if (e & 0x80)
        return PTE_PHYS(e) | (va & 0x1FFFFFull);
    e = ((uint64_t *)phys_to_virt(PTE_PHYS(e)))[(va >> 12) & 0x1FF];
    if (!(e & 1))
        return 0;
    return PTE_PHYS(e) | (va & 0xFFFull);
}

static int vg_alloc_vq(struct vgpu_vq *q, uint16_t size) {
    uint32_t bytes = 16 * size + 6 + 2 * size;
    uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE + 1;
    uint8_t *mem = (uint8_t *)get_kernel_pages(pages);
    if (mem == 0)
        return -1;
    memset(mem, 0, pages * PAGE_SIZE);
    uint64_t phys = vg_v2p(mem);
    q->desc = (volatile void *)mem;
    q->avail = (volatile void *)(mem + 16 * size);
    q->used = (volatile void *)(mem + PAGE_SIZE);
    q->size = size;
    q->last_used = 0;
    q->free_head = 0;
    q->free_count = size;
    for (uint16_t i = 0; i < size; i++) {
        q->desc[i].next = (uint16_t)(i + 1);
    }
    vg.qdesc_phys = phys;
    return 0;
}

static int vg_kick_and_wait(void) {
    volatile uint8_t *nbase =
        vg.notify + vg.notify_mult * VGPU_CTRL_VQ;
    uint16_t before = vg.ctrlq.used->idx;
    vg_mmio_write32((volatile uint8_t *)nbase, 0, VGPU_CTRL_VQ);
    for (uint32_t spins = 0; spins < 50000000u; spins++) {
        if (vg.ctrlq.used->idx != before)
            return 0;
    }
    return -1;
}

static uint8_t vg_req_stage[128];

static int vg_cmd(const void *req, uint32_t reqlen, uint32_t cmd_type) {
    struct vgpu_vq *q = &vg.ctrlq;
    if (q->free_count < 2 || reqlen > sizeof(vg_req_stage))
        return -1;
    memcpy(vg_req_stage, req, reqlen);
    uint16_t d0 = q->free_head;
    uint16_t d1 = (uint16_t)((d0 + 1) % q->size);
    q->free_head = (uint16_t)((d0 + 2) % q->size);
    q->free_count -= 2;

    q->desc[d0].addr = vg_v2p(vg_req_stage);
    q->desc[d0].len = reqlen;
    q->desc[d0].flags = VIRTQ_DESC_F_NEXT;
    q->desc[d0].next = d1;
    (void)cmd_type;

    static struct vgpu_ctrl_resp resp;
    q->desc[d1].addr = vg_v2p(&resp);
    q->desc[d1].len = sizeof(resp);
    q->desc[d1].flags = VIRTQ_DESC_F_WRITE;
    q->desc[d1].next = 0;

    uint16_t avail_idx = q->avail->idx;
    q->avail->ring[avail_idx % q->size] = d0;
    __asm__ volatile("sfence" ::: "memory");
    q->avail->idx = (uint16_t)(avail_idx + 1);
    q->last_used = q->used->idx;

    if (vg_kick_and_wait() != 0)
        return -1;
    uint32_t got = q->used->ring[q->last_used % q->size].idx;
    (void)got;
    q->last_used++;

    return (resp.hdr.type == VGPU_RESP_OK_NODATA) ? 0 : -1;
}

static uint32_t vg_next_resource(void) {
    return ++vg.next_resource;
}

static int vg_probe(void) {
    uint8_t bus = 0, dev = 0;
    if (!pci_find_device(VIRTIO_GPU_VENDOR, VIRTIO_GPU_DEVICE, &bus, &dev)) {
        return -1;
    }
    pci_enable_bus_master(bus, dev, 0x07);
    if (vg_parse_caps(bus, dev) != 0) {
        return -1;
    }
    return 0;
}

static int vg_init(uint32_t w, uint32_t h, uint32_t bpp) {
    (void)bpp;
    if (vg.ready)
        return 0;

    vg_cfg_write8(0x14, 0);
    vg_cfg_write8(0x14, VIRTIO_STATUS_ACK);
    vg_cfg_write8(0x14, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    uint64_t want = VIRTIO_F_VERSION_1;
    vg_mmio_write32(vg.common, 0x00, (uint32_t)(want >> 32));
    uint32_t hi = vg_mmio_read32(vg.common, 0x04);
    vg_mmio_write32(vg.common, 0x00, 0);
    uint32_t lo = vg_mmio_read32(vg.common, 0x04);
    if (!(hi & (uint32_t)(want >> 32))) {
        return -1;
    }
    vg_mmio_write32(vg.common, 0x08, (uint32_t)(want >> 32));
    vg_mmio_write32(vg.common, 0x0C, hi);
    vg_mmio_write32(vg.common, 0x08, 0);
    vg_mmio_write32(vg.common, 0x0C, lo);

    vg_cfg_write8(0x14, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                             VIRTIO_STATUS_FEATURES_OK);
    if (!(vg_cfg_read32(0x14) & 0xFF & VIRTIO_STATUS_FEATURES_OK)) {
        return -1;
    }

    vg_cfg_write16(0x16, VGPU_CTRL_VQ);
    uint16_t qsize = vg_cfg_read32(0x18) & 0xFFFF;
    if (qsize == 0 || qsize > 256)
        qsize = 128;
    vg.queue_size = qsize;
    if (vg_alloc_vq(&vg.ctrlq, qsize) != 0) {
        return -1;
    }

    uint32_t qbase = 0x20;
    vg_cfg_write32(qbase, (uint32_t)(vg.qdesc_phys & 0xFFFFFFFFu));
    vg_cfg_write32(qbase + 4, (uint32_t)(vg.qdesc_phys >> 32));
    vg_cfg_write32(qbase + 8, (uint32_t)((vg.qdesc_phys + 16 * qsize) &
                                         0xFFFFFFFFu));
    vg_cfg_write32(qbase + 12,
                   (uint32_t)((vg.qdesc_phys + 16 * qsize) >> 32));
    uint32_t used_off = (16 * qsize + 6 + 2 * qsize + 0xFFF) & ~0xFFFu;
    vg_cfg_write32(qbase + 16,
                   (uint32_t)((vg.qdesc_phys + used_off) & 0xFFFFFFFFu));
    vg_cfg_write32(qbase + 20,
                   (uint32_t)((vg.qdesc_phys + used_off) >> 32));

    vg_cfg_write16(0x1C, 1);

    vg_cfg_write8(0x14, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                             VIRTIO_STATUS_FEATURES_OK |
                             VIRTIO_STATUS_DRIVER_OK);
    vg.ready = 1;
    (void)h;
    (void)w;
    return 0;
}

struct vgpu_fb {
    uint64_t rid;
    uint8_t *mem;
    uint32_t w, h, size;
};

static struct vgpu_fb vfb;

static int vg_alloc_buffer(uint32_t w, uint32_t h, uint32_t bpp,
                           struct udi_buffer *out) {
    if (vg.ready == 0) {
        return -1;
    }
    uint32_t size = w * h * (bpp / 8);
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint8_t *mem = (uint8_t *)get_kernel_pages(pages);
    if (mem == 0) {
        return -1;
    }
    memset(mem, 0, pages * PAGE_SIZE);

    uint32_t rid = vg_next_resource();
    struct vgpu_resource_create_2d create;
    memset(&create, 0, sizeof(create));
    create.hdr.type = VGPU_CMD_RESOURCE_CREATE_2D;
    create.resource_id = rid;
    create.format = VGPU_FORMAT_B8G8R8A8_UNORM;
    create.width = w;
    create.height = h;
    if (vg_cmd(&create, sizeof(create), create.hdr.type) != 0) {
        free_kernel_page((uint32_t)mem);
        return -1;
    }

    struct {
        struct vgpu_attach_backing a;
        struct vgpu_mem_entry e;
    } attach;
    memset(&attach, 0, sizeof(attach));
    attach.a.hdr.type = VGPU_CMD_ATTACH_BACKING;
    attach.a.resource_id = rid;
    attach.a.nr_entries = 1;
    attach.e.addr = vg_v2p(mem);
    attach.e.length = pages * PAGE_SIZE;
    if (vg_cmd(&attach, sizeof(attach), attach.a.hdr.type) != 0) {
        free_kernel_page((uint32_t)mem);
        return -1;
    }

    struct vgpu_set_scanout ss;
    memset(&ss, 0, sizeof(ss));
    ss.hdr.type = VGPU_CMD_SET_SCANOUT;
    ss.r.w = w;
    ss.r.h = h;
    ss.scanout_id = 0;
    ss.resource_id = rid;
    if (vg_cmd(&ss, sizeof(ss), ss.hdr.type) != 0) {
        free_kernel_page((uint32_t)mem);
        return -1;
    }

    vfb.rid = rid;
    vfb.mem = mem;
    vfb.w = w;
    vfb.h = h;
    vfb.size = size;

    out->handle = rid;
    out->vmem = mem;
    out->w = w;
    out->h = h;
    out->bpp = bpp;
    out->size = size;
    return 0;
}

static void vg_free_buffer(uint64_t handle) {
    struct vgpu_resource_unref un;
    memset(&un, 0, sizeof(un));
    un.hdr.type = VGPU_CMD_RESOURCE_UNREF;
    un.resource_id = (uint32_t)handle;
    vg_cmd(&un, sizeof(un), un.hdr.type);
}

static int vg_commit(uint64_t handle, struct gfx_rect *rects, int n) {
    (void)handle;
    if (vfb.rid == 0)
        return -1;
    int count = (n > 0) ? n : 0;
    for (int i = 0; i < count; i++) {
        struct gfx_rect *r = &rects[i];
        struct vgpu_transfer_to_host_2d t;
        memset(&t, 0, sizeof(t));
        t.hdr.type = VGPU_CMD_TRANSFER_TO_HOST_2D;
        t.r.x = r->x;
        t.r.y = r->y;
        t.r.w = r->w;
        t.r.h = r->h;
        t.offset = (uint64_t)r->y * (uint64_t)vfb.w * 4u + (uint64_t)r->x * 4u;
        t.resource_id = vfb.rid;
        if (vg_cmd(&t, sizeof(t), t.hdr.type) != 0)
            return -1;
    }
    struct vgpu_resource_flush f;
    memset(&f, 0, sizeof(f));
    f.hdr.type = VGPU_CMD_RESOURCE_FLUSH;
    f.r.w = vfb.w;
    f.r.h = vfb.h;
    f.resource_id = vfb.rid;
    return vg_cmd(&f, sizeof(f), f.hdr.type);
}

static void vg_wait_vblank(void) {
    mtime_sleep(16);
}

struct udi_ops udi_virtio_ops = {
    "virtio-gpu", vg_probe,  vg_init,       vg_alloc_buffer,
    vg_free_buffer, vg_commit, vg_wait_vblank,
};
