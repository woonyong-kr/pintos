/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "bitmap.h"
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "threads/thread.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);
static bool anon_copy (struct supplemental_page_table *dst, struct page *src_page);
static size_t swap_slot_alloc (void);
static void swap_slot_ref (size_t slot) UNUSED;
static void swap_slot_unref (size_t slot);

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
static size_t *swap_ref_count;
static size_t swap_slot_count;
static struct lock swap_lock;

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	/* TODO: Set up the swap_disk. */
	swap_disk = disk_get(1,1);
	if (!swap_disk)
		PANIC("NO DISK");

	disk_sector_t sectors = disk_size(swap_disk);

	swap_slot_count = sectors / SWAP_SLOT;
	disk_bitmap = bitmap_create(swap_slot_count);
	if (!disk_bitmap)
		PANIC("NO DISK BITMAP");
	swap_ref_count = malloc (sizeof *swap_ref_count * swap_slot_count);
	if (swap_ref_count == NULL)
		PANIC("NO SWAP REF COUNT");
	for (size_t i = 0; i < swap_slot_count; i++)
		swap_ref_count[i] = 0;
	lock_init(&swap_lock);
}

static size_t
swap_slot_alloc (void) {
	size_t slot;

	lock_acquire (&swap_lock);
	slot = bitmap_scan_and_flip(disk_bitmap, 0, 1, false);
	if (slot != BITMAP_ERROR)
		swap_ref_count[slot] = 1;
	lock_release (&swap_lock);

	return slot;
}

static void
swap_slot_ref (size_t slot) {
	ASSERT (slot != BITMAP_ERROR);
	ASSERT (slot < swap_slot_count);

	lock_acquire (&swap_lock);
	ASSERT (bitmap_test (disk_bitmap, slot));
	ASSERT (swap_ref_count[slot] > 0);
	swap_ref_count[slot]++;
	lock_release (&swap_lock);
}

static void
swap_slot_unref (size_t slot) {
	ASSERT (slot != BITMAP_ERROR);
	ASSERT (slot < swap_slot_count);

	lock_acquire (&swap_lock);
	ASSERT (bitmap_test (disk_bitmap, slot));
	ASSERT (swap_ref_count[slot] > 0);

	swap_ref_count[slot]--;
	if (swap_ref_count[slot] == 0)
		bitmap_set (disk_bitmap, slot, false);
	lock_release (&swap_lock);
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
		lock_acquire(&swap_lock);
		disk_read(swap_disk, anon_page->swap_idx * 8 + i, (uint8_t *)kva + i * 512);
		lock_release(&swap_lock);
	}
	anon_page->swapped = false;
	anon_page->swap_idx = BITMAP_ERROR;
	
	return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	anon_page->swap_idx = swap_slot_alloc ();
	if (anon_page->swap_idx == BITMAP_ERROR)
		return false;
	for (int i = 0; i < SWAP_SLOT; i++){
		lock_acquire(&swap_lock);
		disk_write(swap_disk, anon_page->swap_idx * 8 + i, (uint8_t *)page->frame->kva + i * 512);
		lock_release(&swap_lock);
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

		swap_slot_unref (page->anon.swap_idx);
		page->anon.swapped = false;
		page->anon.swap_idx = SWAP_SLOT_NONE;
	}

}

static bool
anon_copy (struct supplemental_page_table *dst, struct page *src_page) {
	RETURN_FALSE_IF (src_page == NULL);

	RETURN_FALSE_IF (!vm_alloc_page (page_get_type (src_page), src_page->va, src_page->writable));

	struct page *dst_page = spt_find_page (dst, src_page->va);
	RETURN_FALSE_IF (dst_page == NULL);

	if (src_page->frame == NULL || src_page->anon.swapped) {
		if (!src_page->anon.swapped)
			return true;

		RETURN_FALSE_IF (!vm_claim_page (src_page->va));

		lock_acquire (&swap_lock);
		for (int i = 0; i < SWAP_SLOT; i++) {
			disk_read (swap_disk, src_page->anon.swap_idx * 8 + i, (uint8_t *) dst_page->frame->kva + i * 512);
		}
		lock_release (&swap_lock);
		return true;
	}

	struct frame *frame = src_page->frame;
	struct thread *curr = thread_current ();
	RETURN_FALSE_IF (frame == NULL || frame->kva == NULL);
	RETURN_FALSE_IF (curr == NULL || curr->pml4 == NULL);

	if (src_page->writable && frame->ref_count == 1) {
		struct thread *owner = frame->owner_thread;
		RETURN_FALSE_IF (owner == NULL || owner->pml4 == NULL);
		RETURN_FALSE_IF (!pml4_set_page (owner->pml4, src_page->va,
		                                 frame->kva, false));
	}

	if (!pml4_set_page (curr->pml4, dst_page->va, frame->kva, false)) {
		if (src_page->writable && frame->ref_count == 1) {
			struct thread *owner = frame->owner_thread;
			if (owner != NULL && owner->pml4 != NULL)
				pml4_set_page (owner->pml4, src_page->va,
				               frame->kva, src_page->writable);
		}
		spt_remove_page (dst, dst_page);
		return false;
	}

	dst_page->operations = &anon_ops;
	dst_page->anon = src_page->anon;
	dst_page->frame = frame;
	frame->ref_count++;
	return true;
}
