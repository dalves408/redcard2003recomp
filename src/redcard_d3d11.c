/* GPU rasterization for the toolkit's decoded NV2A batches. Vertex programs
 * remain in the executor (GPU VS when eligible). Present readback is pipelined
 * one flip behind so Map(READ) does not wait on the just-submitted frame;
 * dirty RTs sampled as textures use GPU copies; LIN_A8R8G8B8 / LIN_X8R8G8B8
 * upload from guest (XRGB as DXGI B8G8R8X8); swizzled A8/X8R8G8B8 use
 * xbox_unswizzle_rect instead of per-texel sample_tex. RECOMP_SYNC_PIPELINE=0
 * restores immediate present sync. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "nv2a_backend.h"
#include "xbox_memory_layout.h"
#include "redcard_rc_hlsl.h"
#include "d3d/d3d8_swizzle.h"

#define DROP(p) do { if (p) { (p)->lpVtbl->Release(p); (p)=NULL; } } while (0)
#define CHECK(call) do { HRESULT hr_=(call); if (FAILED(hr_)) fatal(#call,hr_); } while (0)
#define MAX_TARGETS 32
#define MAX_TEXTURES 256
typedef struct {
    Nv2aSurface s;
    /* color: single-sample RT at guest×scale (resolve dest / draw target when MSAA off).
     * msaa_color: multisampled draw target when MSAA on; resolved into color before readback.
     * down: guest-sized RT used to downsample color when scale>1 before staging copy.
     * staging: guest-sized CPU-readable copy for guest FB writeback. */
    ID3D11Texture2D *color, *msaa_color, *down, *staging, *depth;
    ID3D11RenderTargetView *rtv, *msaa_rtv, *down_rtv;
    ID3D11ShaderResourceView *color_srv;
    ID3D11DepthStencilView *dsv;
    int scale, ms; /* baked at create; mismatch with s_scale/s_msaa forces recreate */
    int dirty;
    /* CopyResource issued to staging; Map+guest write deferred to the next
     * present sync so Map(READ) does not wait on the just-submitted GPU work. */
    int copy_pending;
} Target;
/* F1 / config.exe: internal resolution multiplier (1..4) and MSAA sample count (1/2/4/8). */
static int s_scale = 1, s_msaa = 1;
void redcard_texture_pack_flush(void);
int redcard_pack_installed(void);
void redcard_set_hires(int on);
static ID3D11VertexShader *blit_vs;
static ID3D11PixelShader *blit_ps;
static ID3D11SamplerState *blit_samp;
typedef struct {
    Nv2aTexture key;
    uint64_t hash;
    uint8_t *source;
    size_t source_bytes;
    uint8_t palette[1024];
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    int has_mips; /* GPU mip chain via GenerateMips (pitch shimmer fix) */
    int repl;     /* hi-res DDS pack replacement */
} Texture;
static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11VertexShader *vs;
static ID3D11PixelShader *ps;
static ID3D11InputLayout *layout;
static ID3D11Buffer *vb, *cb;
static ID3D11VertexShader *rc_vs;
static ID3D11PixelShader *rc_ps;
static ID3D11InputLayout *rc_layout;
static ID3D11Buffer *rc_cb;
static struct {Nv2aCombiner key;ID3D11PixelShader *shader;} rc_cache[128];
static unsigned rc_next;
typedef struct {
    uint32_t hdr[4],hdr2[4];float fog[4],uvs[4][4];uint32_t stages[16][4];
} RcParams;
static size_t vb_size, vb_ring_off;
static int vb_ring_logged;
#define VB_RING_BYTES (4u << 20) /* 4 MB NO_OVERWRITE append ring; kills DISCARD rename stalls */
static Target targets[MAX_TARGETS];
static Texture textures[MAX_TEXTURES];
static unsigned texture_next, frames, draws;
static uint32_t *decoded;
static size_t decoded_size;
static void sync_targets(void);
static void target_resolve(Target *t);
static void blit_to_guest(Target *t);
static int msaa_effective(void);
static void gpu_wait_idle(void);
static void unbind_ps_srvs(void)
{
    ID3D11ShaderResourceView *null_srv[4]={0};
    if(ctx)ID3D11DeviceContext_PSSetShaderResources(ctx,0,4,null_srv);
}
static void target_drop(Target *t)
{
    DROP(t->rtv);DROP(t->msaa_rtv);DROP(t->down_rtv);DROP(t->color_srv);DROP(t->dsv);
    DROP(t->color);DROP(t->msaa_color);DROP(t->down);DROP(t->staging);DROP(t->depth);
    memset(t,0,sizeof *t);
}
static void fatal(const char *what, HRESULT hr)
{
    fprintf(stderr,"[RC DX11] FATAL %s: HRESULT %08lX\n",what,(unsigned long)hr);
    fflush(stderr);
    ExitProcess(3);
}

static const char shader[] =
"cbuffer Params:register(b0){float2 size;float zscale;float aref;"
"uint atest;uint afunc;uint textured;uint pad;float2 invtex;float2 pad2;};"
"Texture2D tex:register(t0);SamplerState smp:register(s0);"
"struct VI{float3 p:POSITION;float rhw:RHW;float4 c:COLOR;float2 uv:TEXCOORD;};"
"struct VO{float4 p:SV_Position;float4 c:COLOR;float2 uv:TEXCOORD;};"
"VO VS(VI i){VO o;float w=abs(i.rhw)>1e-12?1/i.rhw:1;"
"o.p=float4((i.p.x/size.x*2-1)*w,(1-i.p.y/size.y*2)*w,saturate(i.p.z*zscale)*w,w);"
"o.c=i.c.bgra;o.uv=i.uv*invtex;return o;}"
"float4 PS(VO i):SV_Target{float4 c=i.c;if(textured)c*=tex.Sample(smp,i.uv);"
"if(atest){int a=int(c.a*255+0.5);int r=int(aref);bool ok=true;"
"if(afunc==0)ok=false;else if(afunc==1)ok=a<r;else if(afunc==2)ok=a==r;"
"else if(afunc==3)ok=a<=r;else if(afunc==4)ok=a>r;else if(afunc==5)ok=a!=r;"
"else if(afunc==6)ok=a>=r;if(!ok)discard;}return c;}";

static ID3DBlob *compile_source(const char *source,size_t length,const char *entry,const char *profile)
{
    ID3DBlob *b=NULL,*e=NULL;
    HRESULT hr=D3DCompile(source,length,"RedCard NV2A",NULL,NULL,
                         entry,profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&b,&e);
    if(e){fprintf(stderr,"[RC DX11] %s\n",(char*)ID3D10Blob_GetBufferPointer(e));DROP(e);}
    if(FAILED(hr))fatal("shader compilation",hr);
    return b;
}

static void initialize_device(void)
{
    ID3DBlob *v,*p;
    D3D11_BUFFER_DESC bd={0};
    D3D_FEATURE_LEVEL fl;
    D3D11_INPUT_ELEMENT_DESC elems[]={
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"RHW",0,DXGI_FORMAT_R32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,16,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,20,D3D11_INPUT_PER_VERTEX_DATA,0}};
    IDXGIDevice *dxdev=NULL;IDXGIAdapter *adapter=NULL;DXGI_ADAPTER_DESC desc;
    if(dev)return;
    {   /* List the GPUs; RECOMP_D3D11_ADAPTER=<index | part of the name>
         * renders on that one (e.g. "intel" or "nvidia" on a dual-GPU
         * laptop). Default: the adapter Windows picks (on a dual-GPU laptop
         * usually the integrated one). */
        const char *want=getenv("RECOMP_D3D11_ADAPTER");
        IDXGIFactory1 *fac=NULL;IDXGIAdapter1 *pick=NULL;
        if(SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1,(void**)&fac))){
            IDXGIAdapter1 *a;UINT i;
            for(i=0;IDXGIFactory1_EnumAdapters1(fac,i,&a)==S_OK;i++){
                DXGI_ADAPTER_DESC1 d1;char name[128];int match=0;
                IDXGIAdapter1_GetDesc1(a,&d1);
                WideCharToMultiByte(CP_UTF8,0,d1.Description,-1,name,sizeof name,NULL,NULL);
                fprintf(stderr,"[RC DX11] adapter %u: %s (%u MB, vendor %04X)\n",i,name,
                        (unsigned)(d1.DedicatedVideoMemory>>20),d1.VendorId);
                if(want&&*want&&!pick){
                    if(want[0]>='0'&&want[0]<='9')match=(UINT)atoi(want)==i;
                    else{char lw[128],ln[128];size_t k;
                        for(k=0;want[k]&&k<127;k++)lw[k]=(char)tolower((unsigned char)want[k]);lw[k]=0;
                        for(k=0;name[k]&&k<127;k++)ln[k]=(char)tolower((unsigned char)name[k]);ln[k]=0;
                        match=strstr(ln,lw)!=NULL;}
                }
                if(match){pick=a;continue;}
                IDXGIAdapter1_Release(a);
            }
            IDXGIFactory1_Release(fac);
        }
        if(want&&*want&&!pick)fprintf(stderr,"[RC DX11] RECOMP_D3D11_ADAPTER=%s matched no adapter; using the default\n",want);
        if(pick){
            CHECK(D3D11CreateDevice((IDXGIAdapter*)pick,D3D_DRIVER_TYPE_UNKNOWN,NULL,0,NULL,0,
                                   D3D11_SDK_VERSION,&dev,&fl,&ctx));
            IDXGIAdapter1_Release(pick);
        }else
            CHECK(D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,0,NULL,0,
                                   D3D11_SDK_VERSION,&dev,&fl,&ctx));
    }
    CHECK(ID3D11Device_QueryInterface(dev,&IID_IDXGIDevice,(void**)&dxdev));
    CHECK(IDXGIDevice_GetAdapter(dxdev,&adapter));
    CHECK(IDXGIAdapter_GetDesc(adapter,&desc));
    fprintf(stderr,"[RC DX11] hardware rasterizer: %ls; feature level %04X\n",desc.Description,fl);
    DROP(adapter);DROP(dxdev);
    v=compile_source(shader,sizeof(shader)-1,"VS","vs_4_0");p=compile_source(shader,sizeof(shader)-1,"PS","ps_4_0");
    CHECK(ID3D11Device_CreateVertexShader(dev,ID3D10Blob_GetBufferPointer(v),ID3D10Blob_GetBufferSize(v),NULL,&vs));
    CHECK(ID3D11Device_CreatePixelShader(dev,ID3D10Blob_GetBufferPointer(p),ID3D10Blob_GetBufferSize(p),NULL,&ps));
    CHECK(ID3D11Device_CreateInputLayout(dev,elems,4,ID3D10Blob_GetBufferPointer(v),ID3D10Blob_GetBufferSize(v),&layout));
    DROP(v);DROP(p);
    bd.ByteWidth=48;bd.Usage=D3D11_USAGE_DYNAMIC;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    CHECK(ID3D11Device_CreateBuffer(dev,&bd,NULL,&cb));
}

