// NCNN side of the bridge — this TU includes NCNN's vulkan view; the renderer TU uses the SDK's.
#include "ncnn_bridge.h"
#include <net.h>
#include <gpu.h>
#include "rife_ops.h"   // custom rife.Warp layer (nihui/rife-ncnn-vulkan)
static ncnn::Layer* Warp_layer_creator(void*) { return new Warp; }

extern "C" int ncnn_vk_init(NcnnVkHandles* out){
    if (ncnn::create_gpu_instance()) return 1;
    int g = ncnn::get_default_gpu_index();
    const ncnn::GpuInfo& gi = ncnn::get_gpu_info(g);
    ncnn::VulkanDevice* dev = ncnn::get_gpu_device(g);
    if (!dev) return 2;
    out->instance     = (void*)ncnn::get_gpu_instance();
    out->physical     = (void*)gi.physicalDevice();
    out->device       = (void*)dev->vkdevice();
    out->queue_family = gi.compute_queue_family_index();
    return 0;
}
extern "C" void* ncnn_vk_acquire_queue(unsigned qf){
    ncnn::VulkanDevice* dev = ncnn::get_gpu_device(ncnn::get_default_gpu_index());
    return (void*)dev->acquire_queue(qf);
}
extern "C" void ncnn_vk_reclaim_queue(unsigned qf, void* q){
    ncnn::VulkanDevice* dev = ncnn::get_gpu_device(ncnn::get_default_gpu_index());
    dev->reclaim_queue(qf, (VkQueue)q);
}
extern "C" void ncnn_vk_shutdown(void){ ncnn::destroy_gpu_instance(); }

#include <cstdlib>
#include <cstring>
#include <cstdio>
// Real-ESRGAN expects input in [0,1] (upstream's preproc shader does *1/255) and emits [0,1];
// fp16 ARITHMETIC is deliberately OFF upstream (RRDB activations overflow fp16 -> black/blocky
// artifacts); fp16 packed/storage stay ON (that's upstream's fast path).
static const float UPS_NORM[3]  = {1/255.f, 1/255.f, 1/255.f};
static const float UPS_ZERO[3]  = {0,0,0};
static const float UPS_HALF[3]  = {-0.5f/255.f, -0.5f/255.f, -0.5f/255.f}; // (v+0.5/255)*255 = 255v+0.5 (round)
static const float UPS_TO255[3] = {255.f, 255.f, 255.f};
extern "C" unsigned char* ncnn_upscale(const unsigned char* rgb, int w, int h,
        const char* param, const char* bin, const char* in_blob, const char* out_blob,
        int* ow, int* oh){
    ncnn::Net net;
    net.opt.use_vulkan_compute  = true;
    net.opt.use_fp16_packed     = true;
    net.opt.use_fp16_storage    = true;
    net.opt.use_fp16_arithmetic = false;   // match upstream realesrgan-ncnn-vulkan
    if (net.load_param(param)) return 0;
    if (net.load_model(bin))   return 0;
    ncnn::Mat in = ncnn::Mat::from_pixels(rgb, ncnn::Mat::PIXEL_RGB, w, h);
    in.substract_mean_normalize(UPS_ZERO, UPS_NORM);
    ncnn::Extractor ex = net.create_extractor();
    ex.input(in_blob, in);
    ncnn::Mat out;
    if (ex.extract(out_blob, out)) return 0;
    out.substract_mean_normalize(UPS_HALF, UPS_TO255);
    *ow = out.w; *oh = out.h;
    unsigned char* buf = (unsigned char*)malloc((size_t)out.w * out.h * 3);
    if (buf) out.to_pixels(buf, ncnn::Mat::PIXEL_RGB);
    return buf;
}
extern "C" void ncnn_free(void* p){ free(p); }

