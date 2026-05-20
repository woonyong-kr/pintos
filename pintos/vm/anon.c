/* anon.c: 디스크 이미지에 연결되지 않은 페이지, 즉 익명 페이지 구현. */

#include "vm/vm.h"
#include "devices/disk.h"
#include "bitmap.h"
#include "threads/mmu.h"

#define SECTORS_PER_PAGE (PGSIZE / DISK_SECTOR_SIZE)
#define SWAP_SLOT_NONE   ((size_t) -1)

/* 아래 줄은 수정하지 말 것. */
static struct disk *swap_disk;
static struct bitmap *swap_bitmap;
static struct lock swap_lock;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* 이 구조체는 수정하지 말 것. */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
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

// 익명 페이지 초기화
bool
anon_initializer (struct page *page, enum vm_type type, void *kva UNUSED) {
	RETURN_VALUE_IF (page == NULL, false);
	page->operations = &anon_ops;
	page->anon.type = type;
	page->anon.swapped = false;
	page->anon.swap_slot = SWAP_SLOT_NONE;
	return true;
}

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