static void initialize_combiners(void)
{
    ID3DBlob *v;D3D11_BUFFER_DESC bd={0};
    D3D11_INPUT_ELEMENT_DESC e[]={
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"RHW",0,DXGI_FORMAT_R32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,16,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,20,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,28,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",2,DXGI_FORMAT_R32G32B32A32_FLOAT,0,44,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",3,DXGI_FORMAT_R32G32B32A32_FLOAT,0,60,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",4,DXGI_FORMAT_R32G32B32A32_FLOAT,0,76,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",5,DXGI_FORMAT_R32G32B32A32_FLOAT,0,92,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",6,DXGI_FORMAT_R32_FLOAT,0,108,D3D11_INPUT_PER_VERTEX_DATA,0}};
    if(rc_vs)return;
    v=compile_source(k_hlsl_rc,sizeof(k_hlsl_rc)-1,"vs_rc","vs_5_0");
    CHECK(ID3D11Device_CreateVertexShader(dev,ID3D10Blob_GetBufferPointer(v),ID3D10Blob_GetBufferSize(v),NULL,&rc_vs));
    CHECK(ID3D11Device_CreateInputLayout(dev,e,10,ID3D10Blob_GetBufferPointer(v),ID3D10Blob_GetBufferSize(v),&rc_layout));
    DROP(v);
    bd.ByteWidth=sizeof(RcParams);bd.Usage=D3D11_USAGE_DYNAMIC;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    CHECK(ID3D11Device_CreateBuffer(dev,&bd,NULL,&rc_cb));
    fprintf(stderr,"[RC DX11] Xbox register combiners enabled (four texture stages)\n");
}

static ID3D11PixelShader *combiner_shader(const Nv2aCombiner *c)
{
    Nv2aCombiner key=*c;unsigned i,n;size_t used;
    char prefix[2048],*source;ID3DBlob *blob;
    /* Colors remain uniforms. Only control words affect compiled arithmetic. */
    memset(key.factor0,0,sizeof key.factor0);memset(key.factor1,0,sizeof key.factor1);
    key.final_c0=key.final_c1=0;
    for(i=0;i<128;i++)if(rc_cache[i].shader && !memcmp(&key,&rc_cache[i].key,sizeof key))return rc_cache[i].shader;
    n=c->control&255;if(n>8)n=8;
    used=(size_t)snprintf(prefix,sizeof prefix,"#define SPEC 1\n#define SPEC_H0 %uu\n#define SPEC_H1 %uu\n#define SPEC_H2 %uu\n#define SPEC_H3 %uu\n#define SPEC_N %u\n#define SPEC_WORDS ",c->control,c->stage_program,c->final0,c->final1,n);
    for(i=0;i<8;i++)used+=(size_t)snprintf(prefix+used,sizeof(prefix)-used,"%suint4(%uu,%uu,%uu,%uu)",i?",":"",c->color_icw[i],c->alpha_icw[i],c->color_ocw[i],c->alpha_ocw[i]);
    prefix[used++]='\n';prefix[used]=0;
    source=malloc(used+sizeof k_hlsl_rc);if(!source)fatal("combiner source",E_OUTOFMEMORY);
    memcpy(source,prefix,used);memcpy(source+used,k_hlsl_rc,sizeof k_hlsl_rc);
    blob=compile_source(source,used+sizeof(k_hlsl_rc)-1,"ps_rc","ps_5_0");free(source);
    i=rc_next++%128;DROP(rc_cache[i].shader);
    CHECK(ID3D11Device_CreatePixelShader(dev,ID3D10Blob_GetBufferPointer(blob),ID3D10Blob_GetBufferSize(blob),NULL,&rc_cache[i].shader));
    DROP(blob);rc_cache[i].key=key;
    fprintf(stderr,"[RC DX11] compiled combiner variant %u (%u stages)\n",rc_next,n);
    return rc_cache[i].shader;
}

static Target *target(const Nv2aSurface *s)
{
    unsigned i;Target *t=NULL;
    D3D11_TEXTURE2D_DESC td={0};
    uint8_t *mem=(uint8_t*)xbox_GetMemoryOffset();
    D3D11_SUBRESOURCE_DATA data={0};
    int scale,ms,need_seed;
    UINT gw,gh;
    if(!s->width||!s->height||s->width>4096||s->height>4096)return NULL;
    /* Contig page 0 is reserved (never a real MmAllocateContiguousMemory
     * result). After arena exhaustion the title has been seen keeping a
     * color surface at 0x80000002; treating that as a target presents and
     * texture-samples garbage (kickoff checkerboard / magenta). */
    if(s->color_va>=0x80000000u && s->color_va<0x80001000u){
        static int logged_page0;
        if(!logged_page0){
            logged_page0=1;
            fprintf(stderr,"[RC DX11] skipping reserved contig page-0 surface va=%08X %ux%u pitch %u\n",
                    s->color_va,s->width,s->height,s->pitch);
            fflush(stderr);
        }
        return NULL;
    }
    /* Non-32-bit color targets are unsupported. Soft-fail like other invalid
     * surfaces: clear/draw already no-op on NULL. ExitProcess here used to
     * kill the process after guest contig arena exhaustion left a bad bpp. */
    if(s->bytes_per_pixel!=4){
        static int logged;
        if(!logged){
            logged=1;
            fprintf(stderr,"[RC DX11] skipping unsupported color surface bpp=%u va=%08X %ux%u pitch %u (need 32-bit)\n",
                    (unsigned)s->bytes_per_pixel,s->color_va,s->width,s->height,s->pitch);
            fflush(stderr);
        }
        return NULL;
    }
    initialize_device();
    scale=s_scale<1?1:(s_scale>4?4:s_scale);
    /* MSAA is for the display-sized color buffer. Intermediate RTT (e.g. 512x256
     * reflections) at 4x+8x creates huge MSAA surfaces; Intel UHD 605 then AVs in
     * the UMD on the next UpdateSubresource after Resolve/Copy (crash 121646). */
    ms=(s->width>=640u && s->height>=480u)?msaa_effective():1;
    /* Cap intermediate RTT scale independently of the display FB. Crash 172434
     * (post-#26 test13, UHD 620) still AVd after the first 2048×1024 RT→SRV
     * CopyResource of 80600000 while display was 4x+MSAA4 — even with #26's
     * unconditional idle after the copy. The same session's earlier 2x RTT
     * sample (1024×512) was stable; keep reflections ≤2x. */
    if((s->width<640u || s->height<480u) && scale>2)scale=2;
    gw=s->width*(UINT)scale;gh=s->height*(UINT)scale;
    if(s->swizzle_w){
        /* One line per swizzled render-to-texture surface seen (scene dissolves,
         * slow-motion trails), so a session log shows which effects use them. */
        static uint32_t seen[16];static unsigned nseen;unsigned k;
        for(k=0;k<nseen&&seen[k]!=s->color_va;k++){}
        if(k==nseen&&nseen<16){seen[nseen++]=s->color_va;
            fprintf(stderr,"[RC DX11] swizzled render target %08X %ux%u (Morton %ux%u)\n",s->color_va,s->width,s->height,s->swizzle_w,s->swizzle_h);}
    }
    for(i=0;i<MAX_TARGETS;i++){
        if(targets[i].color && targets[i].s.color_va==s->color_va
                && targets[i].s.width==s->width && targets[i].s.height==s->height
                && targets[i].scale==scale && targets[i].ms==ms){
            targets[i].s=*s;return &targets[i];
        }
        if(!targets[i].color && !t)t=&targets[i];
    }
    if(!t){
        /* Loading a new arena can recycle allocations with different sizes. */
        static unsigned victim;
        sync_targets();t=&targets[victim++%MAX_TARGETS];
        ID3D11DeviceContext_OMSetRenderTargets(ctx,0,NULL,NULL);
        unbind_ps_srvs();
        gpu_wait_idle();
        target_drop(t);
    }
    t->s=*s;t->scale=scale;t->ms=ms;
    need_seed=(scale==1 && ms==1);
    td.Width=gw;td.Height=gh;td.MipLevels=1;td.ArraySize=1;
    td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;
    td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    td.Usage=D3D11_USAGE_DEFAULT;
    if(need_seed&&s->swizzle_w&&s->swizzle_h&&s->width<=s->swizzle_w&&s->height<=s->swizzle_h){
        /* Seed a swizzled surface from its Morton-ordered guest bytes. */
        uint32_t *lin=malloc((size_t)s->swizzle_w*s->swizzle_h*4);
        if(!lin)fatal("swizzled target seed",E_OUTOFMEMORY);
        xbox_unswizzle_rect(lin,mem+s->color_va,s->swizzle_w,s->swizzle_h,4);
        data.pSysMem=lin;data.SysMemPitch=s->swizzle_w*4;
        CHECK(ID3D11Device_CreateTexture2D(dev,&td,&data,&t->color));free(lin);}
    else if(need_seed){data.pSysMem=mem+s->color_va;data.SysMemPitch=s->pitch;
        CHECK(ID3D11Device_CreateTexture2D(dev,&td,&data,&t->color));}
    else CHECK(ID3D11Device_CreateTexture2D(dev,&td,NULL,&t->color));
    CHECK(ID3D11Device_CreateRenderTargetView(dev,(ID3D11Resource*)t->color,NULL,&t->rtv));
    CHECK(ID3D11Device_CreateShaderResourceView(dev,(ID3D11Resource*)t->color,NULL,&t->color_srv));
    if(ms>1){
        D3D11_TEXTURE2D_DESC md=td;
        md.SampleDesc.Count=(UINT)ms;
        md.BindFlags=D3D11_BIND_RENDER_TARGET;
        CHECK(ID3D11Device_CreateTexture2D(dev,&md,NULL,&t->msaa_color));
        CHECK(ID3D11Device_CreateRenderTargetView(dev,(ID3D11Resource*)t->msaa_color,NULL,&t->msaa_rtv));
    }
    if(scale>1){
        D3D11_TEXTURE2D_DESC dd={0};
        dd.Width=s->width;dd.Height=s->height;dd.MipLevels=1;dd.ArraySize=1;
        dd.Format=DXGI_FORMAT_B8G8R8A8_UNORM;dd.SampleDesc.Count=1;
        dd.Usage=D3D11_USAGE_DEFAULT;dd.BindFlags=D3D11_BIND_RENDER_TARGET;
        CHECK(ID3D11Device_CreateTexture2D(dev,&dd,NULL,&t->down));
        CHECK(ID3D11Device_CreateRenderTargetView(dev,(ID3D11Resource*)t->down,NULL,&t->down_rtv));
    }
    {D3D11_TEXTURE2D_DESC sd={0};
        sd.Width=s->width;sd.Height=s->height;sd.MipLevels=1;sd.ArraySize=1;
        sd.Format=DXGI_FORMAT_B8G8R8A8_UNORM;sd.SampleDesc.Count=1;
        sd.Usage=D3D11_USAGE_STAGING;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        CHECK(ID3D11Device_CreateTexture2D(dev,&sd,NULL,&t->staging));}
    {D3D11_TEXTURE2D_DESC zd={0};
        zd.Width=gw;zd.Height=gh;zd.MipLevels=1;zd.ArraySize=1;
        zd.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;zd.SampleDesc.Count=(UINT)(ms>1?ms:1);
        zd.Usage=D3D11_USAGE_DEFAULT;zd.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        CHECK(ID3D11Device_CreateTexture2D(dev,&zd,NULL,&t->depth));
        CHECK(ID3D11Device_CreateDepthStencilView(dev,(ID3D11Resource*)t->depth,NULL,&t->dsv));}
    ID3D11DeviceContext_ClearDepthStencilView(ctx,t->dsv,D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,0);
    if(!need_seed){
        float z[4]={0,0,0,0};
        ID3D11DeviceContext_ClearRenderTargetView(ctx,t->msaa_rtv?t->msaa_rtv:t->rtv,z);
    }
    fprintf(stderr,"[RC DX11] target %08X %ux%u pitch %u scale %dx msaa %dx (%ux%u)%s\n",
            s->color_va,s->width,s->height,s->pitch,scale,ms,gw,gh,s->swizzle_w?" swizzled":"");
    return t;
}

/* Executor-thread time spent in readback and draw submission, reported at flip. */
static LONGLONG sync_ticks, draw_ticks, sync_count, decodes, rt_decodes, decoded_texels;
static LONGLONG now_ticks(void){LARGE_INTEGER q;QueryPerformanceCounter(&q);return q.QuadPart;}
static LONGLONG map_write_ticks,map_read_ticks,texture_upload_ticks,draw_api_ticks,texture_decode_ticks,map_peak;
static LONGLONG map_vb_ticks,map_cb_ticks,map_rc_ticks,map_vb_peak,map_cb_peak,state_cache_ticks;
static LONGLONG win_map_vb,win_map_cb,win_map_rc,win_upload,win_draw_api,win_state,win_map_vb_peak,win_map_cb_peak;
static unsigned map_vb_discard,map_vb_no_overwrite,win_vb_discard,win_vb_no_overwrite;
static HRESULT map_resource(ID3D11DeviceContext *c,ID3D11Resource *res,UINT sub,D3D11_MAP mode,UINT flags,D3D11_MAPPED_SUBRESOURCE *m)
{
    LONGLONG t=now_ticks(),elapsed;HRESULT hr=ID3D11DeviceContext_Map(c,res,sub,mode,flags,m);
    elapsed=now_ticks()-t;if(mode==D3D11_MAP_READ)map_read_ticks+=elapsed;else map_write_ticks+=elapsed;
    if(elapsed>map_peak)map_peak=elapsed;return hr;
}
static HRESULT map_bucket(ID3D11DeviceContext *c,ID3D11Resource *res,UINT sub,D3D11_MAP mode,UINT flags,
                          D3D11_MAPPED_SUBRESOURCE *m,LONGLONG *bucket,LONGLONG *peak)
{
    LONGLONG t=now_ticks(),elapsed;HRESULT hr=ID3D11DeviceContext_Map(c,res,sub,mode,flags,m);
    elapsed=now_ticks()-t;
    if(mode==D3D11_MAP_READ)map_read_ticks+=elapsed;else map_write_ticks+=elapsed;
    if(bucket)*bucket+=elapsed;
    if(peak && elapsed>*peak)*peak=elapsed;
    if(elapsed>map_peak)map_peak=elapsed;
    return hr;
}
static void upload_texture(ID3D11DeviceContext *c,ID3D11Resource *res,UINT sub,const D3D11_BOX *box,const void *data,UINT pitch,UINT depth)
{
    LONGLONG t;
    /* UpdateSubresource on a resource still bound as a PS SRV is illegal; Intel
     * UMDs have been seen to AV rather than ignore the hazard. */
    unbind_ps_srvs();
    t=now_ticks();ID3D11DeviceContext_UpdateSubresource(c,res,sub,box,data,pitch,depth);texture_upload_ticks+=now_ticks()-t;
}
static void submit_draw(ID3D11DeviceContext *c,UINT count,UINT first)
{
    LONGLONG t=now_ticks();ID3D11DeviceContext_Draw(c,count,first);draw_api_ticks+=now_ticks()-t;
}
/* Append vertices into a large dynamic ring. WRITE_DISCARD on a small single
 * buffer every draw exhausts driver renames and stalls in Map once the GPU
 * falls behind (RED-6). NO_OVERWRITE appends avoid that; DISCARD only on wrap. */
static UINT upload_vb_ring(const void *data,size_t bytes)
{
    D3D11_BUFFER_DESC bd={0};D3D11_MAPPED_SUBRESOURCE m;D3D11_MAP mode;UINT start;
    static int force_discard=-1;
    if(force_discard<0){const char *e=getenv("RECOMP_VB_RING");force_discard=(e&&e[0]=='0');}
    if(force_discard){
        if(bytes>vb_size){DROP(vb);vb_size=(bytes+65535u)&~65535u;
            bd.ByteWidth=(UINT)vb_size;bd.Usage=D3D11_USAGE_DYNAMIC;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
            CHECK(ID3D11Device_CreateBuffer(dev,&bd,NULL,&vb));}
        CHECK(map_bucket(ctx,(ID3D11Resource*)vb,0,D3D11_MAP_WRITE_DISCARD,0,&m,&map_vb_ticks,&map_vb_peak));
        memcpy(m.pData,data,bytes);ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)vb,0);map_vb_discard++;
        return 0;
    }
    if(!vb||vb_size<VB_RING_BYTES){
        DROP(vb);vb_size=VB_RING_BYTES;vb_ring_off=0;
        bd.ByteWidth=VB_RING_BYTES;bd.Usage=D3D11_USAGE_DYNAMIC;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        CHECK(ID3D11Device_CreateBuffer(dev,&bd,NULL,&vb));
        if(!vb_ring_logged){vb_ring_logged=1;fprintf(stderr,"[RC DX11] vertex ring %u KB (NO_OVERWRITE appends; RECOMP_VB_RING=0 for old DISCARD)\n",VB_RING_BYTES>>10);}
    }
    if(bytes>VB_RING_BYTES)fatal("vertex batch larger than VB ring",E_OUTOFMEMORY);
    /* keep OffsetInBytes a multiple of the vertex stride (append whole batches) */
    if(vb_ring_off+bytes>VB_RING_BYTES){vb_ring_off=0;mode=D3D11_MAP_WRITE_DISCARD;map_vb_discard++;}
    else if(vb_ring_off){mode=D3D11_MAP_WRITE_NO_OVERWRITE;map_vb_no_overwrite++;}
    else{mode=D3D11_MAP_WRITE_DISCARD;map_vb_discard++;}
    CHECK(map_bucket(ctx,(ID3D11Resource*)vb,0,mode,0,&m,&map_vb_ticks,&map_vb_peak));
    memcpy((uint8_t*)m.pData+vb_ring_off,data,bytes);
    ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)vb,0);
    start=(UINT)vb_ring_off;vb_ring_off+=bytes;return start;
}
static int sync_pipeline_on(void)
{
    static int v=-1;const char *e;
    if(v<0){e=getenv("RECOMP_SYNC_PIPELINE");v=!(e&&e[0]=='0');
        if(v)fprintf(stderr,"[RC DX11] pipelined target readback (RECOMP_SYNC_PIPELINE=0 for immediate)\n");}
    return v;
}
static void sync_target_complete(Target *t)
{
    unsigned y;D3D11_MAPPED_SUBRESOURCE m;uint8_t *mem;
    if(!t->copy_pending||!t->staging)return;
    mem=(uint8_t*)xbox_GetMemoryOffset();
    CHECK(map_resource(ctx,(ID3D11Resource*)t->staging,0,D3D11_MAP_READ,0,&m));
    if(t->s.swizzle_w&&t->s.swizzle_h){
        /* Swizzled render-to-texture surface: the guest samples it as a
         * swizzled texture, so write Morton order. Row order here was decoded
         * as swizzled and every captured frame came back as scrambled tiles. */
        uint32_t mx,my,ox,oy=0,x,w=t->s.width,h=t->s.height;uint32_t *dst=(uint32_t*)(mem+t->s.color_va);
        if(w>t->s.swizzle_w)w=t->s.swizzle_w;if(h>t->s.swizzle_h)h=t->s.swizzle_h;
        xbox_swizzle_masks(t->s.swizzle_w,t->s.swizzle_h,&mx,&my);
        for(y=0;y<h;y++){
            const uint32_t *row=(const uint32_t*)((uint8_t*)m.pData+(size_t)y*m.RowPitch);
            for(x=0,ox=0;x<w;x++){dst[ox|oy]=row[x];ox=((ox|~mx)+1u)&mx;}
            oy=((oy|~my)+1u)&my;
        }
    }else
    for(y=0;y<t->s.height;y++)
        memcpy(mem+t->s.color_va+(size_t)y*t->s.pitch,(uint8_t*)m.pData+(size_t)y*m.RowPitch,t->s.width*4u);
    ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)t->staging,0);
    t->copy_pending=0;
}
static void sync_target_issue(Target *t)
{
    ID3D11Texture2D *src;
    if(!t->dirty||!t->color||!t->staging)return;
    target_resolve(t);
    if(t->scale>1){
        blit_to_guest(t);
        src=t->down;
    }else src=t->color;
    if(!src)return;
    ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource*)t->staging,(ID3D11Resource*)src);
    t->copy_pending=1;
    t->dirty=0;
}
static void sync_target_immediate(Target *t)
{
    sync_target_complete(t);
    if(t->dirty){sync_target_issue(t);sync_target_complete(t);}
}
/* Full flush into guest memory — eviction, size-mismatch RTT fallback, dumps. */
static void sync_targets(void)
{
    unsigned i;LONGLONG t0=now_ticks();
    for(i=0;i<MAX_TARGETS;i++)sync_target_immediate(&targets[i]);
    sync_ticks+=now_ticks()-t0;sync_count++;
}
/* Present path: finish the previous frame's Copies (Map usually ready), then
 * kick Copies for newly dirty *display-sized* targets without blocking on them.
 * Small RTs stay dirty until sampled (texture_from_rt) or a full flush.
 * Guest FB for the window snapshot is one flip behind — acceptable vs a
 * 3–5 ms Map stall on the just-submitted GPU work. */
