/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include <string.h>
#include "bitmap.h"
#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/synch.h"
#include "threads/vaddr.h"

#define SECTORS_PER_PAGE (PGSIZE / DISK_SECTOR_SIZE)

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static struct bitmap *swap_table;
static struct lock swap_lock;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	/* TODO: Set up the swap_disk. */
	swap_disk = disk_get (1, 1);
	lock_init (&swap_lock);
	swap_table = swap_disk == NULL ? NULL :
	             bitmap_create (disk_size (swap_disk) / SECTORS_PER_PAGE);
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	struct anon_page *anon_page;

	/* Set up the handler */
	ASSERT(page != NULL);
	
	page->operations = &anon_ops;

	anon_page = &page->anon;
	anon_page->swap_slot = BITMAP_ERROR;
	anon_page->in_swap = false;

	return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;
	size_t i;

	RETURN_VALUE_IF (page == NULL || kva == NULL, false);
	anon_page = &page->anon;

	if (!anon_page->in_swap) {
		memset (kva, 0, PGSIZE);
		return true;
	}

	RETURN_VALUE_IF (swap_disk == NULL || swap_table == NULL, false);
	RETURN_VALUE_IF (anon_page->swap_slot == BITMAP_ERROR, false);

	lock_acquire (&swap_lock);
	for (i = 0; i < SECTORS_PER_PAGE; i++) {
		disk_read (swap_disk,
		           anon_page->swap_slot * SECTORS_PER_PAGE + i,
		           (uint8_t *) kva + i * DISK_SECTOR_SIZE);
	}
	bitmap_reset (swap_table, anon_page->swap_slot);
	anon_page->swap_slot = BITMAP_ERROR;
	anon_page->in_swap = false;
	lock_release (&swap_lock);
	return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	size_t slot;
	size_t i;

	RETURN_VALUE_IF (page == NULL || page->frame == NULL, false);
	RETURN_VALUE_IF (page->frame->kva == NULL, false);
	RETURN_VALUE_IF (swap_disk == NULL || swap_table == NULL, false);

	lock_acquire (&swap_lock);
	slot = bitmap_scan_and_flip (swap_table, 0, 1, false);
	if (slot == BITMAP_ERROR) {
		lock_release (&swap_lock);
		return false;
	}

	for (i = 0; i < SECTORS_PER_PAGE; i++) {
		disk_write (swap_disk, slot * SECTORS_PER_PAGE + i,
		            (uint8_t *) page->frame->kva + i * DISK_SECTOR_SIZE);
	}
	anon_page->swap_slot = slot;
	anon_page->in_swap = true;
	lock_release (&swap_lock);
	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;

	RETURN_IF (page == NULL);
	anon_page = &page->anon;
	if (anon_page->in_swap && swap_table != NULL &&
	    anon_page->swap_slot != BITMAP_ERROR) {
		lock_acquire (&swap_lock);
		bitmap_reset (swap_table, anon_page->swap_slot);
		lock_release (&swap_lock);
		anon_page->swap_slot = BITMAP_ERROR;
		anon_page->in_swap = false;
	}
}
