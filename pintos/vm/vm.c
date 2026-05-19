/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "vm/vm.h"
#include "vm/inspect.h"

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
	/* TODO VM-04: frame table/list와 lock을 초기화한다. eviction을 아직
	 * 구현하지 않더라도 vm_get_frame()이 만든 frame을 추적해야 cleanup과
	 * 이후 eviction 구현으로 이어진다. */
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
	case VM_UNINIT:
		return VM_TYPE (page->uninit.type);
	default:
		return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* 어떤 page가 몇 번 버킷으로 갈지 결정하는 해시값 만들기 */
uint64_t
page_hash (const struct hash_elem *e, void *aux UNUSED) {
	const struct page *page = hash_entry (e, struct page, hash_elem);
	return hash_bytes (&page->va, sizeof page->va);
}

/* 버킷 안에서 같은 page인지 비교하기 */
bool
page_less (const struct hash_elem *a, const struct hash_elem *b,
           void *aux UNUSED) {
	const struct page *pa = hash_entry (a, struct page, hash_elem);
	const struct page *pb = hash_entry (b, struct page, hash_elem);
	return pa->va < pb->va;
}
/*
enum vm_type type = 어떤 종류의 페이지를,
void *upage = 어느 유저 가상주소에,
bool writable = 어떤 권한으로,
vm_initializer *init = 나중에 어떤 함수로 초기화할지, 실제 데이터를 채우는 함수
void *aux = 그리고 그 초기화에 필요한 추가정보는 뭔지
*/

/* SPT에 “아직 실제 frame은 없지만, 나중에 fault 나면 로드할 page 정보”를 등록하는 함수
 * 초기화 함수(initializer)를 사용해서 아직 실제 프레임에 올라가지 않은 대기 상태의 페이지 객체를 만든다.
 *
 * 페이지를 만들고 싶다면 직접 생성하지 말고, 반드시 이 함수나 `vm_alloc_page`를 통해 만들어야 한다.
 */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
                                vm_initializer *init, void *aux) {
	ASSERT (VM_TYPE (type) != VM_UNINIT);

	struct supplemental_page_table *spt = &thread_current ()->spt;
	if (upage == NULL)
		goto err;

	/* upage를 “페이지 시작 주소”로 맞추기 */
	upage = pg_round_down (upage);
	if (spt_find_page (spt, upage) != NULL) {
		goto err;
	}
	/* type에 맞는 page_initializer 고르기 */
	bool (*initializer) (struct page *, enum vm_type, void *) = NULL; /* 나중에 어떤 함수로 페이지를 초기화할지 저장하는 변수 */
	switch (VM_TYPE (type)) {
	case VM_ANON:
		initializer = anon_initializer;
		break;
	case VM_FILE:
		initializer = file_backed_initializer;
		break;
	default:
		goto err;
	}
	struct page *page = malloc (sizeof *page); /* SPT에 오래 보관할 struct page를 heap에 만듦 */
	if (page == NULL)
		goto err;
	uninit_new (page, upage, init, type, aux, initializer); /* 현재는 VM_UNINIT인데 lazy loading을 위해 페이지를 나중에 어떤 타입으로 어떻게 초기화할지 저장 */
	page->writable = writable;
	/* 지금 그냥 heap에 존재하고만 있는 상태니까 spt에 page 삽입해주기 */
	if (!spt_insert_page (spt, page)) {
		free (page); /* heap에 page 만들어둔 상태니까 실패하면 free해주기 */
		goto err;
	}
	return true;
err:
	return false;
}

/* 찾고 싶은 va를 pg_round_down
    임시 page.va에 넣기
    hash_find
    찾은 hash_elem을 struct page로 변환(=hash_elem을 멤버로 포함하고 있는 struct page의 시작 주소를 구한다는 뜻) */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
	struct page real_page;
	struct hash_elem *e;
	real_page.va = pg_round_down (va);
	e = hash_find (&spt->hash_pages, &real_page.hash_elem); /* e와 같은 요소 찾아서 반환 */
	if (e == NULL)
		return NULL;
	return hash_entry (e, struct page, hash_elem);
}

/* SPT에 새 struct page를 등록하려는 함수
같은 va가 있으면 false
같은 va가 없으면 true */
bool
spt_insert_page (struct supplemental_page_table *spt, struct page *page) {
	bool succ = false;
	struct hash_elem *old;
	old = hash_insert (&spt->hash_pages, &page->hash_elem);
	if (old == NULL)
		succ = true;
	return succ;
}

/* page를 SPT 자료구조에서 먼저 제거한 뒤 vm_dealloc_page()를
 * 호출한다. munmap(), process exit, 실패 rollback이 같은 제거 경로를
 * 재사용하게 만든다. */
/* 이제 이 페이지 안 쓸거니까 삭제한다 */
void
spt_remove_page (struct supplemental_page_table *spt UNUSED, struct page *page) {
	vm_dealloc_page (page);
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	/* TODO: The policy for eviction is up to you. */
	/* TODO VM-20: 처음에는 eviction을 구현하지 않고 NULL/PANIC 경로로 둘 수
	 * 있지만, swap tests를 돌리려면 frame table에서 victim을 고르는 정책
	 * clock/second chance 등을 여기에 둔다. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */
	/* TODO VM-21: victim->page에 대해 swap_out(page)을 호출하고, old PTE를
	 * 제거한 뒤 victim->page를 NULL로 되돌려 재사용 가능한 frame으로 반환한다. */

	return NULL;
}

