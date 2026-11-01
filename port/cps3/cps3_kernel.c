/*
 * cps3_kernel.c  Task kernel and interrupts (replaces tasksw.src and intr.src)
 *
 * The board's CPU and each of the eight tasks run as coroutines.
 * Interrupts are taken when the game lowers the interrupt mask.
 * The frame ends (vblank) once the scheduler goes idle.
 */

#include <stdio.h>
#include <stdlib.h>

#include "structs.h"
#include "work.h"
#include "extern.h"
#include "Game.h"
#include "bg000.h"
#include "sys_config.h"
#include "sys_config_3.h"
#include "sys_test.h"
#include "sys_test_4.h"
#include "textsound_2.h"
#include "Com_Pl.h"
#include "cps3_host.h"
#include "cps3.h"
#include "coro.h"
#include "cps3_kernel.h"

/* used only by the interrupt handlers */
void reset_entry(void);
void vblank_user_hook(void);
void Irl_Family(void);
void zoom_regs_update(void);
void dispenser_sw_read(void);
void dispenser_control(void);
void coin_in_sound_request(void);
void coin_lockout_update(void);
void service_coin_check(void);

#define TASK_STACK_SIZE (256 * 1024)
#define IDLE_PASSES     2       /* scheduler passes before an idle pass ends the frame */
#define MAX_PASSES      256     /* end the frame anyway after this many passes */

#define IRQ_DMA_END     10
#define IRQ_VBLANK      12

static coro* host_coro;         /* the host's frame loop */
static coro* cpu_coro;          /* the board's CPU (reset_entry, then the scheduler) */
static coro* task_coro[8];
static s32 imask = 15;
static s32 irq_dma_end_pending;
static s32 frame_done;

/* ---- interrupts ---- */

static void deliver_pending(void);

/* Runs a handler at its level. Interrupts it raises are taken on return. */
static void take_interrupt(s32 level, void (*handler)(void)) {
    s32 saved = imask;
    imask = level;
    handler();
    imask = saved;
    deliver_pending();
}

static void deliver_pending(void) {
    if (irq_dma_end_pending && imask < IRQ_DMA_END) {
        irq_dma_end_pending = 0;
        take_interrupt(15, dma_end_intr);
    }
}

int host_get_imask(void) {
    return imask;
}

void host_set_imask(int level) {
    imask = level & 15;
    deliver_pending();
    /* The scheduler unmasks after each task. If it is idle, end the frame here. */
    if (imask < IRQ_VBLANK && coro_self() == cpu_coro && !frame_done) {
        if ((frame_pass_count >= IDLE_PASSES && frame_ready == 0) || frame_pass_count >= MAX_PASSES) {
            frame_done = 1;
            coro_switch(host_coro);
        }
    }
}

/* video DMA end, interrupt level 10 */
void cps3_dma_end_irq(void) {
    irq_dma_end_pending = 1;
    deliver_pending();
}

/* intr.src: _dma_end_intr */
void dma_end_intr(void) {
    dma_queue_drain_wait();
    *(volatile u16*)IRQ10_ACK = 0;
}

/* intr.src: _vblank_frame_dispatch */
void vblank_frame_dispatch(void) {
    vsync_flag = 1;
    system_timer++;
    frt_tick_count = 0;
    *(volatile u8*)SH2_FRCH = 0;
    *(volatile u8*)SH2_FRCL = 0;
    dma_queue_drain_try();
    Irl_Family();
    *(volatile u16*)(IO_REG + 8) = coin_out_latch | 0xC000;
    screen_window_regs_update();
    sprite_dma_start();
    scroll_layers_finalize_frame();
    zoom_regs_update();
    if (Cd_Error_Flag == 0) {
        switch_read_six_button();
        dispenser_sw_read();
        dispenser_control();
        coin_in_sound_request();
        if (test_flag == 0) {
            coin_sw_shift();
            coin_chutes_update(0);
            coin_lockout_update();
            coin_sw_sample();
            coin_sw_shift();
            coin_chutes_update(1);
            service_coin_check();
            coin_lockout_update();
        }
    }
    switch (task_tick_mode) {
    case 1:
    case 3:
        break;
    case 2:
        if (task_tick_mask & system_timer) {
            break;
        }
        /* fall through */
    default:
        task_sleep_tick();
        break;
    }
    if (system_ctrl_flag & 2) {
        vblank_user_hook();
    }
    sprite_dma_end_wait();
    if (bg_vbl_trans_flag) {
        oh_opening_demo(bg_w.bgw[1].bg_address, 0x20, 0x20, 0x1800, 0x10, 0x40, bg_w.bgw[1].r_limit);
    }
    sprite_display_list_build();
    frame_ready = 0;
    sound_driver_tick();
    *(volatile u16*)IRQ12_ACK = 0;
}