// RIFE frame interpolation: between two RGB frames at timestep t∈(0,1) -> malloc'd RGB (free w/ ncnn_free).
// Net is CACHED across calls (reloaded only when the model path changes) — the frame-gen loop calls this a lot.
static ncnn::Net* g_rife=0; static char g_rife_param[1024]={0};
extern "C" void ncnn_rife_close(void){ delete g_rife; g_rife=0; g_rife_param[0]=0; }
extern "C" unsigned char* ncnn_rife(const unsigned char* rgb0, const unsigned char* rgb1, int w, int h,
        float t, const char* param, const char* bin, int* ow, int* oh){
    if (!g_rife || strcmp(g_rife_param,param)!=0){
        delete g_rife; g_rife=new ncnn::Net();
        g_rife->opt.use_vulkan_compute=true;
        // Upstream rife-ncnn-vulkan fast path: fp16 packed+storage ON, fp16 ARITHMETIC OFF
        // (flownet's flow values lose precision/overflow in fp16 math -> glitchy interpolation).
        // Env VKPLAYFG_RIFE_FP32=1 forces the everything-fp32/no-pack path for debugging.
        bool fp32 = getenv("VKPLAYFG_RIFE_FP32");
        g_rife->opt.use_fp16_packed=!fp32; g_rife->opt.use_fp16_storage=!fp32;
        // upstream sets arithmetic false unconditionally; measured ZERO speedup from fp16 math on the
        // 7900 XTX (overhead-bound), so keep the safe default. HOGS_RIFE_FP16A=1 re-enables.
        const char*e=getenv("HOGS_RIFE_FP16A");
        g_rife->opt.use_fp16_arithmetic = fp32 ? false : (e && e[0]=='1');
        g_rife->opt.use_packing_layout=!fp32; g_rife->opt.use_shader_local_memory=false;
        g_rife->register_custom_layer("rife.Warp", Warp_layer_creator);   // RIFE needs the custom warp op
        if (g_rife->load_param(param)){ delete g_rife; g_rife=0; return 0; }
        if (g_rife->load_model(bin))  { delete g_rife; g_rife=0; return 0; }
        snprintf(g_rife_param,sizeof g_rife_param,"%s",param);
    }
    const float norm01[3]={1/255.f,1/255.f,1/255.f}, zero[3]={0,0,0},
                half[3]={-0.5f/255.f,-0.5f/255.f,-0.5f/255.f}, to255[3]={255.f,255.f,255.f};
    // RIFE v4.25/4.26 (5 IFBlocks, deepest pyramid = input/64) needs dims padded to a multiple of 64.
    // (/32 was only right for v4.0-4.6; /64 is safe for ALL v4 models.) Zero-fill matches upstream.
    int wp=(w+63)/64*64, hp=(h+63)/64*64;
    ncnn::Mat f0 = ncnn::Mat::from_pixels(rgb0, ncnn::Mat::PIXEL_RGB, w, h); f0.substract_mean_normalize(zero, norm01);
    ncnn::Mat f1 = ncnn::Mat::from_pixels(rgb1, ncnn::Mat::PIXEL_RGB, w, h); f1.substract_mean_normalize(zero, norm01);
    ncnn::Mat in0,in1;
    ncnn::copy_make_border(f0,in0,0,hp-h,0,wp-w,ncnn::BORDER_CONSTANT,0.f);
    ncnn::copy_make_border(f1,in1,0,hp-h,0,wp-w,ncnn::BORDER_CONSTANT,0.f);
    ncnn::Mat in2(wp,hp,1); in2.fill(t);
    ncnn::Extractor ex = g_rife->create_extractor();
    ex.input("in0",in0); ex.input("in1",in1); ex.input("in2",in2);
    ncnn::Mat out; if (ex.extract("out0",out)) return 0;
    ncnn::Mat crop; ncnn::copy_cut_border(out,crop,0,hp-h,0,wp-w);   // back to wxh
    crop.substract_mean_normalize(half,to255);                       // 0-1 -> 0-255 (+0.5 round)
    *ow=w; *oh=h;
    unsigned char* buf=(unsigned char*)malloc((size_t)w*h*3);
    if (buf) crop.to_pixels(buf, ncnn::Mat::PIXEL_RGB);
    return buf;
}

