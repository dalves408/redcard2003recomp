/* A CRT jump-table entry omitted by function discovery. This is the original
 * 001E09CD continuation, including its shared x87/RET epilogue at 001DF794.
 * Keep the translated range-reduction helper: it also supplies DL, frame
 * status and the exceptional nonlocal return used by the CRT dispatcher. */
#include "recomp_types.h"
#include <float.h>
#include <stdio.h>
#include <stdlib.h>

extern void sub_001E0B0A(void);
void redcard_crt_exp_dispatch(void)
{
    const uint32_t frame=g_ebp, entry_sp=g_esp;
    double result;
    MEM8(frame-0x90)=0xfe;
    g_ecx &= ~0xff00u;
    /* fldl2e; fmulp st(1). The temporary push/pop cancels. */
    g_fp_stack[g_fp_top]=RECOMP_FP_PC(g_fp_stack[g_fp_top]*1.4426950408889634074);
    PUSH32(g_esp,0x001E09DFu);
    RECOMP_ABI_CALL(0x001E0B0Au,sub_001E0B0A);
    /* Its exceptional branch discards the helper continuation and returns
     * directly to the dispatcher's caller. Do not execute that RET twice. */
    if(g_esp==entry_sp+4)return;
    if(g_esp!=entry_sp){fprintf(stderr,"[CRT] unexpected exp continuation stack %08X -> %08X\n",entry_sp,g_esp);abort();}
    result=RECOMP_FP_PC(g_fp_stack[g_fp_top]+1.0);
    if(MEM8(frame-0x9f)&1)result=RECOMP_FP_PC(1.0/result);
    if(!(g_edx&0x40))result=recomp_fscale(result,g_fp_stack[(g_fp_top+1)&7]);
    if(g_ecx&0xff00u)result=-result;
    /* fxch st(1); fstp st(0): remove the integer exponent, retain result. */
    g_fp_top=(g_fp_top+1)&7;
    g_fp_stack[g_fp_top]=result;
    g_esp+=4;
}

/* Runs against the actual loaded XBE constants and translated helper, not a
 * mock of the dispatcher. The internal entry is selected for finite inputs;
 * NaN classification happens before this jump-table branch. */
int redcard_crt_check(void)
{
    static const double inputs[]={0.0,-0.0,1e-12,-1e-12,.5,-.5,1,-1,10,-10,
                                  100,-100,700,-700,740,-740,1000,-1000,30000,-30000};
    unsigned i;int failures=0;
    for(i=0;i<sizeof inputs/sizeof inputs[0];i++){
        double expected=exp(inputs[i]),actual;
        const uint32_t frame=0x00800000u,sp=frame-0x300;
        memset((void*)((uintptr_t)g_xbox_mem_offset+frame-0x400),0,0x400);
        g_ebp=g_seh_ebp=frame;g_esp=sp;
        MEM32(sp)=0x12345678;
        g_fp_top=4;g_fp_stack[4]=inputs[i];g_fp_stack[5]=123.25;
        g_fp_control_word=0x037f;g_fp_cc=0;g_fp_cmp=0;
        g_ecx=0x1234ff00;g_edx=0;
        redcard_crt_exp_dispatch();actual=g_fp_stack[g_fp_top];
        int numeric=isinf(expected)?isinf(actual)&&actual>0:
            expected==0?actual==0:fabs(actual-expected)<=fabs(expected)*3e-13+DBL_TRUE_MIN;
        int abi=g_fp_top==4&&g_esp==sp+4&&g_ebp==frame&&g_seh_ebp==frame&&
                g_fp_stack[5]==123.25&&g_fp_control_word==0x037f;
        printf("[CRT CHECK] exp(%g) %s: %.17g expected %.17g top=%d esp=%08X\n",
               inputs[i],numeric&&abi?"PASS":"FAIL",actual,expected,g_fp_top,g_esp);
        failures+=!(numeric&&abi);
    }
    printf("[CRT CHECK] %u checks, %d failures\n",i,failures);
    return failures?1:0;
}
