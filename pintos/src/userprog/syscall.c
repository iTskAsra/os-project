#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "filesys/file.h"
#include "lib/kernel/list.h"
#include "filesys/filesys.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "devices/input.h"
#include "devices/shutdown.h"
#include "userprog/process.h"
#include "threads/vaddr.h"

#ifdef USERPROG
#include "userprog/pagedir.h"
#endif
static void syscall_handler(struct intr_frame *);
static void syscall_exit(struct intr_frame *, uint32_t *);
static void syscall_practice(struct intr_frame *, uint32_t *);
#ifdef USERPROG
static void syscall_write(struct intr_frame *, uint32_t *);
static void syscall_create(struct intr_frame *, uint32_t *);
static void syscall_remove(struct intr_frame *, uint32_t *);
static void syscall_open(struct intr_frame *, uint32_t *);
static void syscall_filesize(struct intr_frame *, uint32_t *);
static void syscall_read(struct intr_frame *, uint32_t *);
static void syscall_seek(struct intr_frame *, uint32_t *);
static void syscall_tell(struct intr_frame *, uint32_t *);
static void syscall_close(struct intr_frame *, uint32_t *);
#endif
static void syscall_halt(struct intr_frame *, uint32_t *);
static void syscall_wait(struct intr_frame *, uint32_t *);
static void syscall_exec(struct intr_frame *, uint32_t *);

