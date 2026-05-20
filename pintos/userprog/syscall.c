#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

/* 추가 임포트 */
#include "threads/init.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "devices/input.h"
#include "lib/string.h"
#ifdef VM
#include "vm/vm.h"
#endif

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/* 추가 함수들 */
void halt (void);
void exit (int status);
tid_t fork (const char *thread_name, struct intr_frame *f);
int exec (const char *cmd_line, struct intr_frame *f);
int write (int fd, const void *buffer, unsigned size, struct intr_frame *f);
int read (int fd, void *buffer, unsigned size, struct intr_frame *f);
bool create (const char *file, unsigned initial_size, struct intr_frame *f);
int open (const char *file, struct intr_frame *f);
void close (int fd);
bool remove (const char *file, struct intr_frame *f);
void seek (int fd, unsigned position);
unsigned tell (int fd);
int filesize (int fd);
void check_address (const void *addr);
size_t* mmap(void *addr, size_t length, int writable, int fd, off_t offset);
void munmap (void *addr);
static bool user_page_present (struct thread *curr, const void *addr);
static bool user_page_accessible (struct thread *curr, const void *addr,
                                  bool write);
static bool try_claim_user_addr (const void *addr, bool write,
                                 struct intr_frame *f);
static void validate_user_addr (const void *addr, bool write,
                                struct intr_frame *f);
static void validate_user_buffer (const void *buffer, unsigned size,
                                  bool write, struct intr_frame *f);
static void copy_in (void *dst, const void *src, size_t size,
                     struct intr_frame *f);
static void copy_out (void *dst, const void *src, size_t size,
                      struct intr_frame *f);
static char *copy_in_string (const char *str, struct intr_frame *f);

/* 추가 변수들 */
struct lock filesys_lock;

/*
 * 시스템 콜.
 *
 * 이전에는 시스템 콜 서비스가 인터럽트 핸들러
 * (예: Linux의 int 0x80)로 처리되었다. 하지만 x86-64에서는
 * 제조사가 `syscall` 명령어라는 더 효율적인 시스템 콜 요청 경로를 제공한다.
 *
 * syscall 명령어는 Model Specific Register(MSR)에 저장된 값을 읽어 동작한다.
 * 자세한 내용은 매뉴얼을 참고하라.
 */

/*
 * 세그먼트 셀렉터 MSR.
 */
#define MSR_STAR 0xc0000081
/*
 * Long mode SYSCALL 대상.
 */
#define MSR_LSTAR 0xc0000082
/*
 * eflags용 마스크.
 */
#define MSR_SYSCALL_MASK 0xc0000084

