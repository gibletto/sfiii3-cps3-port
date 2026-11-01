/*
 * FOLLOW02.C  follow-up table
 *
 * Follow02 is the follow-up routine used for every character by Com_Follow; it runs the
 * Follow02_xxxx entries of Follow02_Tbl.
 */

#include "structs.h"
#include "work.h"
#include "romdata.h"
#include "extern.h"
#include "Com_Sub.h"
#include "FOLLOW02.h"

#ifdef HOST_PORT
/* Command_Attack's Ex_Shot, left out: on the board the low half of the word at its entry stack pointer, which is
   Com_Follow's Follow_Jmp_Tbl[0] = &Follow02 (0x06083F70) */
#define FOLLOW_EX_SHOT , 0x3F70
#else
#define FOLLOW_EX_SHOT
#endif



void Follow02(PLW* wk) {
    Follow02_Tbl[Pattern_Index[wk->wu.id]](wk);
}



void Follow02_0000(PLW* wk) {
    switch (CP_Index[wk->wu.id][0]) {
    case 0:
        Normal_Attack(wk, 8, 0x10);
        break;
    case 1:
        Normal_Attack(wk, 8, 0x20);
        break;
    case 2:
        Normal_Attack(wk, 8, 0x100);
        break;
    default:
        End_Pattern(wk);
        break;
    }
}



void Follow02_0001(PLW* wk) {
    switch (CP_Index[wk->wu.id][0]) {
    case 0:
        Normal_Attack(wk, 8, 0x20);
        break;
    case 1:
        Command_Attack(wk, 8, 0x1C, 8 FOLLOW_EX_SHOT);
        break;
    default:
        End_Pattern(wk);
        break;
    }
}



void Follow02_0002(PLW* wk) {
    switch (CP_Index[wk->wu.id][0]) {
    case 0:
        Normal_Attack(wk, 8, 0x10);
        break;
    case 1:
        Normal_Attack(wk, 8, 0x20);
        break;
    case 2:
        Normal_Attack(wk, 8, 0x100);
        break;
    default:
        End_Pattern(wk);
        break;
    }
}



void Follow02_0003(PLW* wk) {
    switch (CP_Index[wk->wu.id][0]) {
    case 0:
        Normal_Attack(wk, 8, 0x20);
        break;
    case 1:
        Command_Attack(wk, 8, 0x1C, 8 FOLLOW_EX_SHOT);
        break;
    default:
        End_Pattern(wk);
        break;
    }
}
