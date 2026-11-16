/* Adapted from nhlhitz2003recomp (9e8968b), generated GPU vertex shaders.
 * Executor callbacks run on one thread; compiler workers only publish shader objects. */
#include <stdarg.h>
static void gvs_log(const char *fmt, ...) {
    va_list ap;va_start(ap,fmt);fputs("[RC GPUVS] ",stderr);vfprintf(stderr,fmt,ap);va_end(ap);
}
static int gvs_device_ready(void) { initialize_device();initialize_combiners();return dev!=NULL; }
static HRESULT shader_create_vs(ID3DBlob *b, ID3D11VertexShader **out) {
    return ID3D11Device_CreateVertexShader(dev,ID3D10Blob_GetBufferPointer(b),ID3D10Blob_GetBufferSize(b),NULL,out);
}
static ID3DBlob *compile_hlsl_src(const char *src,size_t len,const char *entry,const char *profile) {
    ID3DBlob *b=NULL,*err=NULL;
    HRESULT hr=D3DCompile(src,len,"RedCard GPU vertex",NULL,NULL,entry,profile,
        D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_IEEE_STRICTNESS,0,&b,&err);
    if(err){gvs_log("%s\n",(char*)ID3D10Blob_GetBufferPointer(err));DROP(err);}
    if(FAILED(hr)){DROP(b);return NULL;}return b;
}
static HRESULT upload_constants(ID3D11DeviceContext *context,ID3D11Buffer *buf,const void *data,size_t n) {
    D3D11_MAPPED_SUBRESOURCE m;HRESULT hr=map_resource(context,(ID3D11Resource*)buf,0,D3D11_MAP_WRITE_DISCARD,0,&m);
    if(SUCCEEDED(hr)){memcpy(m.pData,data,n);ID3D11DeviceContext_Unmap(context,(ID3D11Resource*)buf,0);}return hr;
}
/* d3d11_gvs.inc -- GPU vertex-program path of the D3D11 back end (default on; RECOMP_D3D11_GPUVS=0 disables).
 * #included by backend_d3d11.c after the register-combiner pipeline. See nv2a_pbx.h
 * (Nv2aGpuVs). Per distinct NV2A vertex program: one generated
 * vs_5_0 (cached by the microcode's hash); every draw reads its data from one raw
 * ByteAddressBuffer ring. */
#include "redcard_gvs_hlsl.h"


#define GVS_RING_BYTES (32u * 1024u * 1024u)
#define GVS_SPEC_MAX   1024
#define GVS_VMAX       4096u         /* vertices read back per verify draw */
#define GVS_VFLOATS    29u           /* pos 4, c 4, d1 4, t0..t3 16, fog 1 */

typedef struct {
    uint32_t hash;
    uint32_t fixed_function;
    uint32_t nslots, prog[NV2A_VSH_SLOTS_MAX][4];
    ID3D11VertexShader *vs;          /* NULL: compile failed (cached) */
    ID3D11VertexShader *vs_v;        /* verify variant (raw program oPos), made on demand */
    ID3D11GeometryShader *gs_v;      /* its stream-out pass-through */
    int vtried;
    volatile LONG pending, ready;    /* RECOMP_D3D11_GPUVS_ASYNC: being compiled by a worker */
} GvsSpec;

static GvsSpec s_gspec[GVS_SPEC_MAX];
static uint32_t s_ngspec, s_gspec_fail, s_gspec_ms;
static ID3D11Buffer *s_gring, *s_gcb, *s_so_buf, *s_so_stage;
static ID3D11ShaderResourceView *s_gring_srv;
static ID3D11Query *s_so_query;
static uint32_t s_gring_off;
static int s_gstate;                  /* 0 not tried, 1 ready, -1 unavailable */
static unsigned long s_gdraws, s_gverts, s_gmapfail, s_gbytes;

typedef struct {
    uint32_t gbase, pad0;
    float    aa_sx, aa_sy;
    uint32_t fog_mode;
    float    fog_px, fog_py, pad1;
} GvsConsts;

