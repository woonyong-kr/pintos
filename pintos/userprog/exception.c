#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "intrinsic.h"
#include "userprog/process.h"


/*
 * 처리된 페이지 폴트의 개수.
 */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);

/*
 * 유저 프로그램이 일으킬 수 있는 인터럽트에 대한 핸들러를 등록한다.
 *
 * 실제 Unix 계열 운영체제에서는 이러한 인터럽트 대부분이 [SV-386] 3-24와
 * 3-25에 설명된 것처럼 시그널 형태로 유저 프로세스에 전달된다. 하지만 우리는
 * 시그널을 구현하지 않으므로, 대신 해당 유저 프로세스를 단순히 종료시킨다.
 *
 * 페이지 폴트는 예외다. 여기서는 다른 예외와 동일하게 처리하지만,
 * 가상 메모리를 구현하려면 이 부분을 바꿔야 한다.
 *
 * 각 예외에 대한 설명은 [IA32-v3a] 5.15절 "Exception and Interrupt
 * Reference"를 참고하라.
 */
void
exception_init (void) {
	/*
	 * 이 예외들은 유저 프로그램이 명시적으로 발생시킬 수 있다.
	 * 예를 들어 INT, INT3, INTO, BOUND 명령으로 발생시킬 수 있다.
	 * 따라서 DPL==3으로 설정하여 유저 프로그램이 이러한 명령으로
	 * 예외를 호출할 수 있도록 한다.
	 */
	intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
	intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
	intr_register_int (5, 3, INTR_ON, kill,
			"#BR BOUND Range Exceeded Exception");

	/*
	 * 이 예외들은 DPL==0을 가지므로 유저 프로세스가 INT 명령으로
	 * 직접 호출할 수 없다. 하지만 간접적으로는 여전히 발생할 수 있다.
	 * 예를 들어 #DE는 0으로 나누면 발생할 수 있다.
	 */
	intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
	intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
	intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
	intr_register_int (7, 0, INTR_ON, kill,
			"#NM Device Not Available Exception");
	intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
	intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
	intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
	intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
	intr_register_int (19, 0, INTR_ON, kill,
			"#XF SIMD Floating-Point Exception");

	/*
	 * 대부분의 예외는 인터럽트를 켠 상태로 처리해도 된다.
	 * 하지만 페이지 폴트는 fault 주소가 CR2에 저장되므로, 그 값을 보존하기 위해
	 * 인터럽트를 꺼야 한다.
	 */
	intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/*
 * 예외 통계를 출력한다.
 */
void
exception_print_stats (void) {
	printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/*
 * 유저 프로세스가 일으킨 것으로 보이는 예외를 처리하는 핸들러.
 */
static void
kill (struct intr_frame *f) {
	/*
	 * 이 인터럽트는 아마도 유저 프로세스가 일으킨 것이다.
	 * 예를 들어 프로세스가 매핑되지 않은 가상 메모리에 접근하려 했을 수 있다
	 * (페이지 폴트). 지금은 유저 프로세스를 단순히 종료한다. 이후에는
	 * 커널에서 페이지 폴트를 처리하고 싶을 것이다. 실제 Unix 계열 운영체제는
	 * 대부분의 예외를 시그널로 프로세스에 다시 전달하지만, 우리는 그것을
	 * 구현하지 않는다.
	 */

	/*
	 * 인터럽트 프레임의 코드 세그먼트 값은 예외가 어디서 발생했는지 알려준다.
	 */
	switch (f->cs) {
		case SEL_UCSEG:
			/*
			 * 유저 코드 세그먼트이므로, 예상대로 유저 예외다.
			 * 유저 프로세스를 종료한다.
			 */
			printf ("%s: dying due to interrupt %#04llx (%s).\n",
					thread_name (), f->vec_no, intr_name (f->vec_no));
			intr_dump_frame (f);
			thread_exit ();

		case SEL_KCSEG:
			/*
			 * 커널 코드 세그먼트이며, 이는 커널 버그를 의미한다.
			 * 커널 코드는 예외를 던지면 안 된다.
			 * (페이지 폴트가 커널 예외를 유발할 수는 있지만, 그 경우도 여기에
			 * 도달해서는 안 된다.) 문제를 분명히 하기 위해 커널 패닉을 낸다.
			 */
			intr_dump_frame (f);
			PANIC ("Kernel bug - unexpected interrupt in kernel");

		default:
			/*
			 * 다른 코드 세그먼트인가? 일어나면 안 된다.
			 * 커널을 패닉시킨다.
			 */
			printf ("Interrupt %#04llx (%s) in unknown segment %04x\n",
					f->vec_no, intr_name (f->vec_no), f->cs);
			thread_exit ();
	}
}

/*
 * 페이지 폴트 핸들러.
 * 이는 가상 메모리를 구현하기 위해 채워 넣어야 하는 스켈레톤 코드다.
 * project 2의 일부 해법도 이 코드를 수정해야 할 수 있다.
 *
 * 진입 시 fault가 발생한 주소는 CR2(Control Register 2)에 있고,
 * fault 정보는 exception.h의 PF_* 매크로에 설명된 형식으로 F의
 * error_code 멤버에 들어 있다. 아래 예제 코드는 그 정보를 어떻게
 * 해석하는지 보여 준다. 두 항목에 대한 자세한 내용은 [IA32-v3a]
 * 5.15절 "Interrupt 14--Page Fault Exception (#PF)" 설명을 참고하라.
 */
static void
page_fault (struct intr_frame *f) {
	/*
	 * 참이면 페이지가 존재하지 않았고, 거짓이면 읽기 전용 페이지에 쓰기를 시도한 것이다.
	 */
	bool not_present;
	/*
	 * 참이면 쓰기 접근이고, 거짓이면 읽기 접근이다.
	 */
	bool write;
	/*
	 * 참이면 유저 접근이고, 거짓이면 커널 접근이다.
	 */
	bool user;
	/*
	 * fault가 발생한 주소.
	 */
	void *fault_addr;

	/*
	 * fault를 일으킨 주소, 즉 접근 시도로 인해 fault가 발생한 가상 주소를 얻는다.
	 * 이 주소는 코드나 데이터를 가리킬 수 있다.
	 * 또한 이것이 fault를 일으킨 명령어의 주소(f->rip)와 같은 것은 아니다.
	 */

	fault_addr = (void *) rcr2();

	/*
	 * 인터럽트를 다시 켠다.
	 * 인터럽트를 껐던 이유는 CR2가 바뀌기 전에 반드시 읽기 위해서뿐이다.
	 */
	intr_enable ();


	/*
	 * 원인을 판별한다.
	 */
	not_present = (f->error_code & PF_P) == 0;
	write = (f->error_code & PF_W) != 0;
	user = (f->error_code & PF_U) != 0;

#ifdef VM
	/*
	 * 프로젝트 3 이후를 위한 코드다.
	 */
	if (vm_try_handle_fault (f, fault_addr, user, write, not_present))
		return;
#endif


	/*
	 * 페이지 폴트 수를 센다.
	 */
	page_fault_cnt++;

		/* 유저가 인자값으로 NULL을 보낼때 커널이 죽는 커널 패닉이(case SEL_KCSEG) 아닌 유저 프로세스를 종료시켜야함(exit -1) */
	if (f->cs == SEL_UCSEG)
	process_exit_with_status (-1);

	/*
	 * 실제 fault라면 정보를 출력하고 종료한다.
	 */
	printf ("Page fault at %p: %s error %s page in %s context.\n",
			fault_addr,
			not_present ? "not present" : "rights violation",
			write ? "writing" : "reading",
			user ? "user" : "kernel");
	kill (f);
}