void syscall_init(void)
{
  intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static bool
is_valid_ptr(const void *ptr)
{
  return ptr != NULL && is_user_vaddr(ptr)
#ifdef USERPROG
         && pagedir_get_page(thread_current()->pagedir, ptr) != NULL
#endif
      ;
}

void exit_error()
{
  struct thread *cur = thread_current();
  cur->wait_info->exit_status = -1;
  printf("%s: exit(%d)\n", cur->name, -1);
  thread_exit();
}

#ifdef USERPROG

static file_descriptor_t *
find_file_descriptor(int fd)
{
  struct list *file_descriptors = &thread_current()->file_descriptors;

  if (list_empty(file_descriptors))
  {
    return NULL;
  }

  struct list_elem *current_element;
  file_descriptor_t *current_fd;

  for (current_element = list_begin(file_descriptors);
       current_element != list_end(file_descriptors);
       current_element = list_next(current_element))
  {
    current_fd = list_entry(current_element, file_descriptor_t, elem);

    if (current_fd->fileno == fd)
    {
      return current_fd;
    }
  }

  return NULL;
}

static void syscall_write(struct intr_frame *f, uint32_t *args)
{
  int fd = args[1];
  void *buffer = (void *)args[2];
  unsigned size = args[3];

  if (!is_valid_ptr(buffer) ||
      !is_valid_ptr(buffer + size - 1) ||
      fd == 0 /* stdin */)
  {
    f->eax = -1;
    exit_error();
  }

  if (fd == 1 /* stdout */)
  {
    putbuf(buffer, size);
    f->eax = size;
    return;
  }

  file_descriptor_t *file_descriptor = find_file_descriptor(fd);

  if (file_descriptor == NULL)
  {
    f->eax = -1;
    exit_error();
  }

  f->eax = file_write(file_descriptor->file, buffer, size);
}

static void syscall_create(struct intr_frame *f, uint32_t *args)
{
  const char *file = (const char *)args[1];
  unsigned initial_size = args[2];

  if (!is_valid_ptr(file) ||
      !is_valid_ptr(file + strlen(file) - 1))
  {
    f->eax = -1;
    exit_error();
  }

  f->eax = filesys_create(file, initial_size);
}

static void syscall_remove(struct intr_frame *f, uint32_t *args)
{
  const char *file = (const char *)args[1];

  if (!is_valid_ptr(file) ||
      !is_valid_ptr(file + strlen(file) - 1))
  {
    f->eax = -1;
    exit_error();
  }

  f->eax = filesys_remove(file);
}

static void syscall_open(struct intr_frame *f, uint32_t *args)
{
  const char *file = (const char *)args[1];

  if (!is_valid_ptr(file) ||
      !is_valid_ptr(file + strlen(file) - 1))
  {
    f->eax = -1;
    exit_error();
  }

  struct file *opened_file = filesys_open(file);

  if (opened_file == NULL)
  {
    f->eax = -1;
    return;
  }

  struct thread *current_thread = thread_current();
  int fileno = current_thread->last_fileno++;
  file_descriptor_t *file_descriptor = malloc(sizeof(file_descriptor_t));
  file_descriptor->file = opened_file;
  file_descriptor->fileno = fileno;
  list_push_back(&current_thread->file_descriptors, &file_descriptor->elem);
  f->eax = fileno;
}

static void syscall_filesize(struct intr_frame *f, uint32_t *args)
{
  int fd = args[1];

  if (fd == 0 || fd == 1)
  {
    f->eax = -1;
    exit_error();
  }

  file_descriptor_t *file_descriptor = find_file_descriptor(fd);

  if (file_descriptor == NULL)
  {
    f->eax = -1;
    exit_error();
  }

  f->eax = file_length(file_descriptor->file);
}

static void syscall_read(struct intr_frame *f, uint32_t *args)
{
  int fd = args[1];
  void *buffer = (void *)args[2];
  unsigned size = args[3];

  if (!is_valid_ptr(buffer) ||
      !is_valid_ptr(buffer + size - 1) ||
      fd == 1 /* stdout */)
  {
    f->eax = -1;
    exit_error();
  }

  if (fd == 0 /* stdin */)
  {
    for (unsigned i = 0; i < size; i++)
    {
      ((char *)buffer)[i] = input_getc();
    }
    f->eax = size;
    return;
  }

  file_descriptor_t *file_descriptor = find_file_descriptor(fd);

  if (file_descriptor == NULL)
  {
    f->eax = -1;
    exit_error();
  }

  f->eax = file_read(file_descriptor->file, buffer, size);
}

static void syscall_seek(struct intr_frame *f, uint32_t *args)
{
  int fd = args[1];
  unsigned position = args[2];
  file_descriptor_t *file_descriptor = find_file_descriptor(fd);
  file_seek(file_descriptor->file, position);
  f->eax = 0;
}

static void syscall_tell(struct intr_frame *f, uint32_t *args)
{
  int fd = args[1];

  file_descriptor_t *file_descriptor = find_file_descriptor(fd);

  if (file_descriptor == NULL)
  {
    f->eax = -1;
    exit_error();
  }

  f->eax = file_tell(file_descriptor->file);
}

static void syscall_close(struct intr_frame *f, uint32_t *args)
{
  int fd = args[1];

  file_descriptor_t *file_descriptor = find_file_descriptor(fd);

  if (file_descriptor == NULL)
  {
    f->eax = -1;
    exit_error();
  }

  file_close(file_descriptor->file);
  list_remove(&file_descriptor->elem);
  free(file_descriptor);
  f->eax = 0;
}

#endif

static void syscall_exit(struct intr_frame *f, uint32_t *args)
{
  int status = args[1];
  f->eax = status;
  printf("%s: exit(%d)\n", (char *)&thread_current()->name, status);
  thread_exit();
}

static void syscall_practice(struct intr_frame *f, uint32_t *args)
{
  int i = args[1];
  f->eax = i + 1;
}

static void syscall_halt(struct intr_frame *f, uint32_t *args)
{
  shutdown_power_off();
}

static void syscall_wait(struct intr_frame *f, uint32_t *args)
{
  tid_t pid = args[1];
  f->eax = process_wait(pid);
}

static void syscall_exec(struct intr_frame *f, uint32_t *args)
{
  const char *file = (const char *)args[1];

  if (!is_valid_ptr(file) ||
      !is_valid_ptr(file + strlen(file) - 1))
  {
    f->eax = -1;
    exit_error();
  }

  f->eax = process_execute(file);
}

syscall_descriptor_t syscall_table[] = {
#ifdef USERPROG
    {SYS_WRITE, &syscall_write, 1},
    {SYS_CREATE, &syscall_create, 1},
    {SYS_REMOVE, &syscall_remove, 1},
    {SYS_OPEN, &syscall_open, 1},
    {SYS_FILESIZE, &syscall_filesize, 1},
    {SYS_READ, &syscall_read, 1},
    {SYS_SEEK, &syscall_seek, 1},
    {SYS_TELL, &syscall_tell, 1},
    {SYS_CLOSE, &syscall_close, 1},
#endif
    {SYS_EXIT, &syscall_exit, 0},
    {SYS_PRACTICE, &syscall_practice, 0},
    {SYS_HALT, &syscall_halt, 0},
    {SYS_WAIT, &syscall_wait, 0},
    {SYS_EXEC, &syscall_exec, 0},
};

static void
syscall_handler(struct intr_frame *f UNUSED)
{
  if (!is_valid_ptr(f->esp))
  {
    exit_error();
  }

  uint32_t *args = ((uint32_t *)f->esp);
  if (!is_valid_ptr(args))
  {
    exit_error();
  }
  /*
   * The following print statement, if uncommented, will print out the syscall
   * number whenever a process enters a system call. You might find it useful
   * when debugging. It will cause tests to fail, however, so you should not
   * include it in your final submission.
   */

  /* printf("System call number: %d\n", args[0]); */

  int syscall_number = args[0];

  for (unsigned i = 0; i < (sizeof(syscall_table) / sizeof(syscall_descriptor_t)); i++)
  {
    if (syscall_number == syscall_table[i].number)
    {
      if (syscall_table[i].is_file_op)
        lock_acquire(&filesys_lock);

      syscall_table[i].syscall_func(f, args);

      if (syscall_table[i].is_file_op)
        lock_release(&filesys_lock);

      return;
    }
  }

  exit_error();
}