static void gvs_lock(int on) { (void)on; }
static int gvs_no_overwrite;
static int gvs_init_u(void)
{
    D3D11_BUFFER_DESC bd;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    D3D11_FEATURE_DATA_D3D11_OPTIONS opt;
    HRESULT hr;

    if (s_gstate)
        return s_gstate > 0;
    s_gstate = -1;
    if (!1 || !dev)
        return 0;
    if (ID3D11Device_GetFeatureLevel(dev) < D3D_FEATURE_LEVEL_11_0) {
        gvs_log("GPU vertex shaders need feature level 11_0: path off\n");
        return 0;
    }
    memset(&opt, 0, sizeof opt);
    gvs_no_overwrite = SUCCEEDED(ID3D11Device_CheckFeatureSupport(dev,
        D3D11_FEATURE_D3D11_OPTIONS,&opt,sizeof opt)) && opt.MapNoOverwriteOnDynamicBufferSRV;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = GVS_RING_BYTES;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &s_gring);
    if (FAILED(hr)) { gvs_log("GPU VS ring buffer 0x%08lX: path off\n", (unsigned long)hr); return 0; }
    memset(&sd, 0, sizeof sd);
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sd.BufferEx.FirstElement = 0;
    sd.BufferEx.NumElements = GVS_RING_BYTES / 4u;
    sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    hr = ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)s_gring, &sd, &s_gring_srv);
    if (FAILED(hr)) { gvs_log("GPU VS ring view 0x%08lX: path off\n", (unsigned long)hr); return 0; }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(GvsConsts);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &s_gcb);
    if (FAILED(hr)) { gvs_log("GPU VS constants 0x%08lX: path off\n", (unsigned long)hr); return 0; }
    s_gstate = 1;
    gvs_log("GPU vertex-shader path ready (vs_5_0, %u MB ring)\n", GVS_RING_BYTES >> 20);
    return 1;
}

static char *gvs_assemble(const char *body, int verify, size_t *len)
{
    static const char defs[] = "#define GVS_ATTR 3072u\n#define GVS_IMM 3328u\n#define GVS_IDX 3584u\n";
    static const char vdef[] = "#define GVS_VERIFY 1\n";
    static const char fdef[] = "#define GVS_FARCLIP 1\n";
    static int farclip = -1;
    const char *mark = strstr(k_hlsl_gvs, "//@@BODY@@");
    size_t pre = mark ? (size_t)(mark - k_hlsl_gvs) : 0;
    size_t cap = sizeof defs + sizeof vdef + sizeof fdef + sizeof k_hlsl_gvs + strlen(body) + 16, n = 0;
    char *s = (char *)malloc(cap);

    if (!s || !mark)
        { free(s); return NULL; }
    memcpy(s + n, defs, sizeof defs - 1); n += sizeof defs - 1;
    if (verify) { memcpy(s + n, vdef, sizeof vdef - 1); n += sizeof vdef - 1; }
    if (farclip < 0) { const char *e = getenv("RECOMP_D3D11_GPUVS_FARCLIP"); farclip = e && e[0] == '1'; }
    if (farclip) { memcpy(s + n, fdef, sizeof fdef - 1); n += sizeof fdef - 1; }
    memcpy(s + n, k_hlsl_gvs, pre); n += pre;
    memcpy(s + n, body, strlen(body)); n += strlen(body);
    memcpy(s + n, mark, strlen(mark)); n += strlen(mark);
    s[n] = 0;
    *len = n;
    return s;
}

/* RECOMP_D3D11_GPUVS_ASYNC (default on): compile a new program's vertex shader on a worker.
 * A synchronous D3DCompile of a generated vs_5_0 costs about 0.3–1 s mid-frame. Until ready
 * the probe returns NULL and the draw takes the CPU path. Verify still compiles sync. */
typedef struct { GvsSpec *sp; char *src; size_t len; } GvsJob;

