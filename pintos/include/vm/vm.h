#ifndef VM_VM_H
#define VM_VM_H
#include <stdbool.h>
#include "threads/palloc.h"
#include "hash.h"
enum vm_type {
	/* 아직 초기화되지 않은 페이지 */
	VM_UNINIT = 0,
	/* 파일과 관련 없는 페이지, 즉 anonymous 페이지 */
	VM_ANON = 1,
	/* 파일과 관련된 페이지 */
	VM_FILE = 2,
	/* 페이지 캐시를 담는 페이지, project 4용 */
	VM_PAGE_CACHE = 3,

	/* 상태를 저장하는 비트 플래그 */

	/* 정보를 저장하기 위한 보조 비트 플래그 표식.
	 * int 안에 들어가는 범위라면 더 추가해도 된다. */
	VM_MARKER_0 = (1 << 3),
	VM_MARKER_1 = (1 << 4),

	/* 이 값을 넘기지 말 것. */
	VM_MARKER_END = (1 << 31),
};

#include "vm/uninit.h"
#include "vm/anon.h"
#include "vm/file.h"
#ifdef EFILESYS
#include "filesys/page_cache.h"
#endif

struct page_operations;
struct thread;

#define VM_TYPE(type) ((type) & 7)

/* "page"의 표현.
 * 일종의 "부모 클래스" 역할을 하며,
 * uninit_page, file_page, anon_page, page cache(project4)라는
 * 네 가지 "자식 클래스"를 가진다.
 * 이 구조체의 미리 정의된 멤버는 삭제하거나 수정하지 말 것. */
struct page {
	const struct page_operations *operations;
	void *va;              /* 유저 공간 기준 주소 */
	struct frame *frame;   /* frame에서 page로 되돌아오는 참조 */

	struct hash_elem hash_elem;
	bool writable;
	
	/* 타입별 데이터는 union에 묶여 있다.
	 * 각 함수는 현재 어떤 union 멤버를 써야 하는지 자동으로 판단한다. */
	union {
		struct uninit_page uninit;
		struct anon_page anon;
		struct file_page file;
#ifdef EFILESYS
		struct page_cache page_cache;
#endif
	};
};

/* "frame"의 표현. */
struct frame {
	void *kva;
	struct page *page;
	struct list_elem elem;   // frame table에 연결하기 위한 필드
	struct thread* owner_thread;
};

/* 페이지 연산용 함수 테이블.
 * C에서 "interface"를 구현하는 한 가지 방식이다.
 * "method" 테이블을 구조체 멤버로 넣고,
 * 필요할 때마다 그 함수를 호출한다. */
struct page_operations {
	bool (*swap_in) (struct page *, void *);
	bool (*swap_out) (struct page *);
	void (*destroy) (struct page *);
	bool (*copy) (struct page *dst_page, struct page *src_page);
	enum vm_type type;
};

#define swap_in(page, v) (page)->operations->swap_in ((page), v)
#define swap_out(page) (page)->operations->swap_out (page)
#define destroy(page) \
	if ((page)->operations->destroy) (page)->operations->destroy (page)
#define copy(dst_page, src_page) (src_page)->operations->copy ((dst_page), (src_page))

struct lazy_load_file_arg {
	struct file *file;
	off_t ofs;
	size_t page_read_bytes;
	size_t page_zero_bytes;
};

/* 현재 프로세스 메모리 공간의 표현.
 * 이 구조체 설계는 특정 방식으로 강제하지 않는다.
 * 설계는 전적으로 구현자 선택이다. */
struct supplemental_page_table {
	struct hash hash_table;
};

struct lazy_load_arg {
	struct file *file;
	off_t ofs;
	size_t page_read_bytes;
	size_t page_zero_bytes;
};

#include "threads/thread.h"
void supplemental_page_table_init (struct supplemental_page_table *spt);
bool supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src);
void supplemental_page_table_kill (struct supplemental_page_table *spt);
struct page *spt_find_page (struct supplemental_page_table *spt,
		void *va);
bool spt_insert_page (struct supplemental_page_table *spt, struct page *page);
void spt_remove_page (struct supplemental_page_table *spt, struct page *page);

void vm_init (void);
bool vm_try_handle_fault (struct intr_frame *f, void *addr, bool user,
		bool write, bool not_present);

#define vm_alloc_page(type, upage, writable) \
	vm_alloc_page_with_initializer ((type), (upage), (writable), NULL, NULL)
bool vm_alloc_page_with_initializer (enum vm_type type, void *upage,
		bool writable, vm_initializer *init, void *aux);
void vm_dealloc_page (struct page *page);
bool vm_claim_page (void *va);
enum vm_type page_get_type (struct page *page);

#endif  /* VM_VM_H */
