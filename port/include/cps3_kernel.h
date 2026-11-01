/*
 * cps3_kernel.h  Task kernel and interrupts (cps3_kernel.c)
 */

#ifndef CPS3_KERNEL_H
#define CPS3_KERNEL_H

void cps3_boot(void);
void cps3_run_frame(void);

/* interrupt and exception handlers (listed in vectors.c) */
void dma_end_intr(void);
void vblank_frame_dispatch(void);
void frt_compare_intr(void);
void frt_overflow_intr(void);
void irq_spare_ack_intr(void);
void exc_trapa(void);
void exc_illegal_instr(void);
void exc_illegal_slot(void);
void exc_cpu_addr_err(void);
void exc_dma_addr_err(void);

void task_yield(void);
void main_thunk(void);
void vector_restart(void* vectors);

#endif
