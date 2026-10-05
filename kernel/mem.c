#include "pico.h"

/* Simple first-fit heap with block splitting and coalescing on free.
 * Deliberately tiny: no paging, no slabs -- this OS is meant to be able to
 * live inside a couple of hundred KB of RAM. */

typedef struct block {
    u32 size;              /* payload bytes */
    struct block *next;
    struct block *prev;
    u8  free;
    u8  pad;
    u16 owner;             /* which task asked for it, 0 = the kernel */
} block_t;

#define HDR sizeof(block_t)
#define ALIGN4(x) (((x) + 3u) & ~3u)

static block_t *head_blk = NULL;
static u32 heap_start = 0, heap_size = 0, used_bytes = 0;

extern char _kernel_end[];   /* from the linker script */

void mem_init(void)
{
    bootinfo_t *bi = bootinfo();
    if (bi && bi->heap_size >= 64 * 1024) {
        /* the UEFI loader walked the firmware memory map for us */
        heap_start = bi->heap_base;
        heap_size  = bi->heap_size;
        goto have_heap;
    }

    /* BIOS path: the boot sector stored "KB above 1MB" at physical 0x500 */
    u16 ext_kb = *(volatile u16 *)0x500;

    if (ext_kb >= 256) {
        heap_start = 0x00100000;
        heap_size  = (u32)ext_kb * 1024u;
        if (heap_size > 60u * 1024u * 1024u) heap_size = 60u * 1024u * 1024u;
    } else {
        /* No extended memory reported, so the heap lives in conventional
         * RAM: everything from just above the kernel to the top of the
         * 640 KB area, leaving the extended bios data area alone.
         *
         * Nothing is reserved for programs. They are relocatable, so they
         * are loaded out of this same heap and cost exactly their own size.
         * That is what puts the floor for PicoOS just above the kernel. */
        heap_start = ALIGN4((u32)_kernel_end);
        heap_size  = 0x0009F000 - heap_start;
    }

have_heap:
    /* If programs were added after the build, their archive is sitting
     * directly behind the kernel and must not be handed out as heap. */
    {
        u32 papp = papp_size();
        if (papp) {
            u32 end = ALIGN4(papp_base() + papp);
            if (end > heap_start && end < heap_start + heap_size) {
                heap_size -= end - heap_start;
                heap_start = end;
            }
        }
    }

    head_blk = (block_t *)heap_start;
    head_blk->size = heap_size - HDR;
    head_blk->next = NULL;
    head_blk->prev = NULL;
    head_blk->free = 1;
    used_bytes = 0;
}

/* --- what this kernel actually costs, measured rather than claimed --- */

extern u32 stack_bottom[], stack_top[];

u32 stack_size(void)
{
    return (u32)((char *)stack_top - (char *)stack_bottom);
}

u32 stack_used(void)
{
    /* entry.S painted the stack with 0xB5B5B5B5; whatever is still painted
     * was never touched. */
    u32 *p = stack_bottom;
    while (p < stack_top && *p == 0xB5B5B5B5u) p++;
    return (u32)((char *)stack_top - (char *)p);
}

u32 mem_footprint(void)
{
    /* The highest byte PicoOS can touch: its own image plus the heap. */
    return heap_start + heap_size;
}

u32 mem_base(void)
{
    return heap_start;
}

/* kmalloc and kfree walk a linked list, and with pre-emption there can be
 * a second task halfway through the same walk. Interrupts go off for the
 * few microseconds that takes -- the cheapest lock there is. */
static void *heap_alloc(size_t n)
{
    if (!n) return NULL;
    n = ALIGN4(n);

    for (block_t *b = head_blk; b; b = b->next) {
        if (!b->free || b->size < n) continue;

        /* split if the leftover can hold a header plus something useful */
        if (b->size >= n + HDR + 16) {
            block_t *nb = (block_t *)((u8 *)b + HDR + n);
            nb->size = b->size - n - HDR;
            nb->free = 1;
            nb->next = b->next;
            nb->prev = b;
            if (b->next) b->next->prev = nb;
            b->next = nb;
            b->size = n;
        }
        b->free = 0;
        b->owner = (u16)task_self();
        used_bytes += b->size + HDR;
        return (u8 *)b + HDR;
    }
    return NULL;    /* out of memory */
}

static void heap_free(void *p)
{
    if (!p) return;
    block_t *b = (block_t *)((u8 *)p - HDR);
    if (b->free) return;
    b->free = 1;
    used_bytes -= b->size + HDR;

    /* coalesce forward */
    if (b->next && b->next->free) {
        b->size += HDR + b->next->size;
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
    }
    /* coalesce backward */
    if (b->prev && b->prev->free) {
        b->prev->size += HDR + b->size;
        b->prev->next = b->next;
        if (b->next) b->next->prev = b->prev;
    }
}

/* kmalloc and kfree walk a linked list, and with pre-emption there can be a
 * second task halfway through the same walk. Interrupts go off for the few
 * microseconds that takes -- with one CPU and no user mode, that is the
 * cheapest correct lock there is. */
void *kmalloc(size_t n)
{
    u32 f = irq_save();
    void *p = heap_alloc(n);
    irq_restore(f);
    return p;
}

/* DMA memory handed to hardware (the USB controller) must outlive the task
 * that happened to be running when it was allocated, so it is given to the
 * kernel (owner 0), which mem_free_task() never touches. */
void mem_disown(void *p)
{
    if (!p) return;
    u32 f = irq_save();
    ((block_t *)((u8 *)p - HDR))->owner = 0;
    irq_restore(f);
}

void kfree(void *p)
{
    u32 f = irq_save();
    heap_free(p);
    irq_restore(f);
}

/* Everything a task asked for and never gave back.
 *
 * A program lives as a task, and a program that mallocs a page buffer and
 * then returns -- or divides by zero on the way out -- used to take that
 * memory with it to the grave. The kernel cannot ask a program to tidy up
 * after itself; it can only know what belonged to the task and take it
 * back when the task dies, which is what this does. Called from the reaper
 * just before the stack goes. */
void mem_free_task(int id)
{
    if (!id) return;

    u32 f = irq_save();
    int again = 1;
    while (again) {
        again = 0;
        for (block_t *b = head_blk; b; b = b->next) {
            if (!b->free && (int)b->owner == id) {
                heap_free((u8 *)b + HDR);
                again = 1;              /* the list just changed under us */
                break;
            }
        }
    }
    irq_restore(f);
}

u32 mem_total(void) { return heap_size; }
u32 mem_used(void)  { return used_bytes; }
u32 mem_free(void)  { return heap_size - used_bytes; }

int mem_blocks(void)
{
    int n = 0;
    for (block_t *b = head_blk; b; b = b->next) n++;
    return n;
}