#include <command.h>   // VkCompute (needed by the GPU-resident chain below)
// ---- GPU-RESIDENT CHAIN: upscale -> (RIFE vs previous upscaled frame) with NO CPU round-trip
//      between the nets. The upscaler emits [0,1] which is exactly RIFE's input domain, so the
//      VkMat feeds straight in. One VkCompute submit per frame. Source dims must be /16 (caller
//      pads) so the 4x output is /64 — RIFE v4.26's alignment — with zero GPU border ops.
static ncnn::Net *g_cu=0, *g_cr=0;
static ncnn::VkAllocator *g_cblob=0, *g_cstg=0;   // held from init to close: g_cprev's memory lives here
static ncnn::VkMat g_cprev; static int g_cprev_ok=0;
static char g_cin[32]="data", g_cout[32]="output"; static int g_cscale=4;   /* upscaler blobs + factor */
extern "C" void ncnn_chain_reset(void){ g_cprev.release(); g_cprev_ok=0; }
extern "C" void ncnn_chain_close(void){
    ncnn_chain_reset();
    delete g_cu; g_cu=0; delete g_cr; g_cr=0;
    if(g_cblob){ ncnn::VulkanDevice* v=ncnn::get_gpu_device(ncnn::get_default_gpu_index());
        v->reclaim_blob_allocator(g_cblob); v->reclaim_staging_allocator(g_cstg); g_cblob=0; g_cstg=0; }
}
extern "C" int ncnn_chain_init(const char* upP, const char* upB, const char* rfP, const char* rfB,
                               const char* up_in, const char* up_out, int up_scale){
    ncnn_chain_close();
    /* 2026-10-27: was hardcoded to x4 "data"/"output". Archive's RealESRGAN_x2plus ("in0"/"out0", x2) was
       therefore forced onto the separate upscale->RIFE path, whose standalone RIFE must run ONE worker
       (multi-worker tile corruption) — ~30% GPU for hours. */
    snprintf(g_cin,sizeof g_cin,"%s",up_in?up_in:"data"); snprintf(g_cout,sizeof g_cout,"%s",up_out?up_out:"output");
    g_cscale = (up_scale==2) ? 2 : 4;
    ncnn::VulkanDevice* vkdev = ncnn::get_gpu_device(ncnn::get_default_gpu_index());
    if(!vkdev) return 1;
    g_cu=new ncnn::Net(); g_cu->opt.use_vulkan_compute=true;
    g_cu->opt.use_fp16_packed=true; g_cu->opt.use_fp16_storage=true; g_cu->opt.use_fp16_arithmetic=false;
    if(g_cu->load_param(upP)||g_cu->load_model(upB)){ ncnn_chain_close(); return 2; }
    g_cr=new ncnn::Net(); g_cr->opt.use_vulkan_compute=true;
    g_cr->opt.use_fp16_packed=true; g_cr->opt.use_fp16_storage=true; g_cr->opt.use_fp16_arithmetic=false;
    g_cr->opt.use_packing_layout=true; g_cr->opt.use_shader_local_memory=false;
    g_cr->register_custom_layer("rife.Warp", Warp_layer_creator);
    if(g_cr->load_param(rfP)||g_cr->load_model(rfB)){ ncnn_chain_close(); return 3; }
    g_cblob=vkdev->acquire_blob_allocator();
    g_cstg =vkdev->acquire_staging_allocator();
    return 0;
}
// downloaded-but-not-yet-converted chain outputs (still normalized [0,1])
struct ChainHold { ncnn::Mat up, mid; };
extern "C" void* ncnn_chain_frame_raw(const unsigned char* rgb, int w, int h, int want_mid){
    if(!g_cu||!g_cr||!g_cblob) return 0;
    ncnn::VulkanDevice* vkdev = ncnn::get_gpu_device(ncnn::get_default_gpu_index());
    ncnn::Option opt = g_cu->opt;
    opt.blob_vkallocator=g_cblob; opt.workspace_vkallocator=g_cblob; opt.staging_vkallocator=g_cstg;
    const float norm01[3]={1/255.f,1/255.f,1/255.f}, zero[3]={0,0,0};
    ncnn::Mat in = ncnn::Mat::from_pixels(rgb, ncnn::Mat::PIXEL_RGB, w, h);
    if(in.empty()) return 0;
    in.substract_mean_normalize(zero, norm01);                 // upscaler wants [0,1]
    ChainHold* hold = new ChainHold();
    {
        ncnn::VkCompute cmd(vkdev);
        ncnn::VkMat in_gpu; cmd.record_upload(in, in_gpu, opt);
        ncnn::VkMat up_gpu, mid_gpu;
        ncnn::Extractor exu = g_cu->create_extractor();        // both extractors must outlive submit
        ncnn::Extractor exr = g_cr->create_extractor();
        exu.set_blob_vkallocator(g_cblob); exu.set_workspace_vkallocator(g_cblob); exu.set_staging_vkallocator(g_cstg);
        exu.input(g_cin, in_gpu);
        if(exu.extract(g_cout, up_gpu, cmd)){ delete hold; return 0; }
        int do_mid = want_mid && g_cprev_ok;
        if(do_mid){
            exr.set_blob_vkallocator(g_cblob); exr.set_workspace_vkallocator(g_cblob); exr.set_staging_vkallocator(g_cstg);
            ncnn::Mat ts(w*g_cscale, h*g_cscale, 1); ts.fill(0.5f);   // timestep plane at full (padded) res
            exr.input("in0", g_cprev);                         // previous upscaled frame — still on GPU
            exr.input("in1", up_gpu);
            exr.input("in2", ts);
            if(exr.extract("out0", mid_gpu, cmd)) do_mid=0;
        }
        cmd.record_download(up_gpu, hold->up, opt);
        if(do_mid) cmd.record_download(mid_gpu, hold->mid, opt);
        if(cmd.submit_and_wait()){ delete hold; return 0; }
        g_cprev = up_gpu; g_cprev_ok = 1;                      // hold for the next frame (refcounted)
    }
    if(hold->up.empty()){ delete hold; return 0; }
    return hold;
}
extern "C" unsigned char* ncnn_chain_finish(void* holder, unsigned char** mid, int* ow, int* oh){
    if(mid) *mid=0;
    if(!holder) return 0;
    ChainHold* hold=(ChainHold*)holder;
    const float half[3]={-0.5f/255.f,-0.5f/255.f,-0.5f/255.f}, to255[3]={255.f,255.f,255.f};
    hold->up.substract_mean_normalize(half, to255);            // [0,1] -> 0..255 (+0.5 round)
    *ow=hold->up.w; *oh=hold->up.h;
    unsigned char* upbuf=(unsigned char*)malloc((size_t)hold->up.w*hold->up.h*3);
    if(upbuf) hold->up.to_pixels(upbuf, ncnn::Mat::PIXEL_RGB);
    if(mid && !hold->mid.empty()){
        hold->mid.substract_mean_normalize(half, to255);
        unsigned char* m=(unsigned char*)malloc((size_t)hold->mid.w*hold->mid.h*3);
        if(m) hold->mid.to_pixels(m, ncnn::Mat::PIXEL_RGB);
        *mid=m;
    }
    delete hold;
    return upbuf;
}
extern "C" unsigned char* ncnn_chain_frame(const unsigned char* rgb, int w, int h,
        int want_mid, unsigned char** mid, int* ow, int* oh){
    if(mid) *mid=0;
    void* hold = ncnn_chain_frame_raw(rgb, w, h, want_mid);
    return ncnn_chain_finish(hold, mid, ow, oh);
}