static DWORD WINAPI gvs_job_main(LPVOID arg)
{
    GvsJob *j = (GvsJob *)arg;
    ID3DBlob *blob = compile_hlsl_src(j->src, j->len, "vs_gpu", "vs_5_0");
    ID3D11VertexShader *vs = NULL;

    if (blob) {
        if (FAILED(shader_create_vs(blob, &vs)))
            vs = NULL;
        ID3D10Blob_Release(blob);
    }
    free(j->src);
    j->sp->vs = vs;
    MemoryBarrier();
    InterlockedExchange(&j->sp->ready, 1);
    free(j);
    return 0;
}

static int gvs_async_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_D3D11_GPUVS_ASYNC");
        /* Default on when unset (redcard_d3d11_register also putenv's "1"). Explicit 0 disables. */
        on = !e || e[0] != '0';
    }
    return on;
}

static int gvs_init(void)
{
    int r;
    gvs_lock(1); r = gvs_init_u(); gvs_lock(0);
    return r;
}

/* Find or compile the shader of `e`'s program; NULL if it cannot be drawn on the GPU. */
static GvsSpec *gvs_get_u(const Nv2aGpuVs *e, int want_verify);
static int gvs_gen_body(const Nv2aGpuVs *e, char *out, size_t cap)
{
    int ok = nv2a_vsh_gen_hlsl(e, out, cap);
    /* RECOMP_GPUVS_DUMP=<dir>: write each program's generated body as gvs_<hash>.hlsl. */
    const char *dir = getenv("RECOMP_GPUVS_DUMP");
    if (ok && dir && *dir) {
        char path[MAX_PATH]; FILE *f;
        snprintf(path, sizeof path, "%s\\gvs_%08X.hlsl", dir, e->hash);
        if ((f = fopen(path, "wb")) != NULL) { fputs(out, f); fclose(f); }
    }
    return ok;
}
static GvsSpec *gvs_get(const Nv2aGpuVs *e, int want_verify)
{
    GvsSpec *r;
    gvs_lock(1); r = gvs_get_u(e, want_verify); gvs_lock(0);
    return r;
}

