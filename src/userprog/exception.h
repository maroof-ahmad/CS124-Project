#ifndef USERPROG_EXCEPTION_H
#define USERPROG_EXCEPTION_H

#include <stdbool.h>
#include "threads/interrupt.h"

/*! Page fault error code bits that describe the cause of the exception. @{ */
#define PF_P 0x1    /*!< 0: not-present page. 1: access rights violation. */
#define PF_W 0x2    /*!< 0: read, 1: write. */
#define PF_U 0x4    /*!< 0: kernel, 1: user process. */
/*! @} */

// stack size of each process is limited to 8MB
#define MAX_STACK_SIZE (8 * 1024 * 1024)

void exception_init(void);
void exception_print_stats(void);

bool should_extend_stack(struct intr_frame *f, void *access);

#endif /* userprog/exception.h */