/* user pool에서 실제 RAM frame 하나를 확보하고, 커널이 그 RAM을 다룰 수 있도록 kva를 반환하는 함수
palloc()으로 페이지를 할당받아 frame을 얻는다.
(이 아래부터는 다음에 구현하라고 깃북에 적혀있음)
사용 가능한 페이지가 없으면, 기존 페이지 하나를 eviction 해서 그 공간을 확보한 뒤 반환한다.
이 함수는 항상 유효한 주소를 반환한다.
즉, 유저 풀 메모리가 가득 차 있으면 사용 중인 frame 하나를 내쫓아서(evict) 사용 가능한 메모리 공간을 만든다.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame;
	void *kva;

	frame = malloc (sizeof *frame);
	if (frame == NULL)
		return NULL;

	kva = palloc_get_page (PAL_USER);
	if (kva == NULL) {
		free (frame);
		return NULL;
	}

	frame->kva = kva; /* 실제 4KB 물리 메모리에 접근할 커널 가상주소 */
	frame->page = NULL;
	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);

	return frame; /* 초기화된 frame을 반환 */
}

/* 스택을 늘린다 = 새 가상 페이지를 spt에 등록하고, 물리 프레임 붙이겠다 */
static void
vm_stack_growth (void *addr) {
	/* TODO VM-13: addr을 page boundary로 내린 뒤 VM_ANON stack page를
	 * SPT에 등록하고 즉시 vm_claim_page()한다. USER_STACK 경계와 최대 stack
	 * 크기 제한을 함께 검사해야 한다. */
	void *stack_addr = pg_round_down (addr);
	vm_alloc_page (VM_ANON | VM_MARKER_0, stack_addr, true);
	vm_claim_page (stack_addr);
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
	/* TODO VM-22: copy-on-write를 구현하지 않는 최소 cycle에서는 false를
	 * 반환해 write-protected fault를 죽인다. COW를 할 때는 새 frame 복사와
	 * writable PTE 재설치를 여기서 처리한다. */
	return false;
}

/* page fault가 처리 가능한 fault인지 판단하고, 가능하면(true) 해결하는 함수 */
bool
vm_try_handle_fault (struct intr_frame *f, void *addr,
                     bool user, bool write, bool not_present) {
	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page = NULL;
	if (is_kernel_vaddr (addr) || (addr == NULL) || (!not_present)) {
		return false;
	}
	page = spt_find_page (spt, addr);
	/* SPT에서 page를 못 찾았을 때, 이 fault 주소가 “스택이 커지면서 발생한 정상 page fault”인지 검사 */
	if (page == NULL) {
		void *rsp = f->rsp;

		if ((uint8_t *) addr >= (uint8_t *) rsp - 8 &&
		    (uint8_t *) addr >= (uint8_t *) USER_STACK - (1 << 20) &&
		    (uint8_t *) addr < (uint8_t *) USER_STACK) {
			vm_stack_growth (addr);
			return true;
		}
		return false;
	}
	if (write && !page->writable) {
		return false;
	}

	return vm_claim_page (addr);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* VA로 SPT에서 page를 찾고, 아직 frame이 없는 page를 실제 메모리에 올리는 것 */
bool
vm_claim_page (void *va UNUSED) {
	struct page *page = NULL;
	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* TODO VM-11: va를 page boundary로 내리고 spt_find_page()로 page를 찾는다.
	 page가 없거나 이미 frame이 있으면 정책에 맞게 처리하고, 있으면
	 * vm_do_claim_page(page)를 호출한다. */

	va = pg_round_down (va);
	page = spt_find_page (spt, va);

	if (page == NULL) {
		return false;
	}
	if (page->frame != NULL) {
		return true;
	}

	return vm_do_claim_page (page);
}

/*
1. frame 확보
   vm_get_frame()
2. page <-> frame 연결
3. PML4 매핑 추가
   pml4_set_page()
4. 실제 데이터 로드
   swap_in()
5. 성공 반환
보조 페이지 테이블에 있던 PAGE를 실제 메모리 프레임에 올린 뒤,
CPU/MMU가 해당 가상 주소로 접근할 수 있도록 va -> kva 매핑을 만든다.
*/

/* 실제 frame 할당, PML4 매핑, swap_in 수행
SPT에 있던 struct page를 실제 물리 frame에 올리고, PML4에 매핑한 뒤, 내용을 채우는 함수 */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();
	if (frame == NULL)
		return false;

	/* 양방향 연결 */
	frame->page = page;  /* 물리 frame이 어떤 가상 page를 담고 있는지 기록 */
	page->frame = frame; /* 가상 page가 어느 물리 frame에 올라와 있는지도 기록 */

	/* 현재 실행 중인 thread의 PML4에 page->va  →  frame->kva 매핑을 추가 */
	if (!pml4_set_page (thread_current ()->pml4, page->va, frame->kva, page->writable)) {
		frame->page = NULL;
		page->frame = NULL;
		palloc_free_page (frame->kva);
		free (frame);
		return false;
	}
	return swap_in (page, frame->kva); /* swap_in()으로 실제 데이터를 frame에 채우기 */
}

/* SPT의 hash_table을 page_hash/page_less 규칙으로 초기화한다 */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	hash_init (&spt->hash_pages, page_hash, page_less, NULL);
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
                              struct supplemental_page_table *src UNUSED) {
	/* TODO VM-18: fork 최소 통과를 위해 src의 각 page를 dst에 복제한다.
	 * UNINIT은 aux를 deep copy하거나 file reference를 다시 열고, resident
	 * page는 child page를 만들고 claim한 뒤 frame bytes를 복사한다. */
	return false;
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	/* TODO VM-19: SPT의 모든 page를 순회하며 dirty mmap page는 write-back,
	 * resident frame은 palloc_free_page(), file/aux metadata는 해제한다.
	 * 순회 중 SPT entry 제거와 page destroy 순서를 분명히 해야 한다. */
}