static void sync_targets_present(void)
{
    unsigned i;LONGLONG t0=now_ticks();
    if(!sync_pipeline_on()){sync_targets();return;}
    for(i=0;i<MAX_TARGETS;i++)sync_target_complete(&targets[i]);
    for(i=0;i<MAX_TARGETS;i++)if(targets[i].dirty
            && targets[i].s.width>=320u && targets[i].s.height>=240u)
        sync_target_issue(&targets[i]);
    sync_ticks+=now_ticks()-t0;sync_count++;
}
/* Dirty RT sampled as a texture: GPU→GPU copy into the cache (no Map/decode).
 * Match Hitz: keep the scaled resolve target (do not downsample for sampling).
 * Guest-sized invtex still maps 0..guest_w to 0..1 across the full scaled image. */
static Texture *texture_from_rt(Target *rt,const Nv2aTexture *key)
{
    size_t i;Texture *existing=NULL,*t;D3D11_TEXTURE2D_DESC td={0};
    UINT tw,th;
    if(!rt->color||rt->s.width!=key->width||rt->s.height!=key->height)return NULL;
    tw=key->width*(UINT)(rt->scale>0?rt->scale:1);
    th=key->height*(UINT)(rt->scale>0?rt->scale:1);
    for(i=0;i<MAX_TEXTURES;i++)if(textures[i].tex&&memcmp(&textures[i].key,key,sizeof(*key))==0){
        existing=&textures[i];break;
    }
    /* Unbind RT + PS SRVs so resolve/copy source/dest are not live hazards.
     * draw_impl rebinds next batch. */
    ID3D11DeviceContext_OMSetRenderTargets(ctx,0,NULL,NULL);
    unbind_ps_srvs();
    target_resolve(rt);
    if(existing){
        D3D11_TEXTURE2D_DESC ed={0};
        ID3D11Texture2D_GetDesc(existing->tex,&ed);
        if(ed.Width==tw&&ed.Height==th){
            ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource*)existing->tex,(ID3D11Resource*)rt->color);
            /* Drain after every RT→SRV copy, not only MSAA resolve. Crash 124405
             * (post-#25, intermediate RTT already msaa 1x) still AVd in the Intel
             * UMD on the next UpdateSubresource after this CopyResource. */
            gpu_wait_idle();
            free(existing->source);existing->source=NULL;existing->source_bytes=0;existing->hash=0;
            return existing;
        }
        DROP(existing->srv);DROP(existing->tex);existing->has_mips=0;
        free(existing->source);existing->source=NULL;existing->source_bytes=0;
        t=existing;
    }else{
        t=&textures[texture_next++%MAX_TEXTURES];DROP(t->srv);DROP(t->tex);t->has_mips=0;
        free(t->source);t->source=NULL;t->source_bytes=0;
    }
    td.Width=tw;td.Height=th;td.MipLevels=1;td.ArraySize=1;
    td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;
    td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    CHECK(ID3D11Device_CreateTexture2D(dev,&td,NULL,&t->tex));
    ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource*)t->tex,(ID3D11Resource*)rt->color);
    CHECK(ID3D11Device_CreateShaderResourceView(dev,(ID3D11Resource*)t->tex,NULL,&t->srv));
    /* Same hazard as the existing-texture path above (crash 124405). */
    gpu_wait_idle();
    t->key=*key;t->hash=0;
    {static unsigned n;if(n++<8)fprintf(stderr,"[RC DX11] RT→SRV GPU copy %08X %ux%u scale %dx (skip readback/decode)\n",
        key->offset,key->width,key->height,rt->scale);}
    return t;
}

static size_t texture_bytes(const Nv2aTexture *t)
{
    size_t bpp;
    switch(t->color){
    case 0x0c:return (size_t)((t->width+3)/4)*((t->height+3)/4)*8;
    case 0x0e:case 0x0f:return (size_t)((t->width+3)/4)*((t->height+3)/4)*16;
    case 0x00:case 0x0b:case 0x19:case 0x13:case 0x1f:bpp=1;break;
    case 0x02:case 0x03:case 0x04:case 0x05:case 0x10:case 0x11:case 0x1c:case 0x1d:case 0x24:case 0x25:bpp=2;break;
    case 0x06:case 0x07:case 0x12:case 0x1e:case 0x3f:case 0x40:case 0x41:bpp=4;break;
    default:return 0;
    }
    return !d3d8_format_is_swizzled(t->color) && t->pitch?(size_t)(t->height-1)*t->pitch+t->width*bpp:(size_t)t->width*t->height*bpp;
}

static void texture_snapshot(Texture *t,const Nv2aTexture *key,size_t bytes,const uint8_t *mem)
{
    if(t->source_bytes!=bytes){
        free(t->source);t->source=bytes?malloc(bytes):NULL;
        if(bytes&&!t->source)fatal("texture source snapshot",E_OUTOFMEMORY);
        t->source_bytes=bytes;
    }
    if(bytes)memcpy(t->source,mem+key->offset,bytes);
    if(key->palette)memcpy(t->palette,mem+key->palette,sizeof t->palette);
}

/* SET_TEXTURE_FILTER: MIN in bits 16-23, MAG in 24-27. Xbox NEAREST is 1 on
 * both; anything else (including 0 / unset) is treated as linear, matching the
 * Hitz DX11 backend. Hard-coded point sampling made the pitched field shimmer
 * under camera motion. */
static int texture_wants_linear(const Nv2aTexture *k)
{
    uint32_t mn=(k->filter>>16)&0xFFu,mg=(k->filter>>24)&0x0Fu;
    return !(mn==1u && mg==1u);
}
static int s_aniso_override=-1;
void redcard_set_anisotropy(int aniso)
{
    if(aniso>=16)aniso=16;else if(aniso>=8)aniso=8;else if(aniso>=4)aniso=4;
    else if(aniso>=2)aniso=2;else aniso=1;
    s_aniso_override=aniso;
}
static unsigned texture_aniso(void)
{
    static int v=-1;const char *e;
    if(s_aniso_override>0)return (unsigned)s_aniso_override;
    if(v<0){e=getenv("RECOMP_D3D11_ANISO");v=e&&e[0]?atoi(e):4;
        v=v>=16?16:v>=8?8:v>=4?4:v>=2?2:1;}
    return (unsigned)v;
}
/* Wait until the GPU finishes queued work that still references our RTs.
 * F1 / config live scale+MSAA changes Release those textures; without an idle
 * wait, Intel UMDs (seen on UHD 605) can AV later in UpdateSubresource /
 * CopyResource on a recycled GPU VA.
 * budget_ms caps the spin: F1 can call this from the NV2A ack/PB thread, and a
 * multi-second drain starves fence retirement → guest redcard_gpu_wait abort
 * (seen as a crash on a slow quad-core Intel PC). RT→SRV still uses a long budget. */