#include <command.h>
#include <cstdio>
#include <cstdint>
// 6a: run the net on the GPU, keep result as a VkMat, print its exact layout (for the conversion shader)
extern "C" int ncnn_probe_gpu(const unsigned char* rgb, int w, int h,
        const char* param, const char* bin, const char* in_blob, const char* out_blob){
    ncnn::Net net;
    net.opt.use_vulkan_compute=true;
    net.opt.use_fp16_packed=true; net.opt.use_fp16_storage=true; net.opt.use_fp16_arithmetic=false;
    if(net.load_param(param)) return 1;
    if(net.load_model(bin))   return 2;
    const ncnn::VulkanDevice* vkdev = ncnn::get_gpu_device(ncnn::get_default_gpu_index());
    ncnn::VkAllocator* blob_alloc = vkdev->acquire_blob_allocator();
    ncnn::VkAllocator* staging_alloc = vkdev->acquire_staging_allocator();
    ncnn::Option opt = net.opt; opt.blob_vkallocator=blob_alloc; opt.workspace_vkallocator=blob_alloc; opt.staging_vkallocator=staging_alloc;
    ncnn::Mat in = ncnn::Mat::from_pixels(rgb, ncnn::Mat::PIXEL_RGB, w, h);
    ncnn::VkCompute cmd(vkdev);
    ncnn::VkMat in_gpu;
    cmd.record_upload(in, in_gpu, opt);   // input must be a VkMat for the GPU-resident extract
    ncnn::Extractor ex = net.create_extractor();
    ex.set_blob_vkallocator(blob_alloc); ex.set_workspace_vkallocator(blob_alloc); ex.set_staging_vkallocator(staging_alloc);
    ex.input(in_blob, in_gpu);
    ncnn::VkMat out;
    int r = ex.extract(out_blob, out, cmd);
    cmd.submit_and_wait();
    printf("[gpu] extract r=%d  VkMat dims=%d w=%d h=%d c=%d  elemsize=%zu elempack=%d  cstep=%zu  buf=%p off=%zu\n",
           r, out.dims, out.w, out.h, out.c, (size_t)out.elemsize, out.elempack,
           (size_t)out.cstep, (void*)(uintptr_t)out.buffer(), (size_t)out.buffer_offset());
    return r;
}

