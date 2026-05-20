#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include <stddef.h>
#include "filesys/off_t.h"
#include "threads/thread.h"

struct file;

tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);
void process_exit_with_status (int status);

/* 파일 디스크립터 헬퍼 */
int process_add_file (struct file *f);
struct file *process_get_file (int fd);
void process_close_file (int fd);

/* lazy_load_segment()가 page fault 시점에 파일 page를 채우는 데 필요한 정보. */


#endif /* userprog/process.h */