static GvsSpec *gvs_get_u(const Nv2aGpuVs *e, int want_verify)
{
    GvsSpec *sp = NULL;
    uint32_t i;
    char *body, *src;
    size_t len;
    ID3DBlob *blob;
    DWORD t0;

    for (i = 0; i < s_ngspec; i++)
        if (s_gspec[i].hash == e->hash && s_gspec[i].fixed_function == e->fixed_function && s_gspec[i].nslots==e->nslots && !memcmp(s_gspec[i].prog,e->prog,e->nslots*sizeof e->prog[0])) { sp = &s_gspec[i]; break; }
    if (sp && sp->pending) {
        if (!sp->ready)
            return NULL;                 /* still compiling: the CPU path draws meanwhile */
        sp->pending = 0;
        if (!sp->vs) s_gspec_fail++;
        if (1)
            gvs_log("GPU VS %08X: async %s\n", e->hash, sp->vs ? "compiled" : "FAILED");
    }
    if (!sp && gvs_async_on() && !want_verify) {
        if (s_ngspec >= GVS_SPEC_MAX)
            return NULL;
        sp = &s_gspec[s_ngspec++];
        memset(sp, 0, sizeof *sp);
        sp->hash = e->hash;sp->nslots=e->nslots;memcpy(sp->prog,e->prog,e->nslots*sizeof e->prog[0]);
        sp->fixed_function = e->fixed_function;
        body = (char *)malloc(1u << 20);
        if (body && gvs_gen_body(e, body, 1u << 20)
            && (src = gvs_assemble(body, 0, &len)) != NULL) {
            GvsJob *j = (GvsJob *)malloc(sizeof *j);
            if (j) {
                j->sp = sp; j->src = src; j->len = len;
                sp->pending = 1;
                if (!QueueUserWorkItem(gvs_job_main, j, WT_EXECUTELONGFUNCTION)) {
                    sp->pending = 0;
                    free(src); free(j);
                }
            } else free(src);
        }
        free(body);
        if (!sp->pending) s_gspec_fail++;
        return NULL;
    }
    if (!sp) {
        if (s_ngspec >= GVS_SPEC_MAX)
            return NULL;
        sp = &s_gspec[s_ngspec++];
        memset(sp, 0, sizeof *sp);
        sp->hash = e->hash;sp->nslots=e->nslots;memcpy(sp->prog,e->prog,e->nslots*sizeof e->prog[0]);
        sp->fixed_function = e->fixed_function;
        t0 = GetTickCount();
        body = (char *)malloc(1u << 20);
        if (body && gvs_gen_body(e, body, 1u << 20)
            && (src = gvs_assemble(body, 0, &len)) != NULL) {
            blob = compile_hlsl_src(src, len, "vs_gpu", "vs_5_0");
            free(src);
            if (blob) {
                if (FAILED(shader_create_vs(blob, &sp->vs)))
                    sp->vs = NULL;
                ID3D10Blob_Release(blob);
            }
        }
        free(body);
        if (!sp->vs) s_gspec_fail++;
        s_gspec_ms += GetTickCount() - t0;
        if (1)
            gvs_log("GPU VS %08X: %u slots -> %s (%lu ms)\n", e->hash, e->nslots,
                  sp->vs ? "compiled" : "FAILED", (unsigned long)(GetTickCount() - t0));
    }
    if (sp->vs && want_verify && !sp->vtried) {
        static const D3D11_SO_DECLARATION_ENTRY so[] = {
            { 0, "SV_Position", 0, 0, 4, 0 }, { 0, "COLOR", 0, 0, 4, 0 },
            { 0, "TEXCOORD", 1, 0, 4, 0 }, { 0, "TEXCOORD", 2, 0, 4, 0 },
            { 0, "TEXCOORD", 3, 0, 4, 0 }, { 0, "TEXCOORD", 4, 0, 4, 0 },
            { 0, "TEXCOORD", 5, 0, 4, 0 }, { 0, "TEXCOORD", 6, 0, 1, 0 },
        };
        UINT stride = GVS_VFLOATS * 4u;
        sp->vtried = 1;
        body = (char *)malloc(1u << 20);
        if (body && gvs_gen_body(e, body, 1u << 20)
            && (src = gvs_assemble(body, 1, &len)) != NULL) {
            blob = compile_hlsl_src(src, len, "vs_gpu", "vs_5_0");
            if (blob) {
                if (FAILED(shader_create_vs(blob, &sp->vs_v)))
                    sp->vs_v = NULL;
                ID3D10Blob_Release(blob);
            }
            blob = compile_hlsl_src(src, len, "gs_pass", "gs_5_0");
            if (blob) {
                if (FAILED(ID3D11Device_CreateGeometryShaderWithStreamOutput(dev,
                        ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), so,
                        (UINT)(sizeof so / sizeof so[0]), &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM,
                        NULL, &sp->gs_v)))
                    sp->gs_v = NULL;
                ID3D10Blob_Release(blob);
            }
            free(src);
        }
        free(body);
    }
    return sp->vs ? sp : NULL;
}

/* The executor asks before it builds a block: can the GPU draw this program? */
static int nv2a_gpuvs_probe(const Nv2aGpuVs *e)
{
    static int env = -1;

    if (env < 0)
        {const char *v=getenv("RECOMP_D3D11_GPUVS");env=v && atoi(v)!=0;}
    if (!env || !gvs_device_ready() || !gvs_init())
        return 0;
    return gvs_get(e, e->verify) != NULL;
}

/* RECOMP_GPUVS_VERIFY: run the program's vertices through stream-out and hand the
 * raw outputs (pos, d0, d1, tex0-3, fog) to the executor to compare. */