// ---- zero-copy GPU upscale: net is CACHED across frames; only per-frame state cycles ----
#include <cstring>
static ncnn::Net*       g_net = 0;
static char             g_param[1024] = {0};   // which model is loaded (reload only on change)
static ncnn::VkCompute* g_cmd = 0;
static ncnn::VkMat*     g_out = 0;
static ncnn::VkAllocator* g_blob = 0;
static ncnn::VkAllocator* g_stg  = 0;

// free only the PER-FRAME state (result VkMat, cmd, allocators); keep the loaded net cached.
extern "C" void ncnn_upscale_gpu_release(void){
    delete g_out; g_out = 0;
    delete g_cmd; g_cmd = 0;
    if (g_blob){
        const ncnn::VulkanDevice* vkdev = ncnn::get_gpu_device(ncnn::get_default_gpu_index());
        vkdev->reclaim_blob_allocator(g_blob);
        vkdev->reclaim_staging_allocator(g_stg);
        g_blob = 0; g_stg = 0;
    }
}
// free the cached net (call once at shutdown / model change).
extern "C" void ncnn_upscale_gpu_close(void){ delete g_net; g_net = 0; g_param[0] = 0; }

// PHASE 2 (bubble): split upscale into a GPU-side RAW pass (extract -> downloaded [0,1] Mat, held)
// and a CPU-side FINISH (denormalize + to_pixels + malloc). The GPU thread runs RAW; the WRITER
// thread runs FINISH — so the ~tens-of-ms 398MB (@4K) conversion overlaps the NEXT frame's GPU work
// instead of stalling the GPU thread between submits (that was the measured ~50% single-stream idle).
struct UpHold { ncnn::Mat up; };   // downloaded-but-not-yet-converted upscale output ([0,1] float)
extern "C" void* ncnn_upscale_cached_raw(const unsigned char* rgb, int w, int h,
        const char* param, const char* bin, const char* in_blob, const char* out_blob){
    if (!g_net || strcmp(g_param, param) != 0){
        delete g_net; g_net = new ncnn::Net();
        g_net->opt.use_vulkan_compute=true;
        g_net->opt.use_fp16_packed=true; g_net->opt.use_fp16_storage=true;
        // fp16 ARITHMETIC measured ZERO speedup on the 7900 XTX (pipeline is overhead-bound, not
        // math-bound) — so keep upstream's safe default (fp32 math). HOGS_UP_FP16A=1 re-enables.
        const char*e=getenv("HOGS_UP_FP16A");
        g_net->opt.use_fp16_arithmetic = e ? (e[0]=='1') : false;
        if (g_net->load_param(param)){ delete g_net; g_net=0; return 0; }
        if (g_net->load_model(bin))  { delete g_net; g_net=0; return 0; }
        snprintf(g_param, sizeof g_param, "%s", param);
    }
    ncnn::Mat in = ncnn::Mat::from_pixels(rgb, ncnn::Mat::PIXEL_RGB, w, h);
    in.substract_mean_normalize(UPS_ZERO, UPS_NORM);    // net expects [0,1] like upstream's preproc
    ncnn::Extractor ex = g_net->create_extractor();
    ex.input(in_blob, in);
    UpHold* hd = new UpHold();
    if (ex.extract(out_blob, hd->up)){ delete hd; return 0; }
    return hd;
}
extern "C" unsigned char* ncnn_upscale_raw_finish(void* holder, int* ow, int* oh){
    if(!holder) return 0;
    UpHold* hd=(UpHold*)holder;
    hd->up.substract_mean_normalize(UPS_HALF, UPS_TO255);   // [0,1] -> 0-255 with rounding
    *ow=hd->up.w; *oh=hd->up.h;
    unsigned char* buf=(unsigned char*)malloc((size_t)hd->up.w*hd->up.h*3);
    if (buf) hd->up.to_pixels(buf, ncnn::Mat::PIXEL_RGB);
    delete hd;
    return buf;
}
// upscale -> CPU RGB, reusing the SAME cached net (for the frame-gen loop, which needs RGB for RIFE).
// Kept as raw+finish so behaviour is byte-identical to before (used by the do_fg-without-chain path).
extern "C" unsigned char* ncnn_upscale_cached(const unsigned char* rgb, int w, int h,
        const char* param, const char* bin, const char* in_blob, const char* out_blob, int* ow, int* oh){
    void* hd = ncnn_upscale_cached_raw(rgb,w,h,param,bin,in_blob,out_blob);
    if(!hd) return 0;
    return ncnn_upscale_raw_finish(hd, ow, oh);
}

