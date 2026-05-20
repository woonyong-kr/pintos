/* file.c: 메모리에 매핑된 파일 객체, 즉 mmap 객체 구현. */

#include <stdint.h>
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "vm/vm.h"

struct mmap_lazy_arg {
	struct file *file;
	off_t ofs;
	size_t read_bytes;
	size_t zero_bytes;
	void *map_start;
};

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);
static bool mmap_lazy_load (struct page *page, void *aux);

/* 이 구조체는 수정하지 말 것. */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* 파일 기반 VM을 초기화한다. */
void
vm_file_init (void) {
}

/* 파일 기반 페이지를 초기화한다. */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva UNUSED) {
	struct file_page *file_page;

	RETURN_VALUE_IF (page == NULL, false);
	page->operations = &file_ops;

	file_page = &page->file;
	file_page->type = type;
	file_page->file = NULL;
	file_page->ofs = 0;
	file_page->read_bytes = 0;
	file_page->zero_bytes = 0;
	file_page->map_start = NULL;

	return true;
}

/* 파일에서 내용을 읽어 페이지를 swap in한다. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	RETURN_VALUE_IF (page == NULL || kva == NULL, false);
	struct file_page *file_page UNUSED = &page->file;
}

/* 내용을 파일에 쓰기 반영해 페이지를 swap out한다. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

/* 파일 기반 페이지를 파괴한다. PAGE 자체는 호출자가 해제한다. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

// mmap
void *
do_mmap (void *addr, size_t length, int writable,
         struct file *file, off_t offset) {
	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct mmap_lazy_arg *aux = NULL;
	void *map_start = addr;
	uint8_t *upage = addr;
	uintptr_t start = (uintptr_t) addr;
	uintptr_t end;
	off_t file_len;
	off_t ofs = offset;
	size_t bytes_to_map = length;
	size_t file_left;

	RETURN_VALUE_IF (addr == NULL || file == NULL || length == 0, NULL);
	RETURN_VALUE_IF (pg_ofs (addr) != 0 || offset < 0 ||
	                         offset % PGSIZE != 0,
	                 NULL);

	end = start + length - 1;
	RETURN_VALUE_IF (end < start, NULL);
	RETURN_VALUE_IF (!is_user_vaddr ((void *) start) ||
	                         !is_user_vaddr ((void *) end),
	                 NULL);

	file_len = file_length (file);
	RETURN_VALUE_IF (file_len <= 0 || offset >= file_len, NULL);

	for (uint8_t *page = addr; (uintptr_t) page <= end; page += PGSIZE)
		RETURN_VALUE_IF (spt_find_page (spt, page) != NULL, NULL);

	file_left = file_len - offset;
	while (bytes_to_map > 0) {
		size_t page_map_bytes = bytes_to_map < PGSIZE ? bytes_to_map : PGSIZE;
		size_t page_read_bytes = file_left < page_map_bytes ? file_left : page_map_bytes;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		aux = malloc (sizeof *aux);
		GOTO_IF (aux == NULL, fail);

		aux->file = file_reopen (file);
		GOTO_IF (aux->file == NULL, fail);

		aux->ofs = ofs;
		aux->read_bytes = page_read_bytes;
		aux->zero_bytes = page_zero_bytes;
		aux->map_start = map_start;

		GOTO_IF (!vm_alloc_page_with_initializer (VM_FILE, upage, writable,
		                                          mmap_lazy_load, aux),
		         fail);

		aux = NULL;
		bytes_to_map -= page_map_bytes;
		file_left -= page_read_bytes;
		upage += PGSIZE;
		ofs += PGSIZE;
	}

	return map_start;

fail:
	if (aux != NULL) {
		if (aux->file != NULL)
			file_close (aux->file);
		free (aux);
	}
	for (uint8_t *rollback = map_start; rollback < upage; rollback += PGSIZE) {
		struct page *page = spt_find_page (spt, rollback);
		if (page != NULL)
			spt_remove_page (spt, page);
	}
	return NULL;
}

/* munmap을 수행한다. */
void
do_munmap (void *addr) {
}

static bool
mmap_lazy_load (struct page *page, void *aux_) {
	struct mmap_lazy_arg *aux = aux_;
	RETURN_VALUE_IF (page == NULL || aux == NULL, false);

	page->file.file = aux->file;
	page->file.ofs = aux->ofs;
	page->file.read_bytes = aux->read_bytes;
	page->file.zero_bytes = aux->zero_bytes;
	page->file.map_start = aux->map_start;
	aux->file = NULL;

	free (aux);
	return true;
}
