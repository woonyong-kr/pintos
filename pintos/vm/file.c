/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "vaddr.h"
#include <round.h>
#include "malloc.h"
static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

static bool validate_mmap_range (size_t page_count, void* addr, struct supplemental_page_table* spt);
static bool register_mmap_pages (size_t page_count, void* addr, off_t offset, size_t length, struct file *file, off_t file_len, bool writable);

static bool 
validate_mmap_range (size_t page_count, void* addr, struct supplemental_page_table* spt) {
	
	for (size_t i = 0 ; i < page_count; i++) {
		void* upage = (uint8_t *) addr + i * PGSIZE;
		
		RETURN_VALUE_IF(!is_user_vaddr (upage), false);
		RETURN_VALUE_IF(!is_user_vaddr ((uint8_t *) upage + PGSIZE - 1) , false);
		RETURN_VALUE_IF(spt_find_page (spt, upage) != NULL, false);

	}

	return true;
}

static bool
register_mmap_pages (size_t page_count, void* addr, off_t offset, size_t length, struct file *file, off_t file_len, bool writable) {
	/* 4. page마다 VM_FILE lazy page 등록 */
	for (size_t i = 0; i < page_count; i++) {
		void *upage = (uint8_t *) addr + i * PGSIZE;
		off_t ofs = offset + (off_t) (i * PGSIZE);

		size_t remaining = length - i * PGSIZE;

		/* remaining 과 PGSIZE 를 비교해서 더 작은 값을 얻음 */
		size_t current_page_bytes = remaining < PGSIZE ? remaining : PGSIZE;
		/* ofs 과 file_len 을 비교해 file_len 이 더크면 file_len - ofs를 하여 파일의 읽을 페이지를 구함 */
		size_t file_left = ofs < file_len ? (size_t) (file_len - ofs) : 0;

		size_t page_read_bytes = file_left < current_page_bytes ? file_left : current_page_bytes;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		struct file_page *aux = malloc (sizeof *aux);
		if (aux == NULL) {
			do_munmap (addr);
			return false;
		}

		aux->file = file_reopen (file);
		if (aux->file == NULL) {
			free (aux);
			do_munmap (addr);
			return false;
		}

		aux->ofs = ofs;
		aux->page_read_bytes = page_read_bytes;
		aux->page_zero_bytes = page_zero_bytes;
		aux->mmap_start = addr;
		aux->page_count = page_count;

		if (!vm_alloc_page_with_initializer (VM_FILE, upage, writable,
					lazy_load_file, aux)) {
			file_close (aux->file);
			free (aux);
			do_munmap (addr);
			return false;
		}
	}

	return true;
}

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	/* 파일을 uninit page 에서 file-backed page로 세팅해주는 함수 */
	page->operations = &file_ops;

	struct file_page *file_page = &page->file;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	struct supplemental_page_table *spt = &thread_current ()->spt;
	off_t file_len;
	size_t page_count;

	RETURN_VALUE_IF(addr == NULL || addr ||  (pg_ofs (addr) != 0) ||
		length == 0 || file == NULL || (offset < 0 || offset % PGSIZE != 0) , false);
		
	file_len = file_length (file);
	RETURN_VALUE_IF(file_len == 0 , false);

	page_count = DIV_ROUND_UP (length, PGSIZE);
	RETURN_VALUE_IF(!validate_mmap_range(page_count, addr, spt), NULL);
	RETURN_VALUE_IF(!register_mmap_pages(page_count, addr, offset, length, file, file_len, writable), NULL);

	return addr;

}

/* Do the munmap */
void
do_munmap (void *addr) {
}

static bool
lazy_load_file(struct page* page, void* aux) {
	RETURN_VALUE_IF(page == NULL || aux == NULL || 
		page->frame == NULL || page->frame->kva == NULL, false);

	struct file_page* file_aux = aux;

	page->file.file = file_aux->file;
	page->file.ofs = file_aux->ofs;
	page->file.page_read_bytes = file_aux->page_read_bytes;
	page->file.page_zero_bytes = file_aux->page_zero_bytes;
	page->file.mmap_start = file_aux->mmap_start;
	page->file.page_count = file_aux->page_count;

	free(aux);
	
	return file_backed_swap_in(page, page->frame->kva);

}