static void gpu_wait_idle_budget(DWORD budget_ms)
{
    D3D11_QUERY_DESC qd={0};
    ID3D11Query *q=NULL;
    BOOL done=FALSE;
    ULONGLONG began;
    if(!dev||!ctx)return;
    qd.Query=D3D11_QUERY_EVENT;
    if(FAILED(ID3D11Device_CreateQuery(dev,&qd,&q))||!q){
        ID3D11DeviceContext_Flush(ctx);
        return;
    }
    ID3D11DeviceContext_End(ctx,(ID3D11Asynchronous*)q);
    ID3D11DeviceContext_Flush(ctx);
    began=GetTickCount64();
    while(ID3D11DeviceContext_GetData(ctx,(ID3D11Asynchronous*)q,&done,sizeof(done),0)==S_FALSE){
        if(GetTickCount64()-began>(ULONGLONG)budget_ms)break;
        Sleep(0);
    }
    DROP(q);
}
static void gpu_wait_idle(void)
{
    gpu_wait_idle_budget(5000);
}
static void targets_reset_all(void)
{
    unsigned i;
    if(ctx){
        ID3D11DeviceContext_OMSetRenderTargets(ctx,0,NULL,NULL);
        unbind_ps_srvs();
        /* Drop pending staging Maps without guest writeback — scale/MSAA change
         * recreates targets; guest FB is refreshed by the next draw/present. */
        for(i=0;i<MAX_TARGETS;i++){
            targets[i].copy_pending=0;
            targets[i].dirty=0;
        }
        /* Short drain only: rapid F1 scale/MSAA cycling must not block the PB
         * consumer past redcard_gpu_wait's fence timeout (crash 171753). */
        gpu_wait_idle_budget(100);
    }
    for(i=0;i<MAX_TARGETS;i++)target_drop(&targets[i]);
}
static int msaa_effective(void)
{
    static int cache[9],done[9];
    int want=s_msaa,n;
    if(want<=1||!dev)return 1;
    if(want>8)want=8;
    if(done[want])return cache[want];
    for(n=want;n>1;n>>=1){
        UINT qc=0,qd=0;
        ID3D11Device_CheckMultisampleQualityLevels(dev,DXGI_FORMAT_B8G8R8A8_UNORM,(UINT)n,&qc);
        ID3D11Device_CheckMultisampleQualityLevels(dev,DXGI_FORMAT_D24_UNORM_S8_UINT,(UINT)n,&qd);
        if(qc>0&&qd>0)break;
    }
    done[want]=1;cache[want]=n<1?1:n;
    return cache[want];
}
static void target_resolve(Target *t)
{
    if(!t||!t->msaa_color||!t->color||t->ms<=1)return;
    if(!t->dirty)return;
    ID3D11DeviceContext_OMSetRenderTargets(ctx,0,NULL,NULL);
    ID3D11DeviceContext_ResolveSubresource(ctx,(ID3D11Resource*)t->color,0,
            (ID3D11Resource*)t->msaa_color,0,DXGI_FORMAT_B8G8R8A8_UNORM);
}
void redcard_set_resolution_scale(int scale)
{
    int prev;
    if(scale<1)scale=1;if(scale>4)scale=4;
    if(scale==s_scale)return;
    prev=s_scale;
    s_scale=scale;
    targets_reset_all();
    fprintf(stderr,"[RC DX11] render scale %dx (%dx%d)\n",s_scale,640*s_scale,480*s_scale);
}
void redcard_set_msaa(int samples)
{
    int m=samples>=8?8:samples>=4?4:samples>=2?2:1;
    if(m==s_msaa)return;
    s_msaa=m;
    targets_reset_all();
    fprintf(stderr,"[RC DX11] MSAA %dx\n",s_msaa);
}
static void ensure_blit(void)
{
    static const char hlsl[]=
        "Texture2D t:register(t0);SamplerState s:register(s0);"
        "struct V{float4 p:SV_Position;float2 uv:TEXCOORD;};"
        "V vs(uint i:SV_VertexID){V o;float2 q=float2((i<<1)&2,i&2);o.uv=q;"
        "o.p=float4(q*float2(2,-2)+float2(-1,1),0,1);return o;}"
        "float4 ps(V i):SV_Target{return t.Sample(s,i.uv);}";
    ID3DBlob *v,*p;D3D11_SAMPLER_DESC sm={0};
    if(blit_vs)return;
    initialize_device();
    v=compile_source(hlsl,sizeof hlsl-1,"vs","vs_4_0");
    p=compile_source(hlsl,sizeof hlsl-1,"ps","ps_4_0");
    CHECK(ID3D11Device_CreateVertexShader(dev,ID3D10Blob_GetBufferPointer(v),ID3D10Blob_GetBufferSize(v),NULL,&blit_vs));
    CHECK(ID3D11Device_CreatePixelShader(dev,ID3D10Blob_GetBufferPointer(p),ID3D10Blob_GetBufferSize(p),NULL,&blit_ps));
    DROP(v);DROP(p);
    sm.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU=sm.AddressV=sm.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    sm.MaxLOD=D3D11_FLOAT32_MAX;
    CHECK(ID3D11Device_CreateSamplerState(dev,&sm,&blit_samp));
}
static void blit_to_guest(Target *t)
{
    D3D11_VIEWPORT vp={0};
    ID3D11ShaderResourceView *srv;
    ID3D11RenderTargetView *null_rtv=NULL;
    ID3D11ShaderResourceView *null_srv[4]={0};
    float factor[4]={0,0,0,0};
    if(!t->down||!t->down_rtv||!t->color_srv)return;
    ensure_blit();
    /* Opaque replace — leftover blend / color-write / depth from the last draw
     * would wipe or darken the guest-sized downsample (Hitz avoids this path by
     * presenting the scaled RT directly; Redcard must write guest FB). */
    ID3D11DeviceContext_OMSetRenderTargets(ctx,0,NULL,NULL);
    ID3D11DeviceContext_PSSetShaderResources(ctx,0,4,null_srv);
    ID3D11DeviceContext_OMSetBlendState(ctx,NULL,factor,0xffffffff);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx,NULL,0);
    ID3D11DeviceContext_RSSetState(ctx,NULL);
    vp.Width=(float)t->s.width;vp.Height=(float)t->s.height;vp.MinDepth=0;vp.MaxDepth=1;
    ID3D11DeviceContext_OMSetRenderTargets(ctx,1,&t->down_rtv,NULL);
    ID3D11DeviceContext_RSSetViewports(ctx,1,&vp);
    ID3D11DeviceContext_IASetInputLayout(ctx,NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx,blit_vs,NULL,0);
    ID3D11DeviceContext_PSSetShader(ctx,blit_ps,NULL,0);
    ID3D11DeviceContext_GSSetShader(ctx,NULL,NULL,0);
    srv=t->color_srv;
    ID3D11DeviceContext_PSSetShaderResources(ctx,0,1,&srv);
    ID3D11DeviceContext_PSSetSamplers(ctx,0,1,&blit_samp);
    ID3D11DeviceContext_Draw(ctx,3,0);
    ID3D11DeviceContext_PSSetShaderResources(ctx,0,4,null_srv);
    ID3D11DeviceContext_OMSetRenderTargets(ctx,1,&null_rtv,NULL);
}

/* ---- DDS texture pack (Hitz-style content-hash replacements) ----
 * Files named <16 hex digits of CRC32|Adler32 of w,h + BGRA>*.dds under
 * RECOMP_TEXPACK, %LOCALAPPDATA%\RedCard2003\texture_pack, or <exe>\texture_pack
 * replace the decoded guest texture when [textures] enabled (any Render Scale). */
#define NTEXCAT 6
static int s_hires = 1;
static int s_cat_on[NTEXCAT] = { 1, 1, 1, 1, 1, 1 };
static int s_tex_flush;
static const char *const TEX_CAT_NM[NTEXCAT] = {
    "crowd", "images", "particle", "stadiums", "teams", "textures"
};
typedef struct {
    uint64_t hash;
    uint8_t cat;
    char dds_path[MAX_PATH];
} PackFile;
static PackFile *s_pack;
static int s_npack, s_pack_init;
static unsigned s_pack_hits[NTEXCAT];

static int dds_block_bytes(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_BC1_UNORM: return 8;
    case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC7_UNORM: return 16;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM: return 0;
    default: return -1;
    }
}
static size_t dds_level(DXGI_FORMAT f, uint32_t w, uint32_t h, UINT *pitch)
{
    int bb = dds_block_bytes(f);
    if (bb > 0) {
        *pitch = ((w + 3) / 4) * (UINT)bb;
        return (size_t)*pitch * ((h + 3) / 4);
    }
    *pitch = w * 4;
    return (size_t)*pitch * h;
}
static uint32_t rd32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int pack_load_dds(const char *path, ID3D11Texture2D **tex, ID3D11ShaderResourceView **srv)
{
    uint8_t header[148];
    D3D11_TEXTURE2D_DESC td = {0};
    D3D11_SUBRESOURCE_DATA levels[15] = {0};
    FILE *f = fopen(path, "rb");
    uint8_t *data = NULL;
    uint32_t w, h, max_mips, i;
    size_t offset = 128, size = 0;
    long file_size;
    HRESULT hr = E_FAIL;

    if (!f) return 0;
    if (fread(header, 1, 128, f) != 128 || memcmp(header, "DDS ", 4) ||
        rd32(header + 4) != 124 || rd32(header + 76) != 32 ||
        (rd32(header + 112) & 0x0020FE00u)) goto out;
    td.Height = rd32(header + 12);
    td.Width = rd32(header + 16);
    td.MipLevels = rd32(header + 28);
    if (!td.MipLevels) td.MipLevels = 1;
    if (!td.Width || !td.Height || td.Width > 16384 || td.Height > 16384) goto out;
    for (w = td.Width > td.Height ? td.Width : td.Height, max_mips = 0; w; w >>= 1) max_mips++;
    if (td.MipLevels > max_mips || td.MipLevels > 15) goto out;

    if (rd32(header + 80) & 4) {
        if (!memcmp(header + 84, "DX10", 4)) {
            if (fread(header + 128, 1, 20, f) != 20 || rd32(header + 132) != 3 ||
                (rd32(header + 136) & 4) || rd32(header + 140) != 1) goto out;
            td.Format = (DXGI_FORMAT)rd32(header + 128);
            offset = 148;
        } else if (!memcmp(header + 84, "DXT1", 4)) td.Format = DXGI_FORMAT_BC1_UNORM;
        else if (!memcmp(header + 84, "DXT3", 4)) td.Format = DXGI_FORMAT_BC2_UNORM;
        else if (!memcmp(header + 84, "DXT5", 4)) td.Format = DXGI_FORMAT_BC3_UNORM;
    } else if ((rd32(header + 80) & 0x40) && rd32(header + 88) == 32 &&
               rd32(header + 96) == 0xFF00 && rd32(header + 104) == 0xFF000000u) {
        if (rd32(header + 92) == 0xFF0000 && rd32(header + 100) == 0xFF)
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        else if (rd32(header + 92) == 0xFF && rd32(header + 100) == 0xFF0000)
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    if (dds_block_bytes(td.Format) < 0) goto out;
    if (dds_block_bytes(td.Format) && ((td.Width & 3) || (td.Height & 3))) goto out;

    for (i = 0, w = td.Width, h = td.Height; i < td.MipLevels; i++) {
        levels[i].SysMemSlicePitch = (UINT)dds_level(td.Format, w, h, &levels[i].SysMemPitch);
        size += levels[i].SysMemSlicePitch;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    if (fseek(f, 0, SEEK_END) || (file_size = ftell(f)) < 0 ||
        offset + size > (size_t)file_size || fseek(f, (long)offset, SEEK_SET)) goto out;
    data = (uint8_t *)malloc(size);
    if (!data || fread(data, 1, size, f) != size) goto out;
    for (i = 0, offset = 0; i < td.MipLevels; i++) {
        levels[i].pSysMem = data + offset;
        offset += levels[i].SysMemSlicePitch;
    }

    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateTexture2D(dev, &td, levels, tex);
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)*tex, NULL, srv);
        if (FAILED(hr)) { ID3D11Texture2D_Release(*tex); *tex = NULL; }
        else {
            static unsigned logged;
            if (logged++ < 8)
                fprintf(stderr, "[RC DX11] DDS replacement %ux%u fmt %u, %u mips: %s\n",
                        td.Width, td.Height, (unsigned)td.Format, td.MipLevels, path);
        }
    }
out:
    free(data);
    fclose(f);
    return SUCCEEDED(hr);
}

