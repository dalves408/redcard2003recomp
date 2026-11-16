/* Actual hardware draws and staging readback: no reference images or game
 * state substitutions. Keep the renderer's toolkit-facing ABI under test. */
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"
#include "nv2a_vsh_interp.h"
#include <math.h>
static uint8_t memory[65536];
static const Nv2aBackend *backend;
static void (*sync_frame)(void);
ptrdiff_t xbox_GetMemoryOffset(void){return (ptrdiff_t)memory;}
void nv2a_backend_register_with_present_sync(const Nv2aBackend *b,void(*s)(void)){backend=b;sync_frame=s;}
int nv2a_backend_decode_texture(const Nv2aTexture *t,uint32_t *p)
{unsigned i;for(i=0;i<t->width*t->height;i++)p[i]=t->color==0x0b?((uint32_t*)(memory+t->palette))[memory[t->offset+i]]:((uint32_t*)(memory+t->offset))[i];return 1;}
extern void redcard_d3d11_register(void);
static Nv2aSurface surface={4096,64,64,256,4,1,1};
static Nv2aRenderState state;
static Nv2aVertex vertices[6];
static int failures;
static Nv2aCombiner combiner;
static Nv2aTexture stages[4];
static int rc_active;
static void quad(uint32_t color,float z,const Nv2aTexture *tex)
{
    static const float xy[6][2]={{0,0},{64,0},{0,64},{0,64},{64,0},{64,64}};
    unsigned i;Nv2aBatch b={vertices,6,tex,&state};
    if(rc_active){b.combiner=&combiner;b.stages=stages;b.stage_valid=15;}
    for(i=0;i<6;i++){
        vertices[i].x=xy[i][0];vertices[i].y=xy[i][1];vertices[i].z=z;
        vertices[i].rhw=1;vertices[i].diffuse=color;vertices[i].u=vertices[i].v=0;
        vertices[i].tex[1][0]=xy[i][0]/64;vertices[i].tex[1][1]=xy[i][1]/64;vertices[i].tex[1][3]=1;
    }
    backend->draw(&surface,&b);
}
static void expect(const char *name,uint32_t wanted,int tolerance)
{
    unsigned i;uint32_t got;int good=1;
    sync_frame();memcpy(&got,memory+4096+32*256+32*4,4);
    for(i=0;i<4;i++){int d=(int)((got>>(i*8))&255)-(int)((wanted>>(i*8))&255);if(abs(d)>tolerance)good=0;}
    printf("%s: %s got %08X expected %08X\n",good?"PASS":"FAIL",name,got,wanted);
    failures+=!good;
}
static void gpu_vertex_check(void)
{
    uint8_t block[4096]={0};Nv2aGpuVs gv={0};Nv2aBatch b={0};
    uint32_t *desc=(uint32_t*)(block+GVS_OFF_ATTR),*idx=(uint32_t*)(block+GVS_OFF_IDX),got=0;
    float *raw=(float*)(block+3648),out[6*29];unsigned i,k;int bad=0;
    static const float xy[6][2]={{0,0},{64,0},{0,64},{0,64},{64,0},{64,64}};
    uint32_t prog[2][4]={{0,(1u<<21)|0x1b,2u<<26,(15u<<12)|(1u<<11)},
                         {0,(1u<<21)|(1u<<9)|0x1b,2u<<26,(15u<<12)|(1u<<11)|(3u<<3)|1u}};
    _putenv_s("RECOMP_D3D11_GPUVS","1");
    nv2a_vsh_set_start_slot(0);nv2a_vsh_set_cxt_write(0);
    for(i=0;i<2;i++)nv2a_vsh_set_instruction(i,prog[i]);
    if(!nv2a_vsh_export(&gv)){puts("FAIL: GPU vertex export");failures++;return;}
    for(i=0;i<16;i++){desc[4*i+3]=1;((float*)(block+GVS_OFF_IMM))[i*4+3]=1;}
    for(i=0;i<2;i++){desc[4*i]=3648+i*16;desc[4*i+1]=32;desc[4*i+2]=2|(4<<8);desc[4*i+3]=0;}
    for(i=0;i<6;i++){
        idx[i]=i;raw[i*8]=xy[i][0];raw[i*8+1]=xy[i][1];raw[i*8+2]=.5f;raw[i*8+3]=1;
        raw[i*8+4]=.25f;raw[i*8+5]=.5f;raw[i*8+6]=.75f;raw[i*8+7]=1;
    }
    gv.block=block;gv.block_bytes=sizeof block;gv.nidx=6;gv.aa_sx=gv.aa_sy=1;
    gv.verify=1;gv.verify_out=out;gv.verify_count=&got;
    memset(&combiner,0,sizeof combiner);combiner.final0=4;combiner.final1=0x1400;
    b.count=6;b.state=&state;b.combiner=&combiner;b.stages=stages;b.gpu=&gv;
    if(!backend->gpu_probe || !backend->gpu_probe(&gv)){puts("FAIL: hardware GPU vertex probe");failures++;return;}
    backend->draw(&surface,&b);expect("GPU vertex shader rasterization",0xff4080bf,1);
    if(got!=6)bad=1;
    for(i=0;i<got && i<6;i++){
        float in[16][4]={{0}},want[29];Nv2aVshOutput ref;
        memcpy(in[0],raw+i*8,16);memcpy(in[1],raw+i*8+4,16);
        nv2a_vsh_run(in,&ref);memcpy(want,ref.pos,16);memcpy(want+4,ref.d0,16);
        memcpy(want+8,ref.d1,16);memcpy(want+12,ref.tex,64);want[28]=1;
        for(k=0;k<29;k++)if(fabsf(want[k]-out[i*29+k])>.003f)bad=1;
    }
    printf("%s: GPU vertex outputs match CPU interpreter (vertices=%u)\n",bad?"FAIL":"PASS",got);failures+=bad;
}
int main(void)
{
    Nv2aTexture tex={0,2,2,8,0x12,3,3,1,0};
    {unsigned i;uint32_t green=0xff00ff00;for(i=0;i<4;i++)memcpy(memory+i*4,&green,4);}
    _putenv_s("RECOMP_D3D11","1");redcard_d3d11_register();
    if(!backend||!sync_frame)return 2;
    state.color_mask=0x01010101;state.depth_func=0x201;state.alpha_func=0x207;
    backend->clear(&surface,&state,0xf3,0xff0000ff,0xffffff00);
    expect("clear and readback",0xff0000ff,0);
    quad(0xffff0000,0.5f,NULL);expect("vertex color channels",0xffff0000,0);
    state.depth_test_enable=1;state.depth_write=1;state.zeta_va=32768;
    backend->clear(&surface,&state,0xf3,0xff0000ff,0xffffff00);
    quad(0xffff0000,0.5f,NULL);quad(0xff00ff00,0.8f,NULL);
    expect("depth occlusion",0xffff0000,0);
    state.alpha_test_enable=1;state.alpha_func=0x204;state.alpha_ref=128;
    quad(0x00ffffff,0.1f,NULL);expect("alpha discard",0xffff0000,0);
    state.alpha_test_enable=0;state.depth_test_enable=0;
    quad(0xffffffff,0,NULL);quad(0xffffffff,0,&tex);
    expect("texture sampling channels",0xff00ff00,0);
    backend->clear(&surface,&state,0xf0,0xff0000ff,0);
    state.blend_enable=1;state.blend_src=0x302;state.blend_dst=0x303;state.blend_eq=0x8006;
    quad(0x80ff0000,0,NULL);expect("source alpha blending",0xbf80007f,1);
    state.blend_enable=0;state.color_mask=0x00010000;
    quad(0xffffffff,0,NULL);expect("channel write mask",0xbfff007f,1);
    state.color_mask=0x01010101;
    quad(0xff2468ac,0,NULL);
    tex.offset=surface.color_va;tex.width=64;tex.height=64;tex.pitch=256;
    quad(0xffffffff,0,&tex);expect("render-to-texture synchronization",0xff2468ac,0);
    rc_active=1;combiner.final0=4;combiner.final1=0x1400;
    quad(0xff3287db,0,NULL);expect("final register combiner and vertex channels",0xff3287db,0);
    stages[1].width=stages[1].height=2;stages[1].pitch=8;stages[1].color=0x12;
    stages[1].addr_u=stages[1].addr_v=3;
    combiner.stage_program=1u<<5;combiner.final0=9;combiner.final1=0x1900;
    quad(0xffffffff,0,NULL);expect("second texture stage through combiners",0xff00ff00,0);
    putchar('\n');
    {uint32_t pattern[4]={0xffff0000,0xff00ff00,0xff0000ff,0xffffff00};memcpy(memory+1024,pattern,sizeof pattern);}
    /* NEAREST (MIN=1,MAG=1): this check is about swizzled UVs, not filtering. */
    stages[1].offset=1024;stages[1].color=6;stages[1].pitch=4096;stages[1].filter=0x01010000u;
    quad(0xffffffff,0,NULL);expect("normalized swizzled UVs ignore stale linear pitch",0xffffff00,0);
    stages[1].offset=0;stages[1].color=0x12;stages[1].pitch=8;
    combiner.control=1;combiner.color_icw[0]=0x04090000;
    combiner.alpha_icw[0]=0x14190000;combiner.color_ocw[0]=0xc0;combiner.alpha_ocw[0]=0xc0;
    combiner.final0=12;combiner.final1=0x1c00;
    quad(0xff808080,0,NULL);expect("general register combiner modulation",0xff008000,1);
    memset(&combiner,0,sizeof combiner);combiner.final0=9;combiner.final1=0x1900;combiner.stage_program=1u<<5;
    stages[1].offset=2048;stages[1].width=stages[1].height=1;stages[1].pitch=4;stages[1].color=0x12;
    {uint32_t color=0xffff0000;memcpy(memory+2048,&color,4);}
    quad(0xffffffff,0,NULL);
    {uint32_t color=0xff0000ff;memcpy(memory+2048,&color,4);}
    quad(0xffffffff,0,NULL);expect("CPU texture rewrite between draws in one frame",0xff0000ff,0);
    stages[1].color=0x0b;stages[1].pitch=0;stages[1].palette=3072;memory[2048]=1;
    {uint32_t color=0xffff0000;memcpy(memory+3076,&color,4);}
    quad(0xffffffff,0,NULL);
    {uint32_t color=0xff00ff00;memcpy(memory+3076,&color,4);}
    quad(0xffffffff,0,NULL);expect("CPU palette rewrite between draws in one frame",0xff00ff00,0);
    gpu_vertex_check();
    backend->flip();
    printf("%d failure(s)\n",failures);return failures?1:0;
}