static void gvs_verify_run(const GvsSpec *sp, const Nv2aGpuVs *gv)
{
    D3D11_BUFFER_DESC bd;
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t n = gv->nidx > GVS_VMAX ? GVS_VMAX : gv->nidx;
    UINT so_off = 0;
    D3D11_QUERY_DATA_SO_STATISTICS stats;

    
    if (!gv->verify_out || !sp->vs_v || !sp->gs_v || !n)
        return;
    if (!s_so_query) {
        D3D11_QUERY_DESC q = {D3D11_QUERY_SO_STATISTICS, 0};
        if (FAILED(ID3D11Device_CreateQuery(dev, &q, &s_so_query))) return;
    }
    if (!s_so_buf) {
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = GVS_VMAX * GVS_VFLOATS * 4u;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
        if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &s_so_buf))) return;
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &s_so_stage))) return;
    }
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext_VSSetShader(ctx, sp->vs_v, NULL, 0);
    ID3D11DeviceContext_GSSetShader(ctx, sp->gs_v, NULL, 0);
    ID3D11DeviceContext_SOSetTargets(ctx, 1, &s_so_buf, &so_off);
    ID3D11DeviceContext_PSSetShader(ctx, NULL, NULL, 0);
    ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)s_so_query);
    submit_draw(ctx, n, 0);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)s_so_query);
    {
        ID3D11Buffer *none = NULL;
        ID3D11DeviceContext_SOSetTargets(ctx, 1, &none, &so_off);
    }
    ID3D11DeviceContext_GSSetShader(ctx, NULL, NULL, 0);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)s_so_stage, (ID3D11Resource *)s_so_buf);
    if (SUCCEEDED(map_resource(ctx, (ID3D11Resource *)s_so_stage, 0, D3D11_MAP_READ, 0, &m))) {
        memcpy(gv->verify_out, m.pData, (size_t)n * GVS_VFLOATS * 4u);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s_so_stage, 0);
        if (gv->verify_count) *gv->verify_count = n;
        if (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)s_so_query, &stats, sizeof stats, 0) == S_OK
                && stats.NumPrimitivesWritten != n) {
            if (gv->verify_count) *gv->verify_count = 0;
            gvs_log("GPUVS verify stream-out: expected %u points, wrote %llu (needed %llu) hash %08X\n",
                  n, stats.NumPrimitivesWritten, stats.PrimitivesStorageNeeded, gv->hash);
        }
    }
}

/* One GPU-vertex-shader draw. be_draw has already bound the render target, the blend,
 * depth and raster state and the viewport, and updated cb. Returns 1 if drawn. */
static int gvs_draw(const Nv2aGpuVs *gv)
{
    GvsSpec *sp;
    D3D11_MAPPED_SUBRESOURCE m;
    GvsConsts gc;
    ID3D11Buffer *pcb[2];
    uint32_t bb = (gv->block_bytes + 15u) & ~15u, mode;


    sp = gvs_get(gv, gv->verify);
    if (!sp || bb > GVS_RING_BYTES) return 0;
    if (!gvs_no_overwrite || s_gring_off + bb > GVS_RING_BYTES) s_gring_off = 0, mode = D3D11_MAP_WRITE_DISCARD;
    else mode = s_gring_off ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD;

    if (FAILED(map_resource(ctx, (ID3D11Resource *)s_gring, 0, (D3D11_MAP)mode, 0, &m))) {
        s_gmapfail++;
        return 0;
    }


    memcpy((uint8_t *)m.pData + s_gring_off, gv->block, gv->block_bytes);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s_gring, 0);



    memset(&gc, 0, sizeof gc);
    gc.gbase = s_gring_off;
    gc.aa_sx = gv->aa_sx; gc.aa_sy = gv->aa_sy;
    gc.fog_mode = gv->fog_mode; gc.fog_px = gv->fog_px; gc.fog_py = gv->fog_py;
    if (FAILED(upload_constants(ctx, s_gcb, &gc, sizeof gc))) return 0;
    s_gring_off += bb;
    s_gbytes += gv->block_bytes;

    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cb);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 2, 1, &s_gcb);
    ID3D11DeviceContext_VSSetShaderResources(ctx, 0, 1, &s_gring_srv);
    if (gv->verify) {  gvs_verify_run(sp, gv);  }
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, sp->vs, NULL, 0);
    pcb[0] = cb; pcb[1] = rc_cb;
    ID3D11DeviceContext_PSSetShader(ctx, rc_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 2, pcb);


    submit_draw(ctx, gv->nidx, 0);
    s_gdraws++;
    s_gverts += gv->nidx;



    return 1;
}
