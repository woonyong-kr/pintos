/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "bitmap.h"
#include "threads/mmu.h"
#include <string.h>

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);
static bool anon_copy (struct supplemental_page_table *dst, struct page *src_page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.copy = anon_copy,
	.type = VM_ANON,
};

#define SWAP_SLOT 8
#define SWAP_SLOT_NONE   ((size_t) -1)

static struct bitmap* disk_bitmap;
static struct lock swap_lock;

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	/* TODO: Set up the swap_disk. */
	swap_disk = disk_get(1,1);
	if (!swap_disk)
		PANIC("NO DISK");

	disk_sector_t sectors = disk_size(swap_disk);

	disk_bitmap = bitmap_create(sectors / SWAP_SLOT);
	if (!disk_bitmap)
		PANIC("NO DISK BITMAP");
	lock_init(&swap_lock);
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	ASSERT(page != NULL);

	page->operations = &anon_ops;

	struct anon_page *anon_page = &page->anon;
	anon_page->swap_idx = BITMAP_ERROR;
	anon_page->swapped = false;

	return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;
	bitmap_flip(disk_bitmap, anon_page->swap_idx);

	for (int i = 0; i < SWAP_SLOT; i++){
		disk_read(swap_disk, anon_page->swap_idx * 8 + i, (uint8_t *)kva + i * 512);
	}
	anon_page->swapped = false;
	anon_page->swap_idx = BITMAP_ERROR;
	pml4_set_page(thread_current()->pml4, page->va, kva, page->writable);

	return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	anon_page->swap_idx = bitmap_scan_and_flip(disk_bitmap, 0, 1, false);
	if (anon_page->swap_idx == BITMAP_ERROR)
		return false;
	for (int i = 0; i < SWAP_SLOT; i++){
		disk_write(swap_disk, anon_page->swap_idx * 8 + i, (uint8_t *)page->frame->kva + i * 512);
	}
	page->anon.swapped = true;
	pml4_clear_page(page->frame->owner_thread->pml4, page->va);
	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
// destroy
static void
anon_destroy (struct page *page) {
	RETURN_IF (page == NULL);

	if (page->anon.swapped) {
		RETURN_IF (disk_bitmap == NULL);

		lock_acquire (&swap_lock);
		bitmap_set (disk_bitmap, page->anon.swap_idx, false);
		page->anon.swapped = false;
		page->anon.swap_idx = SWAP_SLOT_NONE;
		lock_release (&swap_lock);
	}

}

static bool
anon_copy (struct supplemental_page_table *dst, struct page *src_page){
	RETURN_FALSE_IF (src_page == NULL);

	RETURN_FALSE_IF(!vm_alloc_page (page_get_type(src_page), src_page->va, src_page->writable));
	RETURN_FALSE_IF(!vm_claim_page (src_page->va));

	struct page *dst_page = spt_find_page (dst, src_page->va);
	if (src_page->anon.swapped){
		for (int i = 0; i < SWAP_SLOT; i++){
			disk_read(swap_disk, src_page->anon.swap_idx * 8 + i, (uint8_t *)dst_page->frame->kva + i * 512);
		}
	}else{
		memcpy(dst_page->frame->kva, src_page->frame->kva, PGSIZE);
	}
	return true;
}
