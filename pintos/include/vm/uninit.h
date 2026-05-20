#ifndef VM_UNINIT_H
#define VM_UNINIT_H
#include "vm/vm.h"

struct page;
enum vm_type;
typedef bool vm_initializer (struct page *, void *aux);

/* 초기화되지 않은 페이지.
 * "지연 로딩"을 구현하기 위한 타입이다. */
struct uninit_page {
	/* 페이지 내용을 초기화하는 함수. */
	vm_initializer *init;
	enum vm_type type;
	void *aux;
	/* struct page를 초기화하고 kva를 va에 매핑하는 함수. */
	bool (*page_initializer) (struct page *, enum vm_type, void *kva);
};
 
void uninit_new (struct page *page, void *va, vm_initializer *init,
		enum vm_type type, void *aux,
		bool (*initializer)(struct page *, enum vm_type, void *kva));
#endif