void
syscall_init (void) {
	write_msr (MSR_STAR, ((uint64_t) SEL_UCSEG - 0x10) << 48 |
	                             ((uint64_t) SEL_KCSEG) << 32);
	write_msr (MSR_LSTAR, (uint64_t) syscall_entry);

	/*
	 * 인터럽트 서비스 루틴은 syscall_entry가 유저랜드 스택을 커널 모드
	 * 스택으로 교체하기 전까지 어떤 인터럽트도 처리해서는 안 된다. 따라서
	 * FLAG_FL을 마스킹했다.
	 */
	write_msr (MSR_SYSCALL_MASK,
	           FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

	/* 추가 초기화 */
	lock_init (&filesys_lock);
}

/*
 * 메인 시스템 콜 인터페이스.
 */
void
syscall_handler (struct intr_frame *f UNUSED) {
#ifdef VM
	thread_current ()->user_rsp = f->rsp;
#endif
	switch (f->R.rax) {
	case SYS_HALT:
		halt ();
		break;
	case SYS_FORK:
		f->R.rax = fork ((const char *) f->R.rdi, f);
		break;
	case SYS_WAIT:
		f->R.rax = process_wait ((tid_t) f->R.rdi);
		break;
	case SYS_EXEC:
		f->R.rax = exec ((const char *) f->R.rdi, f);
		break;
	case SYS_READ:
		f->R.rax = read (f->R.rdi, (void *) f->R.rsi, f->R.rdx, f);
		break;
	case SYS_WRITE:
		/* fd, buffer, size를 전달받는다. */
		f->R.rax = write (f->R.rdi, (const void *) f->R.rsi, f->R.rdx, f);
		break;
	case SYS_EXIT:
		/* 종료 상태값 받음 */
		exit (f->R.rdi);
		break;
	case SYS_CREATE:
		f->R.rax = create ((const char *) f->R.rdi, f->R.rsi, f);
		break;
	case SYS_OPEN:
		f->R.rax = open ((const char *) f->R.rdi, f);
		break;
	case SYS_CLOSE:
		close (f->R.rdi);
		break;
	case SYS_FILESIZE:
		f->R.rax = filesize (f->R.rdi);
		break;
	case SYS_REMOVE:
		f->R.rax = remove ((const char *) f->R.rdi, f);
		break;
	case SYS_SEEK:
		seek (f->R.rdi, f->R.rsi);
		break;
	case SYS_TELL:
		f->R.rax = tell (f->R.rdi);
		break;
	case SYS_MMAP:
		f->R.rax = mmap(f->R.rdi, f->R.rsi, f->R.rdx,
		                f->R.r10, f->R.r8);
		break;
	case SYS_MUNMAP:
		munmap(f->R.rdi);
		break;
	default:
		break;
	}
}

/* 구현 명령어들 */
/* 시스템 전체 셧다운 */
void
halt (void) {
	power_off ();
}

tid_t
fork (const char *thread_name, struct intr_frame *if_) {
	char *name_copy = copy_in_string (thread_name, if_);
	tid_t tid = process_fork (name_copy, if_);

	palloc_free_page (name_copy);
	return tid;
}

int
exec (const char *cmd_line, struct intr_frame *f) {
	char *cmd_copy;
	int status;

	cmd_copy = copy_in_string (cmd_line, f);
	status = process_exec (cmd_copy);
	if (status < 0)
		exit (-1);
	return status;
}

void
exit (int status) {
	process_exit_with_status (status);
}

int
write (int fd, const void *buffer, unsigned size, struct intr_frame *f) {
	uint8_t *kpage;
	unsigned written = 0;

	if (size == 0)
		return 0;

	if (fd == 0)
		return -1;

	validate_user_buffer (buffer, size, false, f);

	kpage = palloc_get_page (0);
	if (kpage == NULL)
		return -1;

	while (written < size) {
		size_t chunk = size - written;
		int bytes_written;

		if (chunk > PGSIZE)
			chunk = PGSIZE;

		copy_in (kpage, (const uint8_t *) buffer + written, chunk, f);

		lock_acquire (&filesys_lock);
		if (fd == 1) {
			putbuf ((const char *) kpage, chunk);
			bytes_written = chunk;
		} else {
			struct file *file = process_get_file (fd);
			bytes_written = file == NULL ? -1 : file_write (file, kpage, chunk);
		}
		lock_release (&filesys_lock);

		if (bytes_written < 0) {
			if (written == 0)
				written = (unsigned) -1;
			break;
		}
		if (bytes_written == 0)
			break;

		written += bytes_written;
		if ((size_t) bytes_written < chunk)
			break;
	}

	palloc_free_page (kpage);
	return (int) written;
}

bool
create (const char *file, unsigned initial_size, struct intr_frame *f) {
	char *file_copy;
	bool result;

	file_copy = copy_in_string (file, f);
	lock_acquire (&filesys_lock);
	result = filesys_create (file_copy, initial_size);
	lock_release (&filesys_lock);
	palloc_free_page (file_copy);
	return result;
}

int
open (const char *file, struct intr_frame *f) {
	char *file_copy;
	struct file *opened_file;
	int fd;

	file_copy = copy_in_string (file, f);
	lock_acquire (&filesys_lock);

	// 열린 파일 객체의 주소
	opened_file = filesys_open (file_copy);

	if (opened_file == NULL) {
		lock_release (&filesys_lock);
		palloc_free_page (file_copy);
		return -1;
	}

	// 열린 파일 f를 fd테이블 빈칸에 넣고, 그 칸 번호를 돌려준다
	fd = process_add_file (opened_file);
	if (fd == -1)
		file_close (opened_file);
	lock_release (&filesys_lock);
	palloc_free_page (file_copy);
	return fd;
}

void
close (int fd) {
	process_close_file (fd);
}

int
read (int fd, void *buffer, unsigned size, struct intr_frame *f) {
	uint8_t *kpage;
	unsigned read_bytes = 0;

	if (size == 0)
		return 0;

	if (fd == 1)
		return -1;

	validate_user_buffer (buffer, size, true, f);

	kpage = palloc_get_page (0);
	if (kpage == NULL)
		return -1;

	while (read_bytes < size) {
		size_t chunk = size - read_bytes;
		int bytes_read = 0;

		if (chunk > PGSIZE)
			chunk = PGSIZE;

		if (fd == 0) {
			while ((size_t) bytes_read < chunk)
				kpage[bytes_read++] = input_getc ();
		} else {
			struct file *file;

			lock_acquire (&filesys_lock);
			file = process_get_file (fd);
			bytes_read = file == NULL ? -1 : file_read (file, kpage, chunk);
			lock_release (&filesys_lock);
		}

		if (bytes_read <= 0) {
			if (bytes_read < 0 && read_bytes == 0)
				read_bytes = (unsigned) -1;
			break;
		}

		copy_out ((uint8_t *) buffer + read_bytes, kpage, bytes_read, f);
		read_bytes += bytes_read;
		if ((size_t) bytes_read < chunk)
			break;
	}

	palloc_free_page (kpage);
	return (int) read_bytes;
}

int
filesize (int fd) {
	struct file *f;
	int length = -1;

	lock_acquire (&filesys_lock);
	f = process_get_file (fd);
	if (f != NULL)
		length = file_length (f);
	lock_release (&filesys_lock);
	return length;
}

bool
remove (const char *file, struct intr_frame *f) {
	char *file_copy;
	bool result;

	file_copy = copy_in_string (file, f);
	lock_acquire (&filesys_lock);
	result = filesys_remove (file_copy);
	lock_release (&filesys_lock);
	palloc_free_page (file_copy);

	return result;
}

void
seek (int fd, unsigned position) {
	struct file *f;

	lock_acquire (&filesys_lock);
	f = process_get_file (fd);
	if (f != NULL)
		file_seek (f, position);
	lock_release (&filesys_lock);
}
unsigned
tell (int fd) {
	struct file *f;
	unsigned position = 0;

	lock_acquire (&filesys_lock);
	f = process_get_file (fd);
	if (f != NULL)
		position = file_tell (f);
	lock_release (&filesys_lock);
	return position;
}
/* 여기서부턴 헬퍼 함수 기술 */
static bool
user_page_present (struct thread *curr, const void *addr) {
	uint64_t *pte;

	if (curr == NULL || curr->pml4 == NULL)
		return false;

	pte = pml4e_walk (curr->pml4, (uint64_t) addr, 0);
	if (pte == NULL || (*pte & PTE_P) == 0)
		return false;

	return true;
}

static bool
user_page_accessible (struct thread *curr, const void *addr, bool write) {
	uint64_t *pte;

	if (!user_page_present (curr, addr))
		return false;

	pte = pml4e_walk (curr->pml4, (uint64_t) addr, 0);
	return !write || is_writable (pte);
}

static bool
try_claim_user_addr (const void *addr, bool write, struct intr_frame *f) {
	struct thread *curr = thread_current ();

#ifndef VM
	(void) f;
#endif

	if (addr == NULL || curr == NULL || curr->pml4 == NULL)
		return false;

	if (!is_user_vaddr (addr))
		return false;

	if (user_page_accessible (curr, addr, write))
		return true;

	if (write && user_page_present (curr, addr))
		return false;

#ifdef VM
	void *upage = pg_round_down (addr);
	struct page *page = spt_find_page (&curr->spt, upage);

	if (page != NULL) {
		if (!vm_claim_page (upage))
			return false;
		return user_page_accessible (curr, addr, write);
	}

	if (f != NULL) {
		uintptr_t uaddr = (uintptr_t) addr;
		uintptr_t rsp = f->rsp;

		if (uaddr < (uintptr_t) USER_STACK && rsp >= 8 && uaddr >= rsp - 8) {
			if (!vm_try_handle_fault (f, (void *) addr, true, write, true))
				return false;
			return user_page_accessible (curr, addr, write);
		}
	}
#endif

	return false;
}

static void
validate_user_addr (const void *addr, bool write, struct intr_frame *f) {
	if (!try_claim_user_addr (addr, write, f))
		exit (-1);
}

void
check_address (const void *addr) {
	validate_user_addr (addr, false, NULL);
}

static void
validate_user_buffer (const void *buffer, unsigned size, bool write,
                      struct intr_frame *f) {
	const uint8_t *addr;
	const uint8_t *end;
	uintptr_t start;
	uintptr_t last;

	if (size == 0)
		return;

	start = (uintptr_t) buffer;
	last = start + size - 1;
	if (buffer == NULL || last < start)
		exit (-1);

	addr = buffer;
	end = (const uint8_t *) buffer + size;
	while (addr < end) {
		uintptr_t next_page;

		validate_user_addr (addr, write, f);

		next_page = (uintptr_t) pg_round_down (addr) + PGSIZE;
		if (next_page <= (uintptr_t) addr)
			break;
		addr = (const uint8_t *) next_page;
	}
}

static void
copy_in (void *dst, const void *src, size_t size, struct intr_frame *f) {
	uint8_t *kernel = dst;
	const uint8_t *user = src;

	while (size > 0) {
		size_t chunk;

		validate_user_addr (user, false, f);
		chunk = PGSIZE - pg_ofs (user);
		if (chunk > size)
			chunk = size;

		memcpy (kernel, user, chunk);
		kernel += chunk;
		user += chunk;
		size -= chunk;
	}
}

static void
copy_out (void *dst, const void *src, size_t size, struct intr_frame *f) {
	uint8_t *user = dst;
	const uint8_t *kernel = src;

	while (size > 0) {
		size_t chunk;

		validate_user_addr (user, true, f);
		chunk = PGSIZE - pg_ofs (user);
		if (chunk > size)
			chunk = size;

		memcpy (user, kernel, chunk);
		user += chunk;
		kernel += chunk;
		size -= chunk;
	}
}

static char *
copy_in_string (const char *str, struct intr_frame *f) {
	size_t len = 0;
	char *kernel;

	for (; len < PGSIZE; len++) {
		const char *uaddr = str + len;

		if (!try_claim_user_addr (uaddr, false, f))
			exit (-1);

		if (*uaddr == '\0')
			break;
	}

	if (len == PGSIZE)
		exit (-1);

	kernel = palloc_get_page (0);
	if (kernel == NULL)
		exit (-1);

	memcpy (kernel, str, len + 1);
	return kernel;
}

size_t*
mmap(void *addr, size_t length, int writable, int fd, off_t offset){
	RETURN_NULL_IF(addr == NULL || pg_ofs(addr) || is_kernel_vaddr(addr));
	RETURN_NULL_IF(offset % PGSIZE != 0);
	RETURN_NULL_IF(length <= 0);
	void *end = addr + length - 1;
	RETURN_NULL_IF(is_kernel_vaddr(end));

	struct file* file = thread_current()->fd_table[fd];
	RETURN_NULL_IF(file == NULL);

	return do_mmap(addr, length, writable, file, offset);
}

void 
munmap (void *addr){
	RETURN_IF(addr == NULL);
	return do_munmap(addr);
}