/* CRC-32 high / Adler-32 low over width,height + BGRA — same as Hitz / the texture pipeline. */
static uint64_t px_hash(uint32_t w, uint32_t hh, const uint32_t *px)
{
    static uint32_t tab[256]; static int init;
    uint32_t crc = 0xFFFFFFFFu, a = 1, b = 0;
    uint8_t wh[8];
    const uint8_t *p = (const uint8_t *)px;
    size_t n = (size_t)w * hh * 4, i, k;
    if (!init) {
        for (i = 0; i < 256; i++) {
            uint32_t c = (uint32_t)i; int j;
            for (j = 0; j < 8; j++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
        init = 1;
    }
    memcpy(wh, &w, 4); memcpy(wh + 4, &hh, 4);
    for (i = 0; i < 8; i++) { crc = tab[(crc ^ wh[i]) & 0xFF] ^ (crc >> 8); a += wh[i]; b += a; }
    a %= 65521u; b %= 65521u;
    for (i = 0; i < n; ) {
        size_t m = n - i < 5552 ? n - i : 5552;
        for (k = 0; k < m; k++) { uint8_t c = p[i + k]; crc = tab[(crc ^ c) & 0xFF] ^ (crc >> 8); a += c; b += a; }
        a %= 65521u; b %= 65521u; i += m;
    }
    return ((uint64_t)(crc ^ 0xFFFFFFFFu) << 32) | ((b << 16) | a);
}

static void pack_scan(const char *dir, int depth, int cat)
{
    WIN32_FIND_DATAA fd;
    char pat[MAX_PATH + 8];
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char full[MAX_PATH];
        size_t n = strlen(fd.cFileName);
        int dds = n > 20 && !_stricmp(fd.cFileName + n - 4, ".dds");
        if (fd.cFileName[0] == '.') continue;
        snprintf(full, sizeof full, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 8) {
                int c = cat, k;
                if (depth == 0) {
                    c = NTEXCAT - 1;
                    for (k = 0; k < NTEXCAT; k++)
                        if (!_stricmp(fd.cFileName, TEX_CAT_NM[k])) { c = k; break; }
                }
                pack_scan(full, depth + 1, c);
            }
            continue;
        }
        if (dds) {
            char hx[17];
            int i, ok = 1;
            uint64_t v;
            memcpy(hx, fd.cFileName, 16); hx[16] = 0;
            for (i = 0; i < 16; i++) if (!isxdigit((unsigned char)hx[i])) ok = 0;
            if (!ok) continue;
            v = _strtoui64(hx, NULL, 16);
            for (i = 0; i < s_npack; i++) if (s_pack[i].hash == v) break;
            if (i == s_npack) {
                if ((s_npack & 255) == 0) {
                    PackFile *grown = (PackFile *)realloc(s_pack, sizeof *s_pack * (size_t)(s_npack + 256));
                    if (!grown) break;
                    s_pack = grown;
                }
                memset(&s_pack[i], 0, sizeof s_pack[i]);
                s_pack[i].hash = v;
                s_pack[i].cat = (uint8_t)cat;
                s_npack++;
            }
            if (!s_pack[i].dds_path[0])
                snprintf(s_pack[i].dds_path, MAX_PATH, "%s", full);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void pack_load_settings(void)
{
    char path[MAX_PATH], base[MAX_PATH];
    int i;
    if (!GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base) || strlen(base) > MAX_PATH - 40)
        return;
    snprintf(path, sizeof path, "%s\\RedCard2003\\settings.ini", base);
    s_hires = GetPrivateProfileIntA("textures", "enabled", 1, path) != 0;
    for (i = 0; i < NTEXCAT; i++)
        s_cat_on[i] = GetPrivateProfileIntA("textures", TEX_CAT_NM[i], 1, path) != 0;
}

static void pack_init(void)
{
    char dir[MAX_PATH];
    const char *e = getenv("RECOMP_TEXPACK");
    if (s_pack_init) return;
    s_pack_init = 1;
    pack_load_settings();
    if (e && *e) {
        snprintf(dir, sizeof dir, "%s", e);
        pack_scan(dir, 0, NTEXCAT - 1);
    } else {
        if (GetEnvironmentVariableA("LOCALAPPDATA", dir, sizeof dir - 40)) {
            strcat(dir, "\\RedCard2003\\texture_pack");
            pack_scan(dir, 0, NTEXCAT - 1);
        }
        if (GetModuleFileNameA(NULL, dir, sizeof dir - 32)) {
            char *sl = strrchr(dir, '\\');
            if (sl) { strcpy(sl, "\\texture_pack"); pack_scan(dir, 0, NTEXCAT - 1); }
        }
    }
    if (s_npack)
        fprintf(stderr, "[RC DX11] texture pack: %d replacement(s) (hires=%d)\n", s_npack, s_hires);
}

static void texture_cache_flush(void)
{
    unsigned i;
    if (ctx) ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
    for (i = 0; i < MAX_TEXTURES; i++) {
        DROP(textures[i].srv); DROP(textures[i].tex);
        free(textures[i].source);
        memset(&textures[i], 0, sizeof textures[i]);
    }
    texture_next = 0;
}

void redcard_texture_pack_flush(void)
{
    s_tex_flush = 1;
    texture_cache_flush();
}

int redcard_pack_installed(void)
{
    pack_init();
    return s_npack > 0;
}

void redcard_set_hires(int on)
{
    on = on ? 1 : 0;
    if (on == s_hires) return;
    s_hires = on;
    redcard_texture_pack_flush();
}

static const PackFile *pack_lookup(uint64_t hash)
{
    int i;
    if (!hash || !s_hires) return NULL;
    pack_init();
    for (i = 0; i < s_npack; i++)
        if (s_pack[i].hash == hash) return &s_pack[i];
    return NULL;
}

/* Pack misses per texture address (see try_pack in texture()). */
typedef struct{uint32_t off,w,h;unsigned miss;DWORD t0,until;}PackDyn;
static PackDyn s_pack_dyn[128];
static unsigned s_pack_ndyn;
static PackDyn *pack_dyn_slot(const Nv2aTexture *k)
{
    unsigned j;
    for(j=0;j<s_pack_ndyn;j++)
        if(s_pack_dyn[j].off==k->offset&&s_pack_dyn[j].w==k->width&&s_pack_dyn[j].h==k->height)return &s_pack_dyn[j];
    j=s_pack_ndyn<128?s_pack_ndyn++:(k->offset>>12)%128;   /* full: reuse a slot */
    s_pack_dyn[j].off=k->offset;s_pack_dyn[j].w=k->width;s_pack_dyn[j].h=k->height;s_pack_dyn[j].miss=0;s_pack_dyn[j].t0=s_pack_dyn[j].until=0;
    return &s_pack_dyn[j];
}

/* RECOMP_TEXPACK_DIAG=1: one line per distinct texture hash looked up. */
static void pack_diag(const Nv2aTexture *k,uint64_t h,const PackFile *p,const char *how)
{
    static int on=-1;static uint64_t seen[4096];static unsigned n,hits,miss;unsigned i;
    if(on<0){const char *e=getenv("RECOMP_TEXPACK_DIAG");on=e&&*e&&*e!='0';}
    if(!on)return;
    for(i=0;i<n;i++)if(seen[i]==h)return;
    if(n<4096)seen[n++]=h;
    if(p)hits++;else miss++;
    fprintf(stderr,"[RC PACK] %s %016llX%s %08X fmt %X %ux%u pal %08X (hits %u miss %u)%s%s\n",
        p?"HIT ":"miss",(unsigned long long)h,how,k->offset,k->color,k->width,k->height,k->palette,hits,miss,
        p?" ":"",p?p->dds_path:"");
}

/* Build a mip chain for minification. Skip screen-sized linear surfaces that
 * RedCard rewrites every match frame — those are not the pitch, and GenerateMips
 * every upload would dominate. */
static int texture_wants_mips(const Nv2aTexture *k)
{
    if(!texture_wants_linear(k)||k->width<4||k->height<4)return 0;
    if((k->color==0x12||k->color==0x1E)&&k->width>=320&&k->height>=240)return 0;
    return 1;
}

static Texture *texture(const Nv2aTexture *key)
{
    size_t count=(size_t)key->width*key->height,i;uint64_t h=1469598103934665603ull;
    size_t raw_bytes;LONGLONG decode_at;
    const uint8_t *mem=(const uint8_t*)xbox_GetMemoryOffset();
    Texture *t,*existing=NULL;D3D11_TEXTURE2D_DESC td={0};D3D11_SUBRESOURCE_DATA data={0};
    if(!count||count>4096*4096)return NULL;
    /* Render-to-texture: prefer a GPU copy from the dirty RT (no Map/decode).
     * Size mismatch still forces an immediate guest sync then the normal path. */
    for(i=0;i<MAX_TARGETS;i++)if(targets[i].dirty && targets[i].color && targets[i].s.color_va==key->offset){
        Texture *rttex=texture_from_rt(&targets[i],key);
        if(rttex){rt_decodes++;return rttex;}
        sync_target_immediate(&targets[i]);
        break;
    }
    /* A clean target whose pipelined present readback has not landed yet:
     * guest memory still holds the previous capture. Finish that copy before
     * reading it, or the first frame of a scene dissolve overlays the shot
     * before last (the capture surface is >= 320x240, so present issues it). */
    for(i=0;i<MAX_TARGETS;i++)if(targets[i].copy_pending && targets[i].s.color_va==key->offset)
        sync_target_complete(&targets[i]);
    raw_bytes=texture_bytes(key);
    if(raw_bytes>0x4000000u)return NULL;
    /* Guest writes can occur between two batches in the same frame. Compare
     * exact backing bytes and CLUT on every bind until write tracking can
     * prove an allocation unchanged. memcmp uses the host's vectorized path;
     * a frame counter or hash alone cannot establish texture identity. */
    for(i=0;i<MAX_TEXTURES;i++)if(textures[i].tex && memcmp(&textures[i].key,key,sizeof(*key))==0){
        existing=&textures[i];
        if(raw_bytes && existing->source_bytes==raw_bytes &&
           !memcmp(existing->source,mem+key->offset,raw_bytes) &&
           (!key->palette || !memcmp(existing->palette,mem+key->palette,sizeof existing->palette)))return existing;
        break;
    }
    if(count>decoded_size){uint32_t *p=realloc(decoded,count*4);if(!p)fatal("texture decode allocation",E_OUTOFMEMORY);decoded=p;decoded_size=count;}
    decodes++;decoded_texels+=count;
    {   /* Which textures are re-decoded: report each (offset,format,size) on its 2nd, 10th, 100th... decode. */
        static struct {uint32_t off,fmt,w,h,pal;unsigned n;} seen[64];static unsigned nseen;unsigned j,n;
        for(j=0;j<nseen;j++)if(seen[j].off==key->offset&&seen[j].fmt==key->color&&seen[j].w==key->width&&seen[j].h==key->height)break;
        if(j==nseen&&nseen<64){seen[j].off=key->offset;seen[j].fmt=key->color;seen[j].w=key->width;seen[j].h=key->height;seen[j].pal=key->palette;seen[j].n=0;nseen++;}
        if(j<nseen){n=++seen[j].n;while(n>=10&&n%10==0)n/=10;
            if(n==1&&seen[j].n>1)fprintf(stderr,"[RC TEX] re-decode #%u: %08X fmt %X %ux%u palette %08X pitch %u\n",seen[j].n,key->offset,key->color,key->width,key->height,key->palette,key->pitch);}
    }
    for(i=0;i<MAX_TARGETS;i++)if(targets[i].color && targets[i].s.color_va==key->offset){rt_decodes++;break;}
    decode_at=now_ticks();
    /* LIN_A8R8G8B8 is already BGRA in guest RAM. LIN_X8R8G8B8 uploads as
     * DXGI B8G8R8X8_UNORM (GPU forces A=1). Swizzled A8/X8R8G8B8 use the
     * Morton unswizzle helper — orders of magnitude faster than per-texel
     * sample_tex via nv2a_backend_decode_texture (match hot path: 512x256). */
    int direct_guest_bgra=0;DXGI_FORMAT tex_format=DXGI_FORMAT_B8G8R8A8_UNORM;
    if((key->color==0x12||key->color==0x1E) && key->pitch>=key->width*4){
        direct_guest_bgra=1;
        if(key->color==0x1E)tex_format=DXGI_FORMAT_B8G8R8X8_UNORM;
    }else if((key->color==0x06||key->color==0x07) && !(key->width&(key->width-1)) && !(key->height&(key->height-1))){
        xbox_unswizzle_rect(decoded,mem+key->offset,key->width,key->height,4);
        if(key->color==0x07){
            size_t n=(size_t)key->width*key->height,j;
            for(j=0;j<n;j++)decoded[j]|=0xFF000000u;
        }
        {static unsigned n;if(n++<4)fprintf(stderr,"[RC DX11] unswizzle fmt %X %ux%u (skip sample_tex decode)\n",
            key->color,key->width,key->height);}
    }else if(key->color==0x0b && key->palette && !(key->width&(key->width-1)) && !(key->height&(key->height-1))){
        uint32_t mx,my,oy=0,y,x;const uint8_t *src=mem+key->offset;uint32_t pal[256];
        memcpy(pal,mem+key->palette,sizeof pal);xbox_swizzle_masks(key->width,key->height,&mx,&my);
        for(y=0;y<key->height;y++){
            uint32_t ox=0;for(x=0;x<key->width;x++){decoded[(size_t)y*key->width+x]=pal[src[oy|ox]];ox=(ox-mx)&mx;}
            oy=(oy-my)&my;
        }
    }else if(!nv2a_backend_decode_texture(key,decoded)){
        static unsigned warnings;
        if(warnings++<16)fprintf(stderr,"[RC DX11] unsupported texture %08X fmt %X %ux%u\n",key->offset,key->color,key->width,key->height);
        return NULL;
    }
    texture_decode_ticks+=now_ticks()-decode_at;
    if(s_tex_flush){s_tex_flush=0;/* cache already cleared by redcard_texture_pack_flush */}
    if(!raw_bytes && !direct_guest_bgra)for(i=0;i<count;i++){h^=decoded[i];h*=1099511628211ull;}
    if(!raw_bytes && !direct_guest_bgra && existing && existing->hash==h && !existing->repl)return existing;
    /* Same texture, new contents (RedCard rewrites three screen-sized ones
     * every match frame): update it in place. Creating a new texture each
     * time evicted the rest of the cache and, minutes in, left the executor
     * blocked in the driver. */
    if(existing && key->color==0x1E && existing->tex && !existing->repl){
        D3D11_TEXTURE2D_DESC ed={0};ID3D11Texture2D_GetDesc(existing->tex,&ed);
        if(ed.Format!=DXGI_FORMAT_B8G8R8X8_UNORM){
            DROP(existing->srv);DROP(existing->tex);free(existing->source);
            memset(existing,0,sizeof *existing);existing=NULL;
        }
    }
    /* Hi-res DDS pack: hash decoded BGRA (pipeline / Hitz CRC|Adler) and swap in a sharper texture. */
    {
        const PackFile *pack=NULL;
        ID3D11Texture2D *dds_tex=NULL;
        ID3D11ShaderResourceView *dds_srv=NULL;
        uint64_t ph=0;
        int try_pack=s_hires &&
            !((key->color==0x12||key->color==0x1E)&&key->width>=320&&key->height>=240);
        /* Render-to-texture captures (scene dissolves, replays) are drawn by
         * the game each frame and never in the pack; hashing them is pure cost. */
        if(try_pack)for(i=0;i<MAX_TARGETS;i++)
            if(targets[i].color&&targets[i].s.color_va==key->offset){try_pack=0;break;}
        /* Textures the game keeps redrawing on the CPU (e.g. 81070000, a
         * 512x256 animated surface) change every frame and are never in the
         * pack: after 8 misses at one address, stop hashing it. */
        if(try_pack){
            PackDyn *d=pack_dyn_slot(key);DWORD now=GetTickCount();
            if(d->until&&(int)(d->until-now)>0)try_pack=0;
            else if(d->until){d->until=0;d->miss=0;}
        }
        if(try_pack){
            const uint32_t *hash_px=decoded;
            if(direct_guest_bgra){
                uint32_t y,x;const uint8_t *src=mem+key->offset;
                if(count>decoded_size){uint32_t *p=realloc(decoded,count*4);if(!p)fatal("texture decode allocation",E_OUTOFMEMORY);decoded=p;decoded_size=count;}
                for(y=0;y<key->height;y++){
                    const uint32_t *row=(const uint32_t*)(src+(size_t)y*key->pitch);
                    for(x=0;x<key->width;x++){
                        uint32_t px=row[x];
                        if(key->color==0x1E)px|=0xFF000000u;
                        decoded[(size_t)y*key->width+x]=px;
                    }
                }
                hash_px=decoded;
            }
            ph=px_hash(key->width,key->height,hash_px);
            pack=pack_lookup(ph);
            pack_diag(key,ph,pack,"");
            {   /* 8 misses within 3 s at one address: a surface the game redraws
                 * every frame. Skip hashing it for 10 s, then look again. */
                PackDyn *d=pack_dyn_slot(key);DWORD now=GetTickCount();
                if(pack)d->miss=0;
                else{
                    if(!d->miss||now-d->t0>3000){d->miss=0;d->t0=now;}
                    if(++d->miss>=8)d->until=now+10000;
                }
            }
            if(pack && pack->dds_path[0] && s_cat_on[pack->cat] &&
               pack_load_dds(pack->dds_path,&dds_tex,&dds_srv)){
                s_pack_hits[pack->cat]++;
                if(existing){
                    DROP(existing->srv);DROP(existing->tex);free(existing->source);
                    memset(existing,0,sizeof *existing);
                    t=existing;
                }else{
                    t=&textures[texture_next++%MAX_TEXTURES];DROP(t->srv);DROP(t->tex);
                    free(t->source);memset(t,0,sizeof *t);
                }
                t->tex=dds_tex;t->srv=dds_srv;t->has_mips=1;t->repl=1;
                t->key=*key;t->hash=h?h:ph;texture_snapshot(t,key,raw_bytes,mem);
                return t;
            }
        }
    }
    if(existing && existing->repl){
        /* Was a pack texture; content changed or pack miss — remake from guest. */
        DROP(existing->srv);DROP(existing->tex);free(existing->source);
        memset(existing,0,sizeof *existing);existing=NULL;
    }
    if(existing){
        if(direct_guest_bgra)
            upload_texture(ctx,(ID3D11Resource*)existing->tex,0,NULL,mem+key->offset,key->pitch,0);
        else
            upload_texture(ctx,(ID3D11Resource*)existing->tex,0,NULL,decoded,key->width*4,0);
        if(existing->has_mips)ID3D11DeviceContext_GenerateMips(ctx,existing->srv);
        existing->hash=h;existing->repl=0;texture_snapshot(existing,key,raw_bytes,mem);return existing;
    }
    t=&textures[texture_next++%MAX_TEXTURES];DROP(t->srv);DROP(t->tex);t->has_mips=0;t->repl=0;
    td.Width=key->width;td.Height=key->height;td.ArraySize=1;td.Format=tex_format;
    td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    if(texture_wants_mips(key)){
        /* Full chain from level 0; GenerateMips needs RT bind. */
        td.MipLevels=0;td.BindFlags|=D3D11_BIND_RENDER_TARGET;td.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS;
        CHECK(ID3D11Device_CreateTexture2D(dev,&td,NULL,&t->tex));
        if(direct_guest_bgra)
            upload_texture(ctx,(ID3D11Resource*)t->tex,0,NULL,mem+key->offset,key->pitch,0);
        else
            upload_texture(ctx,(ID3D11Resource*)t->tex,0,NULL,decoded,key->width*4,0);
        CHECK(ID3D11Device_CreateShaderResourceView(dev,(ID3D11Resource*)t->tex,NULL,&t->srv));
        ID3D11DeviceContext_GenerateMips(ctx,t->srv);t->has_mips=1;
        {static unsigned n;if(n++<16)fprintf(stderr,"[RC DX11] mip chain %08X fmt %X %ux%u filter %08X aniso %u\n",
            key->offset,key->color,key->width,key->height,key->filter,texture_aniso());}
    }else{
        td.MipLevels=1;
        data.pSysMem=direct_guest_bgra?(const void*)(mem+key->offset):(const void*)decoded;
        data.SysMemPitch=direct_guest_bgra?key->pitch:key->width*4;
        CHECK(ID3D11Device_CreateTexture2D(dev,&td,&data,&t->tex));
        CHECK(ID3D11Device_CreateShaderResourceView(dev,(ID3D11Resource*)t->tex,NULL,&t->srv));
        if(tex_format==DXGI_FORMAT_B8G8R8X8_UNORM){
            static unsigned n;if(n++<4)fprintf(stderr,"[RC DX11] LIN_X8R8G8B8 %08X %ux%u as B8G8R8X8\n",
                key->offset,key->width,key->height);
        }
    }
    t->key=*key;t->hash=h;t->repl=0;texture_snapshot(t,key,raw_bytes,mem);return t;
}

/* State objects are immutable and few; creating five per draw cost the
 * executor thread D3D11 runtime hashing and locking on every batch. */
#define STATE_CACHE 256
#define DEFINE_STATE_CACHE(fn,Desc,Iface,create) \
static Iface *fn(const Desc *d) \
{ \
    static struct {Desc d;Iface *o;} c[STATE_CACHE];static unsigned n,next; \
    unsigned i; \
    for(i=0;i<n;i++)if(!memcmp(&c[i].d,d,sizeof *d))return c[i].o; \
    i=n<STATE_CACHE?n++:next++%STATE_CACHE;DROP(c[i].o); \
    c[i].d=*d;CHECK(create(dev,d,&c[i].o));return c[i].o; \
}
DEFINE_STATE_CACHE(blend_state,D3D11_BLEND_DESC,ID3D11BlendState,ID3D11Device_CreateBlendState)
DEFINE_STATE_CACHE(depth_state,D3D11_DEPTH_STENCIL_DESC,ID3D11DepthStencilState,ID3D11Device_CreateDepthStencilState)
DEFINE_STATE_CACHE(raster_state,D3D11_RASTERIZER_DESC,ID3D11RasterizerState,ID3D11Device_CreateRasterizerState)
DEFINE_STATE_CACHE(sampler_state,D3D11_SAMPLER_DESC,ID3D11SamplerState,ID3D11Device_CreateSamplerState)

static D3D11_COMPARISON_FUNC comparison(uint32_t f){return f>=0x200&&f<=0x207?(D3D11_COMPARISON_FUNC)(f-0x200+1):D3D11_COMPARISON_ALWAYS;}
static D3D11_BLEND blend(uint32_t f)
{
    switch(f){case 0:return D3D11_BLEND_ZERO;case 1:return D3D11_BLEND_ONE;
    case 0x300:return D3D11_BLEND_SRC_COLOR;case 0x301:return D3D11_BLEND_INV_SRC_COLOR;
    case 0x302:return D3D11_BLEND_SRC_ALPHA;case 0x303:return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304:return D3D11_BLEND_DEST_ALPHA;case 0x305:return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306:return D3D11_BLEND_DEST_COLOR;case 0x307:return D3D11_BLEND_INV_DEST_COLOR;
    case 0x308:return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001:case 0x8003:return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002:case 0x8004:return D3D11_BLEND_INV_BLEND_FACTOR;default:return D3D11_BLEND_ONE;}
}
static D3D11_STENCIL_OP stencil(uint32_t f)
{
    switch(f){case 0:return D3D11_STENCIL_OP_ZERO;case 0x1e01:return D3D11_STENCIL_OP_REPLACE;
    case 0x1e02:return D3D11_STENCIL_OP_INCR_SAT;case 0x1e03:return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150a:return D3D11_STENCIL_OP_INVERT;case 0x8507:return D3D11_STENCIL_OP_INCR;
    case 0x8508:return D3D11_STENCIL_OP_DECR;default:return D3D11_STENCIL_OP_KEEP;}
}
static D3D11_TEXTURE_ADDRESS_MODE address(uint32_t f)
{return f==1?D3D11_TEXTURE_ADDRESS_WRAP:f==2?D3D11_TEXTURE_ADDRESS_MIRROR:f==4?D3D11_TEXTURE_ADDRESS_BORDER:D3D11_TEXTURE_ADDRESS_CLAMP;}

static void clear(const Nv2aSurface *s,const Nv2aRenderState *rs,uint32_t flags,uint32_t argb,uint32_t zs)
{
    Target *t=target(s);float c[4]={(float)((argb>>16)&255)/255,(float)((argb>>8)&255)/255,(float)(argb&255)/255,(float)(argb>>24)/255};
    ID3D11RenderTargetView *rtv;UINT mask=0;(void)rs;if(!t)return;
    rtv=t->msaa_rtv?t->msaa_rtv:t->rtv;
    if(flags&0xf0){ID3D11DeviceContext_ClearRenderTargetView(ctx,rtv,c);t->dirty=1;}
    if(flags&1)mask|=D3D11_CLEAR_DEPTH;if(flags&2)mask|=D3D11_CLEAR_STENCIL;
    if(mask)ID3D11DeviceContext_ClearDepthStencilView(ctx,t->dsv,mask,(float)(zs>>8)/16777215,(UINT8)zs);
}

#include "redcard_gvs.inc"

static void draw_impl(const Nv2aSurface *s,const Nv2aBatch *b);
/* Creating analysis\draw_dump.trigger (relative to the working directory)
 * describes every batch of the next frame -- texture, screen bounds, w and
 * texel ranges, cull/combiner state -- to find which draws produce a visual
 * fault. The file is deleted when the dump starts. */
static unsigned dump_frames;
static uint32_t last_draw_va; /* colour surface of the most recent draw */
static uint32_t trace_tex;    /* RC_TRACE_TEX */
static void dump_batch(const Nv2aSurface *s,const Nv2aBatch *b)
{
    static unsigned index;
    const Nv2aRenderState *r=b->state;unsigned i;
    float x0=1e30f,x1=-1e30f,y0=1e30f,y1=-1e30f,w0=1e30f,w1=-1e30f,u0=1e30f,u1=-1e30f,v0=1e30f,v1=-1e30f,z0=1e30f,z1=-1e30f;
    if(!dump_frames)return;
    for(i=0;i<b->count;i++){const Nv2aVertex *p=&b->vertices[i];float w=p->rhw!=0?1/p->rhw:0;
        if(p->x<x0)x0=p->x;if(p->x>x1)x1=p->x;if(p->y<y0)y0=p->y;if(p->y>y1)y1=p->y;
        if(w<w0)w0=w;if(w>w1)w1=w;if(p->z<z0)z0=p->z;if(p->z>z1)z1=p->z;
        if(p->tex[0][0]<u0)u0=p->tex[0][0];if(p->tex[0][0]>u1)u1=p->tex[0][0];
        if(p->tex[0][1]<v0)v0=p->tex[0][1];if(p->tex[0][1]>v1)v1=p->tex[0][1];}
    fprintf(stderr,"[RC DRAW] f%u #%u surf=%08X n=%u x=%.0f..%.0f y=%.0f..%.0f z=%.3f..%.3f w=%.3g..%.3g uv=(%.3f..%.3f,%.3f..%.3f) tex=%08X fmt=%X %ux%u addr=%u/%u rc=%d st0=%08X fmt0=%X %ux%u addr0=%u/%u cull=%u/%X front=%X\n",
        frames,index++,s->color_va,b->count,x0,x1,y0,y1,z0,z1,w0,w1,u0,u1,v0,v1,
        b->texture?b->texture->offset:0,b->texture?b->texture->color:0,b->texture?b->texture->width:0,b->texture?b->texture->height:0,
        b->texture?b->texture->addr_u:0,b->texture?b->texture->addr_v:0,b->combiner!=NULL,
        b->stages&&(b->stage_valid&1)?b->stages[0].offset:0,b->stages&&(b->stage_valid&1)?b->stages[0].color:0,
        b->stages&&(b->stage_valid&1)?b->stages[0].width:0,b->stages&&(b->stage_valid&1)?b->stages[0].height:0,
        b->stages&&(b->stage_valid&1)?b->stages[0].addr_u:0,b->stages&&(b->stage_valid&1)?b->stages[0].addr_v:0,
        r->cull_enable,r->cull_face,r->front_face);
}
/* RED-8: stadium ad boards as FFP inline arrays used to hit the screen-space
 * path (w≈1 with world XY like x±1750), drawing a strip across kickoff/match
 * frames. The toolkit now transforms those batches (RECOMP_FFP_INLINE=1 default).
 * Warn once if that signature returns so a regression is obvious in session logs. */
static void warn_red8_banner(const Nv2aSurface *s,const Nv2aBatch *b)
{
    unsigned i;float x0=1e30f,x1=-1e30f,w0=1e30f,w1=-1e30f;static int warned;
    if(!s||!b||!b->count||b->gpu||warned)return;
    for(i=0;i<b->count;i++){
        const Nv2aVertex *p=&b->vertices[i];float w=p->rhw!=0.f?1.f/p->rhw:0.f;
        if(p->x<x0)x0=p->x;if(p->x>x1)x1=p->x;
        if(w<w0)w0=w;if(w>w1)w1=w;
    }
    if(w0>=0.5f&&w1<=1.5f&&(x1-x0)>(float)(s->width*2u)){
        warned=1;
        fprintf(stderr,"[RC DX11] WARN RED-8 untransformed world-space batch "
                "x=%.0f..%.0f w=%.3g..%.3g (set RECOMP_FFP_INLINE=1 / check FFP inline)\n",
                x0,x1,w0,w1);
        fflush(stderr);
    }
}
static void draw(const Nv2aSurface *s,const Nv2aBatch *b)
{
    LONGLONG t0=now_ticks();
    last_draw_va=s->color_va;
    {   /* The trigger is also polled here (every 16 draws), so a dump can start
         * during a stretch with no flips -- e.g. a loading screen -- and runs
         * until the next flip. */
        static unsigned polls;
        if(!dump_frames && (++polls&15)==0 && GetFileAttributesA("analysis\\draw_dump.trigger")!=INVALID_FILE_ATTRIBUTES){
            DeleteFileA("analysis\\draw_dump.trigger");dump_frames=1;
            fprintf(stderr,"[RC DRAW] dumping from draw %u (frame %u, no flip yet)\n",draws,frames);
        }
    }
    {   /* RC_TRACE_TEX=<hex guest address>: log every draw sampling it, with the
         * frame it lands in and its target -- e.g. a sky dome that goes missing. */
        static int init;uint32_t want;
        if(!init){const char *e=getenv("RC_TRACE_TEX");init=1;trace_tex=e?(uint32_t)strtoul(e,NULL,16):0;}
        want=trace_tex;
        if(want&&((b->texture&&b->texture->offset==want)||(b->stages&&(b->stage_valid&1)&&b->stages[0].offset==want)))
            fprintf(stderr,"[RC TRACE] f%u tex %08X into %08X n=%u gpu=%d\n",frames,want,s->color_va,b->count,b->gpu!=NULL);
    }
    dump_batch(s,b);
    warn_red8_banner(s,b);
    draw_impl(s,b);
    draw_ticks+=now_ticks()-t0;
}
static void draw_impl(const Nv2aSurface *s,const Nv2aBatch *b)
{
    Target *t=target(s);const Nv2aRenderState *r=b->state;Texture *tex=NULL;
    Texture *bound[4]={0};unsigned stage;
    int use_rc=b->combiner!=NULL;
    D3D11_MAPPED_SUBRESOURCE m;LONGLONG state_t0;
    D3D11_BLEND_DESC bl={0};D3D11_DEPTH_STENCIL_DESC ds={0};D3D11_RASTERIZER_DESC ra={0};D3D11_SAMPLER_DESC sm={0};
    ID3D11BlendState *bs=NULL;ID3D11DepthStencilState *dss=NULL;ID3D11RasterizerState *rast=NULL;ID3D11SamplerState *samp=NULL;
    D3D11_VIEWPORT vp={0};UINT stride=sizeof(Nv2aVertex),off=0;
    struct {float size[2],zscale,aref;uint32_t atest,afunc,textured,pad;float invtex[2],pad2[2];} params={0};
    float factor[4]={(float)((r->blend_color>>16)&255)/255,(float)((r->blend_color>>8)&255)/255,(float)(r->blend_color&255)/255,(float)(r->blend_color>>24)/255};
    if(!t||!b->count)return;
    if(use_rc){
        RcParams rc={0};const Nv2aCombiner *c=b->combiner;
        initialize_combiners();
        rc_ps=combiner_shader(c);
        rc.hdr[0]=c->control;rc.hdr[1]=c->stage_program;rc.hdr[2]=c->final0;rc.hdr[3]=c->final1;
        rc.hdr2[0]=c->final_c0;rc.hdr2[1]=c->final_c1;memcpy(rc.fog,b->fog_color,sizeof rc.fog);
        for(stage=0;stage<4;stage++){
            if((b->stage_valid&(1u<<stage)) && ((c->stage_program>>(stage*5))&31u))bound[stage]=texture(&b->stages[stage]);
            if(bound[stage]){
                const Nv2aTexture *k=&bound[stage]->key;
                int normalized=d3d8_format_is_swizzled(k->color)||d3d8_format_dxt_block_bytes(k->color);
                rc.uvs[stage][0]=normalized?1:1.0f/k->width;rc.uvs[stage][1]=normalized?1:1.0f/k->height;rc.uvs[stage][2]=1;
            }
        }
        for(stage=0;stage<8;stage++){
            rc.stages[2*stage][0]=c->color_icw[stage];rc.stages[2*stage][1]=c->alpha_icw[stage];
            rc.stages[2*stage][2]=c->color_ocw[stage];rc.stages[2*stage][3]=c->alpha_ocw[stage];
            rc.stages[2*stage+1][0]=c->factor0[stage];rc.stages[2*stage+1][1]=c->factor1[stage];
        }
        CHECK(map_bucket(ctx,(ID3D11Resource*)rc_cb,0,D3D11_MAP_WRITE_DISCARD,0,&m,&map_rc_ticks,&map_cb_peak));
        memcpy(m.pData,&rc,sizeof rc);ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)rc_cb,0);
        tex=bound[0];
    }else if(b->texture){tex=texture(b->texture);bound[0]=tex;}
    static int log_draws=-1;
    if(log_draws<0)log_draws=getenv("RECOMP_D3D11_LOG")!=NULL; /* getenv per draw showed up as strchr in profiles */
    if(log_draws && frames>10 && !b->gpu){
        static unsigned n;
        if(n++<100)fprintf(stderr,"[RC DX11] batch %u tex=%08X fmt=%X %ux%u decoded=%d v0=(%.1f %.1f %.3f %.3f %08X %.1f %.1f) blend=%u/%X/%X alpha=%u/%X/%u depth=%u/%X cull=%u/%X\n",b->count,b->texture?b->texture->offset:0,b->texture?b->texture->color:0,b->texture?b->texture->width:0,b->texture?b->texture->height:0,tex!=NULL,b->vertices[0].x,b->vertices[0].y,b->vertices[0].z,b->vertices[0].rhw,b->vertices[0].diffuse,b->vertices[0].u,b->vertices[0].v,r->blend_enable,r->blend_src,r->blend_dst,r->alpha_test_enable,r->alpha_func,r->alpha_ref,r->depth_test_enable,r->depth_func,r->cull_enable,r->cull_face);
    }
    if(!b->gpu)off=upload_vb_ring(b->vertices,(size_t)b->count*stride);
    params.size[0]=(float)s->width;params.size[1]=(float)s->height;params.zscale=r->depth_max>1?1/r->depth_max:1;
    params.aref=(float)r->alpha_ref;params.atest=r->alpha_test_enable;params.afunc=r->alpha_func-0x200;params.textured=tex!=NULL;
    params.invtex[0]=tex?1.0f/tex->key.width:1;params.invtex[1]=tex?1.0f/tex->key.height:1;
    CHECK(map_bucket(ctx,(ID3D11Resource*)cb,0,D3D11_MAP_WRITE_DISCARD,0,&m,&map_cb_ticks,&map_cb_peak));
    memcpy(m.pData,&params,sizeof(params));ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)cb,0);
    bl.RenderTarget[0].BlendEnable=r->blend_enable;bl.RenderTarget[0].SrcBlend=blend(r->blend_src);bl.RenderTarget[0].DestBlend=blend(r->blend_dst);
    bl.RenderTarget[0].BlendOp=r->blend_eq==0x800a?D3D11_BLEND_OP_SUBTRACT:r->blend_eq==0x800b?D3D11_BLEND_OP_REV_SUBTRACT:r->blend_eq==0x8007?D3D11_BLEND_OP_MIN:r->blend_eq==0x8008?D3D11_BLEND_OP_MAX:D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha=bl.RenderTarget[0].SrcBlend;bl.RenderTarget[0].DestBlendAlpha=bl.RenderTarget[0].DestBlend;
    if(bl.RenderTarget[0].SrcBlendAlpha==D3D11_BLEND_SRC_COLOR)bl.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_SRC_ALPHA;
    if(bl.RenderTarget[0].SrcBlendAlpha==D3D11_BLEND_INV_SRC_COLOR)bl.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;
    if(bl.RenderTarget[0].SrcBlendAlpha==D3D11_BLEND_DEST_COLOR)bl.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_DEST_ALPHA;
    if(bl.RenderTarget[0].SrcBlendAlpha==D3D11_BLEND_INV_DEST_COLOR)bl.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_INV_DEST_ALPHA;
    if(bl.RenderTarget[0].DestBlendAlpha==D3D11_BLEND_SRC_COLOR)bl.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_SRC_ALPHA;
    if(bl.RenderTarget[0].DestBlendAlpha==D3D11_BLEND_INV_SRC_COLOR)bl.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;
    if(bl.RenderTarget[0].DestBlendAlpha==D3D11_BLEND_DEST_COLOR)bl.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_DEST_ALPHA;
    if(bl.RenderTarget[0].DestBlendAlpha==D3D11_BLEND_INV_DEST_COLOR)bl.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_INV_DEST_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha=bl.RenderTarget[0].BlendOp;
    bl.RenderTarget[0].RenderTargetWriteMask=((r->color_mask&0x10000)?1:0)|((r->color_mask&0x100)?2:0)|((r->color_mask&1)?4:0)|((r->color_mask&0x1000000)?8:0);
    state_t0=now_ticks();
    bs=blend_state(&bl);
    ds.DepthEnable=r->depth_test_enable&&r->zeta_va;ds.DepthWriteMask=r->depth_write?D3D11_DEPTH_WRITE_MASK_ALL:D3D11_DEPTH_WRITE_MASK_ZERO;ds.DepthFunc=comparison(r->depth_func);
    ds.StencilEnable=r->stencil_enable;ds.StencilReadMask=(UINT8)r->stencil_func_mask;ds.StencilWriteMask=(UINT8)r->stencil_write_mask;
    ds.FrontFace.StencilFunc=comparison(r->stencil_func);ds.FrontFace.StencilFailOp=stencil(r->stencil_fail);ds.FrontFace.StencilDepthFailOp=stencil(r->stencil_zfail);ds.FrontFace.StencilPassOp=stencil(r->stencil_zpass);ds.BackFace=ds.FrontFace;
    dss=depth_state(&ds);
    ra.FillMode=D3D11_FILL_SOLID;ra.CullMode=!r->cull_enable?D3D11_CULL_NONE:r->cull_face==0x404?D3D11_CULL_FRONT:D3D11_CULL_BACK;
    ra.FrontCounterClockwise=r->front_face==0x901;ra.DepthClipEnable=b->gpu?TRUE:FALSE;
    rast=raster_state(&ra);
    sm.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sm.MaxLOD=D3D11_FLOAT32_MAX;sm.ComparisonFunc=D3D11_COMPARISON_ALWAYS;
    state_cache_ticks+=now_ticks()-state_t0;
    vp.Width=(float)(s->width*(unsigned)t->scale);vp.Height=(float)(s->height*(unsigned)t->scale);vp.MaxDepth=1;
    {ID3D11RenderTargetView *rtv=t->msaa_rtv?t->msaa_rtv:t->rtv;
    ID3D11DeviceContext_OMSetRenderTargets(ctx,1,&rtv,t->dsv);}
    ID3D11DeviceContext_OMSetBlendState(ctx,bs,factor,0xffffffff);ID3D11DeviceContext_OMSetDepthStencilState(ctx,dss,r->stencil_ref);
    ID3D11DeviceContext_RSSetState(ctx,rast);ID3D11DeviceContext_RSSetViewports(ctx,1,&vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ID3D11DeviceContext_IASetInputLayout(ctx,use_rc?rc_layout:layout);
    ID3D11DeviceContext_IASetVertexBuffers(ctx,0,1,&vb,&stride,&off);
    ID3D11DeviceContext_VSSetShader(ctx,use_rc?rc_vs:vs,NULL,0);ID3D11DeviceContext_PSSetShader(ctx,use_rc?rc_ps:ps,NULL,0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx,0,1,&cb);ID3D11DeviceContext_PSSetConstantBuffers(ctx,0,1,&cb);
    if(use_rc)ID3D11DeviceContext_PSSetConstantBuffers(ctx,1,1,&rc_cb);
    for(stage=0;stage<4;stage++){
        const Nv2aTexture *tk=NULL;unsigned aniso=1;int linear=0;
        ID3D11ShaderResourceView *srv=bound[stage]?bound[stage]->srv:NULL;
        ID3D11DeviceContext_PSSetShaderResources(ctx,stage,1,&srv);
        if(bound[stage])tk=&bound[stage]->key;
        else if(b->texture && stage==0)tk=b->texture;
        if(tk){linear=texture_wants_linear(tk);aniso=linear?texture_aniso():1;}
        sm.Filter=linear?(aniso>1?D3D11_FILTER_ANISOTROPIC:D3D11_FILTER_MIN_MAG_MIP_LINEAR)
                        :D3D11_FILTER_MIN_MAG_MIP_POINT;
        sm.MaxAnisotropy=aniso>1?aniso:1;
        sm.AddressU=tk?address(tk->addr_u):D3D11_TEXTURE_ADDRESS_CLAMP;
        sm.AddressV=tk?address(tk->addr_v):D3D11_TEXTURE_ADDRESS_CLAMP;
        state_t0=now_ticks();samp=sampler_state(&sm);state_cache_ticks+=now_ticks()-state_t0;
        ID3D11DeviceContext_PSSetSamplers(ctx,stage,1,&samp);
    }
    if(b->gpu){if(!gvs_draw(b->gpu))fatal("GPU vertex draw failed after accepted probe",E_FAIL);}
    else submit_draw(ctx,b->count,0);
    t->dirty=1;draws++;
}
static LONGLONG csv_previous_draw,csv_previous_sync;
static void flip(void)
{
    static unsigned last_frames,last_draws;static LONGLONG last_at;
    if(trace_tex){static unsigned prev;fprintf(stderr,"[RC TRACE] flip f%u last_target %08X draws %u\n",frames,last_draw_va,draws-prev);prev=draws;}
    if(dump_frames){
        /* Pair the dumped draw list with the image it produced. */
        static Target *shot;unsigned i;
        shot=NULL;
        for(i=0;i<MAX_TARGETS;i++)if(targets[i].color&&targets[i].s.color_va==last_draw_va)shot=&targets[i];
        if(shot){
            char path[64];FILE *bf;
            sync_target_immediate(shot);
            snprintf(path,sizeof path,"analysis\\draw_dump_f%u.bmp",frames);
            if((bf=fopen(path,"wb"))){
                uint32_t w=shot->s.width,h=shot->s.height,y;const uint8_t *mem=(const uint8_t*)xbox_GetMemoryOffset();
                uint8_t hd[54]={'B','M'};uint32_t sz=54+w*h*4;int32_t nh=-(int32_t)h;
                memcpy(hd+2,&sz,4);hd[10]=54;hd[14]=40;memcpy(hd+18,&w,4);memcpy(hd+22,&nh,4);hd[26]=1;hd[28]=32;
                fwrite(hd,1,54,bf);
                for(y=0;y<h;y++)fwrite(mem+shot->s.color_va+(size_t)y*shot->s.pitch,4,w,bf);
                fclose(bf);
            }
        }
        dump_frames--;
    }
    if(GetFileAttributesA("analysis\\draw_dump.trigger")!=INVALID_FILE_ATTRIBUTES){
        DeleteFileA("analysis\\draw_dump.trigger");dump_frames=1;
        fprintf(stderr,"[RC DRAW] dumping frame %u\n",frames+1);
    }
    {
        static FILE *csv;static int tried;static LONGLONG previous;LARGE_INTEGER f;LONGLONG at=now_ticks();
        if(!tried){const char *path=getenv("RECOMP_RENDER_CSV");tried=1;if(path){csv=fopen(path,"wb");if(csv)fputs("qpc,frame,interval_ms,draw_callback_ms,readback_ms,map_write_ms,map_read_ms,map_vb_ms,map_cb_ms,map_rc_ms,upload_ms,texture_decode_ms,draw_api_ms,state_cache_ms,map_peak_ms,map_vb_peak_ms,map_cb_peak_ms,vb_discard,vb_no_overwrite\n",csv);}}
        if(csv){QueryPerformanceFrequency(&f);double ms=1000.0/f.QuadPart;
            fprintf(csv,"%lld,%u,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%u,%u\n",at,frames+1,previous?(at-previous)*ms:0,(draw_ticks-csv_previous_draw)*ms,(sync_ticks-csv_previous_sync)*ms,map_write_ticks*ms,map_read_ticks*ms,map_vb_ticks*ms,map_cb_ticks*ms,map_rc_ticks*ms,texture_upload_ticks*ms,texture_decode_ticks*ms,draw_api_ticks*ms,state_cache_ticks*ms,map_peak*ms,map_vb_peak*ms,map_cb_peak*ms,map_vb_discard,map_vb_no_overwrite);fflush(csv);}
        previous=at;csv_previous_draw=draw_ticks;csv_previous_sync=sync_ticks;
        win_map_vb+=map_vb_ticks;win_map_cb+=map_cb_ticks;win_map_rc+=map_rc_ticks;win_upload+=texture_upload_ticks;
        win_draw_api+=draw_api_ticks;win_state+=state_cache_ticks;win_vb_discard+=map_vb_discard;win_vb_no_overwrite+=map_vb_no_overwrite;
        if(map_vb_peak>win_map_vb_peak)win_map_vb_peak=map_vb_peak;if(map_cb_peak>win_map_cb_peak)win_map_cb_peak=map_cb_peak;
        map_write_ticks=map_read_ticks=texture_upload_ticks=texture_decode_ticks=draw_api_ticks=map_peak=0;
        map_vb_ticks=map_cb_ticks=map_rc_ticks=map_vb_peak=map_cb_peak=state_cache_ticks=0;map_vb_discard=map_vb_no_overwrite=0;
    }
    frames++;
    if(frames<=3||frames%300==0){
        LARGE_INTEGER f;LONGLONG at=now_ticks();unsigned n=frames-last_frames;QueryPerformanceFrequency(&f);
        fprintf(stderr,"[RC DX11] completed frame %u; GPU draw batches %u\n",frames,draws);
        if(last_at && n){
            double ms=1000.0/f.QuadPart;
            fprintf(stderr,"[RC PERF] frames=%u wall=%.1fms/frame draws=%.0f/frame submit=%.2fms/frame readback=%.2fms/frame (%lld syncs) tex_decodes=%.1f/frame (%.1f from render targets, %.0f texels/frame)\n",
                n,(at-last_at)*ms/n,(double)(draws-last_draws)/n,draw_ticks*ms/n,sync_ticks*ms/n,sync_count,
                (double)decodes/n,(double)rt_decodes/n,(double)decoded_texels/n);
            fprintf(stderr,"[RC PERF] draw_split map_vb=%.2fms map_cb=%.2fms map_rc=%.2fms upload=%.2fms draw=%.2fms state=%.2fms /frame (vb_peak=%.2fms cb_peak=%.2fms discard=%u no_ow=%u)\n",
                win_map_vb*ms/n,win_map_cb*ms/n,win_map_rc*ms/n,win_upload*ms/n,win_draw_api*ms/n,win_state*ms/n,
                win_map_vb_peak*ms,win_map_cb_peak*ms,win_vb_discard,win_vb_no_overwrite);
        }
        last_frames=frames;last_draws=draws;last_at=at;draw_ticks=sync_ticks=sync_count=decodes=rt_decodes=decoded_texels=0;csv_previous_draw=csv_previous_sync=0;
        win_map_vb=win_map_cb=win_map_rc=win_upload=win_draw_api=win_state=win_map_vb_peak=win_map_cb_peak=0;win_vb_discard=win_vb_no_overwrite=0;
    }
}
void redcard_d3d11_register(void)
{
    static const Nv2aBackend backend={clear,draw,flip,nv2a_gpuvs_probe};
    const char *e=getenv("RECOMP_D3D11");
    const char *es,*em;
    if(e && !strcmp(e,"0")){fprintf(stderr,"[RC DX11] software renderer selected\n");return;}
    es=getenv("RECOMP_D3D11_SCALE");
    if(es&&es[0]){int sc=atoi(es);if(sc<1)sc=1;if(sc>4)sc=4;s_scale=sc;}
    em=getenv("RECOMP_D3D11_MSAA");
    if(em&&em[0]){int m=atoi(em);s_msaa=m>=8?8:m>=4?4:m>=2?2:1;}
    nv2a_backend_register_with_present_sync(&backend,sync_targets_present);
    if(!getenv("RECOMP_D3D_PRESENT"))_putenv_s("RECOMP_D3D_PRESENT","1");
    /* Play default: GPU vertex programs on (RED-9 A/B: +~28% match FPS, 0 verify mismatches).
     * RECOMP_D3D11_GPUVS=0 forces the CPU interpreter. Async compile avoids ~0.3–1 s sync
     * D3DCompile hitches; verify mode still compiles synchronously. */
    if(!getenv("RECOMP_D3D11_GPUVS"))_putenv_s("RECOMP_D3D11_GPUVS","1");
    if(!getenv("RECOMP_D3D11_GPUVS_ASYNC"))_putenv_s("RECOMP_D3D11_GPUVS_ASYNC","1");
    pack_init();
    fprintf(stderr,"[RC DX11] registered GPU rasterizer; pipelined present readback + RT→SRV copies\n");
    fprintf(stderr,"[RC DX11] render scale %dx MSAA %dx (F1/config; RECOMP_D3D11_SCALE / RECOMP_D3D11_MSAA)\n",
            s_scale,s_msaa);
    fprintf(stderr,"[RC DX11] GPU VS default on (RECOMP_D3D11_GPUVS=%s, ASYNC=%s; set GPUVS=0 for CPU)\n",
        getenv("RECOMP_D3D11_GPUVS")?getenv("RECOMP_D3D11_GPUVS"):"?",
        getenv("RECOMP_D3D11_GPUVS_ASYNC")?getenv("RECOMP_D3D11_GPUVS_ASYNC"):"?");
}
