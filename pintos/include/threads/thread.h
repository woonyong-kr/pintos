#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "lib/kernel/list.h"
#ifdef VM
#include "vm/vm.h"
#endif
/* struct thread는 실행 파일을 포인터로만 보관하므로 전체 정의 대신 전방 선언만 둔다. */
#ifdef USERPROG
struct file;
#endif
/*
 * 스레드 생애 주기의 상태들.
 */
enum thread_status {
	THREAD_RUNNING, // 실행 중인 스레드
	THREAD_READY,   // 실행 중은 아니지만 실행될 준비가 된 상태
	THREAD_BLOCKED, // 어떤 이벤트가 발생하기를 기다리는 상태
	THREAD_DYING    // 곧 파괴될 상태
};

/*
 * 스레드 식별자 타입.
 * 원하는 타입으로 다시 정의할 수 있다.
 */
typedef int tid_t;

struct child_status {
	tid_t tid;                  // 자식 스레드 식별자
	int exit_status;            // 자식 종료 코드
	bool waited;                // 부모가 이미 wait했는지 여부
	bool exited;                // 자식이 이미 종료했는지 여부
	bool orphaned;
	bool fork_success;          // fork 초기화가 성공했는지 여부
	struct semaphore fork_sema; // fork 초기화 완료 알림용 세마포어
	struct semaphore wait_sema; // 자식 종료 알림용 세마포어
	struct list_elem elem;      // 부모의 child_status_list 원소
};

/*
 * 스레드 이름 버퍼의 최대 크기.
 */
#define THREAD_NAME_MAX 16
/*
 * tid_t의 오류 값.
 */
#define TID_ERROR ((tid_t) - 1)

/*
 * 스레드 우선순위.
 */
/*
 * 가장 낮은 우선순위.
 */
#define PRI_MIN 0
/*
 * 기본 우선순위.
 */
#define PRI_DEFAULT 31
/*
 * 가장 높은 우선순위.
 */
#define PRI_MAX 63

/*
 * 커널 스레드 또는 유저 프로세스.
 *
 * 각 스레드 구조체는 자기 전용 4 kB 페이지에 저장된다. `struct thread`
 * 구조체 자체는 페이지의 가장 아래쪽(오프셋 0)에 위치한다. 나머지 영역은
 * 해당 스레드의 커널 스택으로 예약되며, 커널 스택은 페이지의 맨 위
 * (오프셋 4 kB)에서 아래 방향으로 자란다. 그림으로 나타내면 다음과 같다.
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * 여기서 얻을 수 있는 결론은 두 가지다.
 *
 *    1. 첫째, `struct thread`는 너무 커져서는 안 된다. 너무 커지면
 *       커널 스택을 위한 공간이 충분하지 않게 된다. 기본 `struct thread`
 *       크기는 몇 바이트 정도에 불과하다. 아마도 1 kB보다 한참 작게
 *       유지되어야 한다.
 *
 *    2. 둘째, 커널 스택도 너무 커져서는 안 된다. 스택 오버플로우가 발생하면
 *       스레드 상태를 손상시킨다. 따라서 커널 함수는 큰 구조체나 배열을
 *       non-static 지역 변수로 할당하면 안 된다. 대신 malloc()이나
 *       palloc_get_page()를 사용해 동적으로 할당하라.
 *
 * 이러한 문제의 첫 증상은 보통 thread_current()에서 발생하는 assertion
 * 실패다. thread_current()는 현재 실행 중인 스레드의 `struct thread` 안에
 * 있는 `magic` 멤버가 THREAD_MAGIC으로 설정되어 있는지 검사한다.
 * 일반적으로 스택 오버플로우가 발생하면 이 값이 바뀌고, 그 결과 assertion이
 * 발동한다.
 */
/*
 * `elem` 멤버는 두 가지 용도를 가진다. 하나는 run queue(thread.c)에서의
 * 원소이고, 다른 하나는 semaphore 대기 리스트(synch.c)에서의 원소이다.
 * 이렇게 두 가지로 사용할 수 있는 이유는 두 용도가 서로 배타적이기 때문이다.
 * ready 상태의 스레드만 run queue에 있고, blocked 상태의 스레드만
 * semaphore 대기 리스트에 있기 때문이다.
 */
struct thread {
	tid_t tid;                      // 스레드 식별자
	enum thread_status status;      // 현재 스케줄링 상태
	int priority;                   // 현재 유효 우선순위
	int base_priority;              // 우선순위 기부 전 원래 우선순위
	int64_t wake_tick;              // 깨워야 할 절대 tick
	char name[THREAD_NAME_MAX];     // 디버깅용 이름
	struct list donation_list;      // 나에게 우선순위를 기부한 스레드 목록
	struct list_elem donation_elem; // 다른 스레드 donation_list의 원소
	struct lock *waiting_lock;      // 현재 기다리는 락, 우선순위 기부 전파용
	struct list_elem elem;          // ready_list 또는 semaphore waiters의 원소

#ifdef USERPROG
	uint64_t *pml4; // userprog/process.c가 소유하는 페이지 맵 레벨 4

	struct file **fd_table;           // 파일 디스크립터 테이블
	int next_fd;                      // 다음에 할당할 fd 번호
	struct list child_status_list;    // 부모가 소유하는 자식 상태 레코드 목록
	struct child_status *self_status; // 현재 스레드 자신의 child_status
	struct file *running_file;        // 현재 실행 중인 파일, rox 보호용

#endif
#ifdef VM
	/*
	 * 스레드가 소유한 전체 가상 메모리를 위한 테이블.
	 */
	struct supplemental_page_table spt;
	uintptr_t user_rsp; // 커널 모드에서 참조할 마지막 유저 스택 포인터
	void *stack_bottom; // 현재까지 할당된 유저 스택의 최하단 주소
#endif
	struct intr_frame tf; // 문맥 전환에 필요한 레지스터 문맥
	unsigned magic;       // 스택 오버플로우 감지용 마법값
};

/*
 * false(기본값)면 라운드 로빈 스케줄러를 사용한다.
 * true면 다단계 피드백 큐 스케줄러를 사용한다.
 * 커널 명령줄 옵션 "-o mlfqs"로 제어된다.
 */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
struct thread *thread_root (void);
void thread_set_root (struct thread *t);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void do_iret (struct intr_frame *tf);

/* phase 1 alarm clock을 위해 추가한 함수 선언이다. */
void thread_sleep (int64_t wake_ticks);
bool cmp_thread_ticks (const struct list_elem *a, const struct list_elem *b,
                       void *aux);
void thread_awake (int64_t global_ticks);

/* phase 2 Priority Scheduling을 위해 추가한 함수 선언이다. */
void check_preemption (void);
void refresh_priority (struct thread *t);
bool thread_priority (const struct list_elem *a, const struct list_elem *b,
                      void *aux);
bool thread_priority_donation_elem (const struct list_elem *a,
                                    const struct list_elem *b, void *aux);

#endif /* threads/thread.h */
