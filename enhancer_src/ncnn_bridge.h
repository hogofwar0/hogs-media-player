// Thin bridge so NCNN's (partial) Vulkan headers never collide with the real SDK headers.
// NCNN's instance/device/queue are dispatchable handles (pointers) -> pass as void*.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

typedef struct NcnnVkHandles {
    void* instance;        // VkInstance  (NCNN owns it; patched to allow VK_KHR_win32_surface)
    void* physical;        // VkPhysicalDevice
    void* device;          // VkDevice    (has VK_KHR_swapchain enabled)
    unsigned queue_family; // compute queue family index
} NcnnVkHandles;

int   ncnn_vk_init(NcnnVkHandles* out);                 // 0 = ok
void* ncnn_vk_acquire_queue(unsigned queue_family);     // -> VkQueue
void  ncnn_vk_reclaim_queue(unsigned queue_family, void* queue);
void  ncnn_vk_shutdown(void);

// Upscale one packed-RGB frame with a heavy general model (Vulkan, fp16).
// Returns malloc'd packed RGB (free with ncnn_free); sets *ow,*oh. NULL on failure.
unsigned char* ncnn_upscale(const unsigned char* rgb, int w, int h,
                            const char* param, const char* bin,
                            const char* in_blob, const char* out_blob,
                            int* ow, int* oh);
void ncnn_free(void* p);

// ZERO-COPY: upscale on GPU, leave result as a VkMat; expose its buffer + layout. The VkMat stays
// alive until ncnn_upscale_gpu_release(). Pass VkBuffer as uint64 across the TU boundary.
typedef struct NcnnGpuOut {
    unsigned long long buffer;   // VkBuffer
    unsigned long long offset;   // byte offset of element 0
    int w, h;                    // output dims
    int cstep;                   // per-channel stride in ELEMENTS (planar fp16)
} NcnnGpuOut;
int  ncnn_upscale_gpu(const unsigned char* rgb, int w, int h, const char* param, const char* bin,
                      const char* in_blob, const char* out_blob, NcnnGpuOut* out);  // 0 = ok (net cached)
void ncnn_upscale_gpu_release(void);   // free per-frame state; keep net cached
void ncnn_upscale_gpu_close(void);     // free the cached net (shutdown)

// Upscale -> CPU RGB, reusing the SAME cached net (frame-gen loop needs RGB to feed RIFE).
// Returns malloc'd packed RGB (free with ncnn_free); sets *ow,*oh. NULL on failure.
unsigned char* ncnn_upscale_cached(const unsigned char* rgb, int w, int h,
                                   const char* param, const char* bin,
                                   const char* in_blob, const char* out_blob, int* ow, int* oh);

// PHASE 2 (bubble): split of ncnn_upscale_cached. _raw runs the GPU extract and returns an opaque
// holder ([0,1] float, downloaded) — NULL on failure. _finish (any thread) does denormalize +
// float->RGB (the big 398MB@4K CPU op) and frees the holder, returning malloc'd packed RGB + *ow,*oh.
// Lets the writer thread do the conversion while the GPU thread runs the next frame.
void* ncnn_upscale_cached_raw(const unsigned char* rgb, int w, int h,
                              const char* param, const char* bin,
                              const char* in_blob, const char* out_blob);
unsigned char* ncnn_upscale_raw_finish(void* holder, int* ow, int* oh);

// RIFE frame interpolation between two RGB frames at timestep t in (0,1) -> malloc'd RGB (free with ncnn_free).
// Net is CACHED (reloaded only on model-path change). ncnn_rife_close() frees it at shutdown.
unsigned char* ncnn_rife(const unsigned char* rgb0, const unsigned char* rgb1, int w, int h,
                         float t, const char* param, const char* bin, int* ow, int* oh);
void ncnn_rife_close(void);

// GPU-RESIDENT CHAIN (combined upscale+frame-gen): the upscaled frame stays on the GPU as RIFE's
// input — no CPU round-trip between the two nets, one submit per frame. REQUIREMENT: pass source
// dims that are multiples of 16 (x4 output is then /64-aligned, which RIFE v4.26 needs — pad the
// source with replicated edges and crop the 4x outputs). ncnn_chain_frame upscales rgb (w x h),
// returns malloc'd upscaled RGB (4w x 4h); when want_mid and a previous frame is held, *mid gets
// the malloc'd RIFE midpoint between the PREVIOUS upscaled frame and this one (else *mid = NULL).
// up_in/up_out = the upscaler's blob names, up_scale = its factor (4: realesr-general/x4plus "data"/"output";
// 2: RealESRGAN_x2plus "in0"/"out0"). Caller pads the source to /(64/up_scale) so the output is /64.
int  ncnn_chain_init(const char* up_param, const char* up_bin, const char* rife_param, const char* rife_bin,
                     const char* up_in, const char* up_out, int up_scale);
unsigned char* ncnn_chain_frame(const unsigned char* rgb, int w, int h,
                                int want_mid, unsigned char** mid, int* ow, int* oh);
void ncnn_chain_reset(void);   // forget the held previous frame (new file / scene reset)
void ncnn_chain_close(void);   // free nets + allocators

// PIPELINED chain: same GPU work as ncnn_chain_frame, but the ~20ms/frame CPU post-processing
// (denormalize + float->RGB of the 4x outputs) is DEFERRED so another thread can do it while the
// GPU crunches the next frame. _raw runs the GPU pass and returns an opaque holder (NULL on
// failure); _finish consumes the holder on ANY thread, returning the malloc'd upscaled RGB
// (+ *mid if a midpoint was produced). Call order of _raw is the frame order (the chain keeps
// the previous upscaled frame on-GPU); _finish calls may lag behind arbitrarily.
void* ncnn_chain_frame_raw(const unsigned char* rgb, int w, int h, int want_mid);
unsigned char* ncnn_chain_finish(void* holder, unsigned char** mid, int* ow, int* oh);

#ifdef __cplusplus
}
#endif