/* intr.src: _frt_compare_intr. The second tick of a frame samples the coins. */
void frt_compare_intr(void) {
    frt_tick_count++;
    if (frt_tick_count == 2 && test_flag == 0) {
        coin_sw_sample();
    }
    *(volatile u8*)SH2_FTCSR &= 1;
}

void frt_overflow_intr(void) {
}

void irq_spare_ack_intr(void) {
    *(volatile u16*)IRQ14_ACK = 0;
}

static void exception(const char* what) {
    fprintf(stderr, "CPU exception: %s\n", what);
    abort();
}

void exc_trapa(void) {
    exception("TRAPA");
}
void exc_illegal_instr(void) {
    exception("illegal instruction");
}
void exc_illegal_slot(void) {
    exception("illegal slot instruction");
}
void exc_cpu_addr_err(void) {
    exception("CPU address error");
}
void exc_dma_addr_err(void) {
    exception("DMA address error");
}

/* ---- tasks (tasksw.src) ---- */

static void task_start(void* arg) {
    TCB* tp = arg;
    ((void (*)(void))tp->entry)();
    fprintf(stderr, "task %d returned from its entry\n", (int)(tp - task_tbl));
    tp->status = 0;
    task_free_count++;
    task_exit_to_system();
}

void task_dispatch(TCB* tp) {
    s32 i = tp - task_tbl;
    s32 saved = imask;
    /* sp at the stack top means a new task. Start a fresh coroutine. */
    if (tp->sp == tp->stack_top) {
        coro_delete(task_coro[i]);
        task_coro[i] = coro_create(task_start, tp, TASK_STACK_SIZE);
        tp->sp = tp->stack_top - 1;
    }
    imask = (tp->sr >> 4) & 15;
    coro_switch(task_coro[i]);
    imask = saved;
}

void task_exit_to_system(void) {
    coro_switch(cpu_coro);
}

static void task_save_and_exit(TCB* tp) {
    tp->sr = imask << 4;
    task_exit_to_system();
}

void task_sleep(s32 frames) {
    TCB* tp = current_task;
    s32 level = imask;
    imask = 15;
    tp->status = frames ? 1 : 5;
    tp->state = frames;
    imask = level;
    task_save_and_exit(tp);
}

void task_wait(void) {
    TCB* tp = current_task;
    s32 level = imask;
    imask = 15;
    tp->creator = 0;
    tp->status = 2;
    imask = level;
    task_save_and_exit(tp);
}

void task_yield(void) {
    task_save_and_exit(current_task);
}

void main_thunk(void) {
    Game_Task();
}

void vector_restart(void* vectors) {
    (void)vectors;
    fprintf(stderr, "vector_restart: soft reset is not supported yet\n");
    exit(0);
}

void bios_vector_restart(s32 vector, s32 vbr) {
    (void)vector;
    (void)vbr;
    fprintf(stderr, "bios_vector_restart: return to the BIOS is not supported\n");
    exit(0);
}

/* ---- the frame loop ---- */

static void cpu_main(void* arg) {
    (void)arg;
    reset_entry();
    fprintf(stderr, "reset_entry returned\n");
    abort();
}

void cps3_boot(void) {
    host_coro = coro_self();
    cpu_coro = coro_create(cpu_main, NULL, TASK_STACK_SIZE);
}

/* Runs one frame: the CPU until idle, two timer interrupts, then the vblank. */
void cps3_run_frame(void) {
    frame_done = 0;
    coro_switch(cpu_coro);
    take_interrupt(15, frt_compare_intr);
    take_interrupt(15, frt_compare_intr);
    take_interrupt(15, vblank_frame_dispatch);
}