extern "C" int ncnn_upscale_gpu(const unsigned char* rgb, int w, int h,
        const char* param, const char* bin, const char* in_blob, const char* out_blob, NcnnGpuOut* info){
    ncnn_upscale_gpu_release();
    if (!g_net || strcmp(g_param, param) != 0){      // load the 33MB model ONCE, not every frame
        delete g_net;
        g_net = new ncnn::Net();
        g_net->opt.use_vulkan_compute=true;
        g_net->opt.use_fp16_packed=true; g_net->opt.use_fp16_storage=true; g_net->opt.use_fp16_arithmetic=false;
        if (g_net->load_param(param)){ delete g_net; g_net=0; return 1; }
        if (g_net->load_model(bin))  { delete g_net; g_net=0; return 2; }
        snprintf(g_param, sizeof g_param, "%s", param);
    }
    const ncnn::VulkanDevice* vkdev = ncnn::get_gpu_device(ncnn::get_default_gpu_index());
    g_blob = vkdev->acquire_blob_allocator();
    g_stg  = vkdev->acquire_staging_allocator();
    ncnn::Option opt = g_net->opt;
    opt.blob_vkallocator=g_blob; opt.workspace_vkallocator=g_blob; opt.staging_vkallocator=g_stg;
    ncnn::Mat in = ncnn::Mat::from_pixels(rgb, ncnn::Mat::PIXEL_RGB, w, h);
    g_cmd = new ncnn::VkCompute(vkdev);
    ncnn::VkMat in_gpu;
    g_cmd->record_upload(in, in_gpu, opt);
    ncnn::Extractor ex = g_net->create_extractor();
    ex.set_blob_vkallocator(g_blob); ex.set_workspace_vkallocator(g_blob); ex.set_staging_vkallocator(g_stg);
    ex.input(in_blob, in_gpu);
    g_out = new ncnn::VkMat();
    int r = ex.extract(out_blob, *g_out, *g_cmd);
    g_cmd->submit_and_wait();
    if (r){ return 3; }
    info->buffer = (unsigned long long)(uintptr_t)g_out->buffer();
    info->offset = (unsigned long long)g_out->buffer_offset();
    info->w = g_out->w; info->h = g_out->h; info->cstep = (int)g_out->cstep;
    return 0;
}
