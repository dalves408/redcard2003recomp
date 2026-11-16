/* Regression for the per-voice source-page protection adapted from
 * dalves408/nhlhitz2003recomp (LGPL-2.1-or-later, as apu_voice_pages.inc).
 * Real address-space aliases distinguish silence from later GPU allocations. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MCPX_HW_MAX_VOICES 256
typedef uint64_t hwaddr;
static uint8_t *g_apu_ram_ptr;
static uint8_t window_page[16384];
static uint8_t *mcpx_apu_phys(uint64_t address)
{
    uint32_t a=(uint32_t)address&0x0fffffffu;
    return (uint8_t*)((uintptr_t)g_apu_ram_ptr+a+
                     (a<(64u<<20)&&window_page[a>>12]?0x80000000ull:0));
}
#include "apu_voice_pages.inc"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void)
{
    const uint32_t a=0x214000,b=a+4096;
    const SIZE_T reserve=0x84000000ull;
    g_apu_ram_ptr=VirtualAlloc(NULL,reserve,MEM_RESERVE,PAGE_NOACCESS);
    CHECK(g_apu_ram_ptr);
    CHECK(VirtualAlloc(g_apu_ram_ptr+a,8192,MEM_COMMIT,PAGE_READWRITE));
    CHECK(VirtualAlloc(g_apu_ram_ptr+0x80000000ull+a,8192,MEM_COMMIT,PAGE_READWRITE));
    memset(g_apu_ram_ptr+0x80000000ull+a,0x7f,8192);
    CHECK(vp_ldl(64,a)==0); /* Original silent image buffer. */
    window_page[a>>12]=1;
    CHECK(vp_ldl(64,a)==0); /* A later GPU allocation cannot redirect it. */
    CHECK(vp_ldl(65,a)==0x7f7f7f7f); /* A new voice sees the new owner. */
    window_page[a>>12]=0;
    CHECK(vp_ldl(65,a)==0x7f7f7f7f); /* Protection works in both directions. */
    vp_pages_forget(65);CHECK(vp_ldl(65,a)==0);
    window_page[b>>12]=1;
    uint8_t bytes[8];vp_read(64,a+4092,bytes,sizeof bytes);
    for(unsigned i=0;i<8;i++)CHECK(bytes[i]==(i<4?0:0x7f));
    CHECK(vp_lduw(64,a+4095)==0x7f00);
    CHECK(vp_ldl(64,a+4094)==0x7f7f0000);
    CHECK(vp_phys(64,0x80000000ull+a)==vp_phys(64,a));
    CHECK(VirtualFree(g_apu_ram_ptr,0,MEM_RELEASE));
    puts("PASS: silent source retention, new voices, restart, aliases and page-boundary PCM reads");
    return 0;
}
