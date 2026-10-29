/* ============================================================================
 * hogofwar's ultimate media player  (native)
 * Single native app built on FFmpeg (libavcodec → decodes essentially anything,
 * incl. the lossless FFv1 codec) + SDL2 + OpenGL. Plays any VIDEO / IMAGE / AUDIO.
 * Two AI features (offline: pause -> process whole video -> play result in the SAME window):
 *   AI Upscale   (U) - Real-ESRGAN x4 (NCNN/Vulkan), source capped to 1440p.
 *   AI Frame-Gen (G) - RIFE v4 midpoint interpolation, ~doubles the frame rate.
 *   Both are combinable; pressing either pauses playback and runs hogs_enhance.exe with a
 *   color-coded progress bar, then swaps in the enhanced H.265 file. No separate window.
 * Descends from Hogs Media Player mach_139 (browser) / mach_140 (native engine).
 *
 * Build: see build.bat  (w64devkit gcc + the FFmpeg/SDL2 deps in .\deps)
 * Keys:  Space play/pause  S stop  ←/→ seek  ↑/↓ vol  M mute  F fullscreen
 *        N/P track  R repeat  H shuffle  U AI-Upscale  G AI-FrameGen
 *        A audio-track  B subs  V visualizer  I stats  K keys  L playlist  Esc back
 *        (drag video/image/audio files or folders onto the window)
 * ==========================================================================*/
#define SDL_MAIN_HANDLED
#include "SDL.h"
#include <SDL_opengl.h>      /* pulls in GL types, constants, and PFNGL* typedefs */
#include "stb_easy_font.h"   /* ASCII fallback font for the OSD */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"    /* real Unicode font → on-demand glyph atlas */
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>   /* D3D11VA hardware decode (GPU HEVC/H.264/AV1 decoder) */
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <dirent.h>
#include <SDL_syswm.h>       /* HWND for the native file/folder pickers */
#include <commdlg.h>         /* GetOpenFileName (Add Files) — not pulled by LEAN_AND_MEAN */
#include <shlobj.h>          /* SHBrowseForFolder (Add Folder) */
#include <shellapi.h>        /* SHFileOperation — recursively delete the enhanced temp cache on quit */
#include <dbghelp.h>         /* StackWalk64: main-thread stack in the [HANG] watchdog report */
#include <wchar.h>           /* _wfopen / _wremove / _wfreopen / _wgetenv — Unicode CRT file ops */

/* ---- UTF-8 <-> UTF-16 path helpers -------------------------------------------------------------
   Windows file APIs are UTF-16; ffmpeg and all our internal storage are UTF-8. Convert ONLY at the
   Win32/CRT boundary so ANY-language names work everywhere — media files, playlists, drag-drop, the
   command line, AND the player's own config / cache / log, so a Unicode install dir or username
   (e.g. C:\Users\Wörner\…) never breaks it. Defined up here so every function below can use them. */
static void path_u8_to_w(const char *u8, wchar_t *w, int wcap){
    if(MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, wcap) <= 0) w[0]=0;
}
static void path_w_to_u8(const wchar_t *w, char *u8, int cap){
    if(WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, cap, NULL, NULL) <= 0) u8[0]=0;
}
static FILE *path_fopen(const char *u8, const wchar_t *mode){ wchar_t w[2200]; path_u8_to_w(u8,w,2200); return _wfopen(w,mode); }
static int   path_remove(const char *u8){ wchar_t w[2200]; path_u8_to_w(u8,w,2200); return _wremove(w); }

/* ---------------- thread-safe packet queue ---------------- */
typedef struct PacketList { AVPacket *pkt; struct PacketList *next; } PacketList;
typedef struct {
    PacketList *first, *last;
    int nb_packets;
    SDL_mutex *mutex;
    SDL_cond *cond;
    int abort;
} PacketQueue;

static void pq_init(PacketQueue *q){ memset(q,0,sizeof(*q)); q->mutex=SDL_CreateMutex(); q->cond=SDL_CreateCond(); }
static void pq_flush(PacketQueue *q){
    SDL_LockMutex(q->mutex);
    PacketList *p=q->first;
    while(p){ PacketList *n=p->next; av_packet_free(&p->pkt); av_free(p); p=n; }
    q->first=q->last=NULL; q->nb_packets=0;
    SDL_UnlockMutex(q->mutex);
}
static int pq_put(PacketQueue *q, AVPacket *src){
    PacketList *e=av_malloc(sizeof(PacketList));
    if(!e) return -1;
    e->pkt=av_packet_alloc(); av_packet_move_ref(e->pkt, src); e->next=NULL;
    SDL_LockMutex(q->mutex);
    if(q->last) q->last->next=e; else q->first=e;
    q->last=e; q->nb_packets++;
    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
    return 0;
}
/* returns 1 on packet, 0 on abort */
static int pq_get(PacketQueue *q, AVPacket *dst){
    int ret=0;
    SDL_LockMutex(q->mutex);
    for(;;){
        if(q->abort){ ret=0; break; }
        if(q->first){
            PacketList *e=q->first; q->first=e->next; if(!q->first) q->last=NULL;
            q->nb_packets--;
            av_packet_move_ref(dst, e->pkt); av_packet_free(&e->pkt); av_free(e);
            ret=1; break;
        }
        SDL_CondWait(q->cond, q->mutex);
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}
static void pq_abort(PacketQueue *q){ SDL_LockMutex(q->mutex); q->abort=1; SDL_CondSignal(q->cond); SDL_UnlockMutex(q->mutex); }
/* non-blocking get: 1 on packet, 0 if empty. Used by the audio thread so it never
   blocks (blocking there hangs at EOF and deadlocks SDL_CloseAudioDevice). */
static int pq_get_nb(PacketQueue *q, AVPacket *dst){
    int ret=0;
    SDL_LockMutex(q->mutex);
    if(q->first){
        PacketList *e=q->first; q->first=e->next; if(!q->first) q->last=NULL;
        q->nb_packets--;
        av_packet_move_ref(dst, e->pkt); av_packet_free(&e->pkt); av_free(e);
        ret=1;
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

/* ---------------- player state ---------------- */
typedef struct {
    AVFormatContext *fmt;
    int vstream, astream, sstream;   /* sstream = embedded text-subtitle stream (-1 if none) */
    AVCodecContext *vdec, *adec, *sdec;
    PacketQueue vq, aq, sq;          /* sq = subtitle packets, drained+decoded in the main loop */
    struct SwsContext *sws;
    struct SwrContext *swr;

    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    int tex_w, tex_h;

    SDL_AudioDeviceID audio_dev;
    int out_rate, out_channels;

    /* audio master clock */
    double audio_clock;        /* seconds */
    SDL_mutex *clock_mutex;
    double frame_timer;
    int    audio_stalled;      /* set when a present-but-undecodable audio track (e.g. some WMVs) stops
                                  advancing the clock → pace video off the wall clock so it doesn't freeze */
    double aclk_val, aclk_t;   /* last observed audio_clock value + wall time it last changed (stall watchdog) */
    double aclk_probe;         /* audio callback publishes its clock here even while wall-paced */

    /* leftover decoded audio bytes for the SDL callback */
    uint8_t *abuf; unsigned int abuf_size, abuf_index, abuf_size_used;

    AVFrame *aframe;
    AVPacket *apkt_tmp;

    int paused, muted, quit, eof;
    int upscale, framegen;     /* AI stages for the next Enhance job (U = AI Upscale, G = AI Frame-Gen) */
    int light;                 /* D = Light Enhancement: v141's live shader trio (Lanczos-3 upscale +
                                  blend frame-gen + √2 DELTA gate) at present time — zero pre-compute,
                                  for machines whose GPU can't run the AI models. */
    int anime;                 /* T = AnimeUpscale: the fast anime/toon model — cartoonish on real footage,
                                  so it's a SEPARATE button; mutually exclusive with the real-footage upscaler */
    int fill;                  /* crop-to-fill (cover) vs letterbox (contain) */
    int is_image;              /* source is a still image → hold the frame, don't auto-advance */
    double volume;             /* 0..1 */
    double seek_req;           /* relative seek seconds pending */
    double seek_target;        /* absolute seek target (when seek_abs) */
    int seek_flag, seek_abs;
    int flushed_rs;            /* seek happened: reset the audio-clock CREEP watchdog window */
    double seek_drop_until;    /* >0: DISCARD decoded audio/video below this time — hides the visible
                                  fast-forward when an index-less ASF/WMV seek lands far before target */
    int aclk_creep;            /* stall was BROKEN-PTS creep (net-backward clock): recovery disabled
                                  until a seek — the oscillating pts would flip-flop the latch forever */

    SDL_Thread *demux_thread;
    volatile int demux_stop;   /* tell the demux thread to exit WITHOUT quitting the app */
    volatile int flush_video;  /* demux asks the main thread to flush the video decoder (it owns it) */
    double duration;           /* seconds */
} Player;

static Player P;
/* play-while-enhancing: the demux thread (declared above the EnhanceJob) reads these to clamp
   forward seeks to the render head. Mirrored from g_enh by enhance_poll. */
static volatile int    g_live_playing = 0;
static volatile double g_live_rendered = 0;
static int use_gl = 0;   /* 1 = OpenGL output (shaders), 0 = SDL_Renderer fallback */
/* audio visualizer: PCM ring filled by the audio callback, read by the renderer */
#define VIZ_N 2048
#define VIZ_PI 3.14159265358979f
static float g_viz_pcm[VIZ_N]; static volatile int g_viz_w = 0; static volatile int g_viz_has = 0;
static volatile int64_t g_swr_delay = 0;   /* samples still buffered INSIDE the resampler */
static int g_viz_mode = 3;   /* 0=bars 1=wave 2=ripple 3=Matrix rain (default — the headline) */
/* ---- source / render statistics (for the Stats panel; populated on open) ---- */
static char g_src_codec[80]="", g_src_pix[40]="", g_src_acodec[80]="";
static int  g_src_w=0, g_src_h=0, g_src_arate=0, g_src_ach=0;
static double g_sar = 1.0;   /* source pixel aspect (width/height of one pixel); 1 = square */
static int g_seek_bytes = 0; /* this file's timestamps can't be seeked by time (see ps_duration_fix) */
static double g_src_fps=0; static int64_t g_src_vbr=0, g_src_abr=0;
static int  g_out_w=0, g_out_h=0;          /* video drawn size after letterbox/fill */
static double g_disp_fps=0;                /* measured on-screen present rate (vsync) */
static volatile int g_vid_frames=0;        /* counter: NEW decoded frames shown (for real fps) */
static double g_vid_fps=0;                 /* measured ACTUAL video frame rate (what you see) */
static int  g_orig_w=0, g_orig_h=0; static double g_orig_fps=0;  /* the ORIGINAL's res/fps (kept when swapped to enhanced) */
/* one-shot debug milestones (reset per file in close_media) to localise a crash */
/* HANG WATCHDOG (2026-10-27): Windows logged several "hogofwar.exe stopped interacting" hangs and the
   user saw "crashed when pressing N to skip a few times" — the log just stopped, no [CRASH] line. The main
   loop stamps g_heartbeat; a tiny watcher thread logs WHERE the main thread is stuck (g_stage, set at each
   blocking step of a file switch) once it has been silent for >4s. */
static volatile const char *g_stage = "startup";
static volatile Uint32 g_heartbeat = 0;
static HANDLE g_main_thread = NULL;   /* real handle to the main thread (for the hang stack dump) */
/* print the main thread's call stack as module+offset (offsets in hogofwar.exe map to functions with
   `nm -n hogofwar.exe`). Windows' own AppHang reports for this bug carried no stack at all. */
static void hang_dump_main_stack(void){
    if(!g_main_thread) return;
    if(SuspendThread(g_main_thread) == (DWORD)-1) return;
    CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_FULL;
    if(GetThreadContext(g_main_thread, &c)){
        HANDLE proc = GetCurrentProcess();
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME); SymInitialize(proc, NULL, TRUE);
        STACKFRAME64 f; memset(&f, 0, sizeof f);
        f.AddrPC.Offset = c.Rip; f.AddrPC.Mode = AddrModeFlat;
        f.AddrFrame.Offset = c.Rbp; f.AddrFrame.Mode = AddrModeFlat;
        f.AddrStack.Offset = c.Rsp; f.AddrStack.Mode = AddrModeFlat;
        for(int i = 0; i < 32; i++){
            if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, g_main_thread, &f, &c, NULL,
                            SymFunctionTableAccess64, SymGetModuleBase64, NULL) || !f.AddrPC.Offset) break;
            HMODULE m = NULL; char mod[MAX_PATH] = "?";
            if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCSTR)(uintptr_t)f.AddrPC.Offset, &m) && m){
                GetModuleFileNameA(m, mod, sizeof mod); }
            const char *bn = mod; for(const char *q = mod; *q; q++) if(*q=='\\'||*q=='/') bn = q+1;
            char sym[sizeof(SYMBOL_INFO) + 256]; SYMBOL_INFO *si = (SYMBOL_INFO*)sym;
            memset(sym, 0, sizeof sym); si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255; DWORD64 disp = 0;
            int named = SymFromAddr(proc, f.AddrPC.Offset, &disp, si);
            fprintf(stderr,"[HANG]   #%02d %s+0x%llx  %s\n", i, bn,
                    (unsigned long long)(f.AddrPC.Offset - (DWORD64)(uintptr_t)m), named ? si->Name : "");
        }
    }
    ResumeThread(g_main_thread);
}
static int hang_watch_thread(void *ud){
    (void)ud; int reported = 0;
    for(;;){
        SDL_Delay(1000);
        Uint32 hb = g_heartbeat;
        if(hb && SDL_GetTicks() - hb > 4000){
            if(!reported){ fprintf(stderr,"[HANG] main thread silent %.1fs, stuck at: %s\n",
                                   (SDL_GetTicks()-hb)/1000.0, (const char*)g_stage);
                           hang_dump_main_stack(); reported = 1; }
        } else if(reported){ fprintf(stderr,"[HANG] main thread recovered (was at: %s)\n", (const char*)g_stage); reported = 0; }
    }
}
static int g_dbg[16];
#define DBG_ONCE(i, ...) do{ if(!g_dbg[i]){ g_dbg[i]=1; fprintf(stderr, __VA_ARGS__); } }while(0)

/* ---------------- audio: SDL pulls, we decode on demand ---------------- */
static int decode_audio_frame(void){
    /* fill P.abuf with one resampled frame; return bytes, or -1 on abort/eof */
    int dropped = 0;                 /* stale post-seek audio frames discarded this call */
    for(;;){
        if(P.quit) return -1;
        int r = avcodec_receive_frame(P.adec, P.aframe);
        if(r == 0){
            int out_ch = P.out_channels;
            int64_t apts = P.aframe->pts;                 /* capture before convert/unref */
            g_swr_delay = swr_get_delay(P.swr, P.out_rate);   /* diagnostic: resampler-internal backlog */
            int max_out = (int)av_rescale_rnd(swr_get_delay(P.swr, P.aframe->sample_rate) + P.aframe->nb_samples,
                                              P.out_rate, P.aframe->sample_rate, AV_ROUND_UP);
            int need = max_out * out_ch * (int)sizeof(int16_t);
            if((unsigned)need > P.abuf_size){ P.abuf = av_realloc(P.abuf, need); P.abuf_size = need; }
            uint8_t *outp[1] = { P.abuf };
            DBG_ONCE(6,"[dbg] audio decode in_sr=%d nb=%d out_rate=%d swr=%p\n",
                     P.aframe->sample_rate, P.aframe->nb_samples, P.out_rate, (void*)P.swr);
            int got = swr_convert(P.swr, outp, max_out,
                                  (const uint8_t**)P.aframe->extended_data, P.aframe->nb_samples);
            if(got < 0) return -1;
            DBG_ONCE(7,"[dbg] first audio frame got=%d\n", got);
            if(got == 0) continue;   /* nothing emitted yet → keep decoding (never spin) */
            /* advance audio clock to end of this frame. Some containers (ASF/WMV → WMA) decode audio
               frames with NO pts; the old code only advanced on a valid pts, so the master clock stayed
               pinned at 0, the video-pacing gate never opened, and the picture FROZE on frame 1 (looked
               like "poor framerate"). Fall back to ACCUMULATING the decoded duration when pts is absent,
               and resync to pts whenever it's present. */
            double frame_secs = (double)got / P.out_rate;
            SDL_LockMutex(P.clock_mutex);
            /* WALL-PACED (broken-timestamp file): the video pump owns the master clock. If we ALSO
               stamped it from this file's bad pts, the two writers fought every frame and the value
               oscillated — the visible "seek bar glitching/jittering". Stay silent while stalled;
               the pump's self-healing check hands the clock back when audio starts behaving. */
            double acand = (apts != AV_NOPTS_VALUE)
                         ? apts * av_q2d(P.fmt->streams[P.astream]->time_base) + frame_secs
                         : P.audio_clock + frame_secs;
            P.aclk_probe = acand;              /* always published: lets the pump notice healthy audio */
            /* NEVER drag the master backwards: after an ASF seek the first audio packets decode from
               BEFORE the target, and a regressing clock froze the video (post-seek "speed pulses").
               The pump coasts on the wall clock meanwhile and audio simply re-takes the lead here. */
            if(!P.audio_stalled && acand > P.audio_clock) P.audio_clock = acand;
            double master = P.audio_clock;
            SDL_UnlockMutex(P.clock_mutex);
            /* AUDIO CATCH-UP DROP — the mirror of the video one in gl_video_pump().
               A backward seek lands on the keyframe BEFORE the target, so these first frames are
               audio from BEFORE where the user clicked. Playing them means the device spends real
               seconds on stale sound while the master clock stays pinned at the target and the
               picture holds still. Discard them (decode is ~free; playback is not) so the clock
               reaches the target immediately and video resumes.
               Bounded per call: the callback is a realtime thread — never loop unbounded here. A
               file with no audio pts (acand synthesised above) can't be behind, so it never drops. */
            if(apts != AV_NOPTS_VALUE && !P.audio_stalled && acand < master - 0.05 && dropped < 512){
                dropped++;
                continue;                      /* decode the next frame; emit nothing */
            }
            return got * out_ch * (int)sizeof(int16_t);
        } else if(r == AVERROR(EAGAIN)){
            /* need a new packet — NON-blocking so the audio thread never hangs */
            if(!pq_get_nb(&P.aq, P.apkt_tmp)) return -1;
            if(avcodec_send_packet(P.adec, P.apkt_tmp) < 0){ av_packet_unref(P.apkt_tmp); continue; }
            av_packet_unref(P.apkt_tmp);
        } else {
            return -1; /* EOF/error on this decoder */
        }
    }
}

/* Apply gain to S16 samples and write them into the (zeroed) output buffer, saturating at the int16
   rails. SDL_MixAudioFormat can't do this job: its volume argument is defined only up to
   SDL_MIX_MAXVOLUME, so it cannot express the >100% boost, and there is nothing to mix WITH here —
   one source into a buffer we just cleared. Saturating is what makes 200% safe: a loud passage
   clips (flattens) instead of wrapping around to the opposite rail, which would be violent noise. */
static void audio_apply_gain(Uint8 *dst, const Uint8 *src, int nbytes, float gain){
    int16_t *d = (int16_t*)dst; const int16_t *s = (const int16_t*)src;
    for(int i = 0, n = nbytes/2; i < n; i++){
        int v = (int)lrintf(s[i] * gain);
        if(v >  32767) v =  32767;
        if(v < -32768) v = -32768;
        d[i] = (int16_t)v;
    }
}
static void audio_callback(void *ud, Uint8 *stream, int len){
    (void)ud;
    SDL_memset(stream, 0, len);          /* clean base — a short decode leaves the tail silent */
    if(P.paused) return;
    const float gain = P.muted ? 0.0f : (float)P.volume;
    while(len > 0){
        if(P.abuf_index >= P.abuf_size_used){
            int got = decode_audio_frame();
            if(got <= 0) return;         /* underrun/EOF → leave the rest silent (never spin) */
            P.abuf_size_used = (unsigned)got; P.abuf_index = 0;
        }
        int n = (int)(P.abuf_size_used - P.abuf_index);
        if(n > len) n = len;
        if(gain > 0.0f) audio_apply_gain(stream, P.abuf + P.abuf_index, n, gain);
        /* capture mono PCM into the visualizer ring (full-scale, independent of volume) */
        { const int16_t *sp=(const int16_t*)(P.abuf+P.abuf_index); int fr=n/4; int w=g_viz_w;
          for(int i=0;i<fr;i++){ g_viz_pcm[w]=(sp[i*2]+sp[i*2+1])*(0.5f/32768.0f); w=(w+1)&(VIZ_N-1); }
          g_viz_w=w; if(fr>0) g_viz_has=1; }
        P.abuf_index += n; stream += n; len -= n;
    }
}

/* ---------------- demux thread ---------------- */
static int demux_thread(void *ud){
    (void)ud;
    AVPacket *pkt = av_packet_alloc();
    int live_eof_spins = 0;   /* consecutive live-tail EOF retries (wedge detector) */
    while(!P.quit && !P.demux_stop){
        if(P.seek_flag){
            double target;
            SDL_LockMutex(P.clock_mutex);
            target = P.seek_abs ? P.seek_target : (P.audio_clock + P.seek_req);
            SDL_UnlockMutex(P.clock_mutex);
            P.seek_abs = 0;
            if(target < 0) target = 0;
            if(P.duration > 1 && target > P.duration - 1) target = P.duration - 1;
            /* live progressive render: seeking past the render head is handled in the player
               (enhance_poll holds + waits for the render to reach it), so the demux only ever
               receives targets that are already rendered — clamp defensively to the head. */
            if(g_live_playing && g_live_rendered > 1 && target > g_live_rendered - 1)
                target = g_live_rendered - 1;
            int64_t ts = (int64_t)(target * AV_TIME_BASE);
            int sk;
            if(g_seek_bytes && P.duration > 1 && P.fmt->pb && avio_size(P.fmt->pb) > 0){
                /* VOB whose timestamps restart: a time seek binary-searches those timestamps and lands
                   at the START every time. Seek by byte position instead, aimed ~5s early; the existing
                   audio/video catch-up drops then decode forward to the exact target. */
                double pre = target - 5.0; if(pre < 0) pre = 0;
                int64_t pos = (int64_t)(pre / P.duration * (double)avio_size(P.fmt->pb));
                sk = av_seek_frame(P.fmt, -1, pos, AVSEEK_FLAG_BYTE);
            } else sk = av_seek_frame(P.fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
            if(sk >= 0){
                pq_flush(&P.vq); pq_flush(&P.sq);   /* sub cues already added are kept (time-stamped) */
                /* CRITICAL: each decoder may only be touched by ONE thread.
                   Flush the AUDIO decoder under the audio lock (callback thread owns
                   it), and ask the MAIN thread to flush the VIDEO decoder. Flushing
                   them here directly raced the other threads → the seek crash. */
                if(P.audio_dev) SDL_LockAudioDevice(P.audio_dev);
                pq_flush(&P.aq);
                if(P.adec) avcodec_flush_buffers(P.adec);
                /* Also drop what the RESAMPLER is holding. swr keeps an internal FIFO (that's what
                   swr_get_delay reports); flushing the decoder does not empty it. Left behind, those
                   pre-seek samples are emitted after the seek, ahead of the new audio, while the
                   clock is stamped from the NEW packet's pts — so sound and clock disagree, and each
                   seek adds more. Only bites when the source needs rate conversion (delay is 0 when
                   in_rate == out_rate), which is why it hides on most files. */
                if(P.swr) swr_init(P.swr);
                g_swr_delay = 0;
                P.abuf_index = P.abuf_size_used = 0;
                if(P.audio_dev) SDL_UnlockAudioDevice(P.audio_dev);
                SDL_LockMutex(P.clock_mutex); P.audio_clock = target; SDL_UnlockMutex(P.clock_mutex);
                if(P.astream < 0) P.frame_timer = av_gettime()/1000000.0 - target; /* keep wall clock in sync */
                P.flush_video = 1;
            }
            P.seek_flag = 0; P.eof = 0;
        }
        if(P.vq.nb_packets > 90 && P.aq.nb_packets > 90){ SDL_Delay(8); continue; }
        int r = av_read_frame(P.fmt, pkt);
        if(r < 0){
            /* LIVE ENHANCE tail-follow: the file we're playing is still being APPENDED to by the
               enhancer. av_read_frame hit the current end and cached EOF — but this isn't the real
               end. Clear the cached EOF with a BYTE seek to the current position (NOT a timestamp
               seek, so audio/video never misalign) and keep reading the freshly-appended data.
               This replaces the old close+reopen-on-catchup, which re-seeked an unindexed growing
               mkv and drifted the audio. When the enhancer finishes, g_live_playing goes 0 and
               normal end-of-file handling resumes. */
            if(g_live_playing && P.fmt && P.fmt->pb){
                int64_t pos = avio_tell(P.fmt->pb);
                avio_flush(P.fmt->pb);
                if(pos>=0) avio_seek(P.fmt->pb, pos, SEEK_SET);
                P.eof = 0;
                /* WEDGE RECOVERY: the mkv demuxer can latch an INTERNAL end-of-stream state at the
                   render head that a byte-seek cannot clear — av_read_frame then fails forever while
                   the file keeps growing ("still image forever" bug). If plain retries go nowhere
                   for ~2s, recover through the REAL seek path: av_seek_frame resets the demuxer's
                   state, and the seek branch above reuses the proven flush machinery, so playback
                   resumes in sync from just behind the current clock. */
                live_eof_spins++;
                if(live_eof_spins >= 50){
                    live_eof_spins = 0;
                    double clk; SDL_LockMutex(P.clock_mutex); clk = P.audio_clock; SDL_UnlockMutex(P.clock_mutex);
                    P.seek_target = clk>0.5 ? clk-0.5 : 0; P.seek_abs = 1; P.seek_flag = 1;
                    fprintf(stderr,"[live] demuxer wedged at render head — recovering via seek to %.2fs\n", P.seek_target);
                }
                SDL_Delay(40); continue;                     /* not the true end — wait for more data */
            }
            { static int logged=0; if(logged<4){ char eb[80]; av_strerror(r,eb,sizeof eb);
                fprintf(stderr,"[demux] av_read_frame r=%d (%s) vq=%d aq=%d\n",r,eb,P.vq.nb_packets,P.aq.nb_packets); logged++; } }
            P.eof = 1; SDL_Delay(20); continue;
        }
        live_eof_spins = 0;                                   /* healthy read — reset the wedge detector */
        if(pkt->stream_index == P.vstream) pq_put(&P.vq, pkt);
        else if(pkt->stream_index == P.astream) pq_put(&P.aq, pkt);
        else if(pkt->stream_index == P.sstream) pq_put(&P.sq, pkt);   /* embedded subtitles */
        else av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    return 0;
}

/* ---------------- open / setup ---------------- */
/* ---- hardware (GPU) video decode via D3D11VA ----
   4K/60 10-bit HEVC etc. is far too heavy to decode on the CPU (was ~1.5fps). The AMD/NVIDIA/
   Intel GPU has a dedicated video decoder that does it effortlessly; D3D11VA is the modern
   Windows path. Decoded frames live in GPU memory (AV_PIX_FMT_D3D11); we copy each back to
   system memory and feed it through the existing sws→GL upload. If anything fails we silently
   fall back to (now multithreaded) software decode. */
static AVBufferRef *g_hw_device = NULL;
static enum AVPixelFormat g_hw_pixfmt = AV_PIX_FMT_NONE;
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *fmts){
    (void)ctx;
    for(const enum AVPixelFormat *p=fmts; *p != AV_PIX_FMT_NONE; p++)
        if(*p == g_hw_pixfmt) return *p;
    fprintf(stderr,"[hw] decoder didn't offer the d3d11va format — using software\n");
    g_hw_pixfmt = AV_PIX_FMT_NONE;          /* so the decode loop won't try to transfer */
    return fmts[0];                          /* first entry = software format */
}
static void try_hw_decode(AVCodecContext *c, const AVCodec *dec){
    g_hw_pixfmt = AV_PIX_FMT_NONE;
    const AVCodecHWConfig *cfg; enum AVPixelFormat hwfmt = AV_PIX_FMT_NONE;
    for(int i=0; (cfg = avcodec_get_hw_config(dec, i)); i++)
        if((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
           cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA){ hwfmt = cfg->pix_fmt; break; }
    if(hwfmt == AV_PIX_FMT_NONE) return;     /* this codec has no D3D11VA hwaccel */
    if(!g_hw_device && av_hwdevice_ctx_create(&g_hw_device, AV_HWDEVICE_TYPE_D3D11VA, NULL, NULL, 0) < 0){
        fprintf(stderr,"[hw] d3d11va device create failed — software decode\n"); g_hw_device = NULL; return;
    }
    c->hw_device_ctx = av_buffer_ref(g_hw_device);
    c->get_format = get_hw_format;
    g_hw_pixfmt = hwfmt;
    fprintf(stderr,"[hw] GPU video decode enabled (D3D11VA, %s)\n", av_get_pix_fmt_name(hwfmt));
}
static AVCodecContext *open_decoder(int idx, int video){
    AVStream *st = P.fmt->streams[idx];
    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    if(!dec) return NULL;
    AVCodecContext *c = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(c, st->codecpar);
    c->pkt_timebase = st->time_base;
    if(video){
        int nt = SDL_GetCPUCount(); if(nt < 1) nt = 4; if(nt > 16) nt = 16;
        c->thread_count = nt;                                /* multithreaded software decode */
        c->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;
        if(use_gl) try_hw_decode(c, dec);                    /* prefer the GPU decoder (GL path only) */
    }
    if(avcodec_open2(c, dec, NULL) < 0){ avcodec_free_context(&c); return NULL; }
    return c;
}

/* text-based subtitle codecs we can decode + render (image subs PGS/DVD need a bitmap path) */
static int is_text_sub(enum AVCodecID id){
    switch(id){
        case AV_CODEC_ID_SUBRIP: case AV_CODEC_ID_TEXT: case AV_CODEC_ID_ASS:
        case AV_CODEC_ID_MOV_TEXT: case AV_CODEC_ID_WEBVTT: return 1;
        default: return 0;
    }
}

/* open (or re-open) the audio decoder + SDL audio device for a given stream index */
static int open_audio_stream(int idx){
    P.astream = idx;
    P.adec = open_decoder(idx, 0);
    if(!P.adec) return -1;
    P.out_rate = P.adec->sample_rate;
    P.out_channels = 2;
    if(P.out_rate <= 0 || P.adec->ch_layout.nb_channels <= 0){
        fprintf(stderr,"[audio] bad params (rate=%d ch=%d)\n", P.out_rate, P.adec->ch_layout.nb_channels);
        return -1;   /* caller disables audio */
    }
    AVChannelLayout out_layout; av_channel_layout_default(&out_layout, P.out_channels);
    int sr = swr_alloc_set_opts2(&P.swr, &out_layout, AV_SAMPLE_FMT_S16, P.out_rate,
                        &P.adec->ch_layout, P.adec->sample_fmt, P.adec->sample_rate, 0, NULL);
    av_channel_layout_uninit(&out_layout);
    if(sr < 0 || !P.swr || swr_init(P.swr) < 0){ fprintf(stderr,"[audio] swr init failed\n"); return -1; }
    SDL_AudioSpec want, have; SDL_zero(want);
    want.freq=P.out_rate; want.format=AUDIO_S16SYS; want.channels=P.out_channels;
    want.samples=1024; want.callback=audio_callback;
    P.audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if(!P.audio_dev){ fprintf(stderr,"[audio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError()); return -1; }
    SDL_PauseAudioDevice(P.audio_dev, P.paused?1:0);
    /* capture audio stats for the Stats panel */
    { AVStream *as = P.fmt->streams[idx];
      const AVCodec *c = avcodec_find_decoder(as->codecpar->codec_id);
      snprintf(g_src_acodec,sizeof(g_src_acodec),"%s", c?c->name:"?");
      g_src_arate = as->codecpar->sample_rate;
      g_src_ach   = as->codecpar->ch_layout.nb_channels;
      g_src_abr   = as->codecpar->bit_rate; }
    return 0;
}

static void toast(const char *msg);
/* ---- VOB / MPEG program-stream quirks (2026-10-27) ----
   DURATION: FFmpeg derives a PS file's duration from its LAST timestamps. A VOB whose timestamps restart
   near the end (a short trailing cell) reads as e.g. 3.4s for a 7-minute, 328 MB file — the seek bar and
   the playlist length were nonsense. Such a result implies an impossible data rate (DVD max ~10 Mbit/s),
   so re-estimate from the real rate measured over the first ~16 MB. Returns the new duration, or -1. */
static double ps_duration_fix(AVFormatContext *f, const char *path){
    if(!f || !f->iformat || strcmp(f->iformat->name, "mpeg") || !f->pb) return -1;
    int64_t size = avio_size(f->pb); if(size < 4*1024*1024) return -1;
    double dur = (f->duration != AV_NOPTS_VALUE && f->duration > 0) ? f->duration/(double)AV_TIME_BASE : 0;
    if(dur > 0 && size*8.0/dur < 40e6) return -1;                    /* plausible: leave it alone */
    /* the opening's data rate isn't the file's average (measured: 16 MB from the start under-read a
       428s VOB as 334s). Instead read the video timestamp at byte offsets near the END and take the
       furthest one that is still on the main timeline (beyond the first timestamp): time-per-byte from
       there to the start, extrapolated over the remaining few percent. */
    AVFormatContext *g = NULL; if(avformat_open_input(&g, path, NULL, NULL) < 0) return -1;
    AVPacket *pk = av_packet_alloc(); double est = -1;
    double t0 = -1;                                    /* first video pts */
    for(int n = 0; pk && n < 2000 && av_read_frame(g, pk) >= 0; n++){
        AVStream *st = g->streams[pk->stream_index];
        int v = st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && pk->pts != AV_NOPTS_VALUE;
        if(v) t0 = pk->pts * av_q2d(st->time_base);
        av_packet_unref(pk); if(v) break;
    }
    static const double FR[] = { 0.97, 0.92, 0.85, 0.75, 0.6, 0.45, 0.3 };
    for(int k = 0; t0 >= 0 && pk && k < 7 && est < 0; k++){
        int64_t pos = (int64_t)(size * FR[k]);
        if(av_seek_frame(g, -1, pos, AVSEEK_FLAG_BYTE) < 0) continue;
        for(int n = 0; n < 3000 && av_read_frame(g, pk) >= 0; n++){
            AVStream *st = g->streams[pk->stream_index];
            if(st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && pk->pts != AV_NOPTS_VALUE){
                double t = pk->pts * av_q2d(st->time_base);
                int64_t at = pk->pos > 0 ? pk->pos : pos;
                if(t > t0 + 2.0 && at > 0) est = t0 + (t - t0) * (double)size / (double)at;   /* on the main timeline */
                av_packet_unref(pk); break;            /* (a restarted, smaller pts: try an earlier offset) */
            }
            av_packet_unref(pk);
        }
    }
    av_packet_free(&pk); avformat_close_input(&g);
    return (est > dur + 1.0) ? est : -1;
}
/* CSS: a VOB copied from a protected DVD without decryption has its scrambling bits set on a large share
   of PES packets (measured 17-35% on the user's encrypted copies, 0% on clean ones); it decodes as a
   stream of "ac-tex damaged" errors with dropped frames. The player can't (and won't) decrypt it — say so
   instead of playing garbage silently. Samples 4 MB at a third of the way in; >=5% scrambled = encrypted. */
static int ps_encrypted(AVFormatContext *f, const char *path){
    if(!f || !f->iformat || strcmp(f->iformat->name, "mpeg")) return 0;
    wchar_t w[2200]; path_u8_to_w(path, w, 2200);
    FILE *fh = _wfopen(w, L"rb"); if(!fh) return 0;
    _fseeki64(fh, 0, SEEK_END); long long size = _ftelli64(fh);
    if(size < 8*1024*1024){ fclose(fh); return 0; }
    const size_t N = 4*1024*1024; unsigned char *b = malloc(N); if(!b){ fclose(fh); return 0; }
    _fseeki64(fh, size/3, SEEK_SET); size_t got = fread(b, 1, N, fh); fclose(fh);
    int pes = 0, scr = 0;
    for(size_t i = 0; i + 7 < got; i++){
        if(b[i]==0 && b[i+1]==0 && b[i+2]==1 && b[i+3]==0xE0 && (b[i+6] & 0xC0) == 0x80){
            pes++; if(b[i+6] & 0x30) scr++; i += 6; }
    }
    free(b);
    return pes >= 50 && scr * 20 >= pes;
}
static int open_file(const char *path){
    if(avformat_open_input(&P.fmt, path, NULL, NULL) < 0 || !P.fmt){
        fprintf(stderr, "[open] cannot open: %s\n", path); P.fmt=NULL; return -1; }
    if(avformat_find_stream_info(P.fmt, NULL) < 0){ fprintf(stderr,"[open] no stream info: %s\n",path); return -1; }
    P.vstream = av_find_best_stream(P.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    P.astream = av_find_best_stream(P.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    /* An audio file's embedded album art is a VIDEO stream flagged ATTACHED_PIC (a single still).
       Without this, music files render the cover image instead of the visualizer. Treat cover-art-
       only files as audio so the visualizer shows. */
    if(P.vstream >= 0 && P.astream >= 0 &&
       (P.fmt->streams[P.vstream]->disposition & AV_DISPOSITION_ATTACHED_PIC)){
        P.vstream = -1;
    }
    P.duration = (P.fmt->duration != AV_NOPTS_VALUE) ? P.fmt->duration / (double)AV_TIME_BASE : 0;
    g_seek_bytes = 0;
    { double fx = ps_duration_fix(P.fmt, path);
      if(fx > 0){ fprintf(stderr,"[open] MPEG-PS timestamps restart mid-file: duration %.1fs is impossible, re-estimated %.1fs (byte seeking)\n", P.duration, fx);
                  P.duration = fx; g_seek_bytes = 1; } }
    if(ps_encrypted(P.fmt, path)){
        fprintf(stderr,"[open] DVD file is still CSS-encrypted (scrambled packets): %s\n", path);
        toast("This DVD file is still copy-protected (encrypted) \342\200\224 it was copied without removing the protection, so it can't play cleanly");
    }

    /* reset + capture source stats for the Stats panel */
    g_src_codec[0]=g_src_pix[0]=g_src_acodec[0]=0;
    g_src_w=g_src_h=g_src_arate=g_src_ach=0; g_src_fps=0; g_src_vbr=g_src_abr=0; g_sar=1.0;
    if(P.vstream >= 0){
        P.vdec = open_decoder(P.vstream, 1);
        if(!P.vdec){
            /* decoder unavailable/failed → DISABLE video, don't leave a NULL decoder
               for the render loop to dereference (that was the crash). */
            fprintf(stderr,"[open] video decoder failed for stream %d — disabling video\n", P.vstream);
            P.vstream = -1;
        } else {
            AVStream *vs = P.fmt->streams[P.vstream];
            const AVCodec *c = avcodec_find_decoder(vs->codecpar->codec_id);
            snprintf(g_src_codec,sizeof(g_src_codec),"%s", c?c->name:"?");
            g_src_w = vs->codecpar->width; g_src_h = vs->codecpar->height;
            /* PIXEL ASPECT (2026-10-27): DVD/VOB (720x576 shown as 16:9 = SAR 64:45), anamorphic rips,
               many AVIs store non-square pixels. It was ignored, so they showed squashed/stretched. */
            { AVRational sar = av_guess_sample_aspect_ratio(P.fmt, vs, NULL);
              g_sar = (sar.num > 0 && sar.den > 0) ? (double)sar.num / sar.den : 1.0;
              if(g_sar < 0.2 || g_sar > 5.0) g_sar = 1.0;          /* garbage header: ignore */
              if(g_sar != 1.0) fprintf(stderr,"[open] pixel aspect %d:%d -> displayed as %.0fx%d\n", sar.num, sar.den, g_src_w*g_sar, g_src_h); }
            AVRational fr = vs->avg_frame_rate.num ? vs->avg_frame_rate : vs->r_frame_rate;
            g_src_fps = fr.den ? (double)fr.num/fr.den : 0;
            g_src_vbr = vs->codecpar->bit_rate;
            const char *pf = av_get_pix_fmt_name((enum AVPixelFormat)vs->codecpar->format);
            snprintf(g_src_pix,sizeof(g_src_pix),"%s", pf?pf:"?");
        }
    }
    if(P.astream >= 0){
        if(open_audio_stream(P.astream) != 0){
            /* same guard for audio: no decoder → no NULL deref in the audio callback */
            fprintf(stderr,"[open] audio decoder failed — disabling audio\n");
            if(P.adec) avcodec_free_context(&P.adec);
            if(P.swr)  swr_free(&P.swr);
            P.astream = -1;
        }
    }
    if(P.vstream < 0 && P.astream < 0){ fprintf(stderr,"[open] no playable streams: %s\n",path); return -1; }
    /* embedded TEXT subtitles (subrip/ass/mov_text/webvtt) → decoded live in the main loop so
       the Subs button actually toggles something. The first text track is picked; image-based
       tracks (PGS/DVD/DVB) are skipped (those need a bitmap-overlay path we don't have). An
       external sibling .srt/.vtt, if present, takes precedence (see pl_play_at). */
    P.sstream = -1; P.sdec = NULL;
    for(unsigned i=0; i<P.fmt->nb_streams; i++){
        AVStream *st = P.fmt->streams[i];
        if(st->codecpar->codec_type==AVMEDIA_TYPE_SUBTITLE && is_text_sub(st->codecpar->codec_id)){
            P.sdec = open_decoder((int)i, 0);
            if(P.sdec){ P.sstream=(int)i;
                fprintf(stderr,"[subs] embedded text track %u (%s)\n", i, avcodec_get_name(st->codecpar->codec_id)); }
            break;
        }
    }
    /* still-image detection: the image2 / *_pipe demuxers (png/jpg/bmp/webp/tiff/…) carry a
       lone video stream and no audio. For those we HOLD the decoded frame on screen instead of
       auto-advancing at EOF, so the player doubles as an image viewer. (animated gif/apng still
       play their frames, then hold the last one.) */
    { const char *ifn = P.fmt->iformat ? P.fmt->iformat->name : "";
      int img = (strstr(ifn,"image2")||strstr(ifn,"_pipe")||!strcmp(ifn,"gif")||!strcmp(ifn,"apng"));
      P.is_image = (img && P.astream < 0 && P.vstream >= 0); }
    fprintf(stderr,"[open] OK: %s  (v=%d a=%d, %.0fs%s)\n", path, P.vstream, P.astream, P.duration,
            P.is_image ? ", image" : "");
    return 0;
}

/* switch to a SPECIFIC audio stream index (used by the right-click menu) */
static void set_audio_stream(int ns){
    if(!P.fmt || ns<0 || ns==P.astream) return;
    if(P.fmt->streams[ns]->codecpar->codec_type!=AVMEDIA_TYPE_AUDIO) return;
    if(P.audio_dev){ SDL_CloseAudioDevice(P.audio_dev); P.audio_dev=0; } /* joins the audio callback */
    if(P.adec) avcodec_free_context(&P.adec);
    if(P.swr)  swr_free(&P.swr);
    pq_flush(&P.aq);                 /* drop the old track's queued packets */
    P.abuf_index=P.abuf_size_used=0;
    if(open_audio_stream(ns) != 0){  /* demux routes packets to the new P.astream from here */
        fprintf(stderr,"[audio] track %d failed — disabling audio\n", ns);
        if(P.adec) avcodec_free_context(&P.adec);
        if(P.swr)  swr_free(&P.swr);
        P.astream = -1;
    } else fprintf(stderr,"[audio] -> stream %d\n", ns);
}
/* cycle to the next audio stream in the current file (key 'a' / Audio button) */
static void switch_audio_track(int dir){
    if(!P.fmt) return;
    int idxs[64], na=0;
    for(unsigned i=0;i<P.fmt->nb_streams && na<64;i++)
        if(P.fmt->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_AUDIO) idxs[na++]=(int)i;
    if(na<=1) return;
    int cur=0; for(int i=0;i<na;i++) if(idxs[i]==P.astream) cur=i;
    int nx=(cur + (dir>=0?1:na-1)) % na;
    set_audio_stream(idxs[nx]);
}

/* ================= OpenGL output path ================= */
/* Load only the GL 2.0+/1.3+ entry points we use (1.1 ones come from opengl32). */
#define GLFUNCS(_) \
  _(PFNGLCREATESHADERPROC,            glCreateShader) \
  _(PFNGLSHADERSOURCEPROC,            glShaderSource) \
  _(PFNGLCOMPILESHADERPROC,           glCompileShader) \
  _(PFNGLGETSHADERIVPROC,             glGetShaderiv) \
  _(PFNGLGETSHADERINFOLOGPROC,        glGetShaderInfoLog) \
  _(PFNGLCREATEPROGRAMPROC,           glCreateProgram) \
  _(PFNGLATTACHSHADERPROC,            glAttachShader) \
  _(PFNGLLINKPROGRAMPROC,             glLinkProgram) \
  _(PFNGLGETPROGRAMIVPROC,            glGetProgramiv) \
  _(PFNGLGETPROGRAMINFOLOGPROC,       glGetProgramInfoLog) \
  _(PFNGLUSEPROGRAMPROC,              glUseProgram) \
  _(PFNGLDELETESHADERPROC,            glDeleteShader) \
  _(PFNGLGENBUFFERSPROC,              glGenBuffers) \
  _(PFNGLBINDBUFFERPROC,              glBindBuffer) \
  _(PFNGLBUFFERDATAPROC,              glBufferData) \
  _(PFNGLMAPBUFFERRANGEPROC,          glMapBufferRange) \
  _(PFNGLUNMAPBUFFERPROC,             glUnmapBuffer) \
  _(PFNGLGENVERTEXARRAYSPROC,         glGenVertexArrays) \
  _(PFNGLBINDVERTEXARRAYPROC,         glBindVertexArray) \
  _(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
  _(PFNGLVERTEXATTRIBPOINTERPROC,     glVertexAttribPointer) \
  _(PFNGLGETUNIFORMLOCATIONPROC,      glGetUniformLocation) \
  _(PFNGLUNIFORM1IPROC,               glUniform1i) \
  _(PFNGLUNIFORM1FPROC,               glUniform1f) \
  _(PFNGLUNIFORM2FPROC,               glUniform2f) \
  _(PFNGLUNIFORM4FPROC,               glUniform4f) \
  _(PFNGLDELETEPROGRAMPROC,           glDeleteProgram) \
  _(PFNGLACTIVETEXTUREPROC,           glActiveTexture) \
  _(PFNGLGENFRAMEBUFFERSPROC,         glGenFramebuffers) \
  _(PFNGLBINDFRAMEBUFFERPROC,         glBindFramebuffer) \
  _(PFNGLFRAMEBUFFERTEXTURE2DPROC,    glFramebufferTexture2D) \
  _(PFNGLCHECKFRAMEBUFFERSTATUSPROC,  glCheckFramebufferStatus) \
  _(PFNGLDELETEFRAMEBUFFERSPROC,      glDeleteFramebuffers)
/* FBO enums (in case the bundled GL 1.1 header predates them) */
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
/* single/dual-channel texture formats (GL 3.0; the bundled GL 1.1 header lacks them) */
#ifndef GL_RG
#define GL_RG 0x8227
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif
#ifndef GL_MAP_WRITE_BIT
#define GL_MAP_WRITE_BIT 0x0002
#endif
#ifndef GL_MAP_INVALIDATE_BUFFER_BIT
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#endif
#ifndef GL_R8
#define GL_R8 0x8229
#endif
#ifndef GL_RG8
#define GL_RG8 0x822B
#endif
#ifndef GL_R16
#define GL_R16 0x822A
#endif
#ifndef GL_RG16
#define GL_RG16 0x822C
#endif
#define GL_DECL(type, name) static type p_##name;
GLFUNCS(GL_DECL)
#undef GL_DECL

static SDL_GLContext gl_ctx;
static GLuint gl_prog, gl_vao, gl_vbo;
/* Each video slot holds up to 3 plane textures: RGBA in .y only, or Y/U/V (planar 4:2:0), or
   Y + interleaved UV (NV12/P010 from the D3D11VA decoder). Converting YUV→RGB on the GPU instead
   of sws_scale removes the 8-20ms/frame CPU convert + 60% of the PCIe upload that made enhanced
   1440p/4K60 files miss vsync (visible ~30fps while the decode counter claimed 60). */
typedef struct { GLuint y,u,v; } VidTex;
static VidTex gl_vt, gl_vtB;      /* A (current) / B (next) */
static int gl_tex_w, gl_tex_h;
static int vt_mode = 0;           /* 0=RGBA (sws fallback)  1=NV12/P010  2=planar YUV420 */
static int vt_fullrange = 0, vt_bt709 = 0, vt_hdr = 0;
static double gl_ptsA = -1, gl_ptsB = -1; static int gl_nframes = 0;
static GLint glu_tex, glu_texU, glu_texV, glu_mode, glu_range, glu_bt709, glu_hdr;
static GLint glu_texB, glu_texUB, glu_texVB, glu_light, glu_alpha, glu_texSize;   /* Light Enhancement */
static uint8_t *gl_rgba; static int gl_rgba_w, gl_rgba_h;
static AVFrame *vframe;   /* reused video frame (decoded by main thread) */
static double gl_last_alpha = 0;  /* last frame-gen alpha (for re-draw while paused) */

/* which texture slot gl_present() should draw this frame */
static VidTex pr_vt;
/* monitor native mode — the upscale resolution target and the frame-gen refresh target */
static int g_mon_w = 0, g_mon_h = 0, g_mon_hz = 0;

/* ---- OSD (on-screen control bar) ---- */
static GLuint ui_prog, ui_vao, ui_vbo, ui_atlas; static GLint uiu_screen, uiu_tex;
static float *ui_buf; static int ui_cap, ui_n;     /* CPU verts: x,y,u,v,r,g,b,a (8 floats) */
static int osd_visible; static Uint32 osd_last_active;
static int g_mouse_x, g_mouse_y, g_win_w = 1280, g_win_h = 720;
static float seek_x0, seek_x1, seek_y0, seek_y1;    /* clickable seek-bar region (pixels) */
/* ---- Unicode font atlas (stb_truetype, on-demand glyph cache) ---- */
#define FONT_PX 40
#define ATLAS_W 2048
#define ATLAS_H 2048
#define GLYPH_HASH 8192
static unsigned char *g_font_data = NULL; static stbtt_fontinfo g_font; static int g_font_ok = 0;
static float g_font_scale = 0; static float g_font_ascent_px = 0;
static float g_ui_scale = 1.20f;   /* global GUI text scale — larger, and less 40px-atlas downscaling (crisper) */
static int atlas_penx = 1, atlas_peny = 1, atlas_rowh = 0;
typedef struct { int cp, used; short w,h,xoff,yoff; float ax, u0,v0,u1,v1; } Glyph;
static Glyph g_glyphs[GLYPH_HASH];
static const float WHITE_U = 0.5f/ATLAS_W, WHITE_V = 0.5f/ATLAS_H;   /* reserved opaque texel */
typedef struct { float x,y,w,h; int id; } OsdBtn;
#define OSD_MAXBTN 256
static OsdBtn osd_btn[OSD_MAXBTN]; static int osd_nbtn;
static int show_playlist = 0; static float pl_scroll = 0;   /* playlist panel state */
static int g_pl_jump = 0;                                    /* scroll the panel to the playing item */
static double    *pl_dur = NULL;   /* per-item duration seconds; -1 unknown, -2 unprobeable */
static long long *pl_sz  = NULL;   /* per-item file size bytes; -1 unknown */
static float pl_sb_x0,pl_sb_y0,pl_sb_x1,pl_sb_y1;            /* playlist scrollbar geometry (for clicks) */
static int   g_pl_sb_drag = 0; static float g_pl_rows_vis=1, g_pl_rows_tot=1;
static float g_pl_w = 0; static int g_pl_resizing = 0;      /* draggable panel width (drag its left edge) */
/* playlist search box + media-type filter */
static char g_pl_search[128] = ""; static int g_pl_search_len = 0, g_pl_search_active = 0;
/* text caret: BYTE offset into g_pl_search, always on a UTF-8 boundary. Insert/delete/arrow keys and
   click-to-place all work from this, so editing mid-string no longer means retyping the tail. */
static int   g_pl_caret = 0;
static int   g_pl_anchor = 0;     /* text-selection anchor (byte offset); == caret means no selection */
static int   g_srch_drag = 0;     /* the mouse is drag-selecting text inside the search box */
/* persistent EXCLUDE words: any entry whose path contains one is hidden from the playlist AND never
   picked by next/shuffle, until the word is removed. Saved to hogs_excludes.txt beside the exe. */
#define EXCL_MAX 48
static char g_excl[EXCL_MAX][64]; static int g_excl_n = 0;
static unsigned g_srch_ver = 1;   /* bumped on any search-text / exclude change → terms re-parsed */
static int g_sess_dirty = 0;      /* playlist changed since the last session save */
static int g_pl_jump_sel = 0;     /* scroll the panel so g_pl_selected is visible (session restore) */
static float g_srch_text_x = 0;   /* x of the first glyph, published by draw_playlist_panel */
static int g_pl_filter = 0;   /* 0=All 1=Video 2=Audio 3=Image */
/* click-drag to reorder rows, and sort controls */
static int g_pl_drag_idx = -1;     /* row armed on mouse-down (real index); becomes a drag past a threshold */
static int g_pl_dragging = 0;      /* an active reorder drag is in progress */
static float g_pl_drag_y0 = 0;     /* mouse-down Y (to detect the move threshold) */
static int g_pl_drop_at = -1;      /* real index to insert BEFORE (pl_n = drop at end); -1 = none, published by draw */
static int g_pl_click_pending = -1;/* plain-click landed on an already-multi-selected row: collapse to single on
                                      mouse-UP if it did NOT become a drag (so dragging a selection keeps it) */
static int g_pl_sort_key = -1, g_pl_sort_dir = 1;   /* 0=name 1=size 2=length; dir 1 asc / -1 desc */
/* async playlist work (folder scan / length probe) done on a worker thread so the UI keeps drawing an
   animated spinner instead of freezing. Worker sets g_busy=BUSY_DONE; the main loop applies the result. */
enum { BUSY_NONE=0, BUSY_WORKING=1, BUSY_DONE=2 };
enum { BTASK_SORT=0, BTASK_FOLDER=1 };
static volatile int g_busy = 0; static int g_busy_task = 0;
static char g_busy_label[48] = "";
static SDL_Thread *g_busy_thread = NULL;
static double *g_busy_dur = NULL;                   /* SORT: worker-filled key values (size/length/mtime) per row */
static int g_busy_key = 0;                          /* SORT: which key the worker is probing */
static double *g_sort_mt = NULL; static int g_sort_mt_fresh = 0;   /* date-modified values for the current order */
typedef struct { char **v; int n, cap; } PathList;  /* growable path list (folder scan output) */
static PathList g_scan = {NULL,0,0};
static char g_scan_root[2048] = "";
enum { B_PREV=1, B_PLAY, B_STOP, B_NEXT, B_U, B_G,
       B_ENH,   /* combined Upscale+FrameGen in ONE click (E) */
       B_D,     /* AnimeUpscale — the anime/toon model as its own feature (T) */
       B_PAUSE2,/* dedicated Pause (Play is B_PLAY = the Spacebar action) */
       B_LIGHT, /* Light Enhancement — v141 live shader trio, no AI/GPU horsepower needed (D) */
       B_ARCH,  /* Archive — plus-models remaster saved to a user-chosen file (W) */
       B_FILL, B_R, B_H,
       B_AUDIO, B_SUBS, B_STATS, B_KEYS, B_LIST, B_ADDF, B_ADDD, B_CLEAR,
       B_THEME, /* cycle the OSD colour theme (Y) */
       B_SEARCH, B_FALL, B_FVID, B_FAUD, B_FIMG,
       B_SORTNAME, B_SORTSIZE, B_SORTLEN, B_SORTDATE,   /* sort the playlist by title / size / length / date modified */
       B_SAVEPL, B_LOADPL,                  /* save the playlist as .m3u8 / open (append) a .m3u/.m3u8 */
       B_DONATE,                            /* hotkeys panel: copy the Bitcoin donation address */
       B_EXCL_BASE=900,                     /* + i: remove exclude-word chip i */
       B_PLITEM_BASE=1000 };
/* playlist state (declared early so the OSD can read position/flags) */
static char **pl_items = NULL; static int pl_n = 0, pl_cap = 0, pl_cur = -1;
static int g_repeat = 1, g_shuffle = 1;   /* shuffle ON by default (leave-it-running-all-night use) */
static int g_pl_selected = -1;             /* selection ANCHOR (last-clicked row; shift ranges from here) */
static unsigned char *pl_sel = NULL;       /* per-item multi-select flags (ctrl/shift click; Del removes all) */
static int pl_selcount(void);              /* count of selected rows (defined with the reorder helpers) */
/* external subtitles (SRT/VTT) */
typedef struct { double s, e; char t[256]; } Cue;
static Cue *g_subs = NULL; static int g_nsubs = 0, g_subcap = 0;
static int g_subs_enabled = 1;             /* subtitle display on/off (B / right-click) */
static int g_show_stats = 0, g_show_keys = 0;

/* ====================== AI ENHANCE job state (declared early so the OSD can read it) =====
 * Buttons: U = AI Upscale (realesrgan-plus, real footage) · G = AI Frame-Gen (RIFE) · E = both in one
 * click · T = AnimeUpscale (compact toon model — cartoonish on real footage, so it's separate).
 * Enabling any of them PAUSES the video and runs the headless hogs_enhance.exe over the WHOLE file
 * (progress bar), then plays the enhanced H.265 result in the SAME window. Outputs live in the
 * enhcache\ LRU (≤50GB, wiped on quit) so replays are instant; toggles are STICKY across Next/skip. */
typedef enum { ENH_IDLE=0, ENH_RUN, ENH_ERR } EnhState;
typedef struct { EnhState state; float percent; char phase[64]; char out[1040]; char status_path[1040];
    PROCESS_INFORMATION pi; int has_proc; Uint32 last_poll;
    /* play-while-enhancing (progressive): watch the enhanced file as it renders */
    int live;            /* this is a progressive job — play the growing output */
    int playing;         /* we've started playing the growing file */
    int waiting;         /* paused because playback caught up to the render head */
    double rendered;     /* seconds of contiguous in-order output available (from status) */
    double total;        /* full timeline seconds (from status) */
    double wait_target;  /* >=0: user seeked ahead of the render head — hold here until reached */
    double anchor;       /* source time this render started from (0 = whole file). Seeking outside
                            [anchor, rendered] re-anchors: restart the render at the clicked point. */
    char   src[1040];    /* the input path (to re-spawn on re-anchor) */
    int    r_up, r_fg, r_an;   /* the stages (to re-spawn) */
    int  is_arch;             /* ARCHIVE job: on success, copy the finished render to arch_path */
    char arch_path[1024];     /* user-chosen save location (save-as dialog at button press) */
} EnhanceJob;
static EnhanceJob g_enh;
static int g_arch_sel = 0;    /* archive model policy active during cache-path/model selection */
static int g_playing_enhanced = 0;      /* currently showing the enhanced output (vs the original)? */
static void enhance_cancel(void);       /* fwd decl (playlist navigation cancels a running job) */
static void enhance_apply(void);        /* fwd decl (sticky AI toggles re-apply on the next video) */
static int  enh_running(void){ return g_enh.state==ENH_RUN; }
static char   g_log_path[1024];   /* temp log file, deleted on quit (no logs left on disk) */
static int    g_log_keep = 0;     /* HOGS_LOG=<path> given → keep it */
static char   g_toast[256]; static Uint32 g_toast_until;   /* transient OSD message */
static void toast(const char *msg){ snprintf(g_toast,sizeof(g_toast),"%s",msg); g_toast_until=SDL_GetTicks()+3500; }

/* ---- right-click context menu (audio / subtitle track selection) ---- */
typedef struct { float x,y,w,h; int kind, val; } CtxItem;  /* kind:1=audio 2=sub-off 3=sub-ext */
#define CTX_MAX 64
static CtxItem g_ctx[CTX_MAX]; static int g_ctx_n=0, g_ctx_open=0; static float g_ctx_x=0, g_ctx_y=0;
static const char *UI_VS =
  "#version 330 core\n"
  "layout(location=0) in vec2 a_pos; layout(location=1) in vec2 a_uv; layout(location=2) in vec4 a_col;\n"
  "uniform vec2 u_screen; out vec2 v_uv; out vec4 v_col;\n"
  "void main(){ vec2 p=a_pos/u_screen*2.0-1.0; gl_Position=vec4(p.x,-p.y,0.0,1.0); v_uv=a_uv; v_col=a_col; }\n";
static const char *UI_FS =
  "#version 330 core\n"
  "in vec2 v_uv; in vec4 v_col; out vec4 col; uniform sampler2D u_tex;\n"
  "void main(){ col=vec4(v_col.rgb, v_col.a * texture(u_tex, v_uv).r); }\n";

static const char *VS_SRC =
  "#version 330 core\n"
  "layout(location=0) in vec2 a_pos; out vec2 v_uv;\n"
  "void main(){ gl_Position=vec4(a_pos,0.0,1.0); v_uv=vec2(a_pos.x*0.5+0.5, 0.5-a_pos.y*0.5); }\n";
/* LIGHT ENHANCEMENT (ported from v141, 2026-07-10): Lanczos-3 upscale + edge-adaptive sharpen,
   motion-BLENDED frame-gen toward the display refresh, and the √2 DELTA motion gate — all live in
   the present shader, zero pre-compute. For machines without a GPU that can run the AI models.
   v1.44's frames are YUV planes (three textures per frame), so v141's RGBA sampling became fA()/fB()
   decode helpers; the trio's math is otherwise verbatim. (v141 also had an FBO reaction-diffusion
   DELTA integrator; this port uses its stateless gate — the visible behavior.) */
static const char *FS_SRC =
  "#version 330 core\n"
  "in vec2 v_uv; out vec4 col;\n"
  "uniform sampler2D u_tex;                 // frame A: RGBA frame, or the Y plane\n"
  "uniform sampler2D u_texU;                // NV12: interleaved UV (RG) · planar: U\n"
  "uniform sampler2D u_texV;                // planar: V\n"
  "uniform sampler2D u_texB;                // frame B (next) — Light Enhancement blend\n"
  "uniform sampler2D u_texUB;\n"
  "uniform sampler2D u_texVB;\n"
  "uniform int u_mode;                      // 0=RGBA  1=Y+UV(NV12/P010)  2=planar YUV420\n"
  "uniform int u_range;                     // 1 = full range (JPEG)\n"
  "uniform int u_bt709;                     // matrix: 0 = BT.601 (SD), 1 = BT.709 (HD), 2 = BT.2020 (UHD/HDR)\n"
  "uniform int u_hdr;                       // transfer: 0 = SDR, 1 = PQ (HDR10/DV base), 2 = HLG\n"
  "uniform int u_light;                     // Light Enhancement trio on/off\n"
  "uniform float u_alpha;                   // frame-gen blend position between A and B\n"
  "uniform vec2 u_texSize;\n"
  "vec3 dec(float y, vec2 uv){\n"
  "  if(u_range==1){ uv -= 0.5; }\n"
  "  else { y = (y - 16.0/255.0) * (255.0/219.0); uv = (uv - 0.5) * (255.0/224.0); }\n"
  "  if(u_bt709==2) return vec3(y + 1.4746*uv.y, y - 0.16455*uv.x - 0.57135*uv.y, y + 1.8814*uv.x);\n"
  "  return (u_bt709==1)\n"
  "    ? vec3(y + 1.5748*uv.y, y - 0.18732*uv.x - 0.46812*uv.y, y + 1.8556*uv.x)\n"
  "    : vec3(y + 1.4020*uv.y, y - 0.34414*uv.x - 0.71414*uv.y, y + 1.7720*uv.x);\n"
  "}\n"
  /* HDR -> SDR (2026-10). PQ/HLG 10-bit BT.2020 sent straight to an SDR surface reads flat and grey.
     Decode the transfer to display light, put SDR reference white (203 nits, ITU-R BT.2408) at 1.0,
     convert BT.2020 -> BT.709 primaries in LINEAR light, filmic tone-map the highlights (ACES fit),
     then re-encode with display gamma. */
  "vec3 pq_eotf(vec3 e){ const float m1=0.1593017578125, m2=78.84375, c1=0.8359375, c2=18.8515625, c3=18.6875;\n"
  "  vec3 p=pow(clamp(e,0.0,1.0), vec3(1.0/m2)); return pow(max(p-c1,0.0)/(c2-c3*p), vec3(1.0/m1))*10000.0; }\n"
  "vec3 hlg_eotf(vec3 e){ const float a=0.17883277, b=0.28466892, c=0.55991073; e=clamp(e,0.0,1.0);\n"
  "  vec3 s=mix(e*e/3.0, (exp((e-c)/a)+b)/12.0, step(vec3(0.5),e));\n"
  "  float ys=dot(s, vec3(0.2627,0.6780,0.0593)); return s*pow(max(ys,1e-6),0.2)*1000.0; }\n"
  /* BT.2390 EETF in PQ space on max(R,G,B): identity below the knee (shadows/midtones untouched — no
     crushed faces), Hermite roll-off of 95..1000+ nit highlights into the SDR range. Source peak 1000 nits
     (the common master), target = SDR reference white 203 nits -> 1.0. Hue-preserving (scale all channels). */
  "float pq_inv(float n){ const float m1=0.1593017578125, m2=78.84375, c1=0.8359375, c2=18.8515625, c3=18.6875;\n"
  "  float p=pow(clamp(n/10000.0,0.0,1.0), m1); return pow((c1+c2*p)/(1.0+c3*p), m2); }\n"
  "float pq_fwd(float e){ return pq_eotf(vec3(e)).r; }\n"
  "vec3 eetf(vec3 nits){\n"
  "  float M=max(max(nits.r,nits.g),nits.b); if(M<=1e-4) return nits;\n"
  "  const float LW=1000.0, LT=203.0; float src=pq_inv(LW), maxL=pq_inv(LT)/src, KS=1.5*maxL-0.5;\n"
  "  float E1=pq_inv(M)/src, E2=E1;\n"
  "  if(E1>KS){ float T=min((E1-KS)/(1.0-KS),1.0), T2=T*T, T3=T2*T;\n"
  "    E2=(2.0*T3-3.0*T2+1.0)*KS + (T3-2.0*T2+T)*(1.0-KS) + (-2.0*T3+3.0*T2)*maxL; }\n"
  "  return nits*(pq_fwd(E2*src)/M); }\n"
  "vec3 to_display(vec3 c){\n"
  "  if(u_hdr==0) return c;\n"
  "  vec3 nits = (u_hdr==1) ? pq_eotf(c) : hlg_eotf(c);\n"
  "  nits = max(mat3(1.6605,-0.1246,-0.0182, -0.5876,1.1329,-0.1006, -0.0728,-0.0083,1.1187) * nits, 0.0);\n"
  "  vec3 lin = clamp(eetf(nits)/203.0, 0.0, 1.0);\n"
  "  return pow(lin, vec3(1.0/2.2));\n"
  "}\n"
  "vec3 fA(vec2 uv){\n"
  "  if(u_mode==0) return texture(u_tex, uv).rgb;\n"
  "  vec2 c = (u_mode==1) ? texture(u_texU, uv).rg : vec2(texture(u_texU, uv).r, texture(u_texV, uv).r);\n"
  "  return dec(texture(u_tex, uv).r, c);\n"
  "}\n"
  "vec3 fB(vec2 uv){\n"
  "  if(u_mode==0) return texture(u_texB, uv).rgb;\n"
  "  vec2 c = (u_mode==1) ? texture(u_texUB, uv).rg : vec2(texture(u_texUB, uv).r, texture(u_texVB, uv).r);\n"
  "  return dec(texture(u_texB, uv).r, c);\n"
  "}\n"
  "#define LANCZOS_A 3\n"
  "float lanczos(float x){ x=abs(x); if(x<1e-5) return 1.0; if(x>=float(LANCZOS_A)) return 0.0;\n"
  "  float px=3.14159265*x; return (sin(px)/px)*(sin(px/float(LANCZOS_A))/(px/float(LANCZOS_A))); }\n"
  "vec3 lanczosF(vec2 uv, int which){ vec2 c=uv*u_texSize-0.5; vec2 b=floor(c); vec2 f=c-b;\n"
  "  vec3 r=vec3(0.0); float w=0.0;\n"
  "  for(int j=-2;j<=3;j++){ for(int i=-2;i<=3;i++){\n"
  "    vec2 tap=(b+vec2(float(i),float(j))+0.5)/u_texSize;\n"
  "    float wt=lanczos(float(i)-f.x)*lanczos(float(j)-f.y);\n"
  "    r+=(which==0?fA(tap):fB(tap))*wt; w+=wt; } } return r/w; }\n"
  "const float SQRT2_SQ = 2.0/(255.0*255.0);\n"   /* √2 colour gate: dR²+dG²+dB² > (√2/255)² */
  "float deltaMotion(vec2 uv){ vec2 off=1.0/u_texSize; float m=0.0;\n"
  "  for(int j=-1;j<=1;j++){ for(int i=-1;i<=1;i++){\n"
  "    vec2 t=uv+vec2(float(i),float(j))*off;\n"
  "    vec3 d=fB(t)-fA(t);\n"
  "    m=max(m, step(SQRT2_SQ, dot(d,d))); } } return m; }\n"
  "void main(){\n"
  "  if(u_light==0){ col = vec4(to_display(fA(v_uv)), 1.0); return; }\n"
  "  vec3 cur = lanczosF(v_uv, 0);\n"
  "  vec3 blended = mix(cur, lanczosF(v_uv, 1), u_alpha);\n"
  "  vec3 base = mix(cur, blended, deltaMotion(v_uv));\n"   /* static=crisp+denoised, moving=interp */
  "  { vec2 off=1.0/u_texSize; vec3 lc=vec3(0.2126,0.7152,0.0722); float luma=dot(base,lc);\n"
  "    vec3 l=fA(v_uv+vec2(-off.x,0.0)); vec3 rr=fA(v_uv+vec2(off.x,0.0));\n"
  "    vec3 u=fA(v_uv+vec2(0.0,-off.y)); vec3 d=fA(v_uv+vec2(0.0,off.y));\n"
  "    float edge=abs(dot(l,lc)-luma)+abs(dot(rr,lc)-luma)+abs(dot(u,lc)-luma)+abs(dot(d,lc)-luma);\n"
  "    float st=0.20*(0.5+0.5*clamp(edge*0.75,0.0,1.0));\n"  /* v141 edge-adaptive sharpen, fixed strength */
  "    vec3 sh=base+st*(4.0*base-l-rr-u-d);\n"
  "    vec3 mn=min(min(min(l,rr),min(u,d)),base); vec3 mx=max(max(max(l,rr),max(u,d)),base);\n"
  "    base=clamp(sh,mn,mx); }\n"
  "  col = vec4(to_display(base), 1.0);\n"
  "}\n";

static void font_init(void){
    const char *cands[] = {
        "C:/Windows/Fonts/YuGothR.ttc",  /* Yu Gothic — Japanese + Latin */
        "C:/Windows/Fonts/msyh.ttc",     /* YaHei — Chinese + Latin */
        "C:/Windows/Fonts/malgun.ttf",   /* Malgun — Korean + Latin */
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/segoeui.ttf", NULL };
    for(int i=0; cands[i] && !g_font_ok; i++){
        FILE *f=path_fopen(cands[i],L"rb"); if(!f) continue;
        fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
        if(sz<=0){ fclose(f); continue; }
        unsigned char *d=malloc(sz); if(!d){ fclose(f); continue; }
        if(fread(d,1,sz,f)!=(size_t)sz){ free(d); fclose(f); continue; }
        fclose(f);
        int off=stbtt_GetFontOffsetForIndex(d,0);
        if(off>=0 && stbtt_InitFont(&g_font,d,off)){ g_font_data=d; g_font_ok=1; fprintf(stderr,"[font] loaded %s\n",cands[i]); }
        else free(d);
    }
    if(g_font_ok){
        g_font_scale = stbtt_ScaleForPixelHeight(&g_font, FONT_PX);
        int asc,desc,gap; stbtt_GetFontVMetrics(&g_font,&asc,&desc,&gap);
        g_font_ascent_px = asc*g_font_scale;
    } else fprintf(stderr,"[font] none loaded — OSD falls back to ASCII\n");
    /* atlas: glyphs start at (3,3); reserve a 2x2 opaque white block at (0,0) for solid rects */
    glGenTextures(1,&ui_atlas); glBindTexture(GL_TEXTURE_2D, ui_atlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    unsigned char *z=calloc((size_t)ATLAS_W*ATLAS_H,1);
    if(z){ z[0]=z[1]=z[ATLAS_W]=z[ATLAS_W+1]=255;
        glTexImage2D(GL_TEXTURE_2D,0,GL_R8,ATLAS_W,ATLAS_H,0,GL_RED,GL_UNSIGNED_BYTE,z); free(z); }
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    atlas_penx=3; atlas_peny=3; atlas_rowh=0;
}

char g_gl_last_log[2048]={0};
static GLuint gl_compile(GLenum type, const char *src){
    GLuint s = p_glCreateShader(type);
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    GLint ok = 0; p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if(!ok){ char log[1024]; p_glGetShaderInfoLog(s, sizeof(log), NULL, log); fprintf(stderr,"[GL] shader: %s\n", log);
        extern char g_gl_last_log[2048]; snprintf(g_gl_last_log,2048,"%s",log); return 0; }
    return s;
}

/* returns 1 if the OpenGL output path is ready, 0 to fall back to SDL_Renderer */
static int gl_init(void){
    gl_ctx = SDL_GL_CreateContext(P.win);
    if(!gl_ctx){ fprintf(stderr,"[GL] no context: %s\n", SDL_GetError()); return 0; }
    SDL_GL_MakeCurrent(P.win, gl_ctx);
    SDL_GL_SetSwapInterval(1);
    int ok = 1;
    #define HOGS_GLLOAD(type, name) p_##name = (type)SDL_GL_GetProcAddress(#name); if(!p_##name) ok = 0;
    GLFUNCS(HOGS_GLLOAD)
    #undef HOGS_GLLOAD
    if(!ok){ fprintf(stderr,"[GL] missing entry points\n"); return 0; }

    GLuint vs = gl_compile(GL_VERTEX_SHADER, VS_SRC), fs = gl_compile(GL_FRAGMENT_SHADER, FS_SRC);
    if(!vs || !fs) return 0;
    gl_prog = p_glCreateProgram();
    p_glAttachShader(gl_prog, vs); p_glAttachShader(gl_prog, fs);
    p_glLinkProgram(gl_prog);
    GLint linked = 0; p_glGetProgramiv(gl_prog, GL_LINK_STATUS, &linked);
    if(!linked){ char log[1024]; p_glGetProgramInfoLog(gl_prog, sizeof(log), NULL, log); fprintf(stderr,"[GL] link: %s\n", log); return 0; }
    p_glDeleteShader(vs); p_glDeleteShader(fs);

    glu_tex      = p_glGetUniformLocation(gl_prog, "u_tex");
    glu_texU     = p_glGetUniformLocation(gl_prog, "u_texU");
    glu_texV     = p_glGetUniformLocation(gl_prog, "u_texV");
    glu_mode     = p_glGetUniformLocation(gl_prog, "u_mode");
    glu_range    = p_glGetUniformLocation(gl_prog, "u_range");
    glu_bt709    = p_glGetUniformLocation(gl_prog, "u_bt709");
    glu_hdr      = p_glGetUniformLocation(gl_prog, "u_hdr");
    glu_texB     = p_glGetUniformLocation(gl_prog, "u_texB");
    glu_texUB    = p_glGetUniformLocation(gl_prog, "u_texUB");
    glu_texVB    = p_glGetUniformLocation(gl_prog, "u_texVB");
    glu_light    = p_glGetUniformLocation(gl_prog, "u_light");
    glu_alpha    = p_glGetUniformLocation(gl_prog, "u_alpha");
    glu_texSize  = p_glGetUniformLocation(gl_prog, "u_texSize");

    static const float quad[] = { -1,-1, 1,-1, -1,1, 1,1 };
    p_glGenVertexArrays(1, &gl_vao); p_glBindVertexArray(gl_vao);
    p_glGenBuffers(1, &gl_vbo); p_glBindBuffer(GL_ARRAY_BUFFER, gl_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);

    GLuint texes[6]; glGenTextures(6, texes);
    gl_vt.y  = texes[0]; gl_vt.u  = texes[1]; gl_vt.v  = texes[2];
    gl_vtB.y = texes[3]; gl_vtB.u = texes[4]; gl_vtB.v = texes[5];
    for(int i=0;i<6;i++){
        glBindTexture(GL_TEXTURE_2D, texes[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    /* ---- OSD UI program (solid-colour 2D triangles for the control bar + text) ---- */
    {
        GLuint uvs = gl_compile(GL_VERTEX_SHADER, UI_VS), ufs = gl_compile(GL_FRAGMENT_SHADER, UI_FS);
        if(uvs && ufs){
            ui_prog = p_glCreateProgram();
            p_glAttachShader(ui_prog, uvs); p_glAttachShader(ui_prog, ufs);
            p_glLinkProgram(ui_prog);
            GLint ok2=0; p_glGetProgramiv(ui_prog, GL_LINK_STATUS, &ok2);
            if(ok2){
                p_glDeleteShader(uvs); p_glDeleteShader(ufs);
                uiu_screen = p_glGetUniformLocation(ui_prog, "u_screen");
                uiu_tex    = p_glGetUniformLocation(ui_prog, "u_tex");
                p_glGenVertexArrays(1,&ui_vao); p_glBindVertexArray(ui_vao);
                p_glGenBuffers(1,&ui_vbo); p_glBindBuffer(GL_ARRAY_BUFFER, ui_vbo);
                /* vertex = pos(2) uv(2) col(4) = 8 floats */
                p_glEnableVertexAttribArray(0); p_glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)0);
                p_glEnableVertexAttribArray(1); p_glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(2*sizeof(float)));
                p_glEnableVertexAttribArray(2); p_glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(4*sizeof(float)));
                font_init();   /* load font + create glyph atlas */
            } else { ui_prog = 0; } /* OSD disabled; keyboard still works */
        }
    }
    return 1;
}

/* upload the decoded AVFrame into a video texture slot.
   FAST PATH: 4:2:0 YUV (NV12/P010 from the GPU decoder, yuv420p from software decode) uploads
   the raw planes and lets the fragment shader do YUV→RGB. This is what makes enhanced
   1440p/4K60 files hold full rate: the old per-frame sws_scale to RGBA (8-20ms CPU) + the
   4-byte-per-pixel upload blew the 16.6ms vsync budget, so the pump silently showed every
   other frame (looked ~30fps) while the decode counter still claimed 60.
   Anything else falls back to the old sws→RGBA path. */
static void gl_upload(VidTex *vt, AVFrame *fr){
    int w = fr->width, h = fr->height;
    DBG_ONCE(2,"[dbg] upload enter %dx%d fmt=%d\n", w, h, fr->format);
    if(w<=0 || h<=0) return;                 /* guard against a 0-dim frame */
    enum AVPixelFormat f = (enum AVPixelFormat)fr->format;
    int p010 = (f==AV_PIX_FMT_P010LE);
    int mode = (f==AV_PIX_FMT_NV12 || p010) ? 1
             : (f==AV_PIX_FMT_YUV420P || f==AV_PIX_FMT_YUVJ420P) ? 2 : 0;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if(mode){
        int cw=(w+1)/2, ch=(h+1)/2;
        GLenum ifY = p010?GL_R16:GL_R8, ifC = (mode==1)?(p010?GL_RG16:GL_RG8):ifY;
        GLenum fmC = (mode==1)?GL_RG:GL_RED;
        GLenum typ = p010?GL_UNSIGNED_SHORT:GL_UNSIGNED_BYTE;
        int bppY = p010?2:1, bppC = bppY*((mode==1)?2:1);
        if(gl_tex_w!=w || gl_tex_h!=h || vt_mode!=mode){
            /* (re)size the planes of BOTH slots so A and B always match */
            VidTex *both[2]={&gl_vt,&gl_vtB};
            for(int i=0;i<2;i++){
                glBindTexture(GL_TEXTURE_2D, both[i]->y); glTexImage2D(GL_TEXTURE_2D,0,ifY,w,h,0,GL_RED,typ,NULL);
                glBindTexture(GL_TEXTURE_2D, both[i]->u); glTexImage2D(GL_TEXTURE_2D,0,ifC,cw,ch,0,fmC,typ,NULL);
                if(mode==2){ glBindTexture(GL_TEXTURE_2D, both[i]->v); glTexImage2D(GL_TEXTURE_2D,0,ifY,cw,ch,0,GL_RED,typ,NULL); }
            }
            gl_tex_w=w; gl_tex_h=h; vt_mode=mode;
        }
        /* ASYNC UPLOAD via an orphaned pixel-unpack buffer (2026-10): glTexSubImage2D from client memory
           is synchronous — MEASURED 11ms for one 6912x3456 frame, which alone capped 6K60 at ~50fps.
           Copy the planes into a freshly-orphaned PBO (one sequential memcpy per plane into write-combined
           memory) and let the driver DMA it to the textures asynchronously. Falls back to the direct path
           if the buffer can't be mapped. */
        int planes = (mode==2) ? 3 : 2;
        size_t off[3]={0,0,0}, sz[3]={0,0,0}, tot=0;
        for(int pl=0; pl<planes; pl++){
            int rows = pl==0 ? h : ch;
            if(fr->linesize[pl] <= 0){ tot=0; break; }          /* negative stride: use the direct path */
            off[pl] = tot; sz[pl] = (size_t)fr->linesize[pl]*rows; tot += (sz[pl]+63)&~(size_t)63;
        }
        static GLuint pbo = 0; static int pbo_ok = 1;
        unsigned char *map = NULL;
        if(tot && pbo_ok && p_glMapBufferRange && p_glUnmapBuffer){
            if(!pbo) p_glGenBuffers(1,&pbo);
            p_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
            p_glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)tot, NULL, GL_STREAM_DRAW);   /* orphan: no stall */
            map = (unsigned char*)p_glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, (GLsizeiptr)tot,
                                                     GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_BUFFER_BIT);
            if(!map){ p_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0); pbo_ok = 0;
                      fprintf(stderr,"[gl] PBO map failed — direct texture uploads\n"); }
        }
        if(map){
            for(int pl=0; pl<planes; pl++) memcpy(map+off[pl], fr->data[pl], sz[pl]);
            p_glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
            glBindTexture(GL_TEXTURE_2D, vt->y);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, fr->linesize[0]/bppY);
            glTexSubImage2D(GL_TEXTURE_2D,0,0,0,w,h,GL_RED,typ,(const void*)(uintptr_t)off[0]);
            glBindTexture(GL_TEXTURE_2D, vt->u);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, fr->linesize[1]/bppC);
            glTexSubImage2D(GL_TEXTURE_2D,0,0,0,cw,ch,fmC,typ,(const void*)(uintptr_t)off[1]);
            if(mode==2){
                glBindTexture(GL_TEXTURE_2D, vt->v);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, fr->linesize[2]/bppY);
                glTexSubImage2D(GL_TEXTURE_2D,0,0,0,cw,ch,GL_RED,typ,(const void*)(uintptr_t)off[2]);
            }
            p_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);           /* never leave it bound: atlas uploads use client memory */
        } else {
        glBindTexture(GL_TEXTURE_2D, vt->y);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, fr->linesize[0]/bppY);
        glTexSubImage2D(GL_TEXTURE_2D,0,0,0,w,h,GL_RED,typ,fr->data[0]);
        glBindTexture(GL_TEXTURE_2D, vt->u);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, fr->linesize[1]/bppC);
        glTexSubImage2D(GL_TEXTURE_2D,0,0,0,cw,ch,fmC,typ,fr->data[1]);
        if(mode==2){
            glBindTexture(GL_TEXTURE_2D, vt->v);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, fr->linesize[2]/bppY);
            glTexSubImage2D(GL_TEXTURE_2D,0,0,0,cw,ch,GL_RED,typ,fr->data[2]);
        }
        }
        vt_fullrange = (fr->color_range==AVCOL_RANGE_JPEG || f==AV_PIX_FMT_YUVJ420P);
        vt_bt709 = (fr->colorspace==AVCOL_SPC_BT709) ||
                   (fr->colorspace!=AVCOL_SPC_BT470BG && fr->colorspace!=AVCOL_SPC_SMPTE170M && h>576);
        if(fr->colorspace==AVCOL_SPC_BT2020_NCL || fr->colorspace==AVCOL_SPC_BT2020_CL) vt_bt709 = 2;
        /* HDR transfer: PQ (HDR10 / Dolby Vision's HDR10 base layer) or HLG. A 10-bit BT.2020 stream with no
           transfer tag is almost always PQ in practice — treat it so rather than show it washed out. */
        vt_hdr = (fr->color_trc==AVCOL_TRC_SMPTE2084) ? 1 : (fr->color_trc==AVCOL_TRC_ARIB_STD_B67) ? 2 : 0;
        if(!vt_hdr && p010 && fr->color_primaries==AVCOL_PRI_BT2020 && fr->color_trc==AVCOL_TRC_UNSPECIFIED) vt_hdr = 1;
        if(vt_hdr && vt_bt709 != 2) vt_bt709 = 2;   /* HDR10/HLG are BT.2020 even if the matrix tag is missing */
        { static int last=-1; if(vt_hdr!=last){ last=vt_hdr;
            if(vt_hdr) fprintf(stderr,"[video] HDR %s detected — tone-mapping to SDR (BT.2020->709, ref white 203 nits)\n", vt_hdr==1?"PQ (HDR10/DV)":"HLG"); } }
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        DBG_ONCE(4,"[dbg] tex upload ok (yuv mode=%d)\n", mode);
        return;
    }
    P.sws = sws_getCachedContext(P.sws, w, h, fr->format, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
    if(!P.sws){ DBG_ONCE(8,"[dbg] sws_getCachedContext FAILED for fmt=%d\n", fr->format); return; }
    /* CRITICAL: sws_scale writes up to FFALIGN(width,16) pixels per row, so a tight
       malloc(w*h*4) OVERSHOOTS and corrupts the heap for any width not a multiple of 16
       (e.g. a 572-wide DivX). Use a 32-px-aligned row stride + padded buffer, and tell GL
       the real row length. THIS was the "random crash on certain videos" bug. */
    int aligned_w = (w + 31) & ~31;          /* 32-pixel aligned row */
    int stride = aligned_w * 4;
    if(gl_rgba_w != w || gl_rgba_h != h){ free(gl_rgba); gl_rgba = malloc((size_t)stride*h + 64); gl_rgba_w=w; gl_rgba_h=h; }
    if(!gl_rgba){ gl_rgba_w=gl_rgba_h=0; return; }
    uint8_t *dst[1] = { gl_rgba }; int dstl[1] = { stride };
    sws_scale(P.sws, (const uint8_t* const*)fr->data, fr->linesize, 0, h, dst, dstl);
    DBG_ONCE(3,"[dbg] sws ok\n");
    glBindTexture(GL_TEXTURE_2D, vt->y);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, aligned_w);   /* rows are aligned_w px wide in memory */
    if(gl_tex_w != w || gl_tex_h != h || vt_mode != 0){
        /* (re)size BOTH slots so A and B always match */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, gl_rgba);
        GLuint other = (vt==&gl_vt)?gl_vtB.y:gl_vt.y;
        glBindTexture(GL_TEXTURE_2D, other); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        gl_tex_w=w; gl_tex_h=h; vt_mode=0;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, gl_rgba);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);           /* reset so glyph-atlas uploads stay tight */
    DBG_ONCE(4,"[dbg] tex upload ok\n");
}

/* draw the current frame (pr_vt), aspect-fit into the window (letterbox or fill) */
static void gl_present(double alpha){
    DBG_ONCE(5,"[dbg] present enter tex=%dx%d\n", gl_tex_w, gl_tex_h);
    int ww, wh; SDL_GL_GetDrawableSize(P.win, &ww, &wh);
    int w = gl_tex_w, h = gl_tex_h; if(w<=0||h<=0) return;
    double ar=(double)w*g_sar/h, war=(double)ww/wh; int dw,dh;   /* display aspect = pixel aspect x storage */
    if(!P.fill){   /* letterbox (contain): fit inside, black bars */
        if(war>ar){ dh=wh; dw=(int)(wh*ar); } else { dw=ww; dh=(int)(ww/ar); }
    } else {       /* fill (cover): scale to fill, crop the overflow (clipped by viewport) */
        if(war>ar){ dw=ww; dh=(int)(ww/ar); } else { dh=wh; dw=(int)(wh*ar); }
    }
    int dx=(ww-dw)/2, dy=(wh-dh)/2;
    g_out_w=dw; g_out_h=dh;
    glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
    glViewport(dx, dy, dw, dh);
    p_glUseProgram(gl_prog);
    p_glBindVertexArray(gl_vao);
    VidTex *vt = pr_vt.y ? &pr_vt : &gl_vt;
    p_glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, vt->y);
    p_glActiveTexture(GL_TEXTURE0+1); glBindTexture(GL_TEXTURE_2D, vt->u);
    p_glActiveTexture(GL_TEXTURE0+2); glBindTexture(GL_TEXTURE_2D, vt->v);
    p_glActiveTexture(GL_TEXTURE0);
    p_glUniform1i(glu_tex, 0); p_glUniform1i(glu_texU, 1); p_glUniform1i(glu_texV, 2);
    p_glUniform1i(glu_mode, vt_mode); p_glUniform1i(glu_range, vt_fullrange); p_glUniform1i(glu_bt709, vt_bt709); p_glUniform1i(glu_hdr, vt_hdr);
    /* LIGHT ENHANCEMENT: bind the NEXT frame (B) on units 3-5 and enable the v141 shader trio
       (Lanczos + blend frame-gen + √2 DELTA gate). When only one frame exists, B = A (trio idles). */
    VidTex *vb = (gl_nframes >= 2 && vt->y == gl_vt.y) ? &gl_vtB : vt;   /* showing A → next is B */
    p_glActiveTexture(GL_TEXTURE0+3); glBindTexture(GL_TEXTURE_2D, vb->y);
    p_glActiveTexture(GL_TEXTURE0+4); glBindTexture(GL_TEXTURE_2D, vb->u);
    p_glActiveTexture(GL_TEXTURE0+5); glBindTexture(GL_TEXTURE_2D, vb->v);
    p_glActiveTexture(GL_TEXTURE0);
    p_glUniform1i(glu_texB, 3); p_glUniform1i(glu_texUB, 4); p_glUniform1i(glu_texVB, 5);
    p_glUniform1i(glu_light, P.light ? 1 : 0);
    p_glUniform1f(glu_alpha, (float)((vb==vt)?0.0:alpha));
    p_glUniform2f(glu_texSize, (float)w, (float)h);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    /* NB: no SwapWindow here — the main loop draws the OSD on top, then swaps once. */
}

static int g_vdrained = 0;   /* sent the end-of-stream flush packet? (reset on seek/new file) */
/* HOGS_PROF=1: per-stage video cost (ms) — decode-wait / GPU->CPU transfer / GL upload — logged 1x/s */
static int g_pf_starve=0, g_pf_rep=0; static double g_pf_dclk=0, g_pf_dmax=0, g_pf_dmin=1e9;
static int g_prof = -1; static double g_pf_dec=0,g_pf_xfer=0,g_pf_up=0; static int g_pf_nd=0,g_pf_nx=0,g_pf_nu=0, g_pf_skip=0;
static double prof_now(void){ return av_gettime()/1000.0; }
static int g_pts_viol = 0;   /* consecutive backward-video-pts strikes (broken-container detector) */
static long g_cu_dropped = 0;  /* catch-up frames decoded-and-dropped (never presented) */
static double g_clk_mono = -1e18;  /* monotonic master clock (reset on seek/new file) */
static double g_ui_clk = -1e18;    /* smoothed master, for UI only (bar/title/subs) */
/* Monotonic, rate-limited follower for the SEEK BAR + time text ONLY. g_ui_clk still re-anchors to
   each chunky audio update (snap forward on undershoot, backward on overshoot); invisible on long
   files, visible pulsing on short ones where the bar moves fast. This advances at ~1.0x wall and
   corrects toward truth at low gain — never reverses, never spikes. Does NOT feed pacing, the master
   clock, or subtitle timing. Reset by the large-jump test in its updater (covers seek + new file). */
static double g_bar_clk = -1e18;
/* Volume ceiling. 1.0 = the file's own level; 2.0 doubles the sample amplitude. Above 1.0 the gain
   is a real boost, so quiet sources become audible but already-loud ones clip — see audio_apply_gain. */
#define VOL_MAX 2.0
/* stopped = paused AND rewound to 0. Distinct from paused so the transport buttons can show which
   of the two the user actually asked for. Cleared by any other transport action (play/pause/seek). */
static int g_stopped = 0;
/* Stop pauses, so the pump (which only runs unpaused) never decodes the frame we rewound to and the
   screen keeps the frame we stopped ON. This asks the main loop to pump exactly once, after the
   demux thread has landed the seek, so frame 0 is decoded and presented. */
static int g_stop_step = 0, g_stop_tries = 0;
/* ================= VIDEO DECODE THREAD (GL path) — 2026-10 =================
   MEASURED (HOGS_PROF) before this existed, everything ran on the render thread, per frame:
     4K H.264 @120fps : decode 0.6ms + GPU->CPU transfer 4.7ms + GL upload 4.6ms
     6K HEVC  @60fps  : decode 1.3ms + transfer 12.8ms + upload 8.1ms
   On a 60 Hz display a 120 fps file crosses TWO frames per refresh, and the pump transferred AND
   uploaded both (the first is superseded before it is ever shown) — ~20ms of work per 16.7ms refresh,
   so the loop slowed, even more frames piled up per pass, and it spiralled down to ~9 presented fps
   with video drifting a second behind audio. 6K60 couldn't fit transfer+upload in one refresh at all
   (~3 fps). Now: this thread owns the decoder, decodes AND does the GPU->CPU transfer into a small ring
   of ready CPU frames in parallel with rendering; the pump only uploads frames that will really be
   shown (see the supersede-skip in gl_video_pump). The thread is the ONLY toucher of P.vdec while it
   runs, and it consumes P.flush_video (seek) itself, bumping g_vd.gen so the pump resets its A/B state. */
#define VD_RING 4
#define VD_FREE 8
typedef struct {
    SDL_Thread *th; SDL_mutex *mx;
    int run;                         /* thread started for the current file */
    volatile int stop;               /* ask the thread to exit */
    volatile int gen;                /* bumped each time the thread flushes the decoder (a seek landed) */
    volatile int eof;                /* decoder fully drained for this gen (no more frames) */
    AVFrame *ring[VD_RING]; int head, n;   /* FIFO of ready frames, oldest at head */
    AVFrame *freel[VD_FREE]; int nfree;    /* recycled frames (their CPU buffers are reused) */
} VidDec;
static VidDec g_vd;
static int g_vd_seen_gen = 0;
/* LAZY UPLOAD: with the decode thread on, a texture slot's content may be a PENDING CPU frame that is
   uploaded only if/when that slot is actually presented. A frame promoted through B and superseded in
   the same refresh is recycled without ever costing an upload (120fps on 60Hz = every other frame). */
static AVFrame *gl_pendA = NULL, *gl_pendB = NULL;
static AVFrame *vd_take_free(void){            /* caller holds g_vd.mx */
    if(g_vd.nfree>0) return g_vd.freel[--g_vd.nfree];
    return av_frame_alloc();
}
static void vd_give_free(AVFrame *f){          /* caller holds g_vd.mx */
    if(!f) return;
    if(g_vd.nfree<VD_FREE) g_vd.freel[g_vd.nfree++]=f; else av_frame_free(&f);
}
static int vd_thread(void *arg){
    (void)arg;
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);   /* playback outranks UI work (2026-10-08) */
    AVFrame *dec = av_frame_alloc(); AVPacket *pkt = av_packet_alloc();
    int drained = 0;
    /* SEEK FAST-FORWARD: a seek lands on the KEYFRAME before the target (HEVC/long-GOP: up to ~3s early).
       Frames before the target are never shown, so skip their GPU->CPU transfer and don't queue them —
       the catch-up then runs at raw hardware-decode speed instead of the picture freezing (old) or visibly
       fast-forwarding to the target. Bounded so broken timestamps can never skip forever. */
    double skip_until = -1; int skip_left = 0;
    while(!g_vd.stop){
        if(P.flush_video){                     /* demux flushed the packet queue for a seek */
            avcodec_flush_buffers(P.vdec); P.flush_video = 0; drained = 0;
            { double tgt; SDL_LockMutex(P.clock_mutex); tgt = P.audio_clock; SDL_UnlockMutex(P.clock_mutex);
              skip_until = (tgt > 0.5 && !P.aclk_creep) ? tgt - 0.04 : -1; skip_left = 1200; }
            SDL_LockMutex(g_vd.mx);
            while(g_vd.n>0){ vd_give_free(g_vd.ring[g_vd.head]); g_vd.ring[g_vd.head]=NULL; g_vd.head=(g_vd.head+1)%VD_RING; g_vd.n--; }
            g_vd.head=0; g_vd.eof=0; g_vd.gen++;
            SDL_UnlockMutex(g_vd.mx);
            continue;
        }
        SDL_LockMutex(g_vd.mx); int full = (g_vd.n >= VD_RING); SDL_UnlockMutex(g_vd.mx);
        if(full){ SDL_Delay(1); continue; }
        double t_dec0 = g_prof>0 ? prof_now() : 0;
        int r = avcodec_receive_frame(P.vdec, dec);
        if(g_prof>0) g_pf_dec += prof_now()-t_dec0;
        if(r == 0){
            if(g_prof>0) g_pf_nd++;
            DBG_ONCE(1,"[dbg] first vframe decoded %dx%d fmt=%d\n", dec->width, dec->height, dec->format);
            if(skip_until > 0 && skip_left-- > 0 && P.vstream >= 0 && dec->best_effort_timestamp != AV_NOPTS_VALUE){
                double fpts = dec->best_effort_timestamp * av_q2d(P.fmt->streams[P.vstream]->time_base);
                if(fpts < skip_until){ av_frame_unref(dec); g_cu_dropped++; continue; }   /* before the target */
            }
            skip_until = -1;                     /* reached the target (or gave up): deliver normally */
            SDL_LockMutex(g_vd.mx); AVFrame *out = vd_take_free(); SDL_UnlockMutex(g_vd.mx);
            if(!out){ av_frame_unref(dec); continue; }
            if(g_hw_pixfmt != AV_PIX_FMT_NONE && dec->format == g_hw_pixfmt && dec->hw_frames_ctx){
                enum AVPixelFormat swf = ((AVHWFramesContext*)dec->hw_frames_ctx->data)->sw_format;
                if(!out->buf[0] || !av_buffer_is_writable(out->buf[0]) ||
                   out->format!=swf || out->width!=dec->width || out->height!=dec->height){
                    av_frame_unref(out); out->format=swf; out->width=dec->width; out->height=dec->height;
                    if(av_frame_get_buffer(out, 64) < 0){ av_frame_unref(out); }   /* fall back: let transfer allocate */
                }
                double t_x0 = g_prof>0 ? prof_now() : 0;
                int xr = av_hwframe_transfer_data(out, dec, 0);
                if(g_prof>0){ g_pf_xfer += prof_now()-t_x0; g_pf_nx++; }
                if(xr < 0){ DBG_ONCE(9,"[hw] frame transfer failed\n"); av_frame_unref(dec);
                    SDL_LockMutex(g_vd.mx); vd_give_free(out); SDL_UnlockMutex(g_vd.mx); continue; }
                av_frame_copy_props(out, dec);           /* pts / best_effort_timestamp / colour info */
                av_frame_unref(dec);
            } else {
                av_frame_unref(out); av_frame_move_ref(out, dec);   /* software frame: just hand it over */
            }
            SDL_LockMutex(g_vd.mx);
            g_vd.ring[(g_vd.head+g_vd.n)%VD_RING] = out; g_vd.n++;
            SDL_UnlockMutex(g_vd.mx);
            continue;
        }
        if(r == AVERROR(EAGAIN)){
            if(pq_get_nb(&P.vq, pkt)){
                double t_s0 = g_prof>0 ? prof_now() : 0;
                avcodec_send_packet(P.vdec, pkt); av_packet_unref(pkt); drained = 0;
                if(g_prof>0) g_pf_dec += prof_now()-t_s0;
                continue;
            }
            /* at EOF flush the decoder ONCE so frame-threaded decoders (png/webp/tiff/last frames)
               emit what they buffered */
            if(P.eof && !drained){ avcodec_send_packet(P.vdec, NULL); drained = 1; continue; }
            SDL_Delay(1); continue;
        }
        if(r == AVERROR_EOF){ g_vd.eof = 1; SDL_Delay(2); continue; }
        SDL_Delay(1);                          /* transient decode error: keep going */
    }
    av_frame_free(&dec); av_packet_free(&pkt);
    return 0;
}
static void vd_start(void){
    if(!use_gl || !P.vdec || g_vd.th) return;
    if(!g_vd.mx) g_vd.mx = SDL_CreateMutex();
    g_vd.stop = 0; g_vd.eof = 0; g_vd.head = g_vd.n = 0;
    g_vd_seen_gen = g_vd.gen;
    g_vd.th = SDL_CreateThread(vd_thread, "vdecode", NULL);
    g_vd.run = (g_vd.th != NULL);
    if(g_vd.run) fprintf(stderr,"[video] decode thread on (decode + GPU->CPU transfer off the render thread)\n");
}
static void vd_stop(void){                     /* must run BEFORE P.vdec is freed */
    if(g_vd.th){ g_vd.stop = 1; SDL_WaitThread(g_vd.th, NULL); g_vd.th = NULL; }
    if(g_vd.mx) SDL_LockMutex(g_vd.mx);
    av_frame_free(&gl_pendA); av_frame_free(&gl_pendB);
    while(g_vd.n>0){ av_frame_free(&g_vd.ring[g_vd.head]); g_vd.head=(g_vd.head+1)%VD_RING; g_vd.n--; }
    while(g_vd.nfree>0) av_frame_free(&g_vd.freel[--g_vd.nfree]);
    g_vd.head = 0; g_vd.run = 0; g_vd.stop = 0; g_vd.eof = 0;
    if(g_vd.mx) SDL_UnlockMutex(g_vd.mx);
}
/* main thread: swap *into with the oldest ready frame (its old frame is recycled). 0 if none ready. */
static int vd_pop(AVFrame **into){
    SDL_LockMutex(g_vd.mx);
    if(g_vd.n==0){ SDL_UnlockMutex(g_vd.mx); return 0; }
    AVFrame *f = g_vd.ring[g_vd.head]; g_vd.ring[g_vd.head]=NULL;
    g_vd.head=(g_vd.head+1)%VD_RING; g_vd.n--;
    vd_give_free(*into); *into = f;
    SDL_UnlockMutex(g_vd.mx);
    return 1;
}
/* main thread: timestamp of the next ready frame without taking it */
static int vd_peek_bts(int64_t *bts){
    SDL_LockMutex(g_vd.mx);
    int ok = g_vd.n>0;
    if(ok) *bts = g_vd.ring[g_vd.head]->best_effort_timestamp;
    SDL_UnlockMutex(g_vd.mx);
    return ok;
}
static void vd_recycle(AVFrame **f){
    if(!*f) return;
    if(g_vd.mx){ SDL_LockMutex(g_vd.mx); vd_give_free(*f); SDL_UnlockMutex(g_vd.mx); } else av_frame_free(f);
    *f = NULL;
}
static void gl_pend_reset(void){ vd_recycle(&gl_pendA); vd_recycle(&gl_pendB); }
/* decode exactly one video frame into vframe; 1 if got one, 0 if none available now */
static int gl_decode_one(void){
    if(g_vd.run){                              /* threaded path: just take the next ready frame */
        if(P.flush_video) return 0;            /* a seek is pending: never show pre-seek frames */
        return vd_pop(&vframe);
    }
    if(!vframe && !(vframe = av_frame_alloc())) return 0;   /* lazy path handed the last one off */
    static AVFrame *sw_frame = NULL;   /* receives the GPU→CPU transfer for hw-decoded frames */
    for(;;){
        double t_dec0 = g_prof>0 ? prof_now() : 0;
        int r = avcodec_receive_frame(P.vdec, vframe);
        if(g_prof>0){ g_pf_dec += prof_now()-t_dec0; }
        if(r == 0){
            if(g_prof>0) g_pf_nd++;
            DBG_ONCE(1,"[dbg] first vframe decoded %dx%d fmt=%d\n", vframe->width, vframe->height, vframe->format);
            /* hardware frame lives in GPU memory → copy it to system RAM so the existing
               sws→GL upload can use it (cheap vs. the software decode it replaces). */
            if(g_hw_pixfmt != AV_PIX_FMT_NONE && vframe->format == g_hw_pixfmt){
                if(!sw_frame) sw_frame = av_frame_alloc();
                av_frame_unref(sw_frame);
                double t_x0 = g_prof>0 ? prof_now() : 0;
                int xr = av_hwframe_transfer_data(sw_frame, vframe, 0);
                if(g_prof>0){ g_pf_xfer += prof_now()-t_x0; g_pf_nx++; }
                if(xr < 0){
                    DBG_ONCE(9,"[hw] frame transfer failed\n"); av_frame_unref(vframe); return 0;
                }
                av_frame_copy_props(sw_frame, vframe);   /* keep pts / best_effort_timestamp */
                av_frame_unref(vframe);                  /* release the GPU surface */
                AVFrame *t = vframe; vframe = sw_frame; sw_frame = t;   /* vframe now = CPU copy */
            }
            return 1;
        }
        if(r == AVERROR(EAGAIN)){
            AVPacket *pkt = av_packet_alloc();
            int got = pq_get_nb(&P.vq, pkt);
            if(got){ double t_s0 = g_prof>0 ? prof_now() : 0; avcodec_send_packet(P.vdec, pkt); if(g_prof>0) g_pf_dec += prof_now()-t_s0; av_packet_unref(pkt); av_packet_free(&pkt); g_vdrained=0; continue; }
            av_packet_free(&pkt);
            /* queue empty: at EOF, flush the decoder ONCE (NULL packet) so frame-threaded
               decoders (png/webp/tiff) emit their buffered single frame instead of showing black. */
            if(P.eof && !g_vdrained){ avcodec_send_packet(P.vdec, NULL); g_vdrained=1; continue; }
            return 0;
        }
        return 0;   /* AVERROR_EOF or other → nothing more to give */
    }
}

/* OpenGL video path: decode one frame ahead, render at refresh, interpolate (frame-gen) */
static void gl_video_pump(void){
    DBG_ONCE(0,"[dbg] gl_video_pump enter (vstream=%d)\n", P.vstream);
    int seek_reset = 0;
    if(g_vd.run){ if(g_vd.gen != g_vd_seen_gen){ g_vd_seen_gen = g_vd.gen; seek_reset = 1; } }   /* thread flushed */
    else if(P.flush_video){ if(P.vdec) avcodec_flush_buffers(P.vdec); P.flush_video=0; seek_reset = 1; }
    if(seek_reset){ gl_nframes=0; gl_ptsA=gl_ptsB=-1; g_vdrained=0; gl_pend_reset();
        /* a SEEK just happened: re-arm the audio-stall watchdog. Without this, one slow seek (>0.7s
           audio refill) latched audio_stalled FOREVER: the pump then overwrote the clock from a stale
           frame_timer every frame while the audio callback fought it -> the seek target snapped back
           and playback glitched with ruined pacing until the next file. */
        P.audio_stalled = 0; P.aclk_val = -1; P.aclk_t = 0; P.flushed_rs = 1; P.aclk_creep = 0; g_pts_viol = 0; P.aclk_probe = 0; g_clk_mono = -1e18; g_ui_clk = -1e18; }
    if(P.vstream < 0){ glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT); return; } /* audio-only: black; main swaps */

    /* RESUME RE-ARM (2026-10-08, "audio out of sync after a long pause"). The pump does not run while
       paused, so on resume both watchdogs below compared the clock against wall time from BEFORE the
       pause: the creep check saw ~1s of clock progress over e.g. 60s of "wall" span, called it a
       CREEPING (broken-timestamp) clock, rewound the video up to 1.5s and latched wall pacing + 1/fps
       cadence for the rest of the file — audio ahead of video until the next file. Whether it fired
       depended on whether the audio callback or the pump ran first after resume, hence "sometimes".
       Any gap in pumping (pause, a modal window drag, a stalled main thread) now re-arms them exactly
       as a seek does, and resets the clock smoother's wall anchor. */
    { static double last_pump = 0; double nw = av_gettime()/1000000.0;
      if(last_pump > 0 && nw - last_pump > 0.5){
          P.aclk_val = -1; P.aclk_t = 0; P.flushed_rs = 1; g_clk_mono = -1e18;
          if(getenv("HOGS_FPSLOG")) fprintf(stderr,"[sync] %.2fs gap in video pumping (pause?) — watchdogs re-armed\n", nw-last_pump); }
      last_pump = nw; }
    double clk; SDL_LockMutex(P.clock_mutex); clk = P.audio_clock; SDL_UnlockMutex(P.clock_mutex);
    /* audio-stall watchdog: an audio track that opens but never DECODES (e.g. some WMVs whose wmav2 is
       corrupt) leaves the master clock pinned, so the video-pacing gate never opens and the picture
       FREEZES. If the clock hasn't advanced for >0.7s while playing, fall back to wall-clock pacing.
       SELF-HEALING: while stalled, aclk_val is the last value WE wrote — if the observed clock differs,
       the audio callback is decoding again, so hand the master clock back to the audio. */
    /* un-latch only when the callback's clock lands NEAR the wall position (healthy audio that
       re-synced — after a transient refill its packet pts continue at the demux position ≈ wall).
       A CRAWLING wmav2 clock (writes 1.7s while wall is at 9s) must NOT un-latch: that flip-flop
       was the ".wmv plays very weird" bug — video paced by a creeping clock. */
    /* self-heal: the callback publishes aclk_probe silently while we're wall-pacing. If it starts
       tracking the wall clock again (healthy audio, e.g. after a slow refill), hand the master clock
       back. A CREEP-latched file (broken pts) never recovers — its probe would drag us backwards. */
    if(P.astream >= 0 && P.audio_stalled && !P.aclk_creep &&
       P.aclk_probe > 0 && fabs(P.aclk_probe - clk) < 0.5){
        P.audio_stalled = 0; P.aclk_t = 0; P.aclk_val = P.aclk_probe;
        SDL_LockMutex(P.clock_mutex); P.audio_clock = P.aclk_probe; SDL_UnlockMutex(P.clock_mutex);
        fprintf(stderr,"[audio] clock recovered — audio-master pacing resumed\n");
    }
    /* LIVE render-head: playback caught up to the still-growing enhanced file. The audio underrun
       here is EXPECTED (waiting for the enhancer to append more) — NOT a broken audio track — so the
       stall watchdog must NOT fall back to wall-clock, which would keep the time/seek position (and
       audio) advancing while the picture is frozen. Freeze the master clock AT the head instead;
       tail-follow resumes it in sync when more frames arrive. (Phase-6 fast enhance made reaching the
       head common, which exposed this.) */
    int at_live_head = (g_live_playing && g_live_rendered > 0.5 && clk >= g_live_rendered - 0.3);
    /* during LIVE playback the wall-clock fallback must NEVER arm: any audio stall is the render
       head (or a demux wedge being recovered), not a broken track. Wall-clock here = the "time
       advances while the picture is frozen" bug. Freeze-and-wait is always correct live. */
    if(P.astream >= 0 && !P.audio_stalled && !P.paused && !at_live_head && !g_live_playing){
        double now = av_gettime()/1000000.0;
        /* AUDIO CATCHING UP AFTER A BACKWARD SEEK IS NOT A STALL.
           av_seek_frame lands on the keyframe BEFORE the target, so the first audio packets decode
           from before it. decode_audio_frame refuses to drag the master clock backwards, so
           audio_clock sits PINNED at the seek target until audio climbs back past it — which on a
           long-GOP file is many seconds (the log showed 102 catch-up frames). The 0.7s freeze
           detector below read that as a broken track and latched WALL pacing: video then free-ran
           against real time while audio played at its own rate, so they drifted apart and never
           re-synced. Each seek re-armed it, so the error compounded.
           `aclk_probe` is published by the callback on EVERY decoded frame, monotonic guard or not.
           A moving probe means audio is alive — the master clock is merely waiting for it. A truly
           undecodable track (the wmav2 case this watchdog exists for) never moves the probe, so it
           still latches exactly as before. */
        static double sw_probe = -1e18;
        int audio_alive = (P.aclk_probe != sw_probe);
        if(audio_alive) sw_probe = P.aclk_probe;
        int audio_behind = (P.aclk_probe > 0 && P.aclk_probe < clk - 0.05);  /* still climbing to the target */

        if(clk != P.aclk_val){ P.aclk_val = clk; P.aclk_t = now; }
        else if(audio_alive || audio_behind){ P.aclk_t = now; }   /* decoding, just not past the anchor yet */
        else if(P.aclk_t > 0 && now - P.aclk_t > 0.7){
            P.audio_stalled = 1; P.frame_timer = now - clk;      /* seed the wall clock at the stuck position */
            fprintf(stderr,"[audio] clock stalled (undecodable audio?) — pacing video off wall clock\n");
        }
        /* CREEP watchdog (the ".wmv very weird" bug): some wmav2 tracks decode just often enough to
           keep resetting the freeze detector above, yet the clock advances at a tiny fraction of
           realtime — video then crawls (0.16s of playback in 9s of wall time, "STUTTERING" at full
           decode rate). If the clock gains <30% of wall time over a 1.5s window, treat it exactly
           like a stall and pace off the wall clock. */
        {
            static double rs_c0=-1e18, rs_w0=0;
            /* a SEEK legitimately jumps the clock (and it keeps settling for a moment while the audio
               device refills). Reset the baseline AND hold the watchdog off for a grace period —
               otherwise the jump reads as CREEP/RACE and wrongly latches wall pacing on healthy files.
               NB: this used to be TWO `if(P.flushed_rs)` blocks; the first cleared the flag, so the
               second never ran and rs_grace was never armed. One block, one consumer. */
            static double rs_grace = 0;
            if(P.flushed_rs){ rs_c0=-1e18; P.flushed_rs=0; rs_grace = now + 2.5; }
            /* and while audio is still climbing back to the seek target the clock is legitimately
               pinned — net progress is 0, which looks exactly like CREEP. Hold the baseline. */
            if(now < rs_grace || audio_behind){ rs_c0=-1e18; }
            else if(rs_c0<-1e17){ rs_c0=clk; rs_w0=now; }
            else if(now - rs_w0 > 1.5){
                /* NET progress over the window. Broken asf/wmv timestamps make the clock CREEP
                   (oscillate/backward: net << wall) or RACE (jump toward the end: net >> wall) —
                   both mean the container's clocks are garbage. Latch wall pacing, REWIND to the
                   window's start position (the last sane point), and ignore container pts from
                   here on (pure 1/fps cadence — see the bts override in the promote loop). */
                double net = clk - rs_c0, span = now - rs_w0;
                if(!P.audio_stalled && (net < 0.3*span || net > 3.0*span)){
                    double base = rs_c0 > 0 ? rs_c0 : 0;
                    P.audio_stalled = 1; P.aclk_creep = 1;
                    P.frame_timer = now - base; P.aclk_val = base;
                    SDL_LockMutex(P.clock_mutex); P.audio_clock = base; SDL_UnlockMutex(P.clock_mutex);
                    clk = base; gl_ptsA = gl_ptsB = base;   /* restart the cadence AT the clock */
                    fprintf(stderr,"[audio] clock %s (%.2fs in %.2fs) — wall pacing + pts-free cadence from %.2fs\n",
                            net<0.3*span?"CREEPING":"RACING", net, span, base);
                }
                rs_c0=clk; rs_w0=now;
            }
        }
    }
    if(at_live_head){
        /* hold the clock steady at the head: re-seed frame_timer so it neither advances now nor
           JUMPS when rendering catches up and normal pacing resumes. */
        P.frame_timer = (av_gettime()/1000000.0) - clk;
    } else if(P.astream < 0 || P.audio_stalled){ clk = (av_gettime()/1000000.0) - P.frame_timer;
        SDL_LockMutex(P.clock_mutex); P.audio_clock = clk; SDL_UnlockMutex(P.clock_mutex);
        if(P.audio_stalled) P.aclk_val = clk; }              /* remember OUR write for recovery detection */
    /* SMOOTH the chunky audio clock. The callback advances audio_clock one decoded audio frame at
       a time (~43ms at 48kHz), so between updates the clock is FLAT and then JUMPS. At <=25fps one
       video frame crosses each jump — invisible. On 48fps enhanced files TWO frames cross, and the
       catch-up loop below promoted both in one pass and showed only the second: motion looked like
       the ORIGINAL framerate ("stuttering"). Extrapolate along the wall clock between audio updates
       (bounded, re-anchored on every real update) so the gate opens one frame at a time. */
    /* The 0.1s extrapolation cap above was sized for ~43ms audio frames. MEASURED on WMA/asf:
       audio packets advance the clock only every ~0.37s (max 0.64s!) while video runs at 29.97fps.
       The clock therefore plateaued for 0.1s, froze, then JUMPED 0.37s — releasing ~11 frames at
       once, of which only the newest was shown: ~3 distinct frames/sec. That is the ".wmv stutters
       from the start" bug. Size the cap to the file's OWN measured audio-update interval, and clamp
       the result monotonic so the snap-back when a real audio update lands can never rewind the
       gate (which was the "framerate speed pulses" after seeking). */
    if(P.astream >= 0 && !P.audio_stalled && !P.paused){
        static double sc_last=-1e18, sc_wall=0, sc_gap=0.05;
        double nowt = av_gettime()/1000000.0;
        if(g_clk_mono < -1e17){ sc_last = -1e18; }             /* seek/new file: re-anchor from scratch */
        if(sc_last < -1e17){ sc_last = clk; sc_wall = nowt; }
        else if(clk > sc_last + 1e-9){                          /* a REAL, forward audio update */
            double g = nowt - sc_wall;                          /* learn this file's update interval */
            if(g>0.002 && g<1.5) sc_gap = sc_gap*0.75 + g*0.25;
            sc_last = clk; sc_wall = nowt;
        } else {
            /* Audio is FLAT (chunky 0.37s WMA packets) or has REGRESSED. An ASF seek lands the audio
               BEHIND the target, so the callback drags audio_clock back down and it climbs again.
               Freezing the master here (what a plain monotonic clamp does) stalls the video until the
               audio catches up, then dumps the backlog — the "framerate speed pulses after seeking"
               bug. Instead keep the master moving along the WALL clock from the last good anchor. */
            double cap = sc_gap*1.75;
            if(cap < 0.12) cap = 0.12; if(cap > 0.85) cap = 0.85;
            if(sc_last - clk > 0.05) cap = 3.0;                 /* audio behind: coast until it returns */
            double ext = nowt - sc_wall; if(ext > cap) ext = cap;
            clk = sc_last + ext;                                /* projection (monotonic by construction) */
        }
        g_clk_mono = clk;
        /* Publish for the UI ONLY (bar/title/subtitles). Do NOT write it back into P.audio_clock:
           the pump would then read its own projection next frame, mistake it for a real audio update,
           re-anchor every other frame and halve the presented framerate (measured 15fps). The audio
           clock stays pure audio; the callback keeps it monotonic. */
        g_ui_clk = clk;
    }

    /* PRESENTATION PLL (2026-10). MEASURED with HOGS_PROF: even after the smoother above, the master clock
       advances 4..21ms per 16.7ms refresh (audio lands in 1024-sample = 21.3ms chunks). At <=30fps that's
       invisible, but 50/60/120fps content on a 60Hz display then gets refreshes with NO new frame next to
       refreshes that skip one — judder (measured 10-14 repeats/s on 6K60, ~6/s on 4K120). Pace the frame gate
       with a clock that advances by exactly the wall time between refreshes and is steered gently toward
       the audio master (5%/refresh, ~0.3s time constant), snapping on seeks, pauses, new files and any
       error >150ms. Feeds ONLY the gate below — never P.audio_clock / g_ui_clk (see LESSONS: write-back
       halved the framerate once). Off whenever the clock is wall/stall/live-head driven (already smooth). */
    {
        static double pll = -1e18, pll_wall = 0;
        double nowp = av_gettime()/1000000.0;
        int pll_on = (P.astream >= 0 && !P.audio_stalled && !at_live_head && !g_live_playing && !P.aclk_creep);
        double pred = pll + (nowp - pll_wall);
        if(!pll_on || seek_reset || pll < -1e17 || gl_nframes == 0 || fabs(clk - pred) > 0.15) pll = clk;
        else pll = pred + (clk - pred)*0.05;
        pll_wall = nowp;
        if(pll_on) clk = pll;
    }

    AVStream *vs = P.fmt->streams[P.vstream];
    AVRational tb = vs->time_base;
    /* per-frame duration from the stream fps — used to SYNTHESIZE timestamps when a frame has
       none. Some AVIs (old DivX/XviD) decode frames with no PTS; reusing the previous frame's
       time left the clock frozen, so the pacing gate stayed open and the video raced through at
       decode speed ("plays extremely quickly"). Advancing by 1/fps per frame fixes the speed. */
    AVRational fr = (vs->avg_frame_rate.num>0 && vs->avg_frame_rate.den>0) ? vs->avg_frame_rate : vs->r_frame_rate;
    double frame_dur = (fr.num>0 && fr.den>0) ? (double)fr.den/fr.num : 1.0/25.0;
    int guard = 0;
    double cu_deadline = av_gettime()/1000000.0 + 0.025;   /* catch-up time budget (never stall the UI) */
    while(gl_nframes < 2 || gl_ptsB <= clk){
        /* CATCH-UP DROP (the ".wmv weird fast-forward to the seek point" bug): a backward seek lands
           on the previous KEYFRAME, which in WMV3's long GOPs can be many seconds before the target.
           The old loop uploaded and PRESENTED every one of those frames (12 per vsync), so you watched
           it scrub forward to where you clicked. While far behind, decode and DROP: no texture upload,
           no present. Also absorbs decode falling behind during normal playback (drop, don't stutter). */
        /* THRESHOLD MATTERS: at startup the audio device buffers ~0.3s ahead of the first video
           frame, so a SMALL gap is normal and must be absorbed by simply presenting the newest frame.
           Only a LARGE gap is a real catch-up (backward-keyframe seek, or decode fallen badly behind).
           A 0.25s threshold dropped ~1 frame every pump from the moment playback began — the "glitchy
           framerate from the start, fine after stop→play" bug. Require a full second, and a valid A/B
           pair, so the opening frames always display. */
        static int catching = 0;
        if(seek_reset) catching = 0;
        if((gl_nframes >= 2) && (gl_ptsB >= 0) && (clk - gl_ptsB > 1.0)) catching = 1;      /* real catch-up */
        if(catching && gl_ptsB >= 0 && clk - gl_ptsB <= frame_dur*1.5) catching = 0;         /* caught up */
        int behind = catching && (gl_nframes >= 2) && (gl_ptsB >= 0);
        /* (hysteresis: the old rule stopped dropping at 1.0s behind, then PRESENTED that last second at
           refresh rate — a visible fast-forward after every long-GOP seek.) */
        if(guard++ >= (behind ? 2000 : 12)) break;
        if(behind && av_gettime()/1000000.0 > cu_deadline) break;
        if(!gl_decode_one()) break;
        int64_t bts = vframe->best_effort_timestamp;     /* heuristic ts — robust on broken PTS */
        /* DIRECT broken-pts detector (the ".wmv plays very weird" bug): video pts running BACKWARD
           is definitive container garbage (some wmv/asf edits). A few strikes → switch this file to
           wall pacing + pure 1/fps cadence. Direct detection fires within ~5 frames, so it also
           catches SHORT looping clips that reset the rate watchdog before its window closes. */
        if(!P.aclk_creep && bts != AV_NOPTS_VALUE && gl_ptsB >= 0 &&
           bts * av_q2d(tb) < gl_ptsB - 0.001){
            if(++g_pts_viol >= 5){
                double base = gl_ptsB;                   /* last (highest) position seen = sane-ish */
                double now2 = av_gettime()/1000000.0;
                P.audio_stalled = 1; P.aclk_creep = 1;
                P.frame_timer = now2 - base; P.aclk_val = base;
                SDL_LockMutex(P.clock_mutex); P.audio_clock = base; SDL_UnlockMutex(P.clock_mutex);
                clk = base; gl_ptsA = base;
                fprintf(stderr,"[video] container pts BROKEN (backward x%d) — wall pacing + 1/fps cadence\n",g_pts_viol);
            }
        }
        if(P.aclk_creep) bts = AV_NOPTS_VALUE;           /* CREEP-latched (broken asf/wmv timestamps):
            the VIDEO pts are untrustworthy too — pacing wall clock against them raced to EOF at
            decode speed (the ".wmv very weird" loop). Pure 1/fps cadence instead. */
        double pts = (bts != AV_NOPTS_VALUE) ? bts * av_q2d(tb)
                                             : (gl_ptsB>=0 ? gl_ptsB + frame_dur : 0.0);
        /* NB: a "duplicate-frame upload skip" (sampled-luma signature, MAD<1) lived here briefly
           (2026-07-04) and was REMOVED: on enhanced 2x files RIFE midpoints in slow scenes sit in
           the same sampled-MAD band as re-encoded hold frames, so the skip silently discarded the
           interpolated frames — visibly undoing frame-gen. No threshold separates the two after
           lossy encode, and the skip only saved ~1ms of upload. Every frame uploads. */
        if(behind && pts < clk - frame_dur){
            gl_ptsB = pts;      /* stale catch-up frame: advance the timeline, never show it */
            g_cu_dropped++;
            continue;           /* (gl_nframes deliberately NOT advanced — no valid A/B pair yet) */
        }
        /* promote: A <- old B; new frame -> B */
        VidTex tmp = gl_vt; gl_vt = gl_vtB; gl_vtB = tmp;
        { AVFrame *tp = gl_pendA; gl_pendA = gl_pendB; gl_pendB = tp; }   /* pending content follows its slot */
        gl_ptsA = gl_ptsB;
        if(g_vd.run){
            if(gl_pendB && g_prof>0) g_pf_skip++;         /* old A's never-shown frame: superseded, no upload */
            vd_recycle(&gl_pendB);
            gl_pendB = vframe; vframe = NULL;             /* B's content stays pending until it's presented */
        } else {
            double t_u0 = g_prof>0 ? prof_now() : 0;
            gl_upload(&gl_vtB, vframe);
            if(g_prof>0){ glFinish(); g_pf_up += prof_now()-t_u0; g_pf_nu++; }
        }
        gl_ptsB = pts;
        if(gl_nframes < 2) gl_nframes++;
    }

    if(g_cu_dropped){ fprintf(stderr,"[seek] catch-up: dropped %ld stale frames (silent)\n", g_cu_dropped); g_cu_dropped=0; }
    if(g_prof<0) g_prof = getenv("HOGS_PROF")?1:0;
    if(g_prof>0){ static double pt0=0; double pn=prof_now(); if(pt0==0) pt0=pn;
        if(pn-pt0>=1000){ fprintf(stderr,"[prof] decoded=%d (%.1fms total decode/send)  xfer=%d avg %.2fms  upload=%d avg %.2fms  skipped=%d\n",
            g_pf_nd,g_pf_dec,g_pf_nx,g_pf_nx?g_pf_xfer/g_pf_nx:0,g_pf_nu,g_pf_nu?g_pf_up/g_pf_nu:0,g_pf_skip);
            fprintf(stderr,"[prof]   repeats: ring-empty=%d clock-not-advanced=%d   clk step per present min=%.1fms max=%.1fms\n",g_pf_starve,g_pf_rep,g_pf_dmin*1000,g_pf_dmax*1000);
            g_pf_starve=g_pf_rep=0; g_pf_dmax=0; g_pf_dmin=1e9;
            g_pf_dec=g_pf_xfer=g_pf_up=0; g_pf_nd=g_pf_nx=g_pf_nu=g_pf_skip=0; pt0=pn; } }
    if(gl_nframes >= 1){
        double alpha = 0.0;
        if(gl_nframes >= 2 && gl_ptsB > gl_ptsA) alpha = (clk - gl_ptsA) / (gl_ptsB - gl_ptsA);
        if(alpha < 0) alpha = 0; if(alpha > 1) alpha = 1;
        gl_last_alpha = alpha;
        if(gl_nframes < 2) pr_vt = gl_vtB;                /* one frame yet / still image */
        else if(!P.light && gl_ptsB <= clk) pr_vt = gl_vtB;   /* decode lagging: B is already due — show the newest */
        else               pr_vt = gl_vt;                 /* plain playback: show the current frame */
        if(g_vd.run){                                     /* lazy upload: only what is about to be drawn */
            int needA = (pr_vt.y == gl_vt.y) || (P.light && gl_nframes >= 2);
            int needB = (pr_vt.y == gl_vtB.y) || (P.light && gl_nframes >= 2);
            if(needA && gl_pendA){ double t=g_prof>0?prof_now():0; gl_upload(&gl_vt, gl_pendA);
                if(g_prof>0){ glFinish(); g_pf_up+=prof_now()-t; g_pf_nu++; } vd_recycle(&gl_pendA); }
            if(needB && gl_pendB){ double t=g_prof>0?prof_now():0; gl_upload(&gl_vtB, gl_pendB);
                if(g_prof>0){ glFinish(); g_pf_up+=prof_now()-t; g_pf_nu++; } vd_recycle(&gl_pendB); }
        }
        /* real-fps counter: count frames that actually REACH THE SCREEN, not frames decoded.
           The old counter incremented per decode, so when the loop fell behind and skipped
           every other frame the Stats panel still claimed full rate — hiding the very stutter
           it was meant to expose. */
        { static double shown_pts = -1e18;
          double sp = (gl_nframes < 2 || (!P.light && gl_ptsB <= clk)) ? gl_ptsB : gl_ptsA;
          if(sp != shown_pts){ shown_pts = sp; g_vid_frames++; }
          else if(g_prof>0){ static double pclk=0; if(g_vd.run && g_vd.n==0) g_pf_starve++; else g_pf_rep++; g_pf_dclk += clk-pclk; } 
          if(g_prof>0){ static double lc=0; double d=clk-lc; lc=clk; if(d>g_pf_dmax) g_pf_dmax=d; if(d<g_pf_dmin) g_pf_dmin=d; } }
        gl_present(alpha);
    } else {
        glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
    }
}

/* re-present the current frame WITHOUT advancing (used while paused so the OSD can overlay) */
static void gl_redraw(void){
    if(gl_nframes >= 2){
        /* pr_* still hold the last presented source (normal A/B, or the last integrated D) */
        if(!pr_vt.y) pr_vt=gl_vt;
        gl_present(gl_last_alpha);
    }
    else if(gl_nframes == 1){ pr_vt=gl_vtB; gl_present(0.0); }
    else { glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT); }
}

/* ---- glyph atlas (on-demand) ---- */
/* recycle the glyph atlas: clear the on-demand cache so visible glyphs re-rasterize into a
   fresh atlas. Lets the Matrix rain cycle through far more distinct Unicode glyphs than the
   atlas can hold at once. The reserved white texel at (0,0) sits below the pen start, so it
   survives untouched and solid UI rects keep working. */
static void atlas_reset(void){
    memset(g_glyphs, 0, sizeof(g_glyphs));
    atlas_penx = 3; atlas_peny = 3; atlas_rowh = 0;
}
static Glyph *get_glyph(int cp){
    if(!g_font_ok) return NULL;
    unsigned h = ((unsigned)cp*2654435761u) & (GLYPH_HASH-1);
    for(int probe=0; probe<GLYPH_HASH; probe++){
        Glyph *G=&g_glyphs[h];
        if(G->used && G->cp==cp) return G;
        if(!G->used){
            int w,gh,xo,yo;
            unsigned char *bmp=stbtt_GetCodepointBitmap(&g_font,0,g_font_scale,cp,&w,&gh,&xo,&yo);
            int aw,lsb; stbtt_GetCodepointHMetrics(&g_font,cp,&aw,&lsb);
            G->used=1; G->cp=cp; G->ax=aw*g_font_scale; G->xoff=(short)xo; G->yoff=(short)yo; G->w=(short)w; G->h=(short)gh;
            if(bmp && w>0 && gh>0){
                if(atlas_penx + w + 2 > ATLAS_W){ atlas_penx=1; atlas_peny+=atlas_rowh+2; atlas_rowh=0; }
                if(atlas_peny + gh + 2 <= ATLAS_H){
                    glBindTexture(GL_TEXTURE_2D, ui_atlas); glPixelStorei(GL_UNPACK_ALIGNMENT,1);
                    glTexSubImage2D(GL_TEXTURE_2D,0, atlas_penx, atlas_peny, w, gh, GL_RED, GL_UNSIGNED_BYTE, bmp);
                    G->u0=(float)atlas_penx/ATLAS_W; G->v0=(float)atlas_peny/ATLAS_H;
                    G->u1=(float)(atlas_penx+w)/ATLAS_W; G->v1=(float)(atlas_peny+gh)/ATLAS_H;
                    atlas_penx += w+2; if(gh>atlas_rowh) atlas_rowh=gh;
                } else {
                    /* atlas exhausted → recycle it and retry into the fresh one (safety net;
                       the Matrix rain also recycles proactively at frame start) */
                    if(bmp){ stbtt_FreeBitmap(bmp,NULL); bmp=NULL; }
                    atlas_reset();
                    return get_glyph(cp);
                }
            } else { G->w=G->h=0; }
            if(bmp) stbtt_FreeBitmap(bmp,NULL);
            return G;
        }
        h=(h+1)&(GLYPH_HASH-1);
    }
    return NULL;
}
static int utf8_next(const char **p){
    const unsigned char *s=(const unsigned char*)*p; int cp,n;
    if(s[0]<0x80){ cp=s[0]; n=1; }
    else if((s[0]&0xE0)==0xC0){ cp=s[0]&0x1F; n=2; }
    else if((s[0]&0xF0)==0xE0){ cp=s[0]&0x0F; n=3; }
    else if((s[0]&0xF8)==0xF0){ cp=s[0]&0x07; n=4; }
    else { (*p)++; return 0xFFFD; }
    for(int i=1;i<n;i++){ if((s[i]&0xC0)!=0x80){ (*p)++; return 0xFFFD; } cp=(cp<<6)|(s[i]&0x3F); }
    *p += n; return cp;
}
/* ---- OSD geometry helpers (vertex = pos2,uv2,col4) ---- */
static void ui_vtx(float x,float y,float u,float v,float r,float g,float b,float a){
    if(ui_n+8 > ui_cap){ ui_cap = ui_cap?ui_cap*2:16384; ui_buf = realloc(ui_buf, (size_t)ui_cap*sizeof(float)); }
    float *V=ui_buf+ui_n; V[0]=x;V[1]=y;V[2]=u;V[3]=v;V[4]=r;V[5]=g;V[6]=b;V[7]=a; ui_n+=8;
}
static void ui_quad(float x,float y,float w,float h,float u0,float v0,float u1,float v1,float r,float g,float b,float a){
    ui_vtx(x,y,u0,v0,r,g,b,a); ui_vtx(x+w,y,u1,v0,r,g,b,a); ui_vtx(x+w,y+h,u1,v1,r,g,b,a);
    ui_vtx(x,y,u0,v0,r,g,b,a); ui_vtx(x+w,y+h,u1,v1,r,g,b,a); ui_vtx(x,y+h,u0,v1,r,g,b,a);
}
static void ui_rect(float x,float y,float w,float h,float r,float g,float b,float a){
    ui_quad(x,y,w,h, WHITE_U,WHITE_V,WHITE_U,WHITE_V, r,g,b,a); /* reserved white texel → solid fill */
}

/* ---- OSD themes ------------------------------------------------------------------------------
   Colours are named by ROLE, not by hue, so a new theme is one row in THEMES[] and nothing else
   changes. `text` must stay legible on `bar`/`panel`, and `hover` is alpha-blended ON TOP of
   whatever is beneath it — hence it is white on dark themes and black on light ones, otherwise the
   hover highlight vanishes on a pale bar. */
typedef struct {
    const char *name;
    float bar[4];      /* control-bar background            */
    float panel[4];    /* playlist panel background         */
    float btn[4];      /* idle button fill                  */
    float btn_act[4];  /* active/toggled button fill        */
    float track[4];    /* seek-bar groove                   */
    float fill[4];     /* seek-bar elapsed portion          */
    float knob[4];     /* seek-bar handle                   */
    float text[4];     /* primary text + icons              */
    float text_on[4];  /* text/icon drawn ON an accent fill (active button) — NOT the same as `text`:
                          on a light theme `text` is near-black, which is unreadable on the saturated
                          accent, so light themes flip this to white. */
    float dim[4];      /* secondary text (filename, hints)  */
    float accent[4];   /* focus underline, resize handle    */
    float row_cur[4];  /* playlist: now-playing row         */
    float row_sel[4];  /* playlist: selected row            */
    float hover[4];    /* hover wash, drawn over the above  */
} Theme;

static const Theme THEMES[] = {
 /* name          bar                       panel                     btn                       btn_act                   track                     fill                      knob                      text                      text_on                   dim                       accent                    row_cur                   row_sel                   hover                  */
 {"Dark",       {0.04f,0.04f,0.06f,0.84f},{0.03f,0.03f,0.05f,0.92f},{0.15f,0.15f,0.19f,0.92f},{0.40f,0.31f,0.72f,0.96f},{0.22f,0.22f,0.28f,1.0f},{0.63f,0.49f,1.00f,1.0f},{0.85f,0.78f,1.00f,1.0f},{0.96f,0.96f,1.00f,1.0f},{1.00f,1.00f,1.00f,1.0f},{0.72f,0.75f,0.88f,1.0f},{0.55f,0.45f,0.95f,1.0f},{0.35f,0.26f,0.60f,0.95f},{0.55f,0.22f,0.22f,0.80f},{1,1,1,0.10f}},
 {"Light",      {0.92f,0.92f,0.95f,0.92f},{0.96f,0.96f,0.98f,0.95f},{0.82f,0.82f,0.87f,0.95f},{0.45f,0.40f,0.85f,0.96f},{0.75f,0.75f,0.80f,1.0f},{0.40f,0.34f,0.85f,1.0f},{0.25f,0.20f,0.60f,1.0f},{0.10f,0.10f,0.16f,1.0f},{1.00f,1.00f,1.00f,1.0f},{0.35f,0.35f,0.45f,1.0f},{0.40f,0.34f,0.85f,1.0f},{0.62f,0.58f,0.92f,0.95f},{0.85f,0.62f,0.62f,0.85f},{0,0,0,0.10f}},
 {"Sandstone",  {0.18f,0.13f,0.09f,0.88f},{0.15f,0.11f,0.08f,0.93f},{0.32f,0.24f,0.17f,0.94f},{0.78f,0.52f,0.24f,0.96f},{0.34f,0.26f,0.19f,1.0f},{0.88f,0.62f,0.30f,1.0f},{0.98f,0.85f,0.65f,1.0f},{0.99f,0.94f,0.86f,1.0f},{0.16f,0.09f,0.04f,1.0f},{0.80f,0.70f,0.58f,1.0f},{0.88f,0.62f,0.30f,1.0f},{0.55f,0.36f,0.16f,0.95f},{0.60f,0.30f,0.18f,0.85f},{1,1,1,0.10f}},
 {"Green Fields",{0.07f,0.14f,0.08f,0.88f},{0.05f,0.11f,0.06f,0.93f},{0.14f,0.26f,0.16f,0.94f},{0.30f,0.66f,0.30f,0.96f},{0.16f,0.28f,0.18f,1.0f},{0.42f,0.80f,0.38f,1.0f},{0.76f,0.96f,0.70f,1.0f},{0.94f,0.99f,0.92f,1.0f},{0.04f,0.14f,0.05f,1.0f},{0.70f,0.84f,0.68f,1.0f},{0.42f,0.80f,0.38f,1.0f},{0.20f,0.46f,0.22f,0.95f},{0.52f,0.44f,0.16f,0.85f},{1,1,1,0.10f}},
 {"Cobblestone",{0.13f,0.13f,0.14f,0.88f},{0.10f,0.10f,0.11f,0.93f},{0.26f,0.26f,0.28f,0.94f},{0.52f,0.54f,0.58f,0.96f},{0.28f,0.28f,0.30f,1.0f},{0.66f,0.68f,0.72f,1.0f},{0.90f,0.92f,0.95f,1.0f},{0.95f,0.95f,0.96f,1.0f},{0.08f,0.08f,0.09f,1.0f},{0.72f,0.72f,0.76f,1.0f},{0.66f,0.68f,0.72f,1.0f},{0.38f,0.39f,0.42f,0.95f},{0.50f,0.34f,0.30f,0.85f},{1,1,1,0.10f}},
 {"Beach",      {0.06f,0.22f,0.28f,0.86f},{0.05f,0.18f,0.24f,0.93f},{0.12f,0.34f,0.40f,0.94f},{0.96f,0.74f,0.36f,0.96f},{0.14f,0.36f,0.42f,1.0f},{0.99f,0.82f,0.45f,1.0f},{1.00f,0.95f,0.80f,1.0f},{0.97f,0.99f,0.99f,1.0f},{0.10f,0.20f,0.24f,1.0f},{0.72f,0.88f,0.90f,1.0f},{0.99f,0.82f,0.45f,1.0f},{0.15f,0.50f,0.55f,0.95f},{0.70f,0.45f,0.25f,0.85f},{1,1,1,0.10f}},
 {"Blue Sky",   {0.78f,0.88f,0.97f,0.92f},{0.86f,0.93f,0.99f,0.95f},{0.66f,0.79f,0.92f,0.95f},{0.16f,0.46f,0.80f,0.96f},{0.62f,0.75f,0.88f,1.0f},{0.20f,0.52f,0.86f,1.0f},{0.08f,0.30f,0.60f,1.0f},{0.06f,0.14f,0.24f,1.0f},{1.00f,1.00f,1.00f,1.0f},{0.28f,0.40f,0.52f,1.0f},{0.20f,0.52f,0.86f,1.0f},{0.52f,0.72f,0.94f,0.95f},{0.88f,0.70f,0.55f,0.85f},{0,0,0,0.10f}},
 {"Pink",       {0.20f,0.06f,0.14f,0.88f},{0.17f,0.05f,0.12f,0.93f},{0.36f,0.14f,0.26f,0.94f},{0.92f,0.36f,0.62f,0.96f},{0.38f,0.16f,0.28f,1.0f},{0.98f,0.48f,0.72f,1.0f},{1.00f,0.82f,0.90f,1.0f},{1.00f,0.94f,0.97f,1.0f},{0.22f,0.04f,0.12f,1.0f},{0.88f,0.70f,0.80f,1.0f},{0.98f,0.48f,0.72f,1.0f},{0.60f,0.20f,0.40f,0.95f},{0.62f,0.30f,0.30f,0.85f},{1,1,1,0.10f}},
};
#define N_THEMES ((int)(sizeof(THEMES)/sizeof(THEMES[0])))
static int g_theme = 0;
#define TH (&THEMES[g_theme])
/* draw a rect / text straight from a theme role */
static void ui_text(float x,float y,float px,const char *s,float r,float g,float b,float a);
static void ui_rect4(float x,float y,float w,float h,const float c[4]){ ui_rect(x,y,w,h,c[0],c[1],c[2],c[3]); }
static void ui_text4(float x,float y,float px,const char *s,const float c[4]){ ui_text(x,y,px,s,c[0],c[1],c[2],c[3]); }

/* WHERE THE PLAYER WRITES (2026-10-29, for distribution): next to the exe — a portable install, which is
   how the owner runs it, and their existing playlist/settings live there — UNLESS that folder isn't
   writable (unzipped into Program Files, a read-only share...), in which case everything goes to
   %LOCALAPPDATA%\Hogs media player\. Settings, playlist, search, theme, the enhance cache and the
   enhancer's status file all use this; the program's own files (DLLs, models, presets) stay read-only. */
static const char *data_dir(void){
    static char dir[1100] = "";
    if(dir[0]) return dir;
    char *bp = SDL_GetBasePath(); snprintf(dir, sizeof dir, "%s", bp ? bp : ""); if(bp) SDL_free(bp);
    wchar_t wd[1200], probe[1300]; path_u8_to_w(dir, wd, 1200);
    _snwprintf(probe, 1300, L"%lshogs_write_probe_%lu.tmp", wd, (unsigned long)GetCurrentProcessId());
    HANDLE h = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if(h != INVALID_HANDLE_VALUE){ CloseHandle(h); return dir; }        /* portable: exe folder is writable */
    wchar_t la[MAX_PATH]; DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    if(!n || n >= MAX_PATH) return dir;                                  /* nowhere better: keep trying here */
    wchar_t ud[MAX_PATH + 40]; _snwprintf(ud, MAX_PATH + 40, L"%ls\\Hogs media player\\", la);
    CreateDirectoryW(ud, NULL);
    path_w_to_u8(ud, dir, sizeof dir);
    fprintf(stderr, "[data] program folder is read-only -> settings/playlist/cache in %s\n", dir);
    return dir;
}
/* Theme persistence: stored BY NAME next to the exe, so reordering THEMES[] later can never
   silently change the user's theme (an index would). Unknown/absent name → theme 0. */
static void theme_path(char *buf,size_t cap){
    snprintf(buf,cap,"%shogs_theme.txt",data_dir());
}
static void theme_save(void){
    char p[1024]; theme_path(p,sizeof p);
    FILE *f=path_fopen(p,L"wb"); if(!f) return;        /* read-only / Unicode install dir → just don't persist */
    fprintf(f,"%s\n",THEMES[g_theme].name); fclose(f);
}
static void theme_load(void){
    char p[1024]; theme_path(p,sizeof p);
    FILE *f=path_fopen(p,L"rb"); if(!f) return;
    char n[64]={0};
    if(fgets(n,sizeof n,f)){
        char *e=n+strlen(n); while(e>n && (e[-1]=='\n'||e[-1]=='\r'||e[-1]==' ')) *--e=0;
        for(int i=0;i<N_THEMES;i++) if(!SDL_strcasecmp(n,THEMES[i].name)){ g_theme=i; break; }
    }
    fclose(f);
}
static void theme_cycle(int d){
    g_theme=(g_theme+d+N_THEMES)%N_THEMES;
    char m[64]; snprintf(m,sizeof m,"Theme: %s",THEMES[g_theme].name); toast(m);
    theme_save();
}
static float ui_textw(float px,const char *s){
    px *= g_ui_scale;
    if(!g_font_ok) return stb_easy_font_width((char*)s)*(px/7.0f);
    float sc=px/FONT_PX, w=0; const char *p=s;
    while(*p){ int cp=utf8_next(&p); if(cp=='\n') break; Glyph *G=get_glyph(cp); if(G) w+=G->ax*sc; }
    return w;
}
static void ui_text(float x,float y,float px,const char *s,float r,float g,float b,float a){
    px *= g_ui_scale;
    if(!g_font_ok){ /* ASCII fallback via stb_easy_font as white-texel quads */
        float sc=px/7.0f; static char qb[60000];
        int nq=stb_easy_font_print(0,0,(char*)s,NULL,qb,sizeof(qb));
        struct V{ float x,y,z; unsigned char c[4]; } *qv=(struct V*)qb;
        for(int i=0;i<nq;i++){ struct V *q=qv+i*4; float X[4],Y[4]; for(int k=0;k<4;k++){X[k]=x+q[k].x*sc;Y[k]=y+q[k].y*sc;}
            ui_vtx(X[0],Y[0],WHITE_U,WHITE_V,r,g,b,a); ui_vtx(X[1],Y[1],WHITE_U,WHITE_V,r,g,b,a); ui_vtx(X[2],Y[2],WHITE_U,WHITE_V,r,g,b,a);
            ui_vtx(X[0],Y[0],WHITE_U,WHITE_V,r,g,b,a); ui_vtx(X[2],Y[2],WHITE_U,WHITE_V,r,g,b,a); ui_vtx(X[3],Y[3],WHITE_U,WHITE_V,r,g,b,a); }
        return;
    }
    float sc=px/FONT_PX, penx=x, baseline=y + g_font_ascent_px*sc;
    const char *p=s;
    while(*p){ int cp=utf8_next(&p);
        if(cp=='\n'){ penx=x; baseline += FONT_PX*sc*1.2f; continue; }
        Glyph *G=get_glyph(cp); if(!G) continue;
        if(G->w>0 && G->h>0) ui_quad(penx+G->xoff*sc, baseline+G->yoff*sc, G->w*sc, G->h*sc, G->u0,G->v0,G->u1,G->v1, r,g,b,a);
        penx += G->ax*sc;
    }
}
static void osd_addbtn(float x,float y,float w,float h,int id){
    if(osd_nbtn < OSD_MAXBTN){ osd_btn[osd_nbtn].x=x; osd_btn[osd_nbtn].y=y; osd_btn[osd_nbtn].w=w; osd_btn[osd_nbtn].h=h; osd_btn[osd_nbtn].id=id; osd_nbtn++; }
}
/* ---- vector icons (drawn from rects+triangles so they need no font glyphs) ---- */
enum { IC_PREV, IC_PLAY, IC_PAUSE, IC_STOP, IC_NEXT, IC_FILL, IC_UPSCALE, IC_FRAMEGEN,
       IC_REPEAT, IC_SHUFFLE, IC_AUDIO, IC_SUBS, IC_STATS, IC_KEYS, IC_LIST,
       IC_ADD, IC_FOLDER, IC_TRASH, IC_THEME, IC_SAVE, IC_LOAD };
static void ui_tri(float ax,float ay,float bx,float by,float cx,float cy,float r,float g,float b,float a){
    ui_vtx(ax,ay,WHITE_U,WHITE_V,r,g,b,a); ui_vtx(bx,by,WHITE_U,WHITE_V,r,g,b,a); ui_vtx(cx,cy,WHITE_U,WHITE_V,r,g,b,a);
}
/* draw icon `kind` inside the square box (x,y,s) using colour r,g,b,a */
static void draw_icon(float x,float y,float s,int kind,float r,float g,float b,float a){
    float cx=x+s*0.5f, cy=y+s*0.5f;
    switch(kind){
      case IC_PREV: ui_rect(x,y,s*0.16f,s,r,g,b,a); ui_tri(x+s,y, x+s,y+s, x+s*0.30f,cy, r,g,b,a); break;
      case IC_NEXT: ui_tri(x,y, x,y+s, x+s*0.70f,cy, r,g,b,a); ui_rect(x+s*0.84f,y,s*0.16f,s,r,g,b,a); break;
      case IC_PLAY: ui_tri(x+s*0.12f,y, x+s*0.12f,y+s, x+s*0.92f,cy, r,g,b,a); break;
      case IC_PAUSE: ui_rect(x+s*0.16f,y,s*0.24f,s,r,g,b,a); ui_rect(x+s*0.60f,y,s*0.24f,s,r,g,b,a); break;
      case IC_STOP: ui_rect(x+s*0.13f,y+s*0.13f,s*0.74f,s*0.74f,r,g,b,a); break;
      case IC_SAVE: /* floppy: body, shutter strip, label window */
        ui_rect(x,y,s,s*0.18f,r,g,b,a); ui_rect(x,y,s*0.14f,s,r,g,b,a); ui_rect(x+s*0.86f,y,s*0.14f,s,r,g,b,a);
        ui_rect(x,y+s*0.86f,s,s*0.14f,r,g,b,a); ui_rect(x+s*0.28f,y+s*0.52f,s*0.44f,s*0.34f,r,g,b,a); break;
      case IC_LOAD: /* list sheet with an up arrow */
        ui_rect(x+s*0.05f,y+s*0.62f,s*0.90f,s*0.12f,r,g,b,a); ui_rect(x+s*0.05f,y+s*0.86f,s*0.90f,s*0.12f,r,g,b,a);
        ui_tri(x+s*0.20f,y+s*0.42f, x+s*0.80f,y+s*0.42f, x+s*0.50f,y, r,g,b,a); break;
      case IC_THEME: /* 2x2 swatch grid — one square left hollow to read as "palette" */
        ui_rect(x,           y,           s*0.44f, s*0.44f, r,g,b,a);
        ui_rect(x+s*0.56f,   y,           s*0.44f, s*0.44f, r,g,b,a*0.55f);
        ui_rect(x,           y+s*0.56f,   s*0.44f, s*0.44f, r,g,b,a*0.55f);
        ui_rect(x+s*0.56f,   y+s*0.56f,   s*0.44f, s*0.10f, r,g,b,a);
        ui_rect(x+s*0.56f,   y+s*0.90f,   s*0.44f, s*0.10f, r,g,b,a);
        ui_rect(x+s*0.56f,   y+s*0.56f,   s*0.10f, s*0.44f, r,g,b,a);
        ui_rect(x+s*0.90f,   y+s*0.56f,   s*0.10f, s*0.44f, r,g,b,a); break;
      case IC_FILL: /* expand-to-fill: outer outline + filled inner box */
        ui_rect(x,y,s,s*0.10f,r,g,b,a); ui_rect(x,y+s*0.90f,s,s*0.10f,r,g,b,a);
        ui_rect(x,y,s*0.10f,s,r,g,b,a); ui_rect(x+s*0.90f,y,s*0.10f,s,r,g,b,a);
        ui_rect(x+s*0.34f,y+s*0.34f,s*0.32f,s*0.32f,r,g,b,a); break;
      case IC_UPSCALE: /* sparkle: 4-point star */
        ui_tri(cx,y, cx-s*0.13f,cy, cx+s*0.13f,cy, r,g,b,a);
        ui_tri(cx,y+s, cx-s*0.13f,cy, cx+s*0.13f,cy, r,g,b,a);
        ui_tri(x,cy, cx,cy-s*0.13f, cx,cy+s*0.13f, r,g,b,a);
        ui_tri(x+s,cy, cx,cy-s*0.13f, cx,cy+s*0.13f, r,g,b,a); break;
      case IC_FRAMEGEN: /* two offset frames (interpolation) */
        ui_rect(x,y,s*0.62f,s*0.10f,r,g,b,a); ui_rect(x,y,s*0.10f,s*0.62f,r,g,b,a);
        ui_rect(x,y+s*0.52f,s*0.62f,s*0.10f,r,g,b,a); ui_rect(x+s*0.52f,y,s*0.10f,s*0.62f,r,g,b,a);
        ui_rect(x+s*0.34f,y+s*0.34f,s*0.62f,s*0.10f,r,g,b,a); ui_rect(x+s*0.34f,y+s*0.34f,s*0.10f,s*0.62f,r,g,b,a);
        ui_rect(x+s*0.34f,y+s*0.86f,s*0.62f,s*0.10f,r,g,b,a); ui_rect(x+s*0.86f,y+s*0.34f,s*0.10f,s*0.62f,r,g,b,a); break;
      case IC_REPEAT: /* two arrows looping */
        ui_rect(x+s*0.12f,y+s*0.10f,s*0.62f,s*0.12f,r,g,b,a);
        ui_tri(x+s*0.70f,y, x+s*0.70f,y+s*0.32f, x+s,y+s*0.16f, r,g,b,a);
        ui_rect(x+s*0.26f,y+s*0.78f,s*0.62f,s*0.12f,r,g,b,a);
        ui_tri(x+s*0.30f,y+s, x+s*0.30f,y+s*0.68f, x,y+s*0.84f, r,g,b,a); break;
      case IC_SHUFFLE: /* crossing arrows (X) */
        ui_tri(x,y, x+s*0.22f,y, x+s,y+s, r,g,b,a);
        ui_tri(x,y+s, x+s*0.22f,y+s, x+s,y, r,g,b,a); break;
      case IC_AUDIO: /* speaker */
        ui_rect(x+s*0.06f,y+s*0.34f,s*0.22f,s*0.32f,r,g,b,a);
        ui_tri(x+s*0.28f,cy, x+s*0.58f,y+s*0.16f, x+s*0.58f,y+s*0.84f, r,g,b,a);
        ui_rect(x+s*0.68f,y+s*0.30f,s*0.07f,s*0.40f,r,g,b,a);
        ui_rect(x+s*0.82f,y+s*0.20f,s*0.07f,s*0.60f,r,g,b,a); break;
      case IC_SUBS: /* CC box with two text lines */
        ui_rect(x,y+s*0.18f,s,s*0.10f,r,g,b,a); ui_rect(x,y+s*0.72f,s,s*0.10f,r,g,b,a);
        ui_rect(x,y+s*0.18f,s*0.10f,s*0.64f,r,g,b,a); ui_rect(x+s*0.90f,y+s*0.18f,s*0.10f,s*0.64f,r,g,b,a);
        ui_rect(x+s*0.18f,y+s*0.46f,s*0.30f,s*0.09f,r,g,b,a); ui_rect(x+s*0.55f,y+s*0.46f,s*0.27f,s*0.09f,r,g,b,a); break;
      case IC_STATS: /* bar chart */
        ui_rect(x+s*0.08f,y+s*0.50f,s*0.20f,s*0.50f,r,g,b,a);
        ui_rect(x+s*0.40f,y+s*0.25f,s*0.20f,s*0.75f,r,g,b,a);
        ui_rect(x+s*0.72f,y+s*0.08f,s*0.20f,s*0.92f,r,g,b,a); break;
      case IC_KEYS: /* keyboard: outline + key dots */
        ui_rect(x,y+s*0.22f,s,s*0.08f,r,g,b,a); ui_rect(x,y+s*0.70f,s,s*0.08f,r,g,b,a);
        ui_rect(x,y+s*0.22f,s*0.08f,s*0.56f,r,g,b,a); ui_rect(x+s*0.92f,y+s*0.22f,s*0.08f,s*0.56f,r,g,b,a);
        for(int c=0;c<4;c++){ ui_rect(x+s*(0.16f+c*0.18f),y+s*0.38f,s*0.10f,s*0.10f,r,g,b,a);
                              ui_rect(x+s*(0.16f+c*0.18f),y+s*0.56f,s*0.10f,s*0.10f,r,g,b,a);} break;
      case IC_LIST: ui_rect(x,y+s*0.08f,s,s*0.14f,r,g,b,a); ui_rect(x,y+s*0.43f,s,s*0.14f,r,g,b,a); ui_rect(x,y+s*0.78f,s,s*0.14f,r,g,b,a); break;
      case IC_ADD: ui_rect(cx-s*0.10f,y,s*0.20f,s,r,g,b,a); ui_rect(x,cy-s*0.10f,s,s*0.20f,r,g,b,a); break;
      case IC_FOLDER: ui_rect(x,y+s*0.12f,s*0.45f,s*0.20f,r,g,b,a); ui_rect(x,y+s*0.26f,s,s*0.60f,r,g,b,a); break;
      case IC_TRASH: ui_rect(x+s*0.05f,y+s*0.14f,s*0.90f,s*0.10f,r,g,b,a); /* lid */
        ui_rect(x+s*0.38f,y+s*0.04f,s*0.24f,s*0.08f,r,g,b,a);             /* handle */
        ui_rect(x+s*0.16f,y+s*0.26f,s*0.68f,s*0.70f,r,g,b,a); break;       /* can */
    }
}
static float icon_btn_w(int icon,const char *label,float h){
    (void)icon; float is=h*0.5f; float lw = label?ui_textw(13,label):0;
    return 8 + is + (label?5+lw:0) + 8;
}
static void icon_button(float x,float y,float w,float h,int icon,const char *label,int active,int id){
    ui_rect4(x,y,w,h, active ? TH->btn_act : TH->btn);
    if(g_mouse_x>=x && g_mouse_x<=x+w && g_mouse_y>=y && g_mouse_y<=y+h) ui_rect4(x,y,w,h, TH->hover);
    const float *fg = active ? TH->text_on : TH->text;   /* legible on the accent fill */
    float is=h*0.5f, iy=y+(h-is)*0.5f;
    draw_icon(x+8, iy, is, icon, fg[0],fg[1],fg[2],fg[3]);
    if(label){ float px=13; ui_text4(x+8+is+5, y+(h-px)*0.5f-1, px, label, fg); }
    osd_addbtn(x,y,w,h,id);
}
/* classify a playlist entry by extension: 0=video 1=audio 2=image (used by the filter) */
static int media_kind(const char *name){
    const char *e=strrchr(name,'.'); if(!e) return 0; e++;
    static const char *A[]={"mp3","flac","wav","aac","ogg","opus","m4a","wma","ape","alac",
        "mka","m4b","aif","aiff","wv","ac3","dts","amr","mpc","oga","spx","caf",0};
    static const char *I[]={"jpg","jpeg","jfif","png","gif","bmp","webp","tif","tiff","tga","jxl",
        "avif","heic","heif","ico","ppm","pgm","pnm","jp2","exr","dds","hdr","apng",0};
    for(int i=0;A[i];i++) if(!SDL_strcasecmp(e,A[i])) return 1;
    for(int i=0;I[i];i++) if(!SDL_strcasecmp(e,I[i])) return 2;
    return 0;
}
/* SEARCH-NORMALISED text (2026-10-08): ASCII lower-cased, and every run of non-alphanumeric ASCII
   (spaces, dots, underscores, dashes, brackets, path separators) collapsed to ONE space, trimmed.
   "One.Piece_-_01.mkv" and the typed phrase "one  piece" both normalise to "one piece...", so a phrase
   matches however the file spells its gaps. UTF-8 bytes pass through untouched. Every playlist row stores
   its normalised path + filename once (pl_add), so filtering is a plain strstr — no per-char tolower. */
static int srch_norm(const char *s, char *d, int cap){
    int n=0, gap=0;
    for(; *s; s++){
        unsigned char c=(unsigned char)*s;
        if(c>='A' && c<='Z') c=(unsigned char)(c+32);
        if(c>=0x80 || (c>='a'&&c<='z') || (c>='0'&&c<='9')){
            if(gap && n){ if(n>=cap-1) break; d[n++]=' '; }
            gap=0; if(n>=cap-1) break; d[n++]=(char)c;
        } else gap=1;
    }
    d[n]=0; return n;
}
/* ---- search-box caret helpers (byte offsets, snapped to UTF-8 boundaries) ---- */
static int u8_prev_at(const char *s, int i){
    if(i <= 0) return 0;
    i--; while(i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}
static int u8_next_at(const char *s, int i, int len){
    if(i >= len) return len;
    i++; while(i < len && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}
/* width of the first `n` bytes of the search text, at the box's 15px size */
static float srch_prefix_w(int n){
    char tmp[sizeof g_pl_search];
    if(n < 0) n = 0;
    if(n > g_pl_search_len) n = g_pl_search_len;
    memcpy(tmp, g_pl_search, (size_t)n); tmp[n] = 0;
    return ui_textw(15, tmp);
}
/* byte offset of the character boundary NEAREST to pixel x (click-to-place) */
static int srch_caret_at_x(float mx){
    int best = 0; float bestd = fabsf(mx - g_srch_text_x);
    for(int i = 0; i < g_pl_search_len; ){
        i = u8_next_at(g_pl_search, i, g_pl_search_len);
        float d = fabsf(mx - (g_srch_text_x + srch_prefix_w(i)));
        if(d < bestd){ bestd = d; best = i; }
    }
    return best;
}
/* ---- text selection in the search box: [min(anchor,caret), max(anchor,caret)) ---- */
static int srch_has_sel(void){ return g_pl_anchor>=0 && g_pl_anchor<=g_pl_search_len && g_pl_anchor!=g_pl_caret; }
static void srch_sel_range(int *a, int *b){
    int x=g_pl_anchor, y=g_pl_caret;
    if(x<0) x=0; if(x>g_pl_search_len) x=g_pl_search_len; if(y<0) y=0; if(y>g_pl_search_len) y=g_pl_search_len;
    *a = x<y?x:y; *b = x<y?y:x;
}
static void srch_del_sel(void){
    int a,b; srch_sel_range(&a,&b); if(a==b) return;
    memmove(g_pl_search+a, g_pl_search+b, (size_t)(g_pl_search_len-b)+1);
    g_pl_search_len -= (b-a); g_pl_caret = g_pl_anchor = a; pl_scroll = 0; g_srch_ver++;
}
/* select the word under byte offset c (double-click) */
static void srch_select_word(int c){
    int a=c, b=c;
    while(a>0 && g_pl_search[a-1]!=' ') a--;
    while(b<g_pl_search_len && g_pl_search[b]!=' ') b++;
    g_pl_anchor=a; g_pl_caret=b;
}
static void srch_insert(const char *utf8){
    if(srch_has_sel()) srch_del_sel();                         /* typing replaces the selection */
    for(const char *c = utf8; *c; c++){
        if(g_pl_search_len >= (int)sizeof(g_pl_search)-1) break;
        if((unsigned char)*c < 0x20) continue;                 /* control chars never enter the box */
        memmove(g_pl_search + g_pl_caret + 1, g_pl_search + g_pl_caret,
                (size_t)(g_pl_search_len - g_pl_caret) + 1);   /* +1 moves the NUL too */
        g_pl_search[g_pl_caret++] = *c;
        g_pl_search_len++;
    }
    g_pl_anchor = g_pl_caret; g_srch_ver++;
    pl_scroll = 0;
}
/* delete the character before the caret (backspace) or at it (delete) */
static void srch_erase(int before){
    int a, b;
    if(srch_has_sel()){ srch_del_sel(); return; }              /* backspace/delete removes the selection */
    if(before){ if(g_pl_caret <= 0) return; a = u8_prev_at(g_pl_search, g_pl_caret); b = g_pl_caret; }
    else      { if(g_pl_caret >= g_pl_search_len) return; a = g_pl_caret; b = u8_next_at(g_pl_search, g_pl_caret, g_pl_search_len); }
    memmove(g_pl_search + a, g_pl_search + b, (size_t)(g_pl_search_len - b) + 1);
    g_pl_search_len -= (b - a);
    g_pl_caret = g_pl_anchor = a; g_srch_ver++;
    pl_scroll = 0;
}
/* does playlist row idx pass the current filter + search box? */
/* The search box holds TERMS. Plain words must ALL appear in the filename. A '-' starts an EXCLUSION
   that HIDES every entry whose path contains it; since 2026-10-08 an exclusion is a whole PHRASE that
   runs up to the next " -", a comma, or the end of the box:
       -one piece              hides "One Piece" / "One.Piece" — but NOT files with only "one" or "piece"
       naruto -one piece -bleach   shows naruto, hides both phrases
       -one piece, naruto      a comma ends the phrase, so naruto is a normal search word again
       -"one piece" naruto     quotes work too; "two words" (no '-') is a must-contain phrase
   Matching is on search-normalised text (srch_norm), so dots/underscores/dashes in names count as gaps.
   Press Enter to pin the exclusions as persistent chips. Terms are parsed once per edit (g_srch_ver). */
#define SRCH_MAXT 16
static char g_tpos[SRCH_MAXT][64]; static int g_ntpos = 0;
static char g_tneg[SRCH_MAXT][64]; static int g_ntneg = 0;
static char g_xnorm[EXCL_MAX][64]; static int g_nx = 0;    /* pinned excludes, normalised */
static unsigned g_terms_ver = 0;
/* next term from *ps (0 = none left). out = the term as typed (phrase intact, quotes stripped),
   *neg = it is an exclusion, [*rb,*re) = its raw span in the box (so Enter can lift it out). */
static int srch_next_term(const char **ps, char *out, int cap, int *neg, const char **rb, const char **re){
    const char *s = *ps;
    for(;;){
        while(*s==' ' || *s==',') s++;
        if(!*s){ *ps = s; return 0; }
        const char *b = s, *tb, *te; int ng = 0;
        if(*s=='-'){
            if(!s[1] || s[1]==' ' || s[1]==','){ s++; continue; }   /* a lone '-' while typing: ignore */
            ng = 1; s++;
        }
        if(*s=='"'){ tb = ++s; while(*s && *s!='"') s++; te = s; if(*s=='"') s++; }
        else if(ng){                                   /* -phrase: up to the next " -x", a comma, or the end */
            tb = s;
            while(*s && *s!=','){
                if(*s==' '){ const char *k=s; while(*k==' ') k++;
                    if(*k=='-' && k[1] && k[1]!=' ' && k[1]!=',') break; }
                s++;
            }
            te = s;
        }
        else { tb = s; while(*s && *s!=' ' && *s!=',') s++; te = s; }
        while(te>tb && te[-1]==' ') te--;
        int len = (int)(te-tb); if(len > cap-1) len = cap-1;
        if(len <= 0) continue;                         /* "" or -"" : nothing to match */
        memcpy(out, tb, (size_t)len); out[len] = 0;
        *neg = ng; if(rb) *rb = b; if(re) *re = s; *ps = s;
        return 1;
    }
}
static void srch_terms_refresh(void){
    if(g_terms_ver == g_srch_ver) return;
    g_terms_ver = g_srch_ver; g_ntpos = g_ntneg = 0;
    const char *s = g_pl_search; char t[64]; int neg;
    while(srch_next_term(&s, t, sizeof t, &neg, NULL, NULL)){
        if(neg){ if(g_ntneg<SRCH_MAXT && srch_norm(t, g_tneg[g_ntneg], 64) > 0) g_ntneg++; }
        else   { if(g_ntpos<SRCH_MAXT && srch_norm(t, g_tpos[g_ntpos], 64) > 0) g_ntpos++; }
    }
    g_nx = 0;                                          /* excl_add/excl_remove bump g_srch_ver too */
    for(int i=0;i<g_excl_n;i++) if(srch_norm(g_excl[i], g_xnorm[g_nx], 64) > 0) g_nx++;
}
static const char *pl_npath(int i){ const char *s = pl_items[i]; return s + strlen(s) + 1; }  /* see pl_add */
static int g_pl_add_exempt = 0;      /* pl_add marks rows added while set (files opened from Explorer) */
static int pl_match(int idx){
    const char *np = pl_npath(idx);
    { const char *nb = np + strlen(np) + 1; if(nb[strlen(nb) + 1]) return 1; }   /* opened explicitly: always shown */
    if(g_pl_filter && media_kind(pl_items[idx]) != g_pl_filter-1) return 0;   /* 1=video 2=audio 3=image */
    srch_terms_refresh();
    if(!g_nx && !g_ntneg && !g_ntpos) return 1;
    for(int i=0;i<g_nx;i++)    if(strstr(np, g_xnorm[i])) return 0;    /* pinned excludes (whole path) */
    for(int i=0;i<g_ntneg;i++) if(strstr(np, g_tneg[i]))  return 0;    /* typed exclusions (whole path) */
    if(g_ntpos){ const char *nb = np + strlen(np) + 1;                 /* normalised filename */
        for(int i=0;i<g_ntpos;i++) if(!strstr(nb, g_tpos[i])) return 0; } /* typed words (filename) */
    return 1;
}
/* ---- persistent exclude list (hogs_excludes.txt, UTF-8, one word per line) ---- */
static char g_sess_dir[1024] = "";   /* where session/exclude files live: exe folder, or HOGS_SESSION_DIR */
static void sess_file(char *buf, size_t cap, const char *name){
    if(!g_sess_dir[0]){
        const char *e = getenv("HOGS_SESSION_DIR");
        if(e && *e){ snprintf(g_sess_dir,sizeof g_sess_dir,"%s",e); size_t n=strlen(g_sess_dir);
            if(n && g_sess_dir[n-1]!='\\' && g_sess_dir[n-1]!='/' && n+1<sizeof g_sess_dir){ g_sess_dir[n]='\\'; g_sess_dir[n+1]=0; } }
        else snprintf(g_sess_dir,sizeof g_sess_dir,"%s",data_dir());
    }
    snprintf(buf,cap,"%s%s",g_sess_dir,name);
}
static int g_nosession = 0;           /* HOGS_NOSESSION=1: tests never read/write the user's session */
/* opened with files (Explorer double-click): the PLAYLIST is temporary (saved session neither loaded nor
   overwritten) — but hide-words and the search box are the user's settings and always load/save
   (2026-10-28: tying them to the temporary playlist made them vanish on every double-click launch, and any
   word pinned during such a session was never written). Rows opened that way are always shown. */
static int g_sess_temp = 0;
static void excl_save(void){
    if(g_nosession) return;
    char p[1100]; sess_file(p,sizeof p,"hogs_excludes.txt");
    FILE *f=path_fopen(p,L"wb"); if(!f) return;
    for(int i=0;i<g_excl_n;i++) fprintf(f,"%s\n",g_excl[i]);
    fclose(f);
}
/* 1 = added, 2 = already present, 0 = rejected (empty / list full) */
static int excl_add(const char *w){
    char t[64]; int n=0;
    while(*w==' ') w++;
    for(; *w && n<63; w++) t[n++]=*w;
    while(n>0 && (t[n-1]==' '||t[n-1]=='\r'||t[n-1]=='\n')) n--;
    t[n]=0; if(!n) return 0;
    for(int i=0;i<g_excl_n;i++) if(!SDL_strcasecmp(g_excl[i],t)) return 2;
    if(g_excl_n>=EXCL_MAX) return 0;
    memcpy(g_excl[g_excl_n++],t,(size_t)n+1); g_srch_ver++; pl_scroll=0;
    return 1;
}
static void excl_remove(int i){
    if(i<0 || i>=g_excl_n) return;
    char msg[96]; snprintf(msg,sizeof msg,"Showing \"%.60s\" again",g_excl[i]);
    for(int k=i;k<g_excl_n-1;k++) memcpy(g_excl[k],g_excl[k+1],sizeof g_excl[k]);
    g_excl_n--; g_srch_ver++; excl_save(); toast(msg);
}
static void excl_load(void){
    if(g_nosession) return;
    char p[1100]; sess_file(p,sizeof p,"hogs_excludes.txt");
    FILE *f=path_fopen(p,L"rb"); if(!f) return;
    char line[256];
    while(fgets(line,sizeof line,f)){
        char *l=line; if((unsigned char)l[0]==0xEF && (unsigned char)l[1]==0xBB && (unsigned char)l[2]==0xBF) l+=3;
        excl_add(l);
    }
    fclose(f);
}
/* ---- the SEARCH BOX TEXT persists too (hogs_search.txt), 2026-10-28 ----
   Only Enter-pinned chips used to survive a restart; a "-gintama" typed in the box (Enter not pressed) was
   gone next launch — "I lose my negative search terms". The box now comes back exactly as left, so typed
   exclusions stay until the user deletes them. Saved ~1.5s after typing stops, and on quit. */
static unsigned g_srch_saved_ver = 0;
static void srch_text_save(void){
    if(g_nosession) return;
    char p[1100]; sess_file(p,sizeof p,"hogs_search.txt");
    FILE *f=path_fopen(p,L"wb"); if(!f) return;
    fputs(g_pl_search,f); fclose(f);
}
static void srch_text_load(void){
    if(g_nosession) return;
    char p[1100]; sess_file(p,sizeof p,"hogs_search.txt");
    FILE *f=path_fopen(p,L"rb"); if(!f) return;
    char line[sizeof g_pl_search]; size_t n=fread(line,1,sizeof line-1,f); fclose(f); line[n]=0;
    for(char *q=line;*q;q++) if(*q=='\r'||*q=='\n'||*q=='\t') *q=' ';
    snprintf(g_pl_search,sizeof g_pl_search,"%s",line);
    g_pl_search_len=(int)strlen(g_pl_search); g_pl_caret=g_pl_anchor=g_pl_search_len; g_srch_ver++;
    g_srch_saved_ver=g_srch_ver;
}
/* main loop: save once the text has been unchanged for 1.5s */
static void srch_text_autosave(void){
    static unsigned seen=0; static Uint32 since=0;
    if(g_srch_ver==g_srch_saved_ver) return;
    if(g_srch_ver!=seen){ seen=g_srch_ver; since=SDL_GetTicks(); return; }
    if(SDL_GetTicks()-since>1500){ srch_text_save(); g_srch_saved_ver=g_srch_ver; }
}
/* Enter in the search box: move every typed -word into the persistent exclude list (chips). */
static int srch_pin_negatives(void){
    char out[sizeof g_pl_search]; int on=0, handled=0;
    char msg[200]; int ml=snprintf(msg,sizeof msg,"Hiding:");
    const char *s=g_pl_search, *b, *e; char w[64]; int neg;
    while(srch_next_term(&s, w, sizeof w, &neg, &b, &e)){
        if(neg){
            int r=excl_add(w);                         /* the whole phrase becomes ONE chip */
            if(r){ handled++; if(ml<(int)sizeof msg-70) ml+=snprintf(msg+ml,sizeof msg-ml," \"%s\"",w); continue; }
        }
        int len=(int)(e-b);                            /* keep search words exactly as typed */
        if(on && on<(int)sizeof out-1) out[on++]=' ';
        if(on+len<(int)sizeof out){ memcpy(out+on,b,(size_t)len); on+=len; }
    }
    if(!handled) return 0;
    out[on]=0; memcpy(g_pl_search,out,(size_t)on+1); g_pl_search_len=on; g_pl_caret=g_pl_anchor=on; g_srch_ver++;
    excl_save(); toast(msg);
    return handled;
}
/* lazily-probed per-row info (duration via a quick header open; size via file attributes) */
static double pl_probe_duration(const char *path){
    AVFormatContext *f=NULL; double d=-2;                    /* -2 = tried, unusable */
    if(avformat_open_input(&f,path,NULL,NULL)==0){
        if(f->duration>0) d=(double)f->duration/AV_TIME_BASE;
        else if(avformat_find_stream_info(f,NULL)>=0 && f->duration>0) d=(double)f->duration/AV_TIME_BASE;
        { double fx = ps_duration_fix(f, path); if(fx > 0) d = fx; }   /* VOB with restarting timestamps */
        avformat_close_input(&f);
    }
    return d;
}
static long long pl_file_size(const char *path){
    wchar_t w[1300]; if(MultiByteToWideChar(CP_UTF8,0,path,-1,w,1300)<=0) return -2;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if(!GetFileAttributesExW(w,GetFileExInfoStandard,&fad)) return -2;
    return ((long long)fad.nFileSizeHigh<<32)|fad.nFileSizeLow;
}
/* last-write time as FILETIME ticks (100ns since 1601) in a double; -1 if unreadable */
static double pl_file_mtime(const char *path){
    wchar_t w[1300]; if(MultiByteToWideChar(CP_UTF8,0,path,-1,w,1300)<=0) return -1;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if(!GetFileAttributesExW(w,GetFileExInfoStandard,&fad)) return -1;
    return (double)((((unsigned long long)fad.ftLastWriteTime.dwHighDateTime)<<32)|fad.ftLastWriteTime.dwLowDateTime);
}
static void pl_fmt_info(char *buf,size_t cap,double dur,long long sz){
    char db[24]="", sb[24]="";
    if(dur>=0){ int s=(int)(dur+0.5);
        if(s>=3600) snprintf(db,sizeof db,"%d:%02d:%02d",s/3600,(s/60)%60,s%60);
        else        snprintf(db,sizeof db,"%d:%02d",s/60,s%60); }
    if(sz>=0){ double g=sz/1073741824.0, m=sz/1048576.0;
        if(g>=1.0)      snprintf(sb,sizeof sb,"%.1fG",g);
        else if(m>=1.0) snprintf(sb,sizeof sb,"%.0fM",m);
        else            snprintf(sb,sizeof sb,"%.0fK",sz/1024.0); }
    snprintf(buf,cap,"%s %s",db,sb);
}
/* ---- background row prober (2026-10-08) ----
   The panel used to fill each newly visible row's size + duration ON THE MAIN THREAD, one
   avformat_open_input (+ find_stream_info) per frame. On the media drive that measured up to ~135 ms per
   file, so every search keystroke (which brings a fresh set of rows on screen) froze the video for a few
   frames — the "changing the search lags playback" bug. Rows now QUEUE a request; one worker thread does
   the slow I/O and the panel fills the numbers in when they arrive. Requests carry the row's pointer
   (key) AND its path, so a result still lands on the right row after a drag/sort/remove, and is
   discarded if the row is gone. Requests for rows that scrolled away go stale and are dropped. */
#define PRB_Q 48
typedef struct { const char *key; int idx, need_dur, need_sz, state; Uint32 stamp; double dur; long long sz; char path[2100]; } PrbJob;
static PrbJob g_prb[PRB_Q];               /* state: 0 free, 1 queued, 2 probing, 3 done */
static SDL_mutex *g_prb_mx = NULL; static SDL_sem *g_prb_sem = NULL; static int g_prb_ok = 0;
static Uint32 g_prb_frame = 0;
static int prb_thread(void *ud){
    (void)ud;
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
    for(;;){
        SDL_SemWait(g_prb_sem);
        if(P.quit) return 0;
        SDL_LockMutex(g_prb_mx);
        int best = -1;                     /* the most recently wanted row first (what is on screen NOW) */
        for(int i=0;i<PRB_Q;i++) if(g_prb[i].state==1 && (best<0 || g_prb[i].stamp > g_prb[best].stamp)) best = i;
        char path[2100]; int nd = 0, ns = 0;
        if(best >= 0){ g_prb[best].state = 2; memcpy(path, g_prb[best].path, sizeof path); nd = g_prb[best].need_dur; ns = g_prb[best].need_sz; }
        SDL_UnlockMutex(g_prb_mx);
        if(best < 0) continue;
        long long sz = ns ? pl_file_size(path) : -1;
        double dur = nd ? pl_probe_duration(path) : -1;
        SDL_LockMutex(g_prb_mx);
        g_prb[best].sz = sz; g_prb[best].dur = dur; g_prb[best].state = 3;
        SDL_UnlockMutex(g_prb_mx);
    }
}
static int prb_init(void){
    if(g_prb_ok) return 1;
    g_prb_mx = SDL_CreateMutex(); g_prb_sem = SDL_CreateSemaphore(0);
    if(!g_prb_mx || !g_prb_sem) return 0;
    SDL_Thread *t = SDL_CreateThread(prb_thread, "pl_probe", NULL); if(!t) return 0;
    SDL_DetachThread(t); g_prb_ok = 1; return 1;
}
/* once per panel frame: apply finished results, drop requests for rows no longer on screen */
static void prb_poll(void){
    g_prb_frame++;
    if(!g_prb_ok) return;
    SDL_LockMutex(g_prb_mx);
    for(int i=0;i<PRB_Q;i++){
        PrbJob *j = &g_prb[i];
        if(j->state==1 && g_prb_frame - j->stamp > 30){ j->state = 0; continue; }   /* scrolled away */
        if(j->state!=3) continue;
        int r = -1;
        if(j->idx>=0 && j->idx<pl_n && pl_items[j->idx]==j->key && !strcmp(pl_items[j->idx], j->path)) r = j->idx;
        else for(int k=0;k<pl_n;k++) if(pl_items[k]==j->key && !strcmp(pl_items[k], j->path)){ r = k; break; }
        if(r >= 0){
            if(j->need_sz  && pl_sz  && pl_sz[r]==-1)  pl_sz[r]  = j->sz;
            if(j->need_dur && pl_dur && pl_dur[r]==-1) pl_dur[r] = j->dur;
        }
        j->state = 0;
    }
    SDL_UnlockMutex(g_prb_mx);
}
/* row idx is on screen and lacks size/duration: make sure a probe for it is queued */
static void prb_want(int idx, int need_sz, int need_dur){
    if(!prb_init()){                                   /* no thread: old synchronous behaviour */
        if(need_sz) pl_sz[idx] = pl_file_size(pl_items[idx]);
        if(need_dur) pl_dur[idx] = pl_probe_duration(pl_items[idx]);
        return;
    }
    SDL_LockMutex(g_prb_mx);
    int fr = -1;
    for(int i=0;i<PRB_Q;i++){
        if(g_prb[i].state && g_prb[i].key==pl_items[idx]){
            if(g_prb[i].state==1) g_prb[i].stamp = g_prb_frame;   /* still wanted */
            SDL_UnlockMutex(g_prb_mx); return; }
        if(!g_prb[i].state && fr<0) fr = i;
    }
    if(fr >= 0){
        PrbJob *j = &g_prb[fr];
        j->key = pl_items[idx]; j->idx = idx; j->need_sz = need_sz; j->need_dur = need_dur;
        j->stamp = g_prb_frame; snprintf(j->path, sizeof j->path, "%s", pl_items[idx]);
        j->state = 1; SDL_SemPost(g_prb_sem);
    }
    SDL_UnlockMutex(g_prb_mx);
}
/* The FILTERED VIEW (rows passing pl_match), cached. It used to be rebuilt by scanning every row on
   EVERY frame the panel was open — 11k paths x each search/exclude term, up to 144 times a second, on
   the thread that presents video — which is what made typing in the search box stutter playback
   (2026-10-08). Now it is rebuilt only when the terms, the type chip, or the playlist itself changes
   (count or order: an order-sensitive hash of the row pointers, ~20us). */
static int *g_view = NULL; static int g_view_cap = 0, g_view_n = 0;
static unsigned g_view_sig = 0; static int g_view_ok = 0;
static const int *pl_view(int *n){
    unsigned h = 2166136261u;
    h = (h ^ g_srch_ver) * 16777619u; h = (h ^ (unsigned)g_pl_filter) * 16777619u; h = (h ^ (unsigned)pl_n) * 16777619u;
    for(int i=0;i<pl_n;i++){ uintptr_t p=(uintptr_t)pl_items[i]; h = (h ^ (unsigned)(p>>4) ^ (unsigned)(p>>36)) * 16777619u; }
    if(!g_view_ok || h != g_view_sig || g_view_cap < pl_n){
        Uint64 t0 = SDL_GetPerformanceCounter();
        if(g_view_cap < pl_n){ int *t = realloc(g_view, (size_t)pl_n*sizeof(int));
            if(!t){ *n = 0; return g_view; } g_view = t; g_view_cap = pl_n; }
        g_view_n = 0;
        for(int i=0;i<pl_n;i++) if(pl_match(i)) g_view[g_view_n++] = i;
        g_view_sig = h; g_view_ok = 1;
        if(getenv("HOGS_PROF")) fprintf(stderr,"[prof] playlist view rebuilt: %d/%d rows in %.2f ms\n", g_view_n, pl_n,
            (double)(SDL_GetPerformanceCounter()-t0)*1000.0/(double)SDL_GetPerformanceFrequency());
    }
    *n = g_view_n; return g_view;
}
static void draw_playlist_panel(void){
    float W=g_win_w, H=g_win_h;
    if(g_pl_w <= 0){ g_pl_w = W*0.34f; if(g_pl_w<320)g_pl_w=320; if(g_pl_w>520)g_pl_w=520; } /* first use */
    float pw = g_pl_w; if(pw<300)pw=300; if(pw>W*0.92f)pw=W*0.92f; g_pl_w=pw;
    float px=W-pw, py=0, ph=H-150; if(ph<120)ph=H;
    ui_rect4(px,py,pw,ph, TH->panel);
    /* drag handle on the left edge — widen the panel to read long filenames (THICK = easy to grab) */
    int onHandle = (g_mouse_x>=px-14 && g_mouse_x<=px+6 && g_mouse_y>=py && g_mouse_y<=py+ph);
    if(onHandle||g_pl_resizing) ui_rect(px-5, py, 10, ph, TH->accent[0],TH->accent[1],TH->accent[2],0.9f);
    else                        ui_rect(px-5, py, 10, ph, TH->track[0],TH->track[1],TH->track[2],0.9f);
    ui_text4(px+12, py+8, 17, "Playlist", TH->text);
    /* header: Add Files / Add Folder / Clear(trash) */
    float bh=30, by=py+32, bx=px+10, gap=6;
    float w1=icon_btn_w(IC_ADD,"Files",bh);    icon_button(bx,by,w1,bh,IC_ADD,"Files",0,B_ADDF);   bx+=w1+gap;
    float w2=icon_btn_w(IC_FOLDER,"Folder",bh); icon_button(bx,by,w2,bh,IC_FOLDER,"Folder",0,B_ADDD); bx+=w2+gap;
    { int narrow = pw < 380;                     /* a narrow panel keeps the Save/Open icons, drops their labels */
      float w4=icon_btn_w(IC_SAVE,narrow?NULL:"Save",bh); icon_button(bx,by,w4,bh,IC_SAVE,narrow?NULL:"Save",0,B_SAVEPL); bx+=w4+gap;
      float w5=icon_btn_w(IC_LOAD,narrow?NULL:"Open",bh); icon_button(bx,by,w5,bh,IC_LOAD,narrow?NULL:"Open",0,B_LOADPL); bx+=w5+gap; }
    float w3=icon_btn_w(IC_TRASH,NULL,bh);     icon_button(px+pw-10-w3,by,w3,bh,IC_TRASH,NULL,0,B_CLEAR);
    /* search box (click to focus, then type; Enter plays first match, Esc clears focus) */
    float sy=by+bh+8, sbh=28, sw=pw-20;
    int sf=g_pl_search_active;
    ui_rect4(px+10, sy, sw, sbh, sf ? TH->btn : TH->track);
    if(sf) ui_rect4(px+10, sy+sbh-2, sw, 2, TH->accent);
    g_srch_text_x = px+18;                       /* click hit-testing measures from here */
    if(g_pl_anchor > g_pl_search_len) g_pl_anchor = g_pl_search_len;
    if(sf && srch_has_sel()){                   /* selected text: accent band behind the glyphs */
        int a,b; srch_sel_range(&a,&b);
        float x0=px+18+srch_prefix_w(a), x1=px+18+srch_prefix_w(b);
        ui_rect(x0, sy+4, x1-x0, 20, TH->accent[0],TH->accent[1],TH->accent[2],0.45f);
    }
    if(g_pl_search[0]) ui_text4(px+18, sy+5, 15, g_pl_search, TH->text);
    else               ui_text4(px+18, sy+5, 15, sf ? "words to find,  -word + Enter hides it" : "Search...   (-word + Enter hides titles)", TH->dim);
    if(sf){                                      /* caret sits AT the insertion point, not at the end */
        if(g_pl_caret > g_pl_search_len) g_pl_caret = g_pl_search_len;
        ui_rect4(px+18+srch_prefix_w(g_pl_caret)+1, sy+5, 2, 17, TH->text);
    }
    osd_addbtn(px+10, sy, sw, sbh, B_SEARCH);
    /* media-type filter chips */
    float fy=sy+sbh+6, fh=24, fx=px+10;
    const char *FN[4]={"All","Video","Audio","Image"}; int FID[4]={B_FALL,B_FVID,B_FAUD,B_FIMG};
    for(int k=0;k<4;k++){
        float fw=ui_textw(13,FN[k])+18;
        int act=(g_pl_filter==k);
        ui_rect4(fx,fy,fw,fh, act ? TH->btn_act : TH->btn);
        if(g_mouse_x>=fx&&g_mouse_x<=fx+fw&&g_mouse_y>=fy&&g_mouse_y<=fy+fh) ui_rect4(fx,fy,fw,fh, TH->hover);
        ui_text4(fx+9, fy+4, 13, FN[k], act ? TH->text_on : TH->text);
        osd_addbtn(fx,fy,fw,fh,FID[k]); fx+=fw+5;
    }
    /* sort controls: Title / Size / Length — click the active one again to reverse (▲ asc / ▼ desc) */
    float qy=fy+fh+6, qh=22, qx=px+10;
    ui_text4(qx, qy+4, 13, "Sort", TH->dim); qx += ui_textw(13,"Sort")+8;
    const char *SN[4]={"Title","Size","Length","Date"}; int SID[4]={B_SORTNAME,B_SORTSIZE,B_SORTLEN,B_SORTDATE};
    for(int k=0;k<4;k++){
        char lbl[24]; snprintf(lbl,sizeof lbl,"%s%s", SN[k], g_pl_sort_key==k?(g_pl_sort_dir>0?" \xE2\x96\xB2":" \xE2\x96\xBC"):"");
        float qw=ui_textw(13,lbl)+16; int act=(g_pl_sort_key==k);
        ui_rect4(qx,qy,qw,qh, act ? TH->btn_act : TH->btn);
        if(g_mouse_x>=qx&&g_mouse_x<=qx+qw&&g_mouse_y>=qy&&g_mouse_y<=qy+qh) ui_rect4(qx,qy,qw,qh, TH->hover);
        ui_text4(qx+8, qy+3, 13, lbl, act ? TH->text_on : TH->text);
        osd_addbtn(qx,qy,qw,qh,SID[k]); qx+=qw+6;
    }
    /* pinned exclude words as chips — click one to show those titles again */
    float ctl_bottom = qy+qh;
    if(g_excl_n>0){
        float lw=ui_textw(13,"Hiding")+8, cxx=px+10+lw, cyy=qy+qh+6, ch=22, maxx=px+pw-14;
        ui_text4(px+10, cyy+4, 13, "Hiding", TH->dim);
        for(int i=0;i<g_excl_n;i++){
            char lbl[80]; snprintf(lbl,sizeof lbl,"%s  \xC3\x97",g_excl[i]);       /* word  x */
            float cw=ui_textw(13,lbl)+16;
            if(cxx+cw>maxx && cxx>px+10+lw){ cxx=px+10+lw; cyy+=ch+4; }
            int hov=(g_mouse_x>=cxx&&g_mouse_x<=cxx+cw&&g_mouse_y>=cyy&&g_mouse_y<=cyy+ch);
            ui_rect4(cxx,cyy,cw,ch, TH->btn);
            ui_rect(cxx,cyy,3,ch, TH->accent[0],TH->accent[1],TH->accent[2],0.9f);
            if(hov) ui_rect4(cxx,cyy,cw,ch, TH->hover);
            ui_text4(cxx+9, cyy+3, 13, lbl, TH->text);
            osd_addbtn(cxx,cyy,cw,ch,B_EXCL_BASE+i); cxx+=cw+5;
        }
        ctl_bottom = cyy+ch;
    }
    /* the filtered view (real indices, cached — see pl_view); scroll + render only matching rows */
    int nf=0; const int *fil=pl_view(&nf);
    float rowH=26, top=ctl_bottom+10;
    int maxRows = (int)((py+ph-26-top)/rowH); if(maxRows<1) maxRows=1;
    int maxFirst = nf>maxRows ? nf-maxRows : 0;
    if(g_pl_jump){ g_pl_jump=0;                       /* a new file started: scroll it into view */
        for(int k=0;k<nf;k++) if(fil[k]==pl_cur){ pl_scroll=(float)(k-maxRows/2); break; } }
    if(g_pl_jump_sel){ g_pl_jump_sel=0;               /* restored / newly-added selection: bring it into view */
        for(int k=0;k<nf;k++) if(fil[k]==g_pl_selected){ pl_scroll=(float)(k-maxRows/2); break; } }
    if(pl_scroll < 0) pl_scroll = 0; if(pl_scroll > maxFirst) pl_scroll = maxFirst;
    int first = (int)pl_scroll;
    prb_poll();                                       /* sizes/durations that finished probing off-thread */
    float rowW = pw-30;                               /* rows leave room for the THICK scrollbar */
    for(int i=0;i<maxRows && first+i<nf;i++){
        int idx=fil[first+i]; float ry=top+i*rowH;
        if(idx==pl_cur) ui_rect4(px+6,ry-2,rowW,rowH-2, TH->row_cur);
        else if((pl_sel&&pl_sel[idx])||idx==g_pl_selected) ui_rect4(px+6,ry-2,rowW,rowH-2, TH->row_sel);
        else if(g_mouse_x>=px+6 && g_mouse_x<=px+6+rowW && g_mouse_y>=ry-2 && g_mouse_y<ry-2+rowH-2) ui_rect(px+6,ry-2,rowW,rowH-2, TH->hover[0],TH->hover[1],TH->hover[2],0.07f);
        const char*nm=pl_items[idx]; const char*bn=nm; for(const char*q=nm;*q;q++) if(*q=='/'||*q=='\\')bn=q+1;
        int mk=media_kind(nm); const char *tag = mk==1?"♪ ":mk==2?"▣ ":"▶ ";
        /* lazily fill size + duration for the rows on screen — OFF the main thread (see prb_thread) */
        if(pl_dur && pl_dur[idx]==-1 && mk==2) pl_dur[idx]=-2;   /* images have no duration */
        { int ns = pl_sz && pl_sz[idx]==-1, nd = pl_dur && pl_dur[idx]==-1;
          if(ns || nd) prb_want(idx, ns, nd); }
        char info[48]=""; if(pl_dur&&pl_sz) pl_fmt_info(info,sizeof info,pl_dur[idx],pl_sz[idx]);
        float iw = info[0]? ui_textw(12,info) : 0;
        char line[300]; snprintf(line,sizeof(line),"%s%d. %.250s", tag, idx+1, bn);  /* full name; widen panel to read it */
        ui_text4(px+12, ry+2, 15, line, TH->text);
        if(iw>0){ ui_rect(px+6+rowW-iw-10, ry-1, iw+10, rowH-4, TH->panel[0],TH->panel[1],TH->panel[2],0.85f);  /* backing keeps it readable */
                  ui_text4(px+6+rowW-iw-5, ry+4, 12, info, TH->dim); }
        osd_addbtn(px+6,ry-2,rowW,rowH-2, B_PLITEM_BASE+idx);
    }
    /* reorder drag: draw the insertion line at the nearest row gap + a floating label, publish the drop target */
    if(g_pl_dragging && g_pl_drag_idx>=0 && g_pl_drag_idx<pl_n){
        int shown=maxRows; if(first+shown>nf)shown=nf-first; if(shown<0)shown=0;
        int k=(int)((g_mouse_y-top)/rowH + 0.5f); if(k<0)k=0; if(k>shown)k=shown;
        float ly=top+k*rowH-2;
        g_pl_drop_at = (first+k<nf) ? fil[first+k] : pl_n;
        ui_rect(px+6, ly-1, rowW, 3, TH->accent[0],TH->accent[1],TH->accent[2],1.0f);
        char lbl[300]; int nsel=(pl_sel&&pl_sel[g_pl_drag_idx])?pl_selcount():1;
        if(nsel>1) snprintf(lbl,sizeof lbl,"Move %d items",nsel);
        else { const char*dn=pl_items[g_pl_drag_idx]; for(const char*q=dn;*q;q++) if(*q=='/'||*q=='\\')dn=q+1; snprintf(lbl,sizeof lbl,"%.250s",dn); }
        float lw=ui_textw(14,lbl)+16, lx=px+12, lyy=(float)g_mouse_y-10;
        ui_rect(lx,lyy,lw,22, TH->btn[0],TH->btn[1],TH->btn[2],0.92f);
        ui_text4(lx+8,lyy+3,14,lbl,TH->text);
    } else g_pl_drop_at=-1;
    /* THICK draggable scrollbar for the list (wheel still works) */
    pl_sb_x0=px+pw-20; pl_sb_x1=px+pw-4; pl_sb_y0=top; pl_sb_y1=py+ph-28;
    g_pl_rows_vis=(float)maxRows; g_pl_rows_tot=(float)(nf>0?nf:1);
    if(nf>maxRows){
        float trH=pl_sb_y1-pl_sb_y0;
        float thH=trH*(float)maxRows/nf; if(thH<28)thH=28;
        float thY=pl_sb_y0 + (trH-thH)*(maxFirst>0? pl_scroll/maxFirst : 0);
        int onSb = (g_mouse_x>=pl_sb_x0-4 && g_mouse_x<=pl_sb_x1+4 && g_mouse_y>=pl_sb_y0 && g_mouse_y<=pl_sb_y1);
        ui_rect(pl_sb_x0,pl_sb_y0,pl_sb_x1-pl_sb_x0,trH, TH->track[0],TH->track[1],TH->track[2],0.9f);
        if(onSb||g_pl_sb_drag) ui_rect(pl_sb_x0,thY,pl_sb_x1-pl_sb_x0,thH, TH->accent[0],TH->accent[1],TH->accent[2],0.95f);
        else                   ui_rect(pl_sb_x0,thY,pl_sb_x1-pl_sb_x0,thH, TH->fill[0],TH->fill[1],TH->fill[2],0.75f);
    }
    char foot[128]; snprintf(foot,sizeof(foot),"%d / %d shown   Click=select  Drag=reorder  DblClick=play  Del=remove", nf, pl_n);
    ui_text4(px+12, py+ph-22, 12, foot, TH->dim);
}
static void ui_flush(void){
    if(!ui_prog || ui_n==0) return;
    glViewport(0,0,g_win_w,g_win_h);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    p_glUseProgram(ui_prog);
    p_glBindVertexArray(ui_vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, ui_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, (long)ui_n*sizeof(float), ui_buf, GL_DYNAMIC_DRAW);
    p_glUniform2f(uiu_screen, (float)g_win_w, (float)g_win_h);
    p_glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, ui_atlas);
    p_glUniform1i(uiu_tex, 0);
    glDrawArrays(GL_TRIANGLES, 0, ui_n/8);   /* 8 floats per vertex */
    glDisable(GL_BLEND);
}

/* draw the active subtitle cue (centred, multi-line, with a dark backing) */
static void draw_subtitle(double clk){
    if(g_nsubs==0 || !ui_prog) return;
    /* Cues are in INSERTION order (embedded subs re-demux out of order across seeks), and real
       subtitle files legitimately OVERLAP (one cue's end past the next cue's start). Taking the
       FIRST match therefore showed a stale/older line and flickered between the two as the clock
       moved — the "overlapping / flashing previous" bug. Pick the LATEST-STARTING cue that
       contains the clock: that is always the line currently being spoken. */
    const char *txt=NULL; double best=-1e18;
    for(int i=0;i<g_nsubs;i++)
        if(clk>=g_subs[i].s && clk<g_subs[i].e && g_subs[i].s>best){ best=g_subs[i].s; txt=g_subs[i].t; }
    if(!txt || !txt[0]) return;
    char buf[256]; strncpy(buf,txt,255); buf[255]=0;
    char *lines[6]; int nl=0; lines[nl++]=buf;
    for(char *p=buf; *p && nl<6; p++){ if(*p=='\n'){ *p=0; lines[nl++]=p+1; } }
    float px=30, lineH=px*1.25f, W=g_win_w;
    float baseY = g_win_h - (osd_visible?108.0f:54.0f);
    float startY = baseY - (nl-1)*lineH;
    for(int i=0;i<nl;i++){
        if(!lines[i][0]) continue;
        float tw=ui_textw(px,lines[i]), tx=(W-tw)*0.5f, ty=startY+i*lineH;
        ui_rect(tx-10, ty-3, tw+20, lineH, 0,0,0,0.55f);
        ui_text(tx, ty, px, lines[i], 1,1,1,1);
    }
}

/* ---- audio visualizers ---- */
static void viz_fft(float *re, float *im, int n){
    for(int i=1,j=0;i<n;i++){ int bit=n>>1; for(; j&bit; bit>>=1) j^=bit; j^=bit;
        if(i<j){ float t; t=re[i];re[i]=re[j];re[j]=t; t=im[i];im[i]=im[j];im[j]=t; } }
    for(int len=2; len<=n; len<<=1){
        float ang=-2*VIZ_PI/len, wr=cosf(ang), wi=sinf(ang);
        for(int i=0;i<n;i+=len){
            float cr=1, ci=0;
            for(int k=0;k<len/2;k++){
                float xr=re[i+k+len/2]*cr - im[i+k+len/2]*ci;
                float xi=re[i+k+len/2]*ci + im[i+k+len/2]*cr;
                re[i+k+len/2]=re[i+k]-xr; im[i+k+len/2]=im[i+k]-xi;
                re[i+k]+=xr; im[i+k]+=xi;
                float ncr=cr*wr-ci*wi, nci=cr*wi+ci*wr; cr=ncr; ci=nci;
            }
        }
    }
}
static void viz_hsv(float h,float s,float v,float*r,float*g,float*b){
    float i=floorf(h*6), f=h*6-i, p=v*(1-s), q=v*(1-f*s), t=v*(1-(1-f)*s);
    switch(((int)i)%6){ case 0:*r=v;*g=t;*b=p;break; case 1:*r=q;*g=v;*b=p;break; case 2:*r=p;*g=v;*b=t;break;
        case 3:*r=p;*g=q;*b=v;break; case 4:*r=t;*g=p;*b=v;break; default:*r=v;*g=p;*b=q;break; }
}
static int utf8_enc(int cp, char *o){
    if(cp<0x80){ o[0]=cp; o[1]=0; return 1; }
    if(cp<0x800){ o[0]=0xC0|(cp>>6); o[1]=0x80|(cp&0x3F); o[2]=0; return 2; }
    if(cp<0x10000){ o[0]=0xE0|(cp>>12); o[1]=0x80|((cp>>6)&0x3F); o[2]=0x80|(cp&0x3F); o[3]=0; return 3; }
    o[0]=0xF0|(cp>>18); o[1]=0x80|((cp>>12)&0x3F); o[2]=0x80|((cp>>6)&0x3F); o[3]=0x80|(cp&0x3F); o[4]=0; return 4;
}
/* Unicode matrix rain (showcases the glyph atlas); audio-reactive fall speed */
/* ===== The Matrix: digital rain over EVERY renderable Unicode glyph ===================== */
/* Pool of every code point the loaded font can actually draw (Latin, Greek, Cyrillic, Hebrew,
   Arabic, kana, thousands of kanji, symbols, arrows, math, box-drawing, …). A rotating "active
   set" keeps a bounded number resident in the glyph atlas at once, while slowly swapping in
   fresh code points so that, over time, the rain marches through the whole of Unicode. */
#define MTX_MAXCOL 640
#define MTX_ACTIVE 600          /* distinct glyphs shown at once (comfortably fits the 2048 atlas) */
static int *g_uni_pool = NULL; static int g_uni_n = 0;
static int  g_mtx_active[MTX_ACTIVE]; static int g_mtx_ready = 0;
static unsigned g_mtx_rng = 0x1337beefu;
static unsigned mtx_rand(void){ g_mtx_rng ^= g_mtx_rng<<13; g_mtx_rng ^= g_mtx_rng>>17; g_mtx_rng ^= g_mtx_rng<<5; return g_mtx_rng; }
static void build_unicode_pool(void){
    if(g_uni_pool || !g_font_ok) return;
    int cap = 1<<15; g_uni_pool = malloc((size_t)cap*sizeof(int)); if(!g_uni_pool) return;
    int n=0;
    for(int cp=0x21; cp<=0xFFFF; cp++){               /* the whole BMP */
        if(cp>=0xD800 && cp<=0xDFFF) continue;        /* surrogate halves (not characters) */
        if(cp>=0x2000 && cp<=0x200F) continue;        /* spaces / zero-width (invisible) */
        if((cp & 0xFFFE) == 0xFFFE) continue;         /* non-characters U+xFFFE/U+xFFFF */
        if(stbtt_FindGlyphIndex(&g_font, cp)){        /* keep only glyphs the font can render */
            if(n>=cap){ cap*=2; int *t=realloc(g_uni_pool,(size_t)cap*sizeof(int)); if(!t) break; g_uni_pool=t; }
            g_uni_pool[n++]=cp;
        }
    }
    g_uni_n = n;
    fprintf(stderr,"[matrix] unicode glyph pool = %d code points\n", n);
    for(int i=0;i<MTX_ACTIVE;i++) g_mtx_active[i] = n ? g_uni_pool[mtx_rand()%(unsigned)n] : (0x30A0+(int)(mtx_rand()%96));
    g_mtx_ready = (n>0);
}
static void draw_matrix(void){
    if(!g_uni_pool) build_unicode_pool();
    /* proactively recycle the atlas before it overflows, at frame start, so the whole frame
       renders from one consistent atlas (no mid-frame glyph scramble) */
    if(g_font_ok && atlas_peny > (int)(ATLAS_H*0.84f)) atlas_reset();

    float W=g_win_w, H=g_win_h;
    float px=20.0f, cellW=px*1.12f, cellH=px*1.16f;
    int cols=(int)(W/cellW); if(cols>MTX_MAXCOL) cols=MTX_MAXCOL; if(cols<1) cols=1;
    static float headY[MTX_MAXCOL]; static float spd[MTX_MAXCOL]; static int tlen[MTX_MAXCOL];
    static int init=0; static unsigned tick=0;
    if(!init){ for(int c=0;c<MTX_MAXCOL;c++){ headY[c]=-(float)(mtx_rand()%(unsigned)(H>1?(int)H:900));
        spd[c]=1.0f+(mtx_rand()%100)/38.0f; tlen[c]=14+(int)(mtx_rand()%20); } init=1; }
    tick++;
    /* audio level → fall speed, brightness, and how fast the glyph set churns */
    float amp=0; for(int i=0;i<256;i++) amp+=fabsf(g_viz_pcm[i]); amp/=256.0f;
    float boost = 1.0f + amp*6.0f;
    float lvl   = 0.55f + amp*2.4f; if(lvl>1.35f) lvl=1.35f;
    /* swap a few active glyphs for fresh ones each frame → cycles through ALL of Unicode */
    if(g_mtx_ready){ int rot = 1 + (int)(amp*30.0f); for(int k=0;k<rot;k++) g_mtx_active[mtx_rand()%MTX_ACTIVE] = g_uni_pool[mtx_rand()%(unsigned)g_uni_n]; }

    for(int c=0;c<cols;c++){
        headY[c] += spd[c]*boost;
        if(headY[c] - tlen[c]*cellH > H){            /* off the bottom → respawn at the top */
            headY[c] = -(float)(mtx_rand()%240);
            spd[c]=1.0f+(mtx_rand()%100)/38.0f; tlen[c]=14+(int)(mtx_rand()%20);
        }
        int headRow=(int)(headY[c]/cellH);
        for(int t=0;t<tlen[c];t++){
            int row=headRow - t; float gy=row*cellH;
            if(gy < -cellH || gy > H) continue;
            /* glyph: stable per (col,row) but flickers slowly as the active set rotates */
            unsigned hsh = ((unsigned)c*2654435761u) ^ ((unsigned)row*40503u);
            unsigned slot = (hsh ^ (tick>>4)) % MTX_ACTIVE;
            int cp = g_mtx_ready ? g_mtx_active[slot] : (0x30A0 + (int)(hsh % 96));
            char u[5]; utf8_enc(cp, u);
            float gw = ui_textw(px, u); float gx = c*cellW + (cellW-gw)*0.5f;   /* centre in the cell */
            float r,g,b,a;
            if(t==0){ r=0.82f; g=1.0f; b=0.86f; a=1.0f; }                       /* near-white head */
            else if(t==1){ r=0.45f; g=1.0f; b=0.55f; a=0.95f; }                 /* bright green */
            else { float f=1.0f-(float)t/tlen[c]; r=0.0f; g=0.32f+0.62f*f; b=0.12f*f; a=0.22f+0.7f*f; }
            ui_text(gx, gy, px, u, r*lvl, g*lvl, b*lvl, a);
        }
    }
}
/* ===== visualizer helpers (bars / wave / radial — the Matrix mode is separate & untouched) ===== */
#define VIZ_BANDS 64
static float g_vz_band[VIZ_BANDS];   /* smoothed magnitude 0..1 (fast attack, slow release) */
static float g_vz_peak[VIZ_BANDS];   /* floating peak caps */
static float g_vz_vel [VIZ_BANDS];   /* peak fall velocity (accelerating) */
static float g_vz_rot=0.0f, g_vz_beat=0.0f;   /* rotation phase, bass-driven beat envelope */
static float vz_wrap(float h){ h-=floorf(h); return h; }   /* hue into [0,1) */
/* thick line segment as a perpendicular quad (for glowing strokes / spokes / rings) */
static void viz_seg(float x0,float y0,float x1,float y1,float th,float r,float g,float b,float a){
    float dx=x1-x0, dy=y1-y0, len=sqrtf(dx*dx+dy*dy); if(len<1e-4f) return;
    float nx=-dy/len*th*0.5f, ny=dx/len*th*0.5f;
    ui_vtx(x0+nx,y0+ny,WHITE_U,WHITE_V,r,g,b,a); ui_vtx(x1+nx,y1+ny,WHITE_U,WHITE_V,r,g,b,a); ui_vtx(x1-nx,y1-ny,WHITE_U,WHITE_V,r,g,b,a);
    ui_vtx(x0+nx,y0+ny,WHITE_U,WHITE_V,r,g,b,a); ui_vtx(x1-nx,y1-ny,WHITE_U,WHITE_V,r,g,b,a); ui_vtx(x0-nx,y0-ny,WHITE_U,WHITE_V,r,g,b,a);
}
/* fill g_vz_band[]/g_vz_peak[] from the FFT: log-spaced bins, perceptual compress + high-freq tilt,
   fast-attack/slow-release smoothing, accelerating peak caps, and a bass beat envelope. */
static float g_vz_agc = 0.05f;   /* running loudness estimate → auto-gain, adapts to any track level */
static void viz_analyze(float *re,float *im,int N){
    float raw[VIZ_BANDS]; float fmax=1e-4f;
    for(int bnd=0;bnd<VIZ_BANDS;bnd++){
        int lo=1+(int)(powf((float)bnd/VIZ_BANDS,2.0f)*(N/2-2));
        int hi=1+(int)(powf((float)(bnd+1)/VIZ_BANDS,2.0f)*(N/2-2)); if(hi<=lo)hi=lo+1;
        float mag=0; for(int k=lo;k<hi&&k<N/2;k++){ float m=sqrtf(re[k]*re[k]+im[k]*im[k]); if(m>mag)mag=m; }
        raw[bnd]=mag*(0.75f+1.5f*(float)bnd/VIZ_BANDS);   /* gentle high-freq tilt */
        if(raw[bnd]>fmax) fmax=raw[bnd];
    }
    /* AGC: track the loudest band so the display fills the range at any volume. Rise quickly to a
       new peak, decay slowly, floor so silence doesn't blow up the gain into noise. */
    if(fmax>g_vz_agc) g_vz_agc = g_vz_agc*0.80f + fmax*0.20f;
    else              g_vz_agc *= 0.995f;
    if(g_vz_agc<0.03f) g_vz_agc=0.03f;
    float bass=0;
    for(int bnd=0;bnd<VIZ_BANDS;bnd++){
        float v=raw[bnd]/g_vz_agc; if(v>1.0f) v=1.0f; v=powf(v,0.85f);   /* normalized, mild perceptual lift */
        if(v>g_vz_band[bnd]) g_vz_band[bnd]=g_vz_band[bnd]*0.35f+v*0.65f;   /* fast attack */
        else                 g_vz_band[bnd]=g_vz_band[bnd]*0.84f+v*0.16f;   /* slow release */
        if(g_vz_band[bnd]>=g_vz_peak[bnd]){ g_vz_peak[bnd]=g_vz_band[bnd]; g_vz_vel[bnd]=0; }
        else { g_vz_vel[bnd]+=0.010f; g_vz_peak[bnd]-=g_vz_vel[bnd]; if(g_vz_peak[bnd]<g_vz_band[bnd]) g_vz_peak[bnd]=g_vz_band[bnd]; }
        if(bnd<6) bass+=g_vz_band[bnd];
    }
    bass/=6.0f;
    if(bass>g_vz_beat) g_vz_beat=bass; else g_vz_beat*=0.90f;   /* snap up, decay down */
    g_vz_rot += g_vz_beat*0.020f;                               /* wave phase turns ONLY with the beat (audio-only) */
}
/* ================= MilkDrop-style feedback visualizer (preset engine) ===========================
   Studied against the 8355-preset MegaPack: MilkDrop's variety comes from per-frame MODULATION of a
   core parameter set (zoom, zoomexp, rot, warp, warpscale, cx/cy, sx/sy, decay) + colour flags
   (gamma/solarize/invert/darken-centre) + several WAVE shapes, and it AUTO-CYCLES presets. So this
   is a real preset engine: an authentic per-pixel warp shader, a colour/bloom composite shader, a
   table of hand-authored presets that MORPH into one another every ~24s, and 5 injected wave modes.
   (Matrix mode stays separate & untouched.) */
static const char *MK_WARP_FS =
  "#version 330 core\n"
  "in vec2 v_uv; out vec4 col;\n"
  "uniform sampler2D u_tex;\n"
  "uniform float u_zoom,u_zoomexp,u_rot,u_warp,u_warpscale,u_time,u_cx,u_cy,u_dx,u_dy,u_sx,u_sy,u_decay,u_aspect;\n"
  "void main(){\n"
  "  vec2 c=vec2(0.5,0.5);\n"                                                        /* centre FIXED */
  "  vec2 d=v_uv-c; d.x*=u_aspect;\n"
  "  float rad=length(d)+1e-6;\n"
  "  float ws=u_warpscale;\n"
  /* PURELY RADIAL warp about the fixed centre: displacement is ALONG the radius (concentric ripples)
     and a radial zoom. No x/y shear, no rotation, no translation → it is mathematically impossible for
     the image to drift left/right. u_warp (ripple) and the zoom deviation are driven ONLY by audio. */
  "  float ripple = u_warp*0.05*sin(rad*ws*10.0 - u_time)\n"
  "              + u_warp*0.025*sin(rad*ws*22.0 + u_time*0.7);\n"
  "  d += (d/rad) * ripple;\n"                                                       /* along the radius only */
  "  float z=pow(u_zoom, pow(u_zoomexp, rad*2.0-1.0));\n"
  "  d/=z;\n"                                                                        /* radial zoom */
  "  d.x/=u_aspect; vec2 uv=d+c;\n"
  "  col=vec4(texture(u_tex,uv).rgb*u_decay, 1.0);\n"
  "}\n";
static const char *MK_COMP_FS =
  "#version 330 core\n"
  "in vec2 v_uv; out vec4 col;\n"
  "uniform sampler2D u_tex; uniform vec2 u_px; uniform float u_gamma,u_brighten,u_aspect;\n"
  "uniform int u_solarize,u_invert,u_darkcenter;\n"
  "void main(){\n"
  "  vec3 c=max(texture(u_tex,v_uv).rgb - 0.018, 0.0);\n"                            /* black-crush: dim wash → true black */
  "  vec3 b=vec3(0.0);\n"                                                            /* 8-tap bloom */
  "  b+=texture(u_tex,v_uv+u_px*vec2( 2.0, 2.0)).rgb; b+=texture(u_tex,v_uv+u_px*vec2(-2.0, 2.0)).rgb;\n"
  "  b+=texture(u_tex,v_uv+u_px*vec2( 2.0,-2.0)).rgb; b+=texture(u_tex,v_uv+u_px*vec2(-2.0,-2.0)).rgb;\n"
  "  b+=texture(u_tex,v_uv+u_px*vec2( 5.0, 0.0)).rgb; b+=texture(u_tex,v_uv+u_px*vec2(-5.0, 0.0)).rgb;\n"
  "  b+=texture(u_tex,v_uv+u_px*vec2( 0.0, 5.0)).rgb; b+=texture(u_tex,v_uv+u_px*vec2( 0.0,-5.0)).rgb;\n"
  "  b*=0.125;\n"
  "  vec3 o=c + max(b-0.25,0.0)*0.9;\n"
  "  o=pow(max(o,0.0), vec3(1.0/u_gamma));\n"                                        /* gamma */
  "  if(u_solarize==1) o = o*(1.0-o)*4.0*0.6 + o*0.4;\n"                             /* solarize */
  "  if(u_invert==1) o = 1.0-o;\n"
  "  o*=u_brighten;\n"
  "  if(u_darkcenter==1){ vec2 dd=v_uv-0.5; dd.x*=u_aspect; o*=smoothstep(0.0,0.30,length(dd)); }\n"
  "  o=o/(o+0.90)*1.7;\n"                                                            /* tonemap glow */
  "  o=max(o-0.03, 0.0);\n"                                                          /* final black-crush: bg stays TRUE black */
  "  col=vec4(o,1.0);\n"
  "}\n";
/* ===== PLASMA visualizer: a full-screen flowing field computed FRESH each frame (no feedback → it
   is physically impossible to drift or accumulate a grey wash). It MORPHS IN PLACE (noise animated in
   its Z axis, never scrolled) so there is no directional movement at all. Everything — the morph
   speed, brightness, colour, turbulence — is driven ONLY by the audio; with no sound it holds still.
   Domain-warped 3D value-noise + a cosine palette = a liquid aurora that fills the whole screen. */
static const char *MK_PLASMA_FS =
  "#version 330 core\n"
  "in vec2 v_uv; out vec4 col;\n"
  "uniform float u_time,u_aspect,u_amp,u_bass,u_mid,u_treble,u_hue,u_scale,u_warp;\n"
  "uniform int u_style;\n"
  "float hash(vec3 p){ p=fract(p*0.3183099+0.1); p*=17.0; return fract(p.x*p.y*p.z*(p.x+p.y+p.z)); }\n"
  "float vn(vec3 x){ vec3 i=floor(x),f=fract(x); f=f*f*(3.0-2.0*f);\n"
  "  return mix(mix(mix(hash(i+vec3(0,0,0)),hash(i+vec3(1,0,0)),f.x),mix(hash(i+vec3(0,1,0)),hash(i+vec3(1,1,0)),f.x),f.y),\n"
  "             mix(mix(hash(i+vec3(0,0,1)),hash(i+vec3(1,0,1)),f.x),mix(hash(i+vec3(0,1,1)),hash(i+vec3(1,1,1)),f.x),f.y),f.z); }\n"
  "float fbm(vec3 p){ float s=0.0,a=0.5; for(int i=0;i<5;i++){ s+=a*vn(p); p=p*2.02+vec3(1.7,9.2,3.3); a*=0.5; } return s; }\n"
  "vec3 pal(float t){ return 0.5+0.5*cos(6.28318*(t+vec3(0.0,0.33,0.67))); }\n"
  "void main(){\n"
  "  vec2 uv=(v_uv-0.5); uv.x*=u_aspect; uv*=u_scale;\n"                  /* style sets the scale */
  "  float t=u_time;\n"
  "  vec3 q=vec3(uv, t*0.10);\n"                                          /* time in Z → morph in place */
  "  vec2 w=vec2(fbm(q), fbm(q+vec3(5.2,1.3,2.7)));\n"                    /* domain warp */
  "  vec3 q2=vec3(uv + (u_warp+u_bass*0.4)*w, t*0.16);\n"                 /* style + a touch of bass */
  "  float v=fbm(q2);\n"
  "  v=clamp((v-0.25)*1.9,0.0,1.0); v=pow(v,1.3);\n"                      /* contrast */
  "  float tt=u_hue + v*0.9 + w.x*0.25 + u_treble*0.25;\n"
  "  vec3 pa,pb,pc,pd;\n"                                                  /* per-style cosine palette */
  "  if(u_style==0){ pa=vec3(0.5); pb=vec3(0.5); pc=vec3(1.0); pd=vec3(0.0,0.33,0.67); }\n"                         /* Spectrum */
  "  else if(u_style==1){ pa=vec3(0.5,0.25,0.12); pb=vec3(0.5,0.35,0.2); pc=vec3(1.0,1.0,0.55); pd=vec3(0.0,0.12,0.22); }\n" /* Fire */
  "  else if(u_style==2){ pa=vec3(0.10,0.34,0.46); pb=vec3(0.24,0.4,0.5); pc=vec3(0.8,0.9,1.0); pd=vec3(0.4,0.55,0.7); }\n"  /* Ocean */
  "  else { pa=vec3(0.45,0.18,0.45); pb=vec3(0.45,0.3,0.45); pc=vec3(1.0,0.85,1.0); pd=vec3(0.1,0.3,0.55); }\n"              /* Magenta */
  "  vec3 c=pa+pb*cos(6.28318*(pc*tt+pd));\n"
  "  c*=0.22 + v*1.25;\n"                                                 /* structure/contrast */
  "  c*=0.20 + u_amp*1.25 + u_bass*0.30;\n"                               /* GENTLE brightness — no flashing */
  "  c+=vec3(0.9,0.95,1.0)*smoothstep(0.82,1.0,v)*(0.22+u_mid*0.5);\n"    /* soft highlights */
  "  col=vec4(c,1.0);\n"
  "}\n";
static GLuint mk_plasma=0; static GLint mkp_time,mkp_aspect,mkp_amp,mkp_bass,mkp_mid,mkp_treble,mkp_hue,mkp_scale,mkp_warp,mkp_style;
static GLuint mk_fbo[2]={0,0}, mk_tex[2]={0,0}, mk_warp=0, mk_comp=0;
static int mk_w=0, mk_h=0, mk_cur=0, mk_ready=0, mk_fail=0;
static GLint mkw_tex,mkw_zoom,mkw_zoomexp,mkw_rot,mkw_warp,mkw_warpscale,mkw_time,mkw_cx,mkw_cy,mkw_dx,mkw_dy,mkw_sx,mkw_sy,mkw_decay,mkw_aspect;
static GLint mkc_tex,mkc_px,mkc_gamma,mkc_brighten,mkc_solarize,mkc_invert,mkc_darkcenter,mkc_aspect;
static int mk_init(void){
    if(mk_fail) return 0;
    int W=g_win_w, H=g_win_h; if(W<16||H<16) return 0;
    if(mk_ready && mk_w==W && mk_h==H) return 1;
    if(!mk_warp){                                   /* compile the two programs once */
        GLuint vs=gl_compile(GL_VERTEX_SHADER,VS_SRC), wf=gl_compile(GL_FRAGMENT_SHADER,MK_WARP_FS), cf=gl_compile(GL_FRAGMENT_SHADER,MK_COMP_FS);
        if(!vs||!wf||!cf){ mk_fail=1; return 0; }
        mk_warp=p_glCreateProgram(); p_glAttachShader(mk_warp,vs); p_glAttachShader(mk_warp,wf); p_glLinkProgram(mk_warp);
        mk_comp=p_glCreateProgram(); p_glAttachShader(mk_comp,vs); p_glAttachShader(mk_comp,cf); p_glLinkProgram(mk_comp);
        GLint ok1=0,ok2=0; p_glGetProgramiv(mk_warp,GL_LINK_STATUS,&ok1); p_glGetProgramiv(mk_comp,GL_LINK_STATUS,&ok2);
        if(!ok1||!ok2){ fprintf(stderr,"[viz] milkdrop shader link failed\n"); mk_fail=1; return 0; }
        p_glDeleteShader(vs); p_glDeleteShader(wf); p_glDeleteShader(cf);
        #define MKWU(v,n) v=p_glGetUniformLocation(mk_warp,n)
        MKWU(mkw_tex,"u_tex");MKWU(mkw_zoom,"u_zoom");MKWU(mkw_zoomexp,"u_zoomexp");MKWU(mkw_rot,"u_rot");
        MKWU(mkw_warp,"u_warp");MKWU(mkw_warpscale,"u_warpscale");MKWU(mkw_time,"u_time");MKWU(mkw_cx,"u_cx");
        MKWU(mkw_cy,"u_cy");MKWU(mkw_dx,"u_dx");MKWU(mkw_dy,"u_dy");MKWU(mkw_sx,"u_sx");MKWU(mkw_sy,"u_sy");
        MKWU(mkw_decay,"u_decay");MKWU(mkw_aspect,"u_aspect");
        #undef MKWU
        #define MKCU(v,n) v=p_glGetUniformLocation(mk_comp,n)
        MKCU(mkc_tex,"u_tex");MKCU(mkc_px,"u_px");MKCU(mkc_gamma,"u_gamma");MKCU(mkc_brighten,"u_brighten");
        MKCU(mkc_solarize,"u_solarize");MKCU(mkc_invert,"u_invert");MKCU(mkc_darkcenter,"u_darkcenter");MKCU(mkc_aspect,"u_aspect");
        #undef MKCU
        { GLuint pvs=gl_compile(GL_VERTEX_SHADER,VS_SRC), pf=gl_compile(GL_FRAGMENT_SHADER,MK_PLASMA_FS);   /* PLASMA program */
          if(pvs&&pf){ mk_plasma=p_glCreateProgram(); p_glAttachShader(mk_plasma,pvs); p_glAttachShader(mk_plasma,pf); p_glLinkProgram(mk_plasma);
              p_glDeleteShader(pvs); p_glDeleteShader(pf);
              mkp_time=p_glGetUniformLocation(mk_plasma,"u_time");   mkp_aspect=p_glGetUniformLocation(mk_plasma,"u_aspect");
              mkp_amp=p_glGetUniformLocation(mk_plasma,"u_amp");     mkp_bass=p_glGetUniformLocation(mk_plasma,"u_bass");
              mkp_mid=p_glGetUniformLocation(mk_plasma,"u_mid");     mkp_treble=p_glGetUniformLocation(mk_plasma,"u_treble");
              mkp_hue=p_glGetUniformLocation(mk_plasma,"u_hue");     mkp_scale=p_glGetUniformLocation(mk_plasma,"u_scale");
              mkp_warp=p_glGetUniformLocation(mk_plasma,"u_warp");   mkp_style=p_glGetUniformLocation(mk_plasma,"u_style"); } }
    }
    if(mk_ready){ p_glDeleteFramebuffers(2,mk_fbo); glDeleteTextures(2,mk_tex); }
    mk_w=W; mk_h=H;
    p_glGenFramebuffers(2,mk_fbo); glGenTextures(2,mk_tex);
    for(int i=0;i<2;i++){
        glBindTexture(GL_TEXTURE_2D,mk_tex[i]);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,mk_w,mk_h,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        p_glBindFramebuffer(GL_FRAMEBUFFER,mk_fbo[i]);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,mk_tex[i],0);
        if(p_glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){ fprintf(stderr,"[viz] milkdrop FBO incomplete\n"); mk_fail=1; p_glBindFramebuffer(GL_FRAMEBUFFER,0); return 0; }
        glViewport(0,0,mk_w,mk_h); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER,0);
    mk_ready=1; mk_cur=0;
    return 1;
}
/* one warp pass (prev frame → dst FBO, left bound for the inject) and one composite pass (→ screen) */
static void mk_do_warp(int dst,float zoom,float zoomexp,float rot,float warp,float warpscale,
                       float cx,float cy,float dx,float dy,float sx,float sy,float decay,float time){
    p_glBindFramebuffer(GL_FRAMEBUFFER,mk_fbo[dst]); glViewport(0,0,mk_w,mk_h); glDisable(GL_BLEND);
    p_glUseProgram(mk_warp);
    p_glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,mk_tex[mk_cur]); p_glUniform1i(mkw_tex,0);
    p_glUniform1f(mkw_zoom,zoom);p_glUniform1f(mkw_zoomexp,zoomexp);p_glUniform1f(mkw_rot,rot);
    p_glUniform1f(mkw_warp,warp);p_glUniform1f(mkw_warpscale,warpscale);p_glUniform1f(mkw_time,time);
    p_glUniform1f(mkw_cx,cx);p_glUniform1f(mkw_cy,cy);p_glUniform1f(mkw_dx,dx);p_glUniform1f(mkw_dy,dy);
    p_glUniform1f(mkw_sx,sx);p_glUniform1f(mkw_sy,sy);p_glUniform1f(mkw_decay,decay);p_glUniform1f(mkw_aspect,(float)mk_w/mk_h);
    p_glBindVertexArray(gl_vao); glDrawArrays(GL_TRIANGLE_STRIP,0,4);
}
static void mk_do_comp(int dst,float gamma,float brighten,int solarize,int invert,int darkcenter){
    p_glBindFramebuffer(GL_FRAMEBUFFER,0); glViewport(0,0,g_win_w,g_win_h); glDisable(GL_BLEND);
    p_glUseProgram(mk_comp);
    p_glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,mk_tex[dst]); p_glUniform1i(mkc_tex,0);
    p_glUniform2f(mkc_px,1.0f/mk_w,1.0f/mk_h);
    p_glUniform1f(mkc_gamma,gamma);p_glUniform1f(mkc_brighten,brighten);
    p_glUniform1i(mkc_solarize,solarize);p_glUniform1i(mkc_invert,invert);p_glUniform1i(mkc_darkcenter,darkcenter);
    p_glUniform1f(mkc_aspect,(float)mk_w/mk_h);
    p_glBindVertexArray(gl_vao); glDrawArrays(GL_TRIANGLE_STRIP,0,4);
}
/* a preset: base params + per-frame modulation amplitudes + colour flags + wave shape */
typedef struct {
    const char *name;
    float zoom,zoomexp,rot,warp,warpscale,sx,sy,decay,gamma,brighten;
    float rotmod,cxmod,cymod,warpmod,dxmod,dymod;
    int   wave,solarize,invert,darkcenter;
    float hue_base,hue_rate,wave_scale;
} MilkP;
/* zoom>1 everywhere so shapes EXPAND OUTWARD and fill the whole screen (zoom<1 was pulling everything
   into the middle). No cx/cy/dx/dy modulation → no left-right drift. Rotation is the audio-reactive
   motion; kept moderate so it reads as spin, not slide. */
static const MilkP MK_PRESETS[] = {
 /* name            zoom  zexp  rot   warp wscl  sx    sy   decay gamm brgt  rotm cxm cym wrpm dxm dym wv so iv dc  hueB hueR wsc */
 {"Aurora Drift",  1.006f,1.05f,0.003f,0.6f,1.4f,1.00f,1.00f,0.968f,1.2f,1.00f, 0,0,0,0.30f,0,0, 0,0,0,0, 0.55f,0.010f,1.0f},
 {"Hypertunnel",   1.010f,1.30f,0.004f,0.4f,1.5f,1.00f,1.00f,0.966f,1.3f,1.00f, 0,0,0,0.20f,0,0, 1,0,0,0, 0.60f,0.020f,1.0f},
 {"Liquid Swirl",  1.006f,1.05f,0.010f,1.4f,2.4f,1.00f,1.00f,0.965f,1.3f,1.00f, 0,0,0,0.50f,0,0, 0,0,0,0, 0.50f,0.030f,1.0f},
 {"Kaleido",       1.006f,1.10f,0.008f,0.6f,1.8f,0.80f,1.20f,0.963f,1.4f,1.05f, 0,0,0,0.30f,0,0, 3,0,0,0, 0.00f,0.040f,1.0f},
 {"Nebula Bloom",  1.010f,1.05f,0.004f,0.8f,1.6f,1.00f,1.00f,0.967f,1.6f,1.10f, 0,0,0,0.40f,0,0, 2,0,0,0, 0.80f,0.020f,1.2f},
 {"Vortex",        1.008f,1.15f,0.012f,0.5f,1.6f,1.00f,1.00f,0.968f,1.2f,1.00f, 0,0,0,0.20f,0,0, 3,0,0,0, 0.62f,0.020f,1.0f},
 {"Plasma Web",    1.006f,1.05f,0.006f,0.8f,2.2f,1.00f,1.00f,0.970f,1.3f,1.05f, 0,0,0,0.50f,0,0, 3,1,0,0, 0.40f,0.050f,1.0f},
 {"Starwarp",      1.012f,1.25f,0.003f,0.3f,1.4f,1.00f,1.00f,0.965f,1.2f,1.00f, 0,0,0,0.15f,0,0, 4,0,0,0, 0.58f,0.010f,0.9f},
 {"Inkblot",       1.005f,1.00f,0.004f,1.2f,2.0f,1.00f,1.00f,0.978f,1.1f,1.00f, 0,0,0,0.60f,0,0, 0,0,1,0, 0.10f,0.015f,1.0f},
 {"Solar Flare",   1.008f,1.05f,0.005f,0.5f,1.5f,1.00f,1.00f,0.965f,1.3f,1.20f, 0,0,0,0.30f,0,0, 3,0,0,0, 0.06f,0.010f,1.1f},
 {"Ripple Pond",   1.005f,1.00f,0.002f,1.7f,3.4f,1.00f,1.00f,0.963f,1.25f,1.00f,0,0,0,0.70f,0,0, 0,0,0,0, 0.52f,0.020f,1.0f},
 {"Galaxy Spin",   1.008f,1.15f,0.012f,0.5f,1.6f,1.00f,1.00f,0.970f,1.4f,1.00f, 0,0,0,0.20f,0,0, 2,0,0,0, 0.65f,0.020f,1.0f},
 {"Neon Grid",     1.006f,1.00f,0.004f,0.4f,1.3f,1.00f,1.00f,0.967f,1.2f,1.05f, 0,0,0,0.20f,0,0, 1,0,0,0, 0.50f,0.030f,1.0f},
 {"Fractal Bloom", 1.009f,1.05f,0.006f,0.9f,2.0f,1.00f,1.00f,0.967f,1.7f,1.10f, 0,0,0,0.40f,0,0, 3,0,0,0, 0.30f,0.040f,1.0f},
 {"Whirlpool",     1.008f,1.15f,0.010f,0.6f,1.8f,1.00f,1.00f,0.967f,1.3f,1.00f, 0,0,0,0.40f,0,0, 0,0,0,0, 0.55f,0.030f,1.0f},
 {"Crystal",       1.006f,1.08f,0.008f,0.5f,1.6f,0.88f,1.12f,0.965f,1.4f,1.05f, 0,0,0,0.30f,0,0, 2,1,0,0, 0.72f,0.020f,1.0f},
};
#define MK_NP ((int)(sizeof(MK_PRESETS)/sizeof(MK_PRESETS[0])))
static int g_mk_pidx=0;
/* inject the audio-reactive "seed" the feedback smears into flow — wave shape varies by preset */
static void mk_inject(const MilkP *p, float time, float en){
    float W=g_win_w, H=g_win_h, cx=W*0.5f, cy=H*0.5f, unit=(W<H?W:H), r,g,b;
    float t=g_vz_rot, hue0=p->hue_base + time*p->hue_rate, ws=p->wave_scale;
    float VA=0.5f+0.5f*en;   /* injected-shape alpha: always visible (never freezes), brighter with audio */
    if(p->wave==0){          /* circle: waveform wrapped into a pulsing ring */
        int M=220; float base=unit*(0.14f+g_vz_beat*0.05f), aw=unit*0.10f*ws, px=0,py=0; int have=0;
        for(int i=0;i<=M;i++){ int idx=(g_viz_w-M+i)&(VIZ_N-1); float s=g_viz_pcm[idx];
            float ang=2*VIZ_PI*i/M+t*1.4f, rad=base+s*aw, x=cx+cosf(ang)*rad, y=cy+sinf(ang)*rad;
            viz_hsv(vz_wrap(hue0+0.3f*s),0.8f,1.0f,&r,&g,&b);
            if(have) viz_seg(px,py,x,y, 3.0f+g_vz_beat*4.0f, r,g,b, 0.9f*VA); px=x;py=y;have=1; }
    } else if(p->wave==1){   /* dual oscilloscope: horizontal + vertical scan lines */
        int M=256; float amp1=unit*0.32f*ws, px=0,py=0; int have=0;
        for(int i=0;i<M;i++){ int idx=(g_viz_w-M+i)&(VIZ_N-1); float s=g_viz_pcm[idx];
            float x=W*i/(float)M, y=cy-s*amp1; viz_hsv(vz_wrap(hue0+0.2f*s),0.75f,1.0f,&r,&g,&b);
            if(have) viz_seg(px,py,x,y,2.5f,r,g,b,0.9f*VA); px=x;py=y;have=1; }
        have=0; for(int i=0;i<M;i++){ int idx=(g_viz_w-M+i)&(VIZ_N-1); float s=g_viz_pcm[idx];
            float y=H*i/(float)M, x=cx+s*amp1; viz_hsv(vz_wrap(hue0+0.5f+0.2f*s),0.75f,1.0f,&r,&g,&b);
            if(have) viz_seg(px,py,x,y,2.5f,r,g,b,0.7f*VA); px=x;py=y;have=1; }
    } else if(p->wave==2){   /* spiral: waveform along an expanding golden spiral */
        int M=300; float px=0,py=0; int have=0;
        for(int i=0;i<M;i++){ float fr=(float)i/M; int idx=(g_viz_w-M+i)&(VIZ_N-1); float s=g_viz_pcm[idx];
            float ang=fr*22.0f+t*2.0f, rad=fr*unit*0.46f + s*unit*0.05f*ws;
            float x=cx+cosf(ang)*rad, y=cy+sinf(ang)*rad; viz_hsv(vz_wrap(hue0+fr*0.6f),0.85f,1.0f,&r,&g,&b);
            if(have) viz_seg(px,py,x,y,2.2f,r,g,b,0.85f*VA); px=x;py=y;have=1; }
    } else if(p->wave==3){   /* radial spectrum: mirrored spikes from the core */
        int total=VIZ_BANDS*2;
        for(int i=0;i<total;i++){ int bnd=i<VIZ_BANDS?i:(total-1-i); float v=g_vz_band[bnd];
            float ang=2*VIZ_PI*i/total - t*1.2f, ca=cosf(ang), sa=sinf(ang);
            float r0=unit*0.09f, r1=r0+v*unit*0.42f*ws;
            viz_hsv(vz_wrap(hue0-0.4f*(float)bnd/VIZ_BANDS),0.9f,1.0f,&r,&g,&b);
            viz_seg(cx+ca*r0,cy+sa*r0, cx+ca*r1,cy+sa*r1, 2.5f+v*4.0f, r,g,b, 0.85f*VA); }
    } else {                 /* phyllotaxis seed-head (plant-sim nod) */
        float gold=2.39996323f; int seeds=230;
        for(int i=0;i<seeds;i++){ float fr=(float)i/seeds, ang=i*gold+t*2.0f, rad=sqrtf(fr)*unit*0.46f;
            int bnd=(int)(fr*VIZ_BANDS); if(bnd>=VIZ_BANDS)bnd=VIZ_BANDS-1; float e=g_vz_band[bnd];
            float x=cx+cosf(ang)*rad, y=cy+sinf(ang)*rad, sz=1.5f+e*11.0f*ws;
            viz_hsv(vz_wrap(hue0+fr*0.5f),0.85f,1.0f,&r,&g,&b);
            ui_rect(x-sz*0.5f,y-sz*0.5f,sz,sz, r,g,b, (0.45f+0.55f*e)*VA); }
    }
    if(g_vz_beat>0.12f){     /* beat flash: bright central bloom (triangle fan) — also fades with audio */
        int seg=28; float rr=unit*(0.06f+g_vz_beat*0.14f), aa=0.5f*g_vz_beat*VA;
        for(int s=0;s<seg;s++){ float a0=2*VIZ_PI*s/seg,a1=2*VIZ_PI*(s+1)/seg;
            ui_vtx(cx,cy,WHITE_U,WHITE_V,1,1,1,aa);
            ui_vtx(cx+cosf(a0)*rr,cy+sinf(a0)*rr,WHITE_U,WHITE_V,0.7f,0.85f,1.0f,0.0f);
            ui_vtx(cx+cosf(a1)*rr,cy+sinf(a1)*rr,WHITE_U,WHITE_V,0.7f,0.85f,1.0f,0.0f); } }
}
/* draw the plasma full-screen. time/amp/bass/mid/treble/hue are all audio-derived; render straight to
   whatever framebuffer is bound (the screen). No feedback, no state → no drift, no grey wash. */
/* ============================ REAL MILKDROP PRESET ENGINE ==========================================
   Loads classic .milk presets (scalar params + per_frame/per_pixel equation programs), runs the
   equations each frame (audio fed in via bass/mid/treb vars), warps the previous frame through a
   per-vertex MESH (MilkDrop's motion), seeds it with the audio waveform, and composites. Reuses the
   FBO ping-pong (mk_fbo/mk_tex) + mk_do_comp already built. Falls back to the plasma if no presets. */
/* ---- expression evaluator (compile once to RPN, eval fast against a shared var table) ---- */
#define MD_MAXVARS 320
typedef struct { char nm[MD_MAXVARS][24]; float val[MD_MAXVARS]; int n; } MdVars;
static int md_vid(MdVars*V,const char*s,int len){
    if(len > 23) len = 23;   /* names live in 24-byte slots: a longer identifier overran into val[]/n */
    for(int i=0;i<V->n;i++) if((int)strlen(V->nm[i])==len && !strncmp(V->nm[i],s,len)) return i;
    if(V->n>=MD_MAXVARS) return 0; memcpy(V->nm[V->n],s,len); V->nm[V->n][len]=0; V->val[V->n]=0; return V->n++; }
enum { MDO_NUM=1,MDO_VAR,MDO_ADD,MDO_SUB,MDO_MUL,MDO_DIV,MDO_MOD,MDO_NEG,MDO_FUN,MDO_LP };
enum { MDF_SIN,MDF_COS,MDF_TAN,MDF_ASIN,MDF_ACOS,MDF_ATAN,MDF_ATAN2,MDF_ABS,MDF_SQRT,MDF_SQR,MDF_POW,
       MDF_EXP,MDF_LOG,MDF_LOG10,MDF_MIN,MDF_MAX,MDF_SIGN,MDF_INT,MDF_RAND,MDF_BNOT,MDF_ABOVE,MDF_BELOW,MDF_EQUAL,MDF_IF,MDF_NONE };
typedef struct { int op; float num; int var; int fun; } MdTok;
typedef struct { int dst; MdTok*rpn; int n; } MdStmt;
typedef struct { MdStmt*s; int n; } MdProg;
static int md_fun(const char*s,int len){
    static const struct{const char*n;int f;} T[]={{"sin",MDF_SIN},{"cos",MDF_COS},{"tan",MDF_TAN},{"asin",MDF_ASIN},
      {"acos",MDF_ACOS},{"atan",MDF_ATAN},{"atan2",MDF_ATAN2},{"abs",MDF_ABS},{"sqrt",MDF_SQRT},{"sqr",MDF_SQR},
      {"pow",MDF_POW},{"exp",MDF_EXP},{"log",MDF_LOG},{"log10",MDF_LOG10},{"min",MDF_MIN},{"max",MDF_MAX},
      {"sign",MDF_SIGN},{"int",MDF_INT},{"rand",MDF_RAND},{"bnot",MDF_BNOT},{"above",MDF_ABOVE},{"below",MDF_BELOW},
      {"equal",MDF_EQUAL},{"if",MDF_IF},{0,0}};
    for(int i=0;T[i].n;i++) if((int)strlen(T[i].n)==len && !strncmp(T[i].n,s,len)) return T[i].f; return MDF_NONE; }
static int md_fargc(int f){ return (f==MDF_IF)?3:(f==MDF_ATAN2||f==MDF_POW||f==MDF_MIN||f==MDF_MAX||f==MDF_ABOVE||f==MDF_BELOW||f==MDF_EQUAL)?2:1; }
static int md_prec(int op){ return (op==MDO_ADD||op==MDO_SUB)?1:(op==MDO_MUL||op==MDO_DIV||op==MDO_MOD)?2:(op==MDO_NEG)?4:0; }
static int md_cexpr(const char*p,MdTok*out,int cap,int*outn,MdVars*V){
    MdTok stk[160]; int sp=0,on=0,prevval=0; const char*s=p;
    #define MDE(t) do{ if(on<cap) out[on++]=(t);}while(0)
    #define MDP(t) do{ if(sp<160) stk[sp++]=(t);}while(0)
    while(*s && *s!=';'){
        if(isspace((unsigned char)*s)){s++;continue;}
        if(isdigit((unsigned char)*s)||(*s=='.'&&isdigit((unsigned char)s[1]))){ char*e; float v=strtof(s,&e); s=e; MdTok t={MDO_NUM,v,0,0}; MDE(t); prevval=1; continue; }
        if(isalpha((unsigned char)*s)||*s=='_'){ const char*b=s; while(isalnum((unsigned char)*s)||*s=='_')s++; int len=(int)(s-b);
            const char*w=s; while(isspace((unsigned char)*w))w++;
            if(*w=='('){ MdTok t={MDO_FUN,0,0,md_fun(b,len)}; MDP(t); MdTok lp={MDO_LP,0,0,0}; MDP(lp); s=w+1; prevval=0; continue; }
            MdTok t={MDO_VAR,0,md_vid(V,b,len),0}; MDE(t); prevval=1; continue; }
        char c=*s++;
        if(c=='('){ MdTok t={MDO_LP,0,0,0}; MDP(t); prevval=0; continue; }
        if(c==')'){ while(sp&&stk[sp-1].op!=MDO_LP) MDE(stk[--sp]); if(sp)sp--; if(sp&&stk[sp-1].op==MDO_FUN) MDE(stk[--sp]); prevval=1; continue; }
        if(c==','){ while(sp&&stk[sp-1].op!=MDO_LP) MDE(stk[--sp]); prevval=0; continue; }
        int op=0;
        if(c=='+')op=MDO_ADD; else if(c=='-')op=prevval?MDO_SUB:MDO_NEG; else if(c=='*')op=MDO_MUL;
        else if(c=='/')op=MDO_DIV; else if(c=='%')op=MDO_MOD; else continue;
        if(op==MDO_NEG){ MdTok t={MDO_NEG,0,0,0}; MDP(t); prevval=0; continue; }
        while(sp){ int so=stk[sp-1].op; if(so==MDO_LP||so==MDO_FUN) break; if(md_prec(so)<md_prec(op)) break; MDE(stk[--sp]); }
        MdTok t={op,0,0,0}; MDP(t); prevval=0;
    }
    while(sp){ if(stk[sp-1].op==MDO_LP){sp--;continue;} MDE(stk[--sp]); }
    *outn=on; return (int)(s-p);
}
/* simulate md_run's value stack over an RPN program: 0 if any op would pop an empty stack ("x = 1 + ;",
   a zero-arg call, a 2-arg if()) or the depth would pass md_run's S[160]. Such statements were run anyway
   and wrote below/above the evaluator's stack array — silent memory corruption. They are dropped now. */
static int md_rpn_ok(const MdTok*t,int n){
    int d=0;
    for(int i=0;i<n;i++){
        switch(t[i].op){
        case MDO_NUM: case MDO_VAR: d++; break;
        case MDO_NEG: if(d<1) return 0; break;
        case MDO_FUN: { int a=md_fargc(t[i].fun); if(d<a) return 0; d=d-a+1; } break;
        case MDO_LP: return 0;
        default: if(d<2) return 0; d--; break;          /* binary operators */
        }
        if(d>150) return 0;
    }
    return 1;
}
static MdProg md_compile(const char*text,MdVars*V){
    MdProg P; P.s=0; P.n=0; int cap=0; const char*s=text; if(!text) return P;
    while(*s){ while(*s&&(isspace((unsigned char)*s)||*s==';'))s++; if(!*s)break;
        const char*b=s; while(isalnum((unsigned char)*s)||*s=='_')s++; int len=(int)(s-b);
        const char*w=s; while(isspace((unsigned char)*w))w++;
        if(len==0||*w!='='||w[1]=='='){ while(*s&&*s!=';')s++; continue; }
        int dst=md_vid(V,b,len); s=w+1;
        MdTok tmp[256]; int tn=0; s+=md_cexpr(s,tmp,256,&tn,V);
        if(!md_rpn_ok(tmp,tn)) continue;                 /* malformed: skip the statement, keep the preset */
        if(P.n>=cap){ cap=cap?cap*2:16; P.s=realloc(P.s,cap*sizeof(MdStmt)); }
        MdStmt st; st.dst=dst; st.n=tn; st.rpn=malloc(tn*sizeof(MdTok)); memcpy(st.rpn,tmp,tn*sizeof(MdTok)); P.s[P.n++]=st; }
    return P;
}
static void md_run(const MdProg*P,MdVars*V){
    float S[160];
    for(int i=0;i<P->n;i++){ const MdStmt*st=&P->s[i]; int sp=0;
        for(int j=0;j<st->n;j++){ const MdTok*k=&st->rpn[j];
            switch(k->op){
            case MDO_NUM: S[sp++]=k->num; break;
            case MDO_VAR: S[sp++]=V->val[k->var]; break;
            case MDO_ADD: S[sp-2]+=S[sp-1]; sp--; break;
            case MDO_SUB: S[sp-2]-=S[sp-1]; sp--; break;
            case MDO_MUL: S[sp-2]*=S[sp-1]; sp--; break;
            case MDO_DIV: S[sp-2]= S[sp-1]!=0?S[sp-2]/S[sp-1]:0; sp--; break;
            case MDO_MOD: S[sp-2]= S[sp-1]!=0?fmodf(S[sp-2],S[sp-1]):0; sp--; break;
            case MDO_NEG: S[sp-1]=-S[sp-1]; break;
            case MDO_FUN:{ float a=sp>0?S[sp-1]:0,b=sp>1?S[sp-2]:0,c=sp>2?S[sp-3]:0,r=0;
                switch(k->fun){ case MDF_SIN:r=sinf(a);break;case MDF_COS:r=cosf(a);break;case MDF_TAN:r=tanf(a);break;
                case MDF_ASIN:r=asinf(a);break;case MDF_ACOS:r=acosf(a);break;case MDF_ATAN:r=atanf(a);break;
                case MDF_ATAN2:r=atan2f(b,a);break;case MDF_ABS:r=fabsf(a);break;case MDF_SQRT:r=sqrtf(fabsf(a));break;
                case MDF_SQR:r=a*a;break;case MDF_POW:r=powf(b,a);break;case MDF_EXP:r=expf(a);break;
                case MDF_LOG:r=a>0?logf(a):0;break;case MDF_LOG10:r=a>0?log10f(a):0;break;
                case MDF_MIN:r=b<a?b:a;break;case MDF_MAX:r=b>a?b:a;break;case MDF_SIGN:r=(a>0)-(a<0);break;
                case MDF_INT:r=floorf(a);break;
                /* rand(n) = integer in [0,n). It was rand()%(int)a: any 0<a<1 — rand(0.4), rand(vol),
                   rand(bass*2) on a quiet passage — truncated to %0 = integer DIVIDE BY ZERO (0xC0000094),
                   killing the player "sometimes, after a while" on the MilkDrop visualizer (115 presets). */
                case MDF_RAND:{ int n = (a >= 2147483520.0f) ? 2147483647 : (a >= 1.0f ? (int)a : 0);
                    r = n > 0 ? (float)(rand() % n) : 0; } break;
                case MDF_BNOT:r=(a==0);break;case MDF_ABOVE:r=(b>a);break;case MDF_BELOW:r=(b<a);break;
                case MDF_EQUAL:r=(b==a);break;case MDF_IF:r=(c!=0)?b:a;break; default:r=a;break; }
                sp-=md_fargc(k->fun); S[sp++]=r; } break;
            } }
        float r= sp>0?S[sp-1]:0; if(r!=r||r>1e18f||r<-1e18f) r=0; V->val[st->dst]=r;
    }
}
/* ---- a loaded preset ---- */
typedef struct {
    char name[96]; int valid;
    MdVars V; MdProg pf, pp;
    int iTime,iFrame,iFps,iBass,iBassA,iMid,iMidA,iTreb,iTrebA;
    int iZoom,iZoomexp,iRot,iWarp,iCx,iCy,iDx,iDy,iSx,iSy,iX,iY,iRad,iAng;
    int iWr,iWg,iWb,iWa,iWx,iWy; int iFrameN, iq[32];
    float decay,gamma,warpscale,warpspeed,zoom0,zoomexp0,rot0,warp0,cx0,cy0,sx0,sy0,dx0,dy0;
    int wavemode,solarize,invert,darkcenter; float wave_r,wave_g,wave_b,wave_scale;
    /* MilkDrop2 HLSL warp/comp shaders (translated to GLSL, compiled lazily; 0 = classic path) */
    char *warp_src, *comp_src; int has_warp, has_comp, sh_tried; GLuint pgm_warp, pgm_comp;
} MdPreset;
static void md_freeprog(MdProg*P){ for(int i=0;i<P->n;i++) free(P->s[i].rpn); free(P->s); P->s=0; P->n=0; }
static char* md_collect(FILE*f,const char*pref){ /* concat all `pref*=VALUE;` lines. rewinds f. */
    rewind(f); int pl=strlen(pref); char line[1024]; char*out=malloc(1); out[0]=0; size_t len=0;
    while(fgets(line,sizeof line,f)){ if(!strncmp(line,pref,pl)){ char*eq=strchr(line,'='); if(eq){ char*v=eq+1; char*nl=strpbrk(v,"\r\n"); if(nl)*nl=0;
        size_t vl=strlen(v); out=realloc(out,len+vl+2); memcpy(out+len,v,vl); len+=vl; out[len++]=';'; out[len]=0; } } } return out; }
/* concat MilkDrop2 shader lines `pref<N>=`BODY (backtick-quoted), preserving newlines so // comments
   and statements stay on their own lines. Returns NULL if the preset has no such block. */
static char* md_collect_shader(FILE*f,const char*pref){
    rewind(f); int pl=strlen(pref); char line[1024]; char*out=malloc(1); out[0]=0; size_t len=0; int found=0;
    while(fgets(line,sizeof line,f)){ if(!strncmp(line,pref,pl) && (line[pl]>='0'&&line[pl]<='9')){
        char*eq=strchr(line,'='); if(!eq)continue; char*v=eq+1; char*nl=strpbrk(v,"\r\n"); if(nl)*nl=0;
        if(*v=='`')v++;                                  /* strip the opening backtick */
        found=1; size_t vl=strlen(v); out=realloc(out,len+vl+2); memcpy(out+len,v,vl); len+=vl; out[len++]='\n'; out[len]=0; } }
    if(!found){ free(out); return NULL; } return out; }
static int md_load(const char*path, MdPreset*P){
    memset(P,0,sizeof *P);
    P->decay=0.98f; P->gamma=1; P->warpscale=1; P->warpspeed=1; P->zoom0=1; P->zoomexp0=1; P->sx0=P->sy0=1; P->cx0=P->cy0=0.5f;
    P->wave_r=P->wave_g=P->wave_b=0.8f; P->wave_scale=1;
    wchar_t wp[2200]; path_u8_to_w(path,wp,2200); FILE*f=_wfopen(wp,L"rb"); if(!f) return 0;
    char line[1024];
    while(fgets(line,sizeof line,f)){ char*eq=strchr(line,'='); if(!eq)continue; *eq=0; char*k=line,*v=eq+1;
        if(!strncmp(k,"per_",4)) continue;
        float fv=(float)atof(v);
        #define KF(n,f) else if(!strcmp(k,n)) P->f=fv
        if(0){}
        KF("fDecay",decay); KF("fGammaAdj",gamma); KF("fWarpScale",warpscale); KF("fWarpAnimSpeed",warpspeed);
        KF("fZoomExponent",zoomexp0); KF("zoom",zoom0); KF("rot",rot0); KF("warp",warp0); KF("cx",cx0); KF("cy",cy0);
        KF("sx",sx0); KF("sy",sy0); KF("dx",dx0); KF("dy",dy0); KF("fWaveScale",wave_scale);
        KF("wave_r",wave_r); KF("wave_g",wave_g); KF("wave_b",wave_b);
        else if(!strcmp(k,"nWaveMode")) P->wavemode=(int)fv;
        else if(!strcmp(k,"bSolarize")) P->solarize=(int)fv;
        else if(!strcmp(k,"bInvert")) P->invert=(int)fv;
        else if(!strcmp(k,"bDarkenCenter")) P->darkcenter=(int)fv;
        #undef KF
    }
    /* seed standard vars so they exist */
    const char*seed[]={"time","frame","fps","bass","bass_att","mid","mid_att","treb","treb_att",
        "zoom","zoomexp","rot","warp","cx","cy","dx","dy","sx","sy","x","y","rad","ang",
        "wave_r","wave_g","wave_b","wave_a","wave_x","wave_y",0};
    for(int i=0;seed[i];i++) md_vid(&P->V,seed[i],strlen(seed[i]));
    char*pf=md_collect(f,"per_frame_"); char*pp=md_collect(f,"per_pixel_");
    P->warp_src=md_collect_shader(f,"warp_"); P->comp_src=md_collect_shader(f,"comp_");
    fclose(f);
    /* only treat as a real shader if there is an actual body (a preset with just `shader_body` is empty) */
    P->has_warp = P->warp_src && strchr(P->warp_src,'{') && strstr(P->warp_src,"ret");
    P->has_comp = P->comp_src && strchr(P->comp_src,'{') && strstr(P->comp_src,"ret");
    P->pf=md_compile(pf,&P->V); P->pp=md_compile(pp,&P->V); free(pf); free(pp);
    #define CID(n) md_vid(&P->V,n,strlen(n))
    P->iTime=CID("time");P->iFrame=CID("frame");P->iFps=CID("fps");P->iBass=CID("bass");P->iBassA=CID("bass_att");
    P->iMid=CID("mid");P->iMidA=CID("mid_att");P->iTreb=CID("treb");P->iTrebA=CID("treb_att");
    P->iZoom=CID("zoom");P->iZoomexp=CID("zoomexp");P->iRot=CID("rot");P->iWarp=CID("warp");P->iCx=CID("cx");P->iCy=CID("cy");
    P->iDx=CID("dx");P->iDy=CID("dy");P->iSx=CID("sx");P->iSy=CID("sy");P->iX=CID("x");P->iY=CID("y");P->iRad=CID("rad");P->iAng=CID("ang");
    P->iWr=CID("wave_r");P->iWg=CID("wave_g");P->iWb=CID("wave_b");P->iWa=CID("wave_a");P->iWx=CID("wave_x");P->iWy=CID("wave_y");
    P->iFrameN=CID("frame");
    { for(int i=0;i<32;i++){ char qn[8]; int L=snprintf(qn,sizeof qn,"q%d",i+1); P->iq[i]=md_vid(&P->V,qn,L); } }  /* q1..q32 → shaders */
    #undef CID
    { const char*bn=path; for(const char*q=path;*q;q++) if(*q=='/'||*q=='\\')bn=q+1; snprintf(P->name,sizeof P->name,"%.90s",bn); char*dot=strrchr(P->name,'.'); if(dot)*dot=0; }
    P->valid=1; return 1;
}
/* ---- preset directory list ---- */
static char **md_files=NULL; static int md_nfiles=0, md_scanned=0;
static void md_scan(void){
    if(md_scanned) return; md_scanned=1;
    char dir[1024]; const char*env=getenv("HOGS_PRESETS");
    if(env&&*env) snprintf(dir,sizeof dir,"%s",env);
    else { char*bp=SDL_GetBasePath(); snprintf(dir,sizeof dir,"%spresets",bp?bp:""); if(bp)SDL_free(bp); }
    char pat[1100]; snprintf(pat,sizeof pat,"%s\\*.milk",dir); wchar_t wp[2200]; path_u8_to_w(pat,wp,2200);
    WIN32_FIND_DATAW fd; HANDLE h=FindFirstFileW(wp,&fd); int cap=0;
    if(h!=INVALID_HANDLE_VALUE){ do{ char nm[512]; path_w_to_u8(fd.cFileName,nm,sizeof nm);
        char full[1600]; snprintf(full,sizeof full,"%s\\%s",dir,nm);
        if(md_nfiles>=cap){ cap=cap?cap*2:256; md_files=realloc(md_files,cap*sizeof(char*)); }
        md_files[md_nfiles++]=SDL_strdup(full);
    }while(FindNextFileW(h,&fd)); FindClose(h); }
    fprintf(stderr,"[milk] %d presets in %s\n", md_nfiles, dir);
}
/* ---- GL: warp-mesh program + dynamic mesh VBO ---- */
static const char *MD_WARP_VS =
  "#version 330 core\n"
  "layout(location=0) in vec2 a_pos; layout(location=1) in vec2 a_uv;\n"
  "out vec2 v_uv; void main(){ gl_Position=vec4(a_pos,0.0,1.0); v_uv=a_uv; }\n";
static const char *MD_WARP_FS2 =
  "#version 330 core\n"
  "in vec2 v_uv; out vec4 col; uniform sampler2D u_tex; uniform float u_decay;\n"
  "void main(){ col=vec4(texture(u_tex,v_uv).rgb*u_decay, 1.0); }\n";
#define MDGX 64
#define MDGY 48
static GLuint md_prog=0, md_vao=0, md_vbo=0; static GLint md_uTex, md_uDecay; static int md_glok=0, md_glfail=0;
static float *md_verts=NULL;   /* (MDGX-1)*(MDGY-1)*6 verts * 4 floats */
static int md_glinit(void){
    if(md_glok) return 1; if(md_glfail) return 0;
    GLuint vs=gl_compile(GL_VERTEX_SHADER,MD_WARP_VS), fs=gl_compile(GL_FRAGMENT_SHADER,MD_WARP_FS2);
    if(!vs||!fs){ md_glfail=1; return 0; }
    md_prog=p_glCreateProgram(); p_glAttachShader(md_prog,vs); p_glAttachShader(md_prog,fs); p_glLinkProgram(md_prog);
    GLint ok=0; p_glGetProgramiv(md_prog,GL_LINK_STATUS,&ok); if(!ok){ md_glfail=1; return 0; }
    p_glDeleteShader(vs); p_glDeleteShader(fs);
    md_uTex=p_glGetUniformLocation(md_prog,"u_tex"); md_uDecay=p_glGetUniformLocation(md_prog,"u_decay");
    p_glGenVertexArrays(1,&md_vao); p_glBindVertexArray(md_vao);
    p_glGenBuffers(1,&md_vbo); p_glBindBuffer(GL_ARRAY_BUFFER,md_vbo);
    p_glEnableVertexAttribArray(0); p_glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)0);
    p_glEnableVertexAttribArray(1); p_glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)(2*sizeof(float)));
    md_verts=malloc((size_t)(MDGX-1)*(MDGY-1)*6*4*sizeof(float));
    md_glok=1; return 1;
}
/* ================= MilkDrop2 HLSL warp/comp shader translation =================
   The 2007 pack is 100% classic (equations only); the official final-MilkDrop pack ships presets with
   `warp_N=`/`comp_N=` HLSL shader bodies. MilkDrop's shader dialect is close enough to GLSL that a
   textual translation + a fixed uniform/sampler prelude compiles a large fraction of them; anything
   that fails to translate/compile falls back cleanly to the classic mesh path (never crashes).
   Handled: float2/3/4↔vec, tex2D/lerp/saturate/frac/atan2/fmod/ddx/ddy, mul() overloads, `1.0f`→`1.0`,
   integer-literal→float promotion. Not handled (→ fallback): custom image-texture samplers, for-loops
   with int counters, matrix constructors with row-major intent. */
static GLuint md_noise_tex=0;
static void md_make_noise(void){
    if(md_noise_tex) return;
    const int NS=256; unsigned char*buf=malloc((size_t)NS*NS*4); unsigned s=0x2f6e1cb7u;
    for(int i=0;i<NS*NS*4;i++){ s=s*1664525u+1013904223u; buf[i]=(unsigned char)(s>>15); }
    glGenTextures(1,&md_noise_tex); glBindTexture(GL_TEXTURE_2D,md_noise_tex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,NS,NS,0,GL_RGBA,GL_UNSIGNED_BYTE,buf);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    free(buf);
}
/* the fixed GLSL prelude both translated shaders share (uniforms, samplers, intrinsic shims) */
static const char *MD2_PRELUDE =
  "#version 330 core\n"
  "out vec4 _fragColor;\n"
  "uniform sampler2D sampler_main,sampler_fw_main,sampler_pw_main,sampler_fc_main,sampler_pc_main;\n"
  "uniform sampler2D sampler_blur1,sampler_blur2,sampler_blur3;\n"
  "uniform sampler2D sampler_noise_lq,sampler_noise_lq_lite,sampler_noise_mq,sampler_noise_hq,sampler_noisevol_lq,sampler_noisevol_hq;\n"
  "uniform float time,fps,frame,progress,bass,bass_att,mid,mid_att,treb,treb_att,vol;\n"
  "uniform vec4 texsize,aspect,rand_frame,rand_preset;\n"
  "uniform float q1,q2,q3,q4,q5,q6,q7,q8,q9,q10,q11,q12,q13,q14,q15,q16,q17,q18,q19,q20,q21,q22,q23,q24,q25,q26,q27,q28,q29,q30,q31,q32;\n"
  "uniform float _md_finalmul; uniform vec2 u_res;\n"
  "in vec2 v_uv;\n"
  "uniform vec4 texsize_noise_lq,texsize_noise_lq_lite,texsize_noise_mq,texsize_noise_hq,texsize_noisevol_lq,texsize_noisevol_hq;\n"
  "#define float1 float\n#define float2 vec2\n#define float3 vec3\n#define float4 vec4\n#define float2x2 mat2\n#define float3x3 mat3\n#define float4x4 mat4\n"
  "#define int2 ivec2\n#define int3 ivec3\n#define int4 ivec4\n#define half float\n#define half2 vec2\n#define half3 vec3\n#define half4 vec4\n"
  "#define lerp mix\n#define frac fract\n#define fmod mod\n#define ddx dFdx\n#define ddy dFdy\n#define atan2(y,x) atan((y),(x))\n"
  "#define saturate(x) clamp((x),0.0,1.0)\n"
  /* MilkDrop's tex2D returns float4 but presets routinely assign it straight to a float3 (`ret=tex2D(..)`),
     relying on HLSL's silent truncation. GLSL is strict, so our sampling macros yield vec3 (feedback is RGB;
     alpha is essentially never used). .xyz/.x swizzles still work on the vec3. */
  "#define tex2D(s,u) (texture(s,u).xyz)\n#define tex2d(s,u) (texture(s,u).xyz)\n#define tex3D(s,u) (texture(s,u).xyz)\n"
  "#define tex2Dlod(s,v) (textureLod(s,(v).xy,(v).w).xyz)\n#define tex2Dbias(s,v) (texture(s,(v).xy,(v).w).xyz)\n"
  "#define tex3d(s,u) (texture(s,u).xyz)\n"
  "#define M_PI 3.1415926535\n#define M_PI_2 1.5707963268\n#define M_PI_4 0.7853981634\n#define M_2PI 6.2831853072\n"
  "#define M_INV_PI_2 0.1591549431\n#define M_INV_PI 0.3183098862\n"
  "#define roam_cos cos(time*0.2)\n#define roam_sin sin(time*0.2)\n#define slow_roam_cos cos(time*0.05)\n#define slow_roam_sin sin(time*0.05)\n"
  "vec3 GetPixel(vec2 u){return texture(sampler_main,u).xyz;}\n"
  "float lum(vec3 c){return dot(c,vec3(0.32,0.49,0.29));} float lum(float x){return x;}\n"
  "#define _qa vec4(q1,q2,q3,q4)\n#define _qb vec4(q5,q6,q7,q8)\n#define _qc vec4(q9,q10,q11,q12)\n#define _qd vec4(q13,q14,q15,q16)\n"
  "#define _qe vec4(q17,q18,q19,q20)\n#define _qf vec4(q21,q22,q23,q24)\n#define _qg vec4(q25,q26,q27,q28)\n#define _qh vec4(q29,q30,q31,q32)\n"
  "vec2 mul(vec2 v,mat2 m){return v*m;} vec2 mul(mat2 m,vec2 v){return m*v;}\n"
  "vec3 mul(vec3 v,mat3 m){return v*m;} vec3 mul(mat3 m,vec3 v){return m*v;}\n"
  "vec4 mul(vec4 v,mat4 m){return v*m;} vec4 mul(mat4 m,vec4 v){return m*v;}\n"
  "vec3 GetBlur1(vec2 u){return texture(sampler_blur1,u).xyz;} vec3 GetBlur2(vec2 u){return texture(sampler_blur2,u).xyz;} vec3 GetBlur3(vec2 u){return texture(sampler_blur3,u).xyz;}\n";
/* normalise a shader body: strip leading `shader_body`, drop `f` float-suffixes, promote integer
   literals to floats (outside [] subscripts), so GLSL's strict typing accepts MilkDrop's loose code. */
static char* md_normalise(const char*body){
    /* work on a private copy with every `shader_body` marker blanked (it can appear after a leading
       comment, not just at the very start) — it is a MilkDrop keyword, never a real identifier. */
    char*bc=malloc(strlen(body)+1); strcpy(bc,body);
    for(char*h=bc; (h=strstr(h,"shader_body")); ){ for(int k=0;k<11;k++) h[k]=' '; }
    const char*b=bc; while(*b&&isspace((unsigned char)*b))b++;
    size_t cap=strlen(b)*2+64, len=0; char*o=malloc(cap); int brk=0;
    #define OPUT(ch) do{ if(len+2>=cap){cap*=2;o=realloc(o,cap);} o[len++]=(ch); }while(0)
    for(const char*p=b; *p; ){
        char c=*p; int prev=(len>0)?o[len-1]:' ';
        if(c=='['){brk++; OPUT(c); p++; continue;}
        if(c==']'){ if(brk)brk--; OPUT(c); p++; continue;}
        int numstart=(isdigit((unsigned char)c)||(c=='.'&&isdigit((unsigned char)p[1]))) && !(isalnum((unsigned char)prev)||prev=='_'||prev=='.');
        if(numstart){ const char*q=p; int hasdot=0,hasexp=0;
            while(isdigit((unsigned char)*q))q++;
            if(*q=='.'){hasdot=1;q++; while(isdigit((unsigned char)*q))q++;}
            if(*q=='e'||*q=='E'){hasexp=1;q++; if(*q=='+'||*q=='-')q++; while(isdigit((unsigned char)*q))q++;}
            size_t nlen=(size_t)(q-p); if(len+nlen+4>=cap){cap=(len+nlen+64)*2;o=realloc(o,cap);}
            memcpy(o+len,p,nlen); len+=nlen;
            if(!hasdot && !hasexp && brk==0){ o[len++]='.'; o[len++]='0'; }
            if(*q=='f'||*q=='F')q++;                  /* drop 1.0f-style float suffix */
            p=q; continue; }
        OPUT(c); p++;
    }
    o[len]=0; free(bc); return o;
    #undef OPUT
}
/* build a full GLSL fragment source for a warp (is_comp=0) or comp (is_comp=1) shader body */
static char* md_build_frag_src(const char*body,int is_comp){
    char*nb=md_normalise(body);
    const char*io = is_comp
      ? "void main(){\n vec2 uv=v_uv; vec2 uv_orig=v_uv;\n"
        " float rad=length((uv-0.5)*vec2(aspect.x,aspect.y))*2.0; float ang=atan(uv.y-0.5,uv.x-0.5);\n"
        " vec3 ret=vec3(0.0); vec3 hue_shader=vec3(1.0);\n"
      : "void main(){\n vec2 uv=v_uv; vec2 uv_orig=vec2(gl_FragCoord.x/u_res.x,1.0-gl_FragCoord.y/u_res.y);\n"
        " float rad=length((uv_orig-0.5)*vec2(aspect.x,aspect.y))*2.0; float ang=atan(uv_orig.y-0.5,uv_orig.x-0.5);\n"
        " vec3 ret=vec3(0.0); vec3 hue_shader=vec3(1.0);\n";
    const char*tail = " ret*=_md_finalmul; _fragColor=vec4(ret,1.0);\n}\n";
    size_t n=strlen(MD2_PRELUDE)+strlen(io)+strlen(nb)+strlen(tail)+8;
    char*out=malloc(n); snprintf(out,n,"%s%s%s%s",MD2_PRELUDE,io,nb,tail); free(nb); return out;
}
static GLuint md_build_prog(const char*vs_src,const char*fs_src){
    GLuint vs=gl_compile(GL_VERTEX_SHADER,vs_src), fs=gl_compile(GL_FRAGMENT_SHADER,fs_src);
    if(!vs||!fs){ if(vs)p_glDeleteShader(vs); if(fs)p_glDeleteShader(fs); return 0; }
    GLuint pr=p_glCreateProgram(); p_glAttachShader(pr,vs); p_glAttachShader(pr,fs); p_glLinkProgram(pr);
    GLint ok=0; p_glGetProgramiv(pr,GL_LINK_STATUS,&ok); p_glDeleteShader(vs); p_glDeleteShader(fs);
    if(!ok){ p_glDeleteProgram(pr); return 0; } return pr;
}
/* set the shared MilkDrop uniforms + bind samplers (main→unit0, noise→unit1) for a translated program */
static void md2_set_uniforms(GLuint pgm, MdPreset*P, float time, GLuint mainTex, float finalmul){
    MdVars*V=&P->V; float w=(float)mk_w, h=(float)mk_h, asp=h>0?w/h:1.0f;
    #define U1(n,val) do{ GLint _l=p_glGetUniformLocation(pgm,n); if(_l>=0)p_glUniform1f(_l,(val)); }while(0)
    #define U4(n,a,b,c,d) do{ GLint _l=p_glGetUniformLocation(pgm,n); if(_l>=0)p_glUniform4f(_l,(a),(b),(c),(d)); }while(0)
    U1("time",time); U1("fps",60.0f); U1("frame",V->val[P->iFrameN]); U1("progress",0.0f);
    U1("bass",V->val[P->iBass]); U1("bass_att",V->val[P->iBassA]); U1("mid",V->val[P->iMid]); U1("mid_att",V->val[P->iMidA]);
    U1("treb",V->val[P->iTreb]); U1("treb_att",V->val[P->iTrebA]);
    U1("vol",(V->val[P->iBass]+V->val[P->iMid]+V->val[P->iTreb])/3.0f);
    U4("texsize",w,h,1.0f/w,1.0f/h);
    U4("texsize_noise_lq",256,256,1.0f/256,1.0f/256); U4("texsize_noise_lq_lite",32,32,1.0f/32,1.0f/32);
    U4("texsize_noise_mq",256,256,1.0f/256,1.0f/256); U4("texsize_noise_hq",256,256,1.0f/256,1.0f/256);
    U4("texsize_noisevol_lq",32,32,1.0f/32,1.0f/32); U4("texsize_noisevol_hq",32,32,1.0f/32,1.0f/32);
    U4("aspect", asp>1?asp:1.0f, asp>1?1.0f:1.0f/asp, asp>1?1.0f/asp:1.0f, asp>1?1.0f:asp);
    { float r0=fmodf(time*0.37f,1.0f),r1=fmodf(time*0.71f,1.0f),r2=fmodf(time*0.13f,1.0f),r3=fmodf(time*0.53f,1.0f);
      U4("rand_frame",r0,r1,r2,r3); U4("rand_preset",0.37f,0.11f,0.83f,0.52f); }
    U1("_md_finalmul",finalmul); { GLint l=p_glGetUniformLocation(pgm,"u_res"); if(l>=0)p_glUniform2f(l,w,h); }
    for(int i=0;i<32;i++){ char qn[8]; int L=snprintf(qn,sizeof qn,"q%d",i+1); (void)L; GLint l=p_glGetUniformLocation(pgm,qn); if(l>=0)p_glUniform1f(l,V->val[P->iq[i]]); }
    p_glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,mainTex);
    p_glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D,md_noise_tex);
    static const char*mainS[]={"sampler_main","sampler_fw_main","sampler_pw_main","sampler_fc_main","sampler_pc_main","sampler_blur1","sampler_blur2","sampler_blur3",0};
    for(int i=0;mainS[i];i++){ GLint l=p_glGetUniformLocation(pgm,mainS[i]); if(l>=0)p_glUniform1i(l,0); }
    static const char*noiseS[]={"sampler_noise_lq","sampler_noise_lq_lite","sampler_noise_mq","sampler_noise_hq","sampler_noisevol_lq","sampler_noisevol_hq",0};
    for(int i=0;noiseS[i];i++){ GLint l=p_glGetUniformLocation(pgm,noiseS[i]); if(l>=0)p_glUniform1i(l,1); }
    p_glActiveTexture(GL_TEXTURE0);
    #undef U1
    #undef U4
}
/* compile a preset's shaders once (on first use); failures leave pgm_*=0 → classic fallback for that stage */
static void md_shaders_ensure(MdPreset*P){
    if(P->sh_tried) return; P->sh_tried=1;
    if(!P->has_warp && !P->has_comp) return;
    md_make_noise();
    if(P->has_warp){ char*s=md_build_frag_src(P->warp_src,0); P->pgm_warp=md_build_prog(MD_WARP_VS,s); free(s);
        if(!P->pgm_warp) fprintf(stderr,"[milk2] warp shader translate failed: %s\n",P->name); }
    if(P->has_comp){ char*s=md_build_frag_src(P->comp_src,1); P->pgm_comp=md_build_prog(VS_SRC,s); free(s);
        if(!P->pgm_comp) fprintf(stderr,"[milk2] comp shader translate failed: %s\n",P->name); }
}
static void md_free_preset(MdPreset*P){
    md_freeprog(&P->pf); md_freeprog(&P->pp);
    if(P->pgm_warp){ p_glDeleteProgram(P->pgm_warp); P->pgm_warp=0; }
    if(P->pgm_comp){ p_glDeleteProgram(P->pgm_comp); P->pgm_comp=0; }
    free(P->warp_src); free(P->comp_src); P->warp_src=P->comp_src=0;
}
/* run one preset frame: warp prev(mk_cur) → dst, seed waveform, (caller composites) */
static void md_frame(MdPreset*P,int dst,float time,float bass,float mid,float treb,float ba,float ma,float ta){
    MdVars*V=&P->V;
    md_shaders_ensure(P); V->val[P->iFrameN]+=1.0f;
    V->val[P->iTime]=time; V->val[P->iBass]=bass; V->val[P->iBassA]=ba; V->val[P->iMid]=mid; V->val[P->iMidA]=ma;
    V->val[P->iTreb]=treb; V->val[P->iTrebA]=ta; V->val[P->iFps]=60;
    /* base params (per_frame may overwrite) */
    V->val[P->iZoom]=P->zoom0; V->val[P->iZoomexp]=P->zoomexp0; V->val[P->iRot]=P->rot0; V->val[P->iWarp]=P->warp0;
    V->val[P->iCx]=P->cx0; V->val[P->iCy]=P->cy0; V->val[P->iDx]=P->dx0; V->val[P->iDy]=P->dy0; V->val[P->iSx]=P->sx0; V->val[P->iSy]=P->sy0;
    V->val[P->iWr]=P->wave_r; V->val[P->iWg]=P->wave_g; V->val[P->iWb]=P->wave_b; V->val[P->iWa]=1.0f; V->val[P->iWx]=0.5f; V->val[P->iWy]=0.5f;
    md_run(&P->pf,V);
    float bz=V->val[P->iZoom],bze=V->val[P->iZoomexp],br=V->val[P->iRot],bw=V->val[P->iWarp];
    float bcx=V->val[P->iCx],bcy=V->val[P->iCy],bsx=V->val[P->iSx],bsy=V->val[P->iSy],bdx=V->val[P->iDx],bdy=V->val[P->iDy];
    float wt=time*P->warpspeed, wsi=P->warpscale!=0?1.0f/P->warpscale:1.0f;
    float f0=11.68f+4*cosf(wt*1.413f+10), f1=8.77f+3*cosf(wt*1.113f+7), f2=10.54f+3*cosf(wt*1.233f+3), f3=11.49f+4*cosf(wt*0.933f+5);
    /* compute source uv per grid vertex */
    static float uu[MDGY][MDGX], vv[MDGY][MDGX];
    for(int j=0;j<MDGY;j++) for(int i=0;i<MDGX;i++){
        float gx=(float)i/(MDGX-1), gy=(float)j/(MDGY-1); float x=gx*2-1, y=gy*2-1;
        float rad=sqrtf(x*x+y*y)*0.70711f, ang=atan2f(y,x);
        if(P->pp.n){ V->val[P->iZoom]=bz;V->val[P->iZoomexp]=bze;V->val[P->iRot]=br;V->val[P->iWarp]=bw;
            V->val[P->iCx]=bcx;V->val[P->iCy]=bcy;V->val[P->iSx]=bsx;V->val[P->iSy]=bsy;V->val[P->iDx]=bdx;V->val[P->iDy]=bdy;
            V->val[P->iX]=gx;V->val[P->iY]=gy;V->val[P->iRad]=rad;V->val[P->iAng]=ang; md_run(&P->pp,V); }
        float zoom=V->val[P->iZoom],zexp=V->val[P->iZoomexp],rot=V->val[P->iRot],warp=V->val[P->iWarp];
        float cx=V->val[P->iCx],cy=V->val[P->iCy],sx=V->val[P->iSx],sy=V->val[P->iSy],dxv=V->val[P->iDx],dyv=V->val[P->iDy];
        if(zoom<0.001f)zoom=0.001f; float zoom2=powf(zoom, powf(zexp, rad*2-1)); float zr=zoom2!=0?1.0f/zoom2:1.0f;
        float u=x*0.5f*zr+0.5f, v=y*0.5f*zr+0.5f;
        if(sx!=0){u=(u-0.5f)/sx+0.5f;} if(sy!=0){v=(v-0.5f)/sy+0.5f;}
        float u2=u-cx,v2=v-cy,cr=cosf(rot),sr=sinf(rot); u=u2*cr-v2*sr+cx; v=u2*sr+v2*cr+cy;
        u += warp*0.0035f*( sinf(wt*0.333f+wsi*(x*f0-y*f3)) + cosf(wt*0.375f-wsi*(x*f1+y*f2)) );
        v += warp*0.0035f*( cosf(wt*0.288f-wsi*(x*f2-y*f0)) + sinf(wt*0.418f+wsi*(x*f3+y*f1)) );
        u -= dxv; v -= dyv;
        uu[j][i]=u; vv[j][i]=v;
    }
    /* build triangle list (pos in NDC, uv = source) */
    int n=0; float*V2=md_verts;
    for(int j=0;j<MDGY-1;j++) for(int i=0;i<MDGX-1;i++){
        float x0=(float)i/(MDGX-1)*2-1, x1=(float)(i+1)/(MDGX-1)*2-1;
        float y0=(float)j/(MDGY-1)*2-1, y1=(float)(j+1)/(MDGY-1)*2-1;
        #define VtX(px,py,ii,jj) do{ V2[n*4]=px; V2[n*4+1]=py; V2[n*4+2]=uu[jj][ii]; V2[n*4+3]=vv[jj][ii]; n++; }while(0)
        VtX(x0,y0,i,j); VtX(x1,y0,i+1,j); VtX(x1,y1,i+1,j+1);
        VtX(x0,y0,i,j); VtX(x1,y1,i+1,j+1); VtX(x0,y1,i,j+1);
        #undef VtX
    }
    /* warp pass: prev(mk_cur) → dst. If the preset has a translated MilkDrop2 warp shader, run it over
       the SAME warped mesh (the mesh gives it the sampling coord `uv`); otherwise the classic sample×decay. */
    p_glBindFramebuffer(GL_FRAMEBUFFER,mk_fbo[dst]); glViewport(0,0,mk_w,mk_h); glDisable(GL_BLEND);
    if(P->pgm_warp){ p_glUseProgram(P->pgm_warp); md2_set_uniforms(P->pgm_warp,P,time,mk_tex[mk_cur],1.0f); }
    else { p_glUseProgram(md_prog);
        p_glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,mk_tex[mk_cur]); p_glUniform1i(md_uTex,0);
        p_glUniform1f(md_uDecay,P->decay); }
    p_glBindVertexArray(md_vao); p_glBindBuffer(GL_ARRAY_BUFFER,md_vbo);
    p_glBufferData(GL_ARRAY_BUFFER,(long)n*4*sizeof(float),md_verts,GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES,0,n);
    /* seed waveform (audio) into dst — additive so it accumulates into the flow */
    ui_n=0;
    { float W=g_win_w,H=g_win_h,unit=(W<H?W:H);
      float r=V->val[P->iWr], g=V->val[P->iWg], b=V->val[P->iWb], a=V->val[P->iWa];   /* animated wave colour */
      float cx=W*V->val[P->iWx], cy=H*V->val[P->iWy];                                 /* animated wave position */
      if(r<0)r=0; if(g<0)g=0; if(b<0)b=0; if(a<0.3f)a=0.9f; if(a>1)a=1;
      float mx=r>g?r:g; mx=mx>b?mx:b; if(mx<0.15f){r=0.8f;g=0.85f;b=0.95f;}          /* avoid near-black waves */
      if(P->wavemode==0 || P->wavemode==2){ int M=220; float base=unit*(0.12f+bass*0.03f), aw=unit*0.10f*P->wave_scale,px=0,py=0; int have=0;
          for(int i=0;i<=M;i++){ int idx=(g_viz_w-M+i)&(VIZ_N-1); float s=g_viz_pcm[idx];
              float an=2*VIZ_PI*i/M, rad=base+s*aw, x=cx+cosf(an)*rad, y=cy+sinf(an)*rad;
              if(have) viz_seg(px,py,x,y,2.4f,r,g,b,a); px=x;py=y;have=1; } }
      else { int M=250; float A=unit*0.26f*P->wave_scale,px=0,py=0; int have=0;
          for(int i=0;i<M;i++){ int idx=(g_viz_w-M+i)&(VIZ_N-1); float s=g_viz_pcm[idx];
              float x=W*i/(float)M, y=cy-s*A; if(have) viz_seg(px,py,x,y,2.4f,r,g,b,a); px=x;py=y;have=1; } }
    }
    ui_flush();   /* renders into dst (still bound) */
}
static const char *MK_STYLE_NAME[4] = {"Spectrum","Fire","Ocean","Magenta"};
static const float MK_STYLE_SCALE[4] = {2.3f, 3.0f, 1.8f, 2.6f};   /* noise zoom per style */
static const float MK_STYLE_WARP [4] = {0.55f, 0.9f, 0.35f, 0.7f}; /* turbulence per style */
static void mk_plasma_draw(float time,float amp,float bass,float mid,float treble,float hue,int style){
    glDisable(GL_BLEND);
    p_glUseProgram(mk_plasma);
    p_glUniform1f(mkp_time,time); p_glUniform1f(mkp_aspect,(float)g_win_w/g_win_h);
    p_glUniform1f(mkp_amp,amp); p_glUniform1f(mkp_bass,bass); p_glUniform1f(mkp_mid,mid);
    p_glUniform1f(mkp_treble,treble); p_glUniform1f(mkp_hue,hue);
    p_glUniform1f(mkp_scale,MK_STYLE_SCALE[style]); p_glUniform1f(mkp_warp,MK_STYLE_WARP[style]); p_glUniform1i(mkp_style,style);
    p_glBindVertexArray(gl_vao); glDrawArrays(GL_TRIANGLE_STRIP,0,4);
}
/* fallback flowing plasma (used when no .milk presets are installed) */
static void draw_plasma(int mode){
    if(!mk_plasma) return;
    float amp=0; for(int i=0;i<256;i++) amp+=fabsf(g_viz_pcm[i]); amp/=256.0f;
    static float g_energy=0.0f;
    float e_in = amp*9.0f - 0.03f; if(e_in<0) e_in=0; if(e_in>1) e_in=1;
    g_energy += (e_in - g_energy)*0.05f; float en=g_energy;
    static float mtime=0.0f; mtime += 0.006f + en*0.14f;
    static float sb=0,sm=0,st=0;
    { float bass=0,mid=0,treble=0; for(int i=0;i<8;i++)bass+=g_vz_band[i]; bass/=8;
      for(int i=8;i<30;i++)mid+=g_vz_band[i]; mid/=22; for(int i=30;i<VIZ_BANDS;i++)treble+=g_vz_band[i]; treble/=(VIZ_BANDS-30);
      sb+=(bass-sb)*0.1f; sm+=(mid-sm)*0.1f; st+=(treble-st)*0.1f; }
    static int last_mode=-1, style=0; static Uint32 sw_t=0; Uint32 nowt=SDL_GetTicks();
    if(!sw_t) sw_t=nowt;
    if(mode!=last_mode){ last_mode=mode; style=mode&3; sw_t=nowt; toast(MK_STYLE_NAME[style]); }
    else if(nowt-sw_t>25000){ sw_t=nowt; style=(style+1)&3; toast(MK_STYLE_NAME[style]); }
    p_glBindFramebuffer(GL_FRAMEBUFFER,0); glViewport(0,0,g_win_w,g_win_h);
    mk_plasma_draw(mtime, en, sb, sm, st, mtime*0.04f, style);
    ui_n=0;
}
/* diagnostic: translate+compile every preset's MilkDrop2 shaders through the live GL context and write
   a pass/fail report. Triggered by HOGS_MILKTEST=<outfile>; runs once then the app keeps running. */
static void md_selftest(const char*outpath){
    FILE*o=fopen(outpath,"w"); if(!o) return;
    int nw=0,nwok=0,nc=0,ncok=0,nfile=0;
    for(int i=0;i<md_nfiles;i++){ MdPreset P; if(!md_load(md_files[i],&P)) continue; nfile++;
        if(P.has_warp){ nw++; char*s=md_build_frag_src(P.warp_src,0); g_gl_last_log[0]=0; GLuint g=md_build_prog(MD_WARP_VS,s); free(s);
            if(g){ nwok++; p_glDeleteProgram(g); } else fprintf(o,"WARP FAIL: %s :: %s\n",P.name,g_gl_last_log); }
        if(P.has_comp){ nc++; char*s=md_build_frag_src(P.comp_src,1); g_gl_last_log[0]=0; GLuint g=md_build_prog(VS_SRC,s); free(s);
            if(g){ ncok++; p_glDeleteProgram(g); } else fprintf(o,"COMP FAIL: %s :: %s\n",P.name,g_gl_last_log); }
        md_free_preset(&P); }
    fprintf(o,"\n=== %d presets: warp %d/%d ok, comp %d/%d ok ===\n",nfile,nwok,nw,ncok,nc);
    fclose(o);
}
static void draw_milkdrop(int mode){
    if(!mk_init()) return;
    md_scan();
    { static int st=0; const char*mt=getenv("HOGS_MILKTEST"); if(mt&&!st){ st=1; md_make_noise(); md_selftest(mt); } }
    const int N=1024; static float re[1024], im[1024]; int w=g_viz_w;
    for(int i=0;i<N;i++){ int idx=(w-N+i)&(VIZ_N-1); float win=0.5f-0.5f*cosf(2*VIZ_PI*i/(N-1)); re[i]=g_viz_pcm[idx]*win; im[i]=0; }
    viz_fft(re,im,N); viz_analyze(re,im,N);
    if(md_nfiles==0 || !md_glinit()){ draw_plasma(mode); return; }   /* no presets installed → plasma */
    /* audio → MilkDrop bass/mid/treb (~1.0 = recent average, >1 on hits) + attack-smoothed *_att */
    static float ra_b=0.15f,ra_m=0.15f,ra_t=0.15f, ab=1,am=1,at=1;
    float rb=0,rm=0,rt=0; for(int i=0;i<8;i++)rb+=g_vz_band[i]; rb/=8;
    for(int i=8;i<30;i++)rm+=g_vz_band[i]; rm/=22; for(int i=30;i<VIZ_BANDS;i++)rt+=g_vz_band[i]; rt/=(VIZ_BANDS-30);
    ra_b+=(rb-ra_b)*0.01f; ra_m+=(rm-ra_m)*0.01f; ra_t+=(rt-ra_t)*0.01f;
    float bass=ra_b>1e-3f?rb/ra_b:1, mid=ra_m>1e-3f?rm/ra_m:1, treb=ra_t>1e-3f?rt/ra_t:1;
    if(bass>4)bass=4; if(mid>4)mid=4; if(treb>4)treb=4;
    ab+=(bass-ab)*0.2f; am+=(mid-am)*0.2f; at+=(treb-at)*0.2f;
    /* preset: load lazily; auto-cycle ~20s; V advances immediately. Skip presets that fail to load. */
    static MdPreset cur; static int loaded=0, idx=0, last_mode=-1; static Uint32 sw_t=0; static float pbass=1;
    Uint32 nowt=SDL_GetTicks(); if(!sw_t)sw_t=nowt;
    /* AUDIO-DRIVEN ORCHESTRATION: change preset on a strong bass transition (rising edge, 12s cooldown),
       so the show turns over WITH the music — not just on a blind timer (which is the 25s backstop). */
    int beatswitch = (bass>2.6f && pbass<=2.6f && (nowt-sw_t)>12000); pbass=bass;
    /* Random pick over the WHOLE set so any of the (potentially 8000+) presets has an equal chance
       to appear each switch — sequential cycling would never reach the tail. Small self-contained LCG. */
    static unsigned mrng=0; if(!mrng) mrng=nowt|1u;
    #define MD_PICK() do{ mrng=mrng*1664525u+1013904223u; idx=(int)((mrng>>8)%(unsigned)md_nfiles); }while(0)
    int want = !loaded;
    if(mode!=last_mode){ last_mode=mode; MD_PICK(); want=1; sw_t=nowt; }
    else if(beatswitch || nowt-sw_t>25000){ sw_t=nowt; MD_PICK(); want=1; }
    /* test hook: HOGS_MILKFAST=<ms> walks EVERY preset in order, one per <ms> (crash/leak sweep) */
    { static int fast=-1; if(fast<0){ const char*e=getenv("HOGS_MILKFAST"); fast=e?atoi(e):0; }
      if(fast>0 && loaded && nowt-sw_t>=(Uint32)fast){ sw_t=nowt; idx=(idx+1)%md_nfiles; want=1;
          if(idx%500==0) fprintf(stderr,"[milkfast] preset %d/%d\n", idx, md_nfiles); } }
    if(want){ int tries=0; for(; tries<md_nfiles; tries++){
            MdPreset np; if(md_load(md_files[idx],&np)){ if(loaded){ md_free_preset(&cur); } cur=np; loaded=1; toast(cur.name); break; }
            idx=(idx+1)%md_nfiles; }
        if(!loaded){ draw_plasma(mode); return; } }
    float mtime=SDL_GetTicks()*0.001f;
    int dst=mk_cur^1;
    md_frame(&cur, dst, mtime, bass,mid,treb, ab,am,at);
    if(cur.pgm_comp){   /* MilkDrop2 composite shader → screen */
        p_glBindFramebuffer(GL_FRAMEBUFFER,0); glViewport(0,0,g_win_w,g_win_h); glDisable(GL_BLEND);
        p_glUseProgram(cur.pgm_comp); md2_set_uniforms(cur.pgm_comp,&cur,mtime,mk_tex[dst], cur.gamma>0?cur.gamma:1.0f);
        p_glBindVertexArray(gl_vao); glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    } else {
        mk_do_comp(dst, cur.gamma, 1.0f, cur.solarize, cur.invert, cur.darkcenter);
    }
    mk_cur=dst; ui_n=0;   /* clean UI buffer for the OSD on top */
}
static void draw_visualizer(void){
    if(!g_viz_has) return;
    if(g_viz_mode==3){ draw_matrix(); return; }
    draw_milkdrop(g_viz_mode);   /* 0/1/2 seed different preset regions; all auto-cycle */
}
/* ---- ambient idle screen: the feedback engine driven autonomously (no audio) so the player has
   soul while it sits waiting. A morphing harmonograph ribbon + breathing seed-head feed the flow.
   `wake` fades the injected energy after a minute of no interaction, so it calms to a slow drift
   instead of hammering the GPU all night (kind to a card with the TDR history). ~30fps capped. */
static void idle_inject(float it, float wake){
    float W=g_win_w, H=g_win_h, cx=W*0.5f, cy=H*0.5f, unit=(W<H?W:H), r,g,b;
    /* harmonograph: two summed sinusoids per axis, frequencies & phases drifting slowly with time,
       so the Lissajous figure morphs endlessly and never repeats on any human timescale. */
    float f1=2.0f+0.30f*sinf(it*0.050f), f2=3.0f+0.20f*sinf(it*0.037f);
    float f3=3.0f+0.25f*sinf(it*0.041f), f4=2.0f+0.30f*sinf(it*0.033f);
    float p1=it*0.11f,p2=it*0.07f,p3=it*0.13f,p4=it*0.09f, A=unit*0.30f;
    int M=420; float px=0,py=0; int have=0;
    for(int i=0;i<=M;i++){ float s=(float)i/M, T=s*6.2831853f*3.0f;
        float x=cx+A*(sinf(f1*T+p1)+0.6f*sinf(f2*T+p2))*0.55f;
        float y=cy+A*(sinf(f3*T+p3)+0.6f*sinf(f4*T+p4))*0.55f;
        viz_hsv(vz_wrap(0.55f+s*0.5f+it*0.02f),0.70f,1.0f,&r,&g,&b);
        if(have) viz_seg(px,py,x,y, 2.5f, r,g,b, 0.6f*wake);
        px=x; py=y; have=1; }
    float gold=2.39996323f; int seeds=180;
    for(int i=0;i<seeds;i++){ float fr=(float)i/seeds, ang=i*gold+it*0.4f, rad=sqrtf(fr)*unit*0.42f;
        float e=0.35f+0.30f*sinf(it*0.5f+fr*8.0f), x=cx+cosf(ang)*rad, y=cy+sinf(ang)*rad, sz=1.5f+e*5.0f;
        viz_hsv(vz_wrap(fr*0.4f+it*0.03f),0.70f,1.0f,&r,&g,&b);
        ui_rect(x-sz*0.5f,y-sz*0.5f,sz,sz, r,g,b, (0.30f+0.40f*e)*wake); }
}
static void draw_idle(void){
    if(!mk_init() || !mk_plasma){ glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT); return; }
    static float it=0; it+=0.016f;
    float idle_ms=(float)(SDL_GetTicks()-osd_last_active), wake=1.0f;
    if(idle_ms>60000.0f){ wake=1.0f-(idle_ms-60000.0f)/30000.0f; if(wake<0.12f) wake=0.12f; }
    /* idle screensaver (no audio): the plasma drifts gently on its own and dims after a minute idle */
    p_glBindFramebuffer(GL_FRAMEBUFFER,0); glViewport(0,0,g_win_w,g_win_h);
    mk_plasma_draw(it*0.5f, 0.28f*wake, 0.16f*wake, 0.12f*wake, 0.09f*wake, it*0.02f, 2);   /* calm Ocean */
    /* faint wordmark, crisp on top of the flow (theme-coloured, fades with wake) */
    ui_n=0;
    { const char *nm="hogofwar"; float ps=64.0f, tw=ui_textw(ps,nm);
      ui_text((g_win_w-tw)*0.5f, g_win_h*0.5f-ps*0.75f, ps, nm, TH->text[0],TH->text[1],TH->text[2],0.14f*wake+0.05f);
      const char *sb="media player"; float pz=22.0f, zw=ui_textw(pz,sb);
      ui_text((g_win_w-zw)*0.5f, g_win_h*0.5f+ps*0.35f, pz, sb, TH->text[0],TH->text[1],TH->text[2],0.11f*wake+0.03f); }
    ui_flush(); ui_n=0;
}

/* detailed source-vs-render statistics overlay (Stats button / I key) */
static void draw_stats_panel(void){
    float pad=16, px=15, lh=26, x=18, y=18;
    /* colour palette: 0 neutral · 1 green(active+improving) · 2 amber(working) · 3 grey(off) ·
       4 red(not effective) · 5 title. Lines store a colour INDEX (ternary-friendly). */
    static const float CPAL[6][3] = {
        {0.90f,0.92f,1.00f},{0.40f,0.92f,0.45f},{0.97f,0.78f,0.32f},
        {0.55f,0.55f,0.62f},{0.95f,0.45f,0.45f},{0.66f,0.58f,1.00f} };
    enum { CW=0, CG=1, CA=2, CX=3, CR=4, CT=5 };
    char L[24][168]; int LCI[24]; int n=0;
    #define LINE(ci,...) do{ LCI[n]=(ci); snprintf(L[n],168,__VA_ARGS__); n++; }while(0)
    const char*nm=(pl_cur>=0&&pl_cur<pl_n)?pl_items[pl_cur]:""; const char*bn=nm; for(const char*q=nm;*q;q++) if(*q=='/'||*q=='\\')bn=q+1;
    int enh = g_playing_enhanced;   /* showing an enhanced-output file → report upscaled res / smoother fps */

    LINE(CT, "STATISTICS");
    LINE(CW, "File: %.46s", bn[0]?bn:"(none)");
    if(g_src_w>0 && P.is_image){
        LINE(CW, "Still image  %dx%d", g_src_w, g_src_h);
    } else if(g_src_w>0){
        LINE(CW, "Source: %s  %dx%d  %.4g fps", g_src_codec, g_orig_w?g_orig_w:g_src_w, g_orig_h?g_orig_h:g_src_h, g_orig_fps>0?g_orig_fps:g_src_fps);
        LINE(g_hw_pixfmt!=AV_PIX_FMT_NONE?CG:CW, "Decode: %s%s", g_hw_pixfmt!=AV_PIX_FMT_NONE?"GPU hardware (D3D11VA)":"CPU software",
             g_vd.run?"  + decode thread":"");
        if(vt_hdr) LINE(CG, "HDR: %s  \342\206\222 tone-mapped to SDR (BT.2390)", vt_hdr==1?"PQ / HDR10":"HLG");
        int better_res = enh && g_src_w>g_orig_w;
        LINE(better_res?CG:CW, "Now showing: %dx%d%s", g_src_w, g_src_h, better_res?"   (AI upscaled)":"");
        double cfps = g_src_fps>0?g_src_fps:g_orig_fps;
        int hitting = P.paused || cfps<=0 || g_vid_fps >= cfps*0.85;
        int better_fps = enh && g_orig_fps>0 && cfps > g_orig_fps+0.5;
        LINE(!hitting?CR:(better_fps?CG:CW), "Playing at: %.1f fps (target %.4g)%s",
             g_vid_fps, cfps, P.paused?"  paused":(!hitting?"  STUTTERING":(better_fps?"  (smoother)":"")));
    } else {
        LINE(CX, "Audio only - visualizer active (V to change)");
    }
    if(g_src_arate>0) LINE(CW, "Audio: %s  %d Hz  %d ch", g_src_acodec, g_src_arate, g_src_ach);

    /* If enhancement is toggled ON but the source is already high-res, the player skips it and plays
       the original (Phase 6). Report that honestly instead of a misleading "Upscale ON". Thresholds
       match enhance_apply / the enhancer (upscale >=1440, frame-gen >=1800). */
    int req_enh = (P.upscale||P.framegen||P.anime);
    int up_eff  = (P.upscale||P.anime);   /* x1 = native-res RESTORE now, so upscale is always effective */
    int fg_eff  = P.framegen;   /* 4K frame-gen is back (RIFE at capped working res) */
    int skipped_hires = req_enh && !g_playing_enhanced && !enh_running() && !up_eff && !fg_eff;
    if(skipped_hires){
        LINE(CW, "AI Enhance:  source already %dp — upscale/frame-gen not needed", g_src_h);
    } else {
        LINE(P.light?CG:CX, "Light Enhance: %s   (Lanczos-3 + blend frame-gen + \342\210\2322 DELTA, live shader)", P.light?"ON":"off");
        if(g_enh.is_arch && enh_running()) LINE(CG, "Archive: rendering \342\200\224 will save to %.60s", g_enh.arch_path);
        LINE((req_enh||g_playing_enhanced)?CG:CX, "AI Enhance:  Upscale %s   Frame-Gen %s   Anime %s%s",
             P.upscale?"ON":"off", P.framegen?"ON":"off", P.anime?"ON":"off",
             g_playing_enhanced?"   (showing enhanced)":(enh_running()?"   (rendering\342\200\246)":""));
    }

    LINE(CW, "Aspect: %s", P.fill?"Fill":"Letterbox");
    LINE((g_subs_enabled&&g_nsubs>0)?CG:CX, "Subtitles: %s  (%d cues)", (g_subs_enabled&&g_nsubs>0)?"on":"off", g_nsubs);

    float w=0; for(int i=0;i<n;i++){ float lw=ui_textw(i==0?18.0f:px, L[i]); if(lw>w) w=lw; }
    w += 2*pad;
    float h=n*lh+2*pad;
    ui_rect(x,y,w,h, 0.03f,0.03f,0.06f,0.92f);
    ui_rect(x,y,w,3, 0.5f,0.4f,0.9f,1.0f);
    for(int i=0;i<n;i++){ const float*c=CPAL[LCI[i]]; ui_text(x+pad, y+pad+i*lh, i==0?18.0f:px, L[i], c[0],c[1],c[2],1.0f); }
    #undef LINE
}
/* full hotkey list, anchored to the bottom of the screen (Keys button / K / ?) */
/* donations (owner request 2026-10-29). Checksum-verified bech32 P2WPKH mainnet address — change it ONLY
   together with the README/README.txt copies. */
static const char HOGS_BTC[] = "bc1q5m6vvn7w2htcglefmvqq5aku64fhuvezf9sg29";
/* the owner's QR code for that address (33x33, version 4-Q), one row per entry, bit c = column c.
   Taken from the owner's image and VERIFIED by a full decode (format BCH + Reed-Solomon syndromes all
   zero) to read exactly HOGS_BTC. Regenerate both together if the address ever changes. */
static const unsigned long long HOGS_BTC_QR[33] = {
0x1FCCB997Full, 0x10495CA41ull, 0x175559D5Dull,
0x17496FD5Dull, 0x174FCEE5Dull, 0x1042ECF41ull,
0x1FD55557Full, 0x000633700ull, 0x16E21B7EAull,
0x1A4579ABBull, 0x1341CE2C5ull, 0x103E53D38ull,
0x1A6352CE8ull, 0x135352AAFull, 0x0D28B1FE8ull,
0x187B5AF0Full, 0x19C76854Bull, 0x009A12B03ull,
0x17864C054ull, 0x081356319ull, 0x00BA5554Full,
0x1ED2A7126ull, 0x1352E21D3ull, 0x0A3FAD696ull,
0x03F3CC9E5ull, 0x1B17DDF00ull, 0x0D526E37Full,
0x031F91941ull, 0x1BF20725Dull, 0x0BD76775Dull,
0x178B30E5Dull, 0x0332B8741ull, 0x09B270A7Full };
static void donate_copy(void){
    if(SDL_SetClipboardText(HOGS_BTC) == 0) toast("Bitcoin address copied \342\200\224 thank you for supporting Hogs media player!");
    else toast("Could not copy \342\200\224 the address is in README.txt");
}
static void draw_keys_panel(void){
    static const char *K[] = {
      "Space  Play / Pause","S  Stop","Left/Right  Seek -/+10s","Up/Down  Volume (0-200%)","M  Mute","Y  Theme (Shift+Y back)",
      "F  Fullscreen","Dbl-click  Fullscreen","C  Fill / Letterbox","N / P  Next / Prev",
      "R  Repeat","H  Shuffle","U  AI Upscale (real footage)","G  AI Frame-Gen (RIFE)",
      "E  Upscale+FrameGen (one click)","T  AnimeUpscale (toon model)",
      "A  Audio track","B  Subtitles on/off","V  Visualizer","I  Stats","K or ?  This list",
      "L  Playlist","Del  Remove selected","Right-click  Audio/Sub menu","Esc  Exit fullscreen","Drag files/folders to add" };
    int n=(int)(sizeof(K)/sizeof(K[0]));
    float W=g_win_w; int cols=3; if(W<980)cols=2; if(W<640)cols=1; float colw=W/cols;
    float px=15, lh=24; int rows=(n+cols-1)/cols;
    const float qm=4, qz=2*qm, qs=33*qm+2*qz;         /* QR: 4 px modules + 2-module white margin */
    float dh=qs+20;                                   /* donation section under the key list */
    float ph=rows*lh+44+dh;
    float y=g_win_h - (osd_visible?150.0f:16.0f) - ph; if(y<8)y=8;
    ui_rect(8,y,W-16,ph, 0.02f,0.02f,0.05f,0.92f);
    ui_rect(8,y,W-16,3, 0.5f,0.4f,0.9f,1.0f);
    ui_text(18,y+8,17,"HOTKEYS   (K or ? to close)",0.85f,0.8f,1.0f,1.0f);
    for(int i=0;i<n;i++){ int c=i/rows, r=i%rows; ui_text(18+c*colw, y+38+r*lh, px, K[i], 0.9f,0.92f,0.96f,1.0f); }
    /* SUPPORT: scannable QR, then label, the address, and a Copy button (Ctrl+B does the same) */
    float dy=y+ph-dh+6;
    ui_rect(18,dy-6,W-52,1, 0.5f,0.4f,0.9f,0.5f);
    float qx=18, qy=dy+4;
    ui_rect(qx,qy,qs,qs, 1,1,1,1);                    /* white quiet zone: phones need it to lock on */
    for(int r=0;r<33;r++) for(int c=0;c<33;c++)
        if((HOGS_BTC_QR[r]>>c)&1ull) ui_rect(qx+qz+c*qm, qy+qz+r*qm, qm, qm, 0,0,0,1);
    float tx=qx+qs+18, ty=qy+8;
    const char *lab="\342\231\245 Enjoying Hogs media player? Donate Bitcoin:";
    ui_text(tx,ty,16,lab,1.0f,0.62f,0.80f,1.0f);
    ui_text(tx,ty+30,16,HOGS_BTC,0.95f,0.95f,0.80f,1.0f);
    float bx=tx, by=ty+64, bw=ui_textw(14,"Copy address  (Ctrl+B)")+20, bh=28;
    int hov=(g_mouse_x>=bx&&g_mouse_x<=bx+bw&&g_mouse_y>=by&&g_mouse_y<=by+bh);
    ui_rect(bx,by,bw,bh, 0.5f,0.4f,0.9f, hov?1.0f:0.75f);
    ui_text(bx+10,by+6,14,"Copy address  (Ctrl+B)",1,1,1,1);
    osd_addbtn(bx,by,bw,bh,B_DONATE);
    ui_text(tx,by+44,13,"or scan the code with your phone's Bitcoin wallet",0.65f,0.65f,0.8f,1.0f);
}
/* right-click context menu: pick audio track / toggle subtitles */
static void draw_context_menu(void){
    g_ctx_n=0;
    float px=15, lh=26, w=320, pad=8;
    int aidx[32], na=0;
    if(P.fmt) for(unsigned i=0;i<P.fmt->nb_streams && na<32;i++)
        if(P.fmt->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_AUDIO) aidx[na++]=(int)i;
    int lines = 1 + (na?na:1) + 1 + 1 + (g_nsubs>0?1:0);
    float h=lines*lh+2*pad;
    float x=g_ctx_x, y=g_ctx_y;
    if(x+w>g_win_w) x=g_win_w-w-4; if(y+h>g_win_h) y=g_win_h-h-4; if(x<0)x=0; if(y<0)y=0;
    ui_rect(x,y,w,h, 0.06f,0.06f,0.09f,0.97f);
    ui_rect(x,y,w,3, 0.5f,0.4f,0.9f,1.0f);
    float cy=y+pad;
    ui_text(x+10,cy,13,"AUDIO TRACKS",0.65f,0.65f,0.82f,1.0f); cy+=lh;
    if(na==0){ ui_text(x+18,cy,px,"(none)",0.6f,0.6f,0.6f,1.0f); cy+=lh; }
    for(int i=0;i<na;i++){
        int si=aidx[i]; int cur=(si==P.astream);
        if(g_mouse_x>=x&&g_mouse_x<=x+w&&g_mouse_y>=cy-2&&g_mouse_y<cy+lh-2) ui_rect(x+2,cy-2,w-4,lh,1,1,1,0.08f);
        AVDictionaryEntry*lang=av_dict_get(P.fmt->streams[si]->metadata,"language",NULL,0);
        const AVCodec*c=avcodec_find_decoder(P.fmt->streams[si]->codecpar->codec_id);
        char line[120]; snprintf(line,sizeof(line),"%s %d: %s %s", cur?">":"  ", i+1, c?c->name:"?", lang?lang->value:"");
        ui_text(x+18,cy,px,line, cur?0.8f:0.92f, cur?0.7f:0.92f, 1.0f,1.0f);
        if(g_ctx_n<CTX_MAX){ g_ctx[g_ctx_n]=(CtxItem){x,cy-2,w,lh,1,si}; g_ctx_n++; }
        cy+=lh;
    }
    ui_text(x+10,cy,13,"SUBTITLES",0.65f,0.65f,0.82f,1.0f); cy+=lh;
    { int off=!g_subs_enabled;
      if(g_mouse_x>=x&&g_mouse_x<=x+w&&g_mouse_y>=cy-2&&g_mouse_y<cy+lh-2) ui_rect(x+2,cy-2,w-4,lh,1,1,1,0.08f);
      char line[64]; snprintf(line,sizeof(line),"%s Off", off?">":"  ");
      ui_text(x+18,cy,px,line, 0.92f,0.92f,1.0f,1.0f);
      if(g_ctx_n<CTX_MAX){ g_ctx[g_ctx_n]=(CtxItem){x,cy-2,w,lh,2,0}; g_ctx_n++; } cy+=lh; }
    if(g_nsubs>0){
      int on=g_subs_enabled;
      if(g_mouse_x>=x&&g_mouse_x<=x+w&&g_mouse_y>=cy-2&&g_mouse_y<cy+lh-2) ui_rect(x+2,cy-2,w-4,lh,1,1,1,0.08f);
      char line[80]; snprintf(line,sizeof(line),"%s External (%d cues)", on?">":"  ", g_nsubs);
      ui_text(x+18,cy,px,line, 0.92f,0.92f,1.0f,1.0f);
      if(g_ctx_n<CTX_MAX){ g_ctx[g_ctx_n]=(CtxItem){x,cy-2,w,lh,3,0}; g_ctx_n++; } cy+=lh;
    }
}
/* animated busy spinner — a rotating ring of dots with a trailing fade + a label. Drawn on top while a
   background playlist worker (folder scan / length probe) runs, so the app reads as WORKING, not frozen. */
static void draw_spinner(void){
    if(!ui_prog) return;
    float W=g_win_w, H=g_win_h, cx, cy;
    if(show_playlist){ float pw=g_pl_w>0?g_pl_w:W*0.34f; if(pw<300)pw=300; if(pw>W*0.92f)pw=W*0.92f; cx=W-pw*0.5f; cy=(H-150)*0.5f; }
    else { cx=W*0.5f; cy=H*0.5f; }
    ui_n=0;
    float bw=220, bh=120;
    ui_rect(cx-bw*0.5f, cy-bh*0.5f, bw, bh, 0,0,0,0.55f);          /* dim backing card */
    float t=SDL_GetTicks()*0.001f, R=26, ringY=cy-16, head=t*2.4f; /* rotating ring above the label */
    int Nd=12;
    for(int i=0;i<Nd;i++){
        float a=head - (float)i*(2*VIZ_PI/Nd);
        float px=cx+cosf(a)*R, py=ringY+sinf(a)*R, al=1.0f-(float)i/Nd, sz=5.0f;
        ui_rect(px-sz*0.5f, py-sz*0.5f, sz, sz, TH->accent[0],TH->accent[1],TH->accent[2], al);
    }
    float tw=ui_textw(15,g_busy_label);
    ui_text4(cx-tw*0.5f, cy+26, 15, g_busy_label, TH->text);
    ui_flush(); ui_n=0;
}
/* build + draw the auto-hiding control bar + overlays */
static void draw_osd(double clk){
    ui_n = 0; osd_nbtn = 0;
    if(!ui_prog){ seek_x0=seek_x1=0; return; }

    if(P.fmt && P.vstream < 0) draw_visualizer();   /* audio-only → show a visualizer */

    float W=g_win_w, H=g_win_h;
    if(osd_visible){
        /* button descriptors (icon, label, active, id) */
        struct { int ic; const char*lbl; int act; int id; } BT[] = {
            { IC_PREV,"Prev",0,B_PREV },
            /* explicit transport: a dedicated Play (= what Spacebar does: start/resume) next to Pause,
               instead of one button that changes meaning. Both light up to show the current state. */
            { IC_PLAY, "Play",  P.fmt && !P.paused, B_PLAY },
            { IC_PAUSE,"Pause", P.fmt &&  P.paused && !g_stopped, B_PAUSE2 },
            { IC_STOP, "Stop",  P.fmt &&  g_stopped, B_STOP },
            { IC_NEXT,"Next",0,B_NEXT },
            { IC_FILL,"Fill",P.fill,B_FILL },
            { IC_UPSCALE,"AI Upscale",P.upscale,B_U },      /* U — Real-ESRGAN plus (real footage) */
            { IC_FRAMEGEN,"AI FrameGen",P.framegen,B_G },    /* G — RIFE frame-gen (offline whole-video) */
            { IC_UPSCALE,"Up+FrameGen",P.upscale&&P.framegen,B_ENH }, /* E — both in ONE click */
            { IC_UPSCALE,"AnimeUpscale",P.anime,B_D },       /* T — toon model (cartoonish on real footage) */
            { IC_UPSCALE,"Light Enhance",P.light,B_LIGHT },  /* D — v141 shader trio: no GPU horsepower needed */
            { IC_UPSCALE,"Archive",g_enh.is_arch&&enh_running(),B_ARCH }, /* W — plus-models remaster, save-as */
            { IC_REPEAT,"Repeat",g_repeat,B_R },
            { IC_SHUFFLE,"Shuffle",g_shuffle,B_H },
            { IC_AUDIO,"Audio",0,B_AUDIO },
            { IC_SUBS,"Subs",g_subs_enabled&&g_nsubs>0,B_SUBS },
            { IC_STATS,"Stats",g_show_stats,B_STATS },
            { IC_KEYS,"Keys",g_show_keys,B_KEYS },
            { IC_LIST,"Playlist",show_playlist,B_LIST },
            { IC_THEME,TH->name,g_theme!=0,B_THEME },   /* label IS the current theme name */
        };
        int nb=(int)(sizeof(BT)/sizeof(BT[0]));
        float bh=36, gap=10, leftpad=18, avail=W-2*leftpad;
        /* measure each button + assign to rows (greedy fill), so the bar height + the
           justify pass below agree on which buttons share a row */
        float bw[24]; int rowOf[24]; int nrows=1;
        { float rx=0; int r=0; for(int i=0;i<nb;i++){ bw[i]=icon_btn_w(BT[i].ic,BT[i].lbl,bh);
            if(rx>0 && rx+bw[i]>avail){ r++; rx=0; } rowOf[i]=r; rx+=bw[i]+gap; } nrows=r+1; }
        float barH = 56 + nrows*(bh+8) + 10; if(barH>H*0.6f) barH=H*0.6f;
        float y0=H-barH;
        ui_rect4(0,y0,W,barH, TH->bar);

        /* time (left) + filename (right). Use the smoothed bar follower so the seconds tick evenly
           with the bar; subtitles below stay on the true clk. */
        double bclk = (g_bar_clk > -1e17) ? g_bar_clk : clk;
        char tb[96]; snprintf(tb,sizeof(tb),"%d:%02d / %d:%02d",(int)bclk/60,(int)bclk%60,(int)P.duration/60,(int)P.duration%60);
        ui_text4(18, y0+6, 15, tb, TH->text);
        const char*nm=(pl_cur>=0&&pl_cur<pl_n)?pl_items[pl_cur]:""; const char*bn=nm; for(const char*q=nm;*q;q++) if(*q=='/'||*q=='\\')bn=q+1;
        char rb[160]; snprintf(rb,sizeof(rb),"[%d/%d] %.64s",pl_cur+1,pl_n,bn);
        float rbw=ui_textw(14,rb); ui_text4(W-18-rbw, y0+6, 14, rb, TH->dim);

        /* seek bar — THICK, with a fat clickable band around it */
        float pad=18, sx=pad, sw=W-2*pad, sy=y0+26, sh=18;
        seek_x0=sx; seek_x1=sx+sw; seek_y0=y0+20; seek_y1=y0+50;
        ui_rect4(sx,sy,sw,sh, TH->track);
        double frac=(P.duration>0)? bclk/P.duration:0; if(frac<0)frac=0; if(frac>1)frac=1;
        ui_rect4(sx,sy,(float)(sw*frac),sh, TH->fill);
        ui_rect4(sx+(float)(sw*frac)-3,sy-3,7,sh+6, TH->knob);

        /* buttons spread across the bar: justify each row (space-between). On an ultrawide the
           gap is capped and the row is centred so the controls stay a cohesive group rather than
           drifting to absurdly distant corners; on normal/narrow widths they justify edge-to-edge. */
        float ry=y0+56, GAPMAX=110.0f;
        for(int r=0, i=0; r<nrows; r++){
            int start=i; float sumw=0; int cnt=0;
            while(i<nb && rowOf[i]==r){ sumw+=bw[i]; cnt++; i++; }
            float g = (cnt>1) ? (avail - sumw)/(cnt-1) : 0;
            float x;
            if(g <= GAPMAX){ if(g<gap) g=gap; x=leftpad; }                  /* fill the full width */
            else { g=GAPMAX; x=(W-(sumw+(cnt-1)*g))*0.5f; }                 /* cap + centre on ultrawide */
            for(int k=start;k<start+cnt;k++){
                icon_button(x,ry,bw[k],bh,BT[k].ic,BT[k].lbl,BT[k].act,BT[k].id);
                x += bw[k]+g;
            }
            ry += bh+8;
        }
    } else { seek_x0=seek_x1=0; }

    /* AI ENHANCE progress bar. Offline / initial-buffering → centered, always shown (you're waiting).
       LIVE playback → it's just a background status while you WATCH, so tuck it top-centre and
       auto-hide it with the OSD (only on mouse movement) — a persistent overlay is annoying. */
    if(g_enh.state==ENH_RUN){
        int liveplay = (g_enh.live && g_enh.playing);
        if(!liveplay || osd_visible){
            float bw=W*0.46f, bh=58, bx=(W-bw)*0.5f, by = liveplay ? H*0.055f : H*0.5f-bh*0.5f;
            ui_rect(bx-3,by-3,bw+6,bh+6, 0.03f,0.03f,0.06f,0.93f);              /* panel */
            char hd[128];
            if(liveplay) snprintf(hd,sizeof hd,"Rendering ahead… %.0f%%", g_enh.percent);
            else         snprintf(hd,sizeof hd,"AI %.24s   %.0f%%", g_enh.phase, g_enh.percent);
            ui_text(bx+16, by+9, 16, hd, 0.96f,0.96f,1.0f,1.0f);
            float tx=bx+16, tw=bw-32, ty=by+34, tbh=13;
            ui_rect(tx,ty,tw,tbh, 0.14f,0.14f,0.20f,1.0f);                      /* track */
            float fr=g_enh.percent/100.0f; if(fr<0)fr=0; if(fr>1)fr=1;
            ui_rect(tx,ty,tw*fr,tbh, 1.0f-0.55f*fr, 0.55f+0.35f*fr, 0.28f, 1.0f); /* amber->green fill */
        }
    }

    /* transient toast message (selections, errors, completion) */
    if(g_toast_until > SDL_GetTicks() && g_toast[0]){
        float tw=ui_textw(15,g_toast)+28, th=30, tx=(W-tw)*0.5f, ty=H*0.085f;
        ui_rect(tx,ty,tw,th, 0.08f,0.08f,0.12f,0.93f);
        ui_text(tx+14, ty+7, 15, g_toast, 0.96f,0.93f,0.72f,1.0f);
    }

    if(g_show_stats) draw_stats_panel();
    if(g_show_keys)  draw_keys_panel();
    if(g_subs_enabled) draw_subtitle(clk);   /* subtitles show regardless of bar visibility */
    /* the playlist HIDES TEMPORARILY like the OSD (mouse-idle during playback) instead of being
       dismissed — show_playlist keeps its state, so scroll/selection survive and any mouse move or
       key press brings it straight back. It stays pinned while paused, idle, or being interacted
       with (search/resize/scrollbar-drag), so browsing is never yanked away mid-read. */
    if(show_playlist && (osd_visible || P.paused || !P.fmt ||
                         g_pl_search_active || g_pl_resizing || g_pl_sb_drag))
        draw_playlist_panel();
    if(g_ctx_open)    draw_context_menu();
    ui_flush();
}

/* ---------------- video display (SDL_Renderer fallback) ---------------- */
static void ensure_texture(int w, int h){
    if(P.tex && P.tex_w==w && P.tex_h==h) return;
    if(P.tex) SDL_DestroyTexture(P.tex);
    P.tex = SDL_CreateTexture(P.ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, w, h);
    P.tex_w=w; P.tex_h=h;
}

static void render_frame(AVFrame *fr){
    int w = fr->width, h = fr->height;
    ensure_texture(w, h);
    /* convert whatever pixel format → YUV420P for the IYUV texture */
    P.sws = sws_getCachedContext(P.sws, w, h, fr->format, w, h, AV_PIX_FMT_YUV420P,
                                 SWS_BILINEAR, NULL, NULL, NULL);
    static uint8_t *data[4]; static int line[4]; static int alloc_w, alloc_h;
    if(alloc_w!=w || alloc_h!=h){
        if(data[0]) av_freep(&data[0]);
        av_image_alloc(data, line, w, h, AV_PIX_FMT_YUV420P, 1);
        alloc_w=w; alloc_h=h;
    }
    sws_scale(P.sws, (const uint8_t* const*)fr->data, fr->linesize, 0, h, data, line);
    SDL_UpdateYUVTexture(P.tex, NULL, data[0], line[0], data[1], line[1], data[2], line[2]);

    int ww, wh; SDL_GetRendererOutputSize(P.ren, &ww, &wh);
    /* preserve aspect: letterbox (contain) or fill (cover, cropped) */
    double ar = (double)w*g_sar/h, war = (double)ww/wh;
    SDL_Rect dst;
    if(!P.fill){ if(war > ar){ dst.h=wh; dst.w=(int)(wh*ar); } else { dst.w=ww; dst.h=(int)(ww/ar); } }
    else       { if(war > ar){ dst.w=ww; dst.h=(int)(ww/ar); } else { dst.h=wh; dst.w=(int)(wh*ar); } }
    dst.x=(ww-dst.w)/2; dst.y=(wh-dst.h)/2;
    g_out_w=dst.w; g_out_h=dst.h;
    SDL_SetRenderDrawColor(P.ren, 0,0,0,255);
    SDL_RenderClear(P.ren);
    SDL_RenderCopy(P.ren, P.tex, NULL, &dst);
    SDL_RenderPresent(P.ren);
}

/* decode one video frame if its time has come relative to the audio clock */
static void video_step(void){
    /* the main thread OWNS the video decoder, so it performs the seek flush */
    if(P.flush_video){ if(P.vdec) avcodec_flush_buffers(P.vdec); P.flush_video = 0; }
    if(P.vstream < 0){ SDL_Delay(10); return; }
    int r = avcodec_receive_frame(P.vdec, vframe);
    if(r == AVERROR(EAGAIN)){
        /* NON-blocking get — blocking here would freeze the UI when the queue drains */
        AVPacket *pkt = av_packet_alloc();
        if(pq_get_nb(&P.vq, pkt)){
            avcodec_send_packet(P.vdec, pkt);
            av_packet_unref(pkt);
        } else {
            SDL_Delay(4); /* queue momentarily empty (demux behind / EOF) */
        }
        av_packet_free(&pkt);
        return;
    } else if(r < 0){ SDL_Delay(5); return; }

    /* best_effort_timestamp + a synthesized fallback (advance by 1/fps) so AVIs with no PTS
       don't race — same fix as the GL path. */
    static double sf_last = 0;
    { AVStream *vs = P.fmt->streams[P.vstream];
      AVRational fr = (vs->avg_frame_rate.num>0&&vs->avg_frame_rate.den>0)?vs->avg_frame_rate:vs->r_frame_rate;
      sf_last = vframe->best_effort_timestamp != AV_NOPTS_VALUE
              ? vframe->best_effort_timestamp * av_q2d(vs->time_base)
              : sf_last + ((fr.num>0&&fr.den>0)?(double)fr.den/fr.num:1.0/25.0); }
    double pts = sf_last;
    double clk;
    SDL_LockMutex(P.clock_mutex); clk = P.audio_clock; SDL_UnlockMutex(P.clock_mutex); if(g_ui_clk > clk) clk = g_ui_clk;
    if(P.astream < 0){ /* no audio → drive clock from wall time */
        clk = (av_gettime()/1000000.0) - P.frame_timer;
        SDL_LockMutex(P.clock_mutex); P.audio_clock = clk; SDL_UnlockMutex(P.clock_mutex);
    }
    double delay = pts - clk;
    if(delay > 0.0 && delay < 1.0) SDL_Delay((Uint32)(delay*1000));
    render_frame(vframe);   /* SDL_Renderer fallback path only; GL path uses gl_video_pump() */
}

/* ---------------- subtitles (external SRT/VTT + embedded text tracks) ---------------- */
static double g_sub_maxt = 0;   /* latest cue start added (embedded de-dup across re-demux/seek) */
static void free_subtitles(void){ free(g_subs); g_subs=NULL; g_nsubs=0; g_subcap=0; g_sub_maxt=0; }
static double sub_parse_ts(const char *s){
    int h=0,m=0,se=0,ms=0;
    while(*s==' '||*s=='\t') s++;
    if(sscanf(s,"%d:%d:%d,%d",&h,&m,&se,&ms)==4) return h*3600.0+m*60+se+ms/1000.0;
    if(sscanf(s,"%d:%d:%d.%d",&h,&m,&se,&ms)==4) return h*3600.0+m*60+se+ms/1000.0;
    if(sscanf(s,"%d:%d,%d",&m,&se,&ms)==3)       return m*60.0+se+ms/1000.0;
    if(sscanf(s,"%d:%d.%d",&m,&se,&ms)==3)       return m*60.0+se+ms/1000.0;
    return -1;
}
static void sub_strip_tags(char *d, const char *s){ /* drop <...> and {...}; collapse CRs */
    int o=0; int skip=0;
    for(; *s && o<255; s++){
        if(*s=='<'||*s=='{') skip++;
        else if(*s=='>'||*s=='}'){ if(skip)skip--; }
        else if(*s=='\r'){}
        else if(!skip) d[o++]=*s;
    }
    d[o]=0;
}
static void subs_add(double s,double e,const char *t){
    if(s<0||e<=s||!t[0]) return;
    /* de-dup: a backward seek re-demuxes the same region, and the embedded-cue time gate can let an
       identical cue through at the boundary. Duplicates stack up and make overlapping cues flicker. */
    for(int i=g_nsubs-1;i>=0 && i>g_nsubs-64;i--)
        if(fabs(g_subs[i].s-s)<0.01 && fabs(g_subs[i].e-e)<0.01) return;
    if(g_nsubs>=g_subcap){ g_subcap=g_subcap?g_subcap*2:256; g_subs=realloc(g_subs,(size_t)g_subcap*sizeof(Cue)); }
    g_subs[g_nsubs].s=s; g_subs[g_nsubs].e=e; sub_strip_tags(g_subs[g_nsubs].t, t); g_nsubs++;
}
/* add an embedded cue, skipping ones we already accumulated (a backward seek re-demuxes the
   same region; cues are time-stamped so we only need each once). */
static void subs_add_embedded(double s,double e,const char *t){
    if(s < g_sub_maxt - 0.05) return;       /* already have this region */
    subs_add(s,e,t);
    if(s > g_sub_maxt) g_sub_maxt = s;
}
/* pull the dialogue text out of one subtitle rect. ASS rects look like
   "...,<8-or-9 comma-separated fields>,Actual text\Ngoes here" — take the field after the
   last metadata comma, turn \N/\n into newlines and drop {\override} blocks. */
static void sub_extract_text(char *dst, const char *src, int is_ass){
    if(!is_ass){ sub_strip_tags(dst, src); return; }
    const char *p = src; int skip = 8;                 /* avcodec ass line: 8 fields before Text */
    const char *d = strstr(src, "Dialogue:");
    if(d){ p = d + 9; skip = 9; }                       /* file-style line: 9 fields before Text */
    for(int c=0; c<skip && *p; p++) if(*p==',') c++;    /* walk past the metadata fields */
    int o=0;
    for(; *p && o<255; p++){
        if(p[0]=='\\' && (p[1]=='N'||p[1]=='n')){ dst[o++]='\n'; p++; }
        else if(*p=='{'){ while(p[1] && *p!='}') p++; } /* skip an {\...} override block */
        else if(*p=='\r'){}
        else dst[o++]=*p;
    }
    dst[o]=0;
}
/* decode whatever embedded-subtitle packets the demux thread queued and add their cues. Runs
   on the main thread (owns P.sdec); bounded per call so it never stalls the render loop. */
static void drain_subtitles(void){
    if(P.sstream < 0 || !P.sdec || !P.fmt) return;
    AVPacket *pkt = av_packet_alloc(); if(!pkt) return;
    AVRational tb = P.fmt->streams[P.sstream]->time_base;
    int budget = 24;
    while(budget-- > 0 && pq_get_nb(&P.sq, pkt)){
        AVSubtitle sub; int got=0;
        int ret = avcodec_decode_subtitle2(P.sdec, &sub, &got, pkt);
        if(ret >= 0 && got){
            double pts_s = (sub.pts != AV_NOPTS_VALUE) ? sub.pts/(double)AV_TIME_BASE
                          : (pkt->pts==AV_NOPTS_VALUE ? 0 : pkt->pts*av_q2d(tb));
            double st = pts_s + sub.start_display_time/1000.0;
            double en = pts_s + (sub.end_display_time>0 ? sub.end_display_time/1000.0
                                 : (pkt->duration>0 ? pkt->duration*av_q2d(tb) : 4.0));
            char text[256]; int tl=0; text[0]=0;
            for(unsigned i=0;i<sub.num_rects;i++){
                AVSubtitleRect *r = sub.rects[i];
                const char *s = r->ass ? r->ass : r->text; if(!s) continue;
                char line[256]; sub_extract_text(line, s, r->ass!=NULL);
                if(line[0]){
                    if(tl && tl<255){ text[tl++]='\n'; text[tl]=0; }
                    int L=(int)strlen(line); if(tl+L>255) L=255-tl;
                    if(L>0){ memcpy(text+tl,line,L); tl+=L; text[tl]=0; }
                }
            }
            if(text[0]) subs_add_embedded(st, en, text);
        }
        if(ret >= 0 && got) avsubtitle_free(&sub);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
}
static void load_subtitles(const char *mediapath){
    free_subtitles();
    if(!mediapath) return;
    /* derive sibling path: replace extension with .srt, else .vtt */
    char base[1024]; strncpy(base,mediapath,sizeof(base)-5); base[sizeof(base)-5]=0;
    char *dot=strrchr(base,'.'); if(dot) *dot=0;
    char path[1040]; FILE *f=NULL;
    const char *exts[2]={".srt",".vtt"};
    for(int i=0;i<2 && !f;i++){ snprintf(path,sizeof(path),"%s%s",base,exts[i]); f=path_fopen(path,L"rb"); }  /* wide: Unicode sidecar names */
    if(!f) return;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if(sz<=0||sz>20*1024*1024){ fclose(f); return; }
    char *buf=malloc(sz+1); if(!buf){ fclose(f); return; }
    sz=fread(buf,1,sz,f); buf[sz]=0; fclose(f);
    char *p=buf; if((unsigned char)p[0]==0xEF&&(unsigned char)p[1]==0xBB&&(unsigned char)p[2]==0xBF) p+=3;
    /* split into an array of lines (CR/LF stripped) */
    int cap=1024, n=0; char **L=malloc((size_t)cap*sizeof(char*));
    L[n++]=p;
    for(char *q=p; *q; q++){
        if(*q=='\n'){ *q=0; if(q>p && q[-1]=='\r') q[-1]=0; if(n>=cap){ cap*=2; L=realloc(L,(size_t)cap*sizeof(char*)); } L[n++]=q+1; }
    }
    /* parse: a line with "-->" starts a cue; following non-blank lines are its text */
    for(int i=0;i<n;i++){
        char *arrow=strstr(L[i],"-->");
        if(!arrow) continue;
        double st=sub_parse_ts(L[i]), en=sub_parse_ts(arrow+3);
        if(st<0 || en<=st) continue;
        char text[256]; int tl=0; text[0]=0;
        int j=i+1;
        for(; j<n; j++){
            int blank=1; for(char *q=L[j]; *q; q++) if(*q!=' '&&*q!='\t'){ blank=0; break; }
            if(blank) break;
            if(tl && tl<255){ text[tl++]='\n'; text[tl]=0; }
            int len=(int)strlen(L[j]); if(tl+len>255) len=255-tl;
            if(len>0){ memcpy(text+tl,L[j],len); tl+=len; text[tl]=0; }
        }
        subs_add(st,en,text);          /* subs_add strips <...>/{...} tags */
        i = (j>i)? j-1 : i;            /* resume after the consumed text block */
    }
    free(L); free(buf);
    fprintf(stderr,"[subs] loaded %d cues from %s\n", g_nsubs, path);
}

/* ---------------- teardown (reused by drag-drop and quit) ---------------- */
static void close_media(void){
    P.demux_stop = 1;                              /* tell demux thread to exit (it loops on this) */
    pq_abort(&P.vq); pq_abort(&P.aq); pq_abort(&P.sq); /* unblock any queue waiters */
    g_stage = "close_media: joining demux thread";
    if(P.demux_thread){ SDL_WaitThread(P.demux_thread, NULL); P.demux_thread = NULL; }
    g_stage = "close_media: joining video decode thread";
    vd_stop();                                     /* join the video decode thread BEFORE freeing P.vdec */
    g_stage = "close_media: closing audio device";
    if(P.audio_dev){ SDL_CloseAudioDevice(P.audio_dev); P.audio_dev = 0; } /* joins audio thread */
    g_stage = "close_media: freeing decoders";
    if(P.vdec) avcodec_free_context(&P.vdec);
    if(P.adec) avcodec_free_context(&P.adec);
    if(P.sdec) avcodec_free_context(&P.sdec);
    if(P.swr)  swr_free(&P.swr);
    if(P.fmt)  avformat_close_input(&P.fmt);
    pq_flush(&P.vq); pq_flush(&P.aq); pq_flush(&P.sq);
    P.vq.abort = P.aq.abort = P.sq.abort = 0;       /* re-arm queues for the next file */
    P.demux_stop = 0;
    P.audio_clock = 0; P.abuf_index = P.abuf_size_used = 0;
    P.audio_stalled = 0; P.aclk_val = -1; P.aclk_t = 0; P.aclk_creep = 0; P.flushed_rs = 1; g_pts_viol = 0; P.aclk_probe = 0; g_clk_mono = -1e18; g_ui_clk = -1e18;   /* re-arm the audio-stall + creep watchdogs for the new file */
    P.eof = 0; P.seek_flag = 0; P.paused = 0; P.flush_video = 0; P.is_image = 0; g_stopped = 0;
    P.vstream = P.astream = P.sstream = -1;
    /* reset the GL frame-gen buffer so a new file doesn't inherit stale frames/PTS */
    gl_nframes = 0; gl_ptsA = gl_ptsB = -1; gl_tex_w = gl_tex_h = 0; g_vdrained = 0;
    memset(&pr_vt, 0, sizeof(pr_vt));
    g_hw_pixfmt = AV_PIX_FMT_NONE;                        /* next file's decoder re-decides hw vs sw */
    memset(g_dbg, 0, sizeof(g_dbg));   /* re-arm the one-shot debug milestones */
    free_subtitles();
    g_stage = "main loop";
}

static int load_media(const char *path){
    g_stage = "load_media: opening file (avformat open / probe / decoders / audio device)";
    if(open_file(path) != 0){
        if(P.fmt) avformat_close_input(&P.fmt);   /* don't leave a half-open file behind */
        P.vstream = P.astream = -1;
        g_stage = "main loop";
        return -1;
    }
    P.frame_timer = av_gettime() / 1000000.0;
    P.demux_thread = SDL_CreateThread(demux_thread, "demux", NULL);
    vd_start();                                    /* video decode + transfer thread (GL path) */
    g_stage = "main loop";
    return 0;
}

/* ---------------- playlist ---------------- */
static void pl_add(const char *path){
    if(pl_n >= pl_cap){
        int nc = pl_cap ? pl_cap*2 : 16;
        char **t = realloc(pl_items, (size_t)nc*sizeof(char*));
        if(!t) return;                 /* OOM → drop silently rather than crash */
        pl_items = t; pl_cap = nc;
        double *td=realloc(pl_dur,(size_t)nc*sizeof(double));     if(td) pl_dur=td;
        long long *ts=realloc(pl_sz,(size_t)nc*sizeof(long long)); if(ts) pl_sz=ts;
        unsigned char *tl=realloc(pl_sel,(size_t)nc);              if(tl) pl_sel=tl;
    }
    /* one allocation: "path\0norm(path)\0norm(filename)\0" — the search keys ride along with the row
       through every move/sort/remove (they are just pointer shuffles) and are freed with it. */
    size_t L = strlen(path); const char *bn = path;
    for(const char *q=path; *q; q++) if(*q=='/'||*q=='\\') bn=q+1;
    size_t BL = strlen(bn);
    char *d = SDL_malloc(L+1 + L+1 + BL+1 + 1); if(!d) return;
    memcpy(d, path, L+1);
    int n1 = srch_norm(path, d+L+1, (int)L+1);
    int n2 = srch_norm(bn, d+L+1+n1+1, (int)BL+1);
    d[L+1+n1+1+n2+1] = (char)g_pl_add_exempt;      /* 1 = opened explicitly: never hidden by filters */
    g_sess_dirty = 1;
    if(pl_dur) pl_dur[pl_n]=-1; if(pl_sz) pl_sz[pl_n]=-1;   /* lazily probed when visible */
    if(pl_sel) pl_sel[pl_n]=0;
    pl_items[pl_n++] = d;
}
/* recognise media files by extension (so folder scans skip junk) */
static int is_media_ext(const char *name){
    const char *e=strrchr(name,'.'); if(!e) return 0; e++;
    static const char *X[]={"mp4","m4v","mov","mkv","webm","avi","wmv","flv","mpg","mpeg",
        "ts","m2ts","mts","ogv","3gp","divx","vob","m2v","nut","y4m","mxf",
        /* 2026-10: RealMedia (.rmvb/.rm — 470 files in one library were silently skipped), OGM, ASF, F4V,
           HDV .m2t, .mpe, .qt, .mk3d, 3g2 — all decoded by our FFmpeg build */
        "rmvb","rm","ogm","asf","f4v","m2t","mpe","qt","mk3d","3g2","dv","evo",
        "mp3","flac","wav","aac","ogg","opus","m4a","wma","ape","alac",
        "mka","m4b","aif","aiff","wv","ac3","dts","amr","mpc","oga","spx","caf",
        /* images — the player doubles as an image viewer */
        "jpg","jpeg","jfif","png","gif","bmp","webp","tif","tiff","tga","jxl","avif",
        "heic","heif","ico","ppm","pgm","pnm","jp2","exr","dds","hdr","apng",0};
    for(int i=0;X[i];i++) if(!SDL_strcasecmp(e,X[i])) return 1;
    return 0;
}
/* Add a file, or scan a directory tree for media. Breadth-first via an explicit
   queue (NO deep recursion → can't blow the stack), Win32 FindFirstFileW (handles
   files >2GB that 32-bit stat() chokes on AND Unicode names), skips reparse points
   (junctions/symlinks → no infinite loops), and is bounded + allocation-checked. */
#define SCAN_CAP 200000
static void plist_add(PathList *L, const char *p){
    if(L->n>=L->cap){ int nc=L->cap?L->cap*2:128; char**t=realloc(L->v,(size_t)nc*sizeof(char*)); if(!t)return; L->v=t; L->cap=nc; }
    char *d=SDL_strdup(p); if(d) L->v[L->n++]=d;
}
static void plist_free(PathList *L){ for(int i=0;i<L->n;i++) SDL_free(L->v[i]); free(L->v); L->v=NULL; L->n=L->cap=0; }
/* ---- .m3u / .m3u8 playlists ---- */
static int is_playlist_ext(const char *name){
    const char *e=strrchr(name,'.'); if(!e) return 0;
    return !SDL_strcasecmp(e,".m3u8") || !SDL_strcasecmp(e,".m3u");
}
static int utf8_valid(const char *s){
    const unsigned char *u=(const unsigned char*)s;
    while(*u){
        int n = *u<0x80?0 : (*u>>5)==6?1 : (*u>>4)==14?2 : (*u>>3)==30?3 : -1;
        if(n<0) return 0; u++;
        while(n--){ if((*u & 0xC0)!=0x80) return 0; u++; }
    }
    return 1;
}
/* Read a playlist into `out` (relative entries resolve against the playlist's folder; legacy ANSI .m3u
   lines are converted from the system codepage). Session extras: #HOGS-SEL:<path>, #HOGS-FILTER:<n>.
   Returns 1 if the file could be read. */
static int pl_read_m3u(const char *path, PathList *out, char *sel, size_t selcap, int *filter){
    FILE *f=path_fopen(path,L"rb"); if(!f) return 0;
    char dir[2048]; snprintf(dir,sizeof dir,"%s",path);
    { char *sl=NULL; for(char *q=dir;*q;q++) if(*q=='\\'||*q=='/') sl=q; if(sl) sl[1]=0; else dir[0]=0; }
    static char line[4096]; int first=1;
    while(fgets(line,sizeof line,f)){
        char *l=line;
        if(first && (unsigned char)l[0]==0xEF && (unsigned char)l[1]==0xBB && (unsigned char)l[2]==0xBF) l+=3;
        first=0;
        size_t n=strlen(l); while(n && (l[n-1]=='\n'||l[n-1]=='\r'||l[n-1]==' '||l[n-1]=='\t')) l[--n]=0;
        while(*l==' '||*l=='\t') l++;
        if(!*l) continue;
        if(l[0]=='#'){
            if(sel && !strncmp(l,"#HOGS-SEL:",10)) snprintf(sel,selcap,"%s",l+10);
            else if(filter && !strncmp(l,"#HOGS-FILTER:",13)){ int v=atoi(l+13); if(v>=0&&v<=3) *filter=v; }
            continue;
        }
        char u8[2048];
        if(utf8_valid(l)) snprintf(u8,sizeof u8,"%s",l);
        else { wchar_t w[2048]; if(MultiByteToWideChar(CP_ACP,0,l,-1,w,2048)<=0) continue; path_w_to_u8(w,u8,sizeof u8); }
        if(!strncmp(u8,"file:///",8)) memmove(u8,u8+8,strlen(u8+8)+1);       /* bare file URLs */
        int absolute = (isalpha((unsigned char)u8[0]) && u8[1]==':') || (u8[0]=='\\'&&u8[1]=='\\') || u8[0]=='/' || u8[0]=='\\';
        if(!absolute && strstr(u8,"://")) continue;                           /* skip network streams */
        char full[2048];
        if(absolute) snprintf(full,sizeof full,"%s",u8); else snprintf(full,sizeof full,"%s%s",dir,u8);
        for(char *q=full;*q;q++) if(*q=='/') *q='\\';
        if(full[0]=='\\' && full[1]!='\\' && isalpha((unsigned char)dir[0])){ char t[2048]; snprintf(t,sizeof t,"%c:%s",dir[0],full); snprintf(full,sizeof full,"%s",t); }
        plist_add(out, full);
    }
    fclose(f); return 1;
}
/* UTF-8 path -> wide, normalised to backslashes, with the \\?\ extended-length prefix for long
   drive-letter paths so FindFirstFileW / GetFileAttributesW don't silently fail past MAX_PATH (260). */
static void scan_wide(const char *u8, wchar_t *w, int cap){
    wchar_t tmp[2200]; path_u8_to_w(u8, tmp, 2200);
    for(wchar_t *q=tmp; *q; q++) if(*q==L'/') *q=L'\\';
    size_t n=wcslen(tmp);
    if(n>=240 && ((tmp[0]>=L'A'&&tmp[0]<=L'Z')||(tmp[0]>=L'a'&&tmp[0]<=L'z')) && tmp[1]==L':' && tmp[2]==L'\\')
        _snwprintf(w,cap,L"\\\\?\\%ls",tmp);
    else { wcsncpy(w,tmp,cap-1); w[cap-1]=0; }
}
/* scan a file/dir tree into `out` (does NOT touch the playlist) — safe to run on a worker thread. */
static void scan_into(PathList *out, const char *root){
    wchar_t wroot[2048]; scan_wide(root, wroot, 2048);
    DWORD attr = GetFileAttributesW(wroot);
    if(attr == INVALID_FILE_ATTRIBUTES) return;
    if(!(attr & FILE_ATTRIBUTE_DIRECTORY)){
        if(is_playlist_ext(root)) pl_read_m3u(root,out,NULL,0,NULL);   /* dropped/passed playlist → its entries */
        else if(is_media_ext(root)) plist_add(out,root);
        return; }

    char **dirs=NULL; int dn=0, dcap=0, head=0, added=0;
    #define PUSHDIR(p) do{ if(dn>=dcap){ int nc=dcap?dcap*2:64; char**t=realloc(dirs,(size_t)nc*sizeof(char*)); \
        if(!t) goto done; dirs=t; dcap=nc; } char*_s=SDL_strdup(p); if(!_s) goto done; dirs[dn++]=_s; }while(0)
    PUSHDIR(root);
    while(head < dn){
        char *dir = dirs[head++];
        char pat[2048]; snprintf(pat,sizeof(pat),"%s\\*", dir);
        wchar_t wpat[2200]; scan_wide(pat, wpat, 2200);
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(wpat, &fd);
        if(h != INVALID_HANDLE_VALUE){
            do{
                char nm[1024]; path_w_to_u8(fd.cFileName, nm, sizeof(nm));
                if(nm[0]=='.'&&(nm[1]==0||(nm[1]=='.'&&nm[2]==0))) continue;
                char full[2048]; snprintf(full,sizeof(full),"%s\\%s", dir, nm);
                if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){
                    /* skip junction/symlink DIRECTORIES only (loop guard). Files carrying a reparse tag
                       (dedup / cloud placeholders / file symlinks) are readable media — keep them. */
                    if(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                    PUSHDIR(full); }
                else if(is_media_ext(full)){
                    plist_add(out,full);
                    if(++added >= SCAN_CAP){ fprintf(stderr,"[scan] hit %d-file cap\n",SCAN_CAP); FindClose(h); goto done; }
                }
            }while(FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
done:
    for(int i=0;i<dn;i++) SDL_free(dirs[i]);
    free(dirs);
    #undef PUSHDIR
}
/* case-insensitive FNV-1a of a path (ASCII-folded; Windows paths compare case-insensitively) */
static unsigned path_hash(const char *s){
    unsigned h=2166136261u;
    for(; *s; s++){ unsigned char c=(unsigned char)*s; if(c=='/') c='\\'; h ^= (unsigned)tolower(c); h *= 16777619u; }
    return h;
}
/* Append every path in L that isn't ALREADY in the playlist (re-adding a folder, restoring a session and
   then opening a file from it, etc. never duplicates rows). O(n) via a temporary hash table.
   Returns the playlist index of L's FIRST path (new or pre-existing), or -1 if L is empty;
   *added (optional) receives how many rows were actually appended. */
static int pl_append_unique(PathList *L, int *added){
    int first=-1, add=0;
    if(added) *added=0;
    if(!L || L->n==0) return -1;
    size_t cap=64; while(cap < (size_t)(pl_n+L->n)*2) cap<<=1;
    int *tab=malloc(cap*sizeof(int));
    if(!tab){ for(int i=0;i<L->n;i++){ pl_add(L->v[i]); add++; } if(added)*added=add; return pl_n-add; }
    for(size_t k=0;k<cap;k++) tab[k]=-1;
    #define TAB_PUT(ix) do{ size_t _h=path_hash(pl_items[ix])&(cap-1); while(tab[_h]>=0) _h=(_h+1)&(cap-1); tab[_h]=(ix); }while(0)
    for(int i=0;i<pl_n;i++) TAB_PUT(i);
    for(int i=0;i<L->n;i++){
        size_t h=path_hash(L->v[i])&(cap-1); int found=-1;
        while(tab[h]>=0){ if(!SDL_strcasecmp(pl_items[tab[h]],L->v[i])){ found=tab[h]; break; } h=(h+1)&(cap-1); }
        if(found<0){ int before=pl_n; pl_add(L->v[i]); if(pl_n==before) continue; found=pl_n-1; add++; TAB_PUT(found); }
        if(first<0) first=found;
    }
    #undef TAB_PUT
    free(tab);
    if(added) *added=add;
    return first;
}
/* synchronous convenience wrapper used by drag-drop / CLI args / Add Files (small, immediate).
   Returns the playlist index of the first media path found under root (or -1). */
static int add_path_recursive(const char *root){
    PathList L={NULL,0,0}; scan_into(&L,root);
    int added=0, first=pl_append_unique(&L,&added);
    fprintf(stderr,"[scan] %s -> %d media files, %d new (playlist now %d)\n", root, L.n, added, pl_n);
    plist_free(&L);
    return first;
}
/* a search box or a media-type filter is currently narrowing the playlist */
static int pl_filter_active(void){ return g_pl_filter != 0 || g_pl_search[0] != 0 || g_excl_n > 0; }
/* next index (in dir) from `from` whose row passes the active filter, wrapping if repeat is on;
   -1 if none. With no filter active every row matches, so this is plain +1/-1 navigation. */
static int pl_step_match(int from, int dir){
    if(pl_n == 0) return -1;
    for(int k=1; k<=pl_n; k++){
        int i = from + dir*k;
        if(i < 0 || i >= pl_n){ if(!g_repeat) return -1; i = ((i % pl_n) + pl_n) % pl_n; }
        if(pl_match(i)) return i;
    }
    return -1;
}
/* ---- shuffle "bag": play every matching file once in random order, then reshuffle ----
   Replaces the old modulo-time pick, which on a small filtered set (a handful of audio/images)
   missed the filter on almost every random index and fell back to "first match", so it just
   ping-ponged the top two. A Fisher-Yates–shuffled bag of the filtered indices guarantees NO
   file repeats until the whole filtered set has played. */
static unsigned g_rng = 0;
static unsigned rng_next(void){
    if(!g_rng) g_rng = (unsigned)(av_gettime() & 0xffffffff) | 1u;   /* seed once from the clock */
    g_rng ^= g_rng<<13; g_rng ^= g_rng>>17; g_rng ^= g_rng<<5; return g_rng;   /* xorshift32 */
}
static int *g_bag = NULL; static int g_bag_cap = 0, g_bag_n = 0, g_bag_pos = 0;
static unsigned g_bag_sig = 0; static int g_bag_valid = 0;
/* play HISTORY (indices in play order) so Prev returns to what actually played, even shuffled */
static int g_hist[512]; static int g_hist_n = 0; static int g_hist_nav = 0;
/* signature of the current filtered set — when it changes, the bag is rebuilt */
static unsigned shuffle_sig(void){
    unsigned h = 2166136261u;
    h ^= (unsigned)g_pl_filter; h *= 16777619u;
    h ^= (unsigned)pl_n;        h *= 16777619u;
    for(const char *s=g_pl_search; *s; s++){ h ^= (unsigned char)*s; h *= 16777619u; }
    h ^= g_srch_ver; h *= 16777619u;                     /* exclude-list edits change the set too */
    return h;
}
static void shuffle_rebuild(void){
    int n=0; for(int i=0;i<pl_n;i++) if(pl_match(i)) n++;
    if(g_bag_cap < n){ int *t=realloc(g_bag,(size_t)(n>0?n:1)*sizeof(int));
        if(!t){ g_bag_n=g_bag_pos=0; return; } g_bag=t; g_bag_cap=(n>0?n:1); }
    g_bag_n=0; for(int i=0;i<pl_n;i++) if(pl_match(i)) g_bag[g_bag_n++]=i;
    for(int i=g_bag_n-1;i>0;i--){ int j=(int)(rng_next()%(unsigned)(i+1));   /* Fisher-Yates */
        int t=g_bag[i]; g_bag[i]=g_bag[j]; g_bag[j]=t; }
    g_bag_pos=0;
    /* the currently-playing file counts as ALREADY PLAYED this cycle: move it to slot 0 and start
       past it. (The old "seam swap" only pushed it one slot down — it then repeated mid-cycle.) */
    for(int i=0;i<g_bag_n;i++) if(g_bag[i]==pl_cur){
        int t=g_bag[0]; g_bag[0]=g_bag[i]; g_bag[i]=t; g_bag_pos=1; break; }
}
/* consume a file from the current bag cycle (it just played — by ANY means: auto-advance,
   double-click, or the unplayable-skip substituting a different file). Without this, manually
   started files stayed in the bag and repeated before the cycle finished. */
static void shuffle_consume(int idx){
    if(!g_bag_valid) return;
    for(int k=g_bag_pos;k<g_bag_n;k++) if(g_bag[k]==idx){
        int t=g_bag[g_bag_pos]; g_bag[g_bag_pos]=g_bag[k]; g_bag[k]=t; g_bag_pos++; break; }
}
/* next shuffled index in the filtered set; -1 when a full cycle is done and repeat is off */
static int pl_shuffle_next(void){
    unsigned sig = shuffle_sig();
    if(!g_bag_valid || sig != g_bag_sig){ shuffle_rebuild(); g_bag_sig=sig; g_bag_valid=1; }
    else if(g_bag_pos >= g_bag_n){ if(!g_repeat) return -1; shuffle_rebuild(); }   /* cycle done */
    if(g_bag_n==0 || g_bag_pos>=g_bag_n) return -1;
    return g_bag[g_bag_pos++];
}
static void pl_play_at(int i){
    if(i < 0 || i >= pl_n) return;
    /* navigating away cancels a running LIVE enhance (you were watching it) — but NOT an archive render:
       that is an hours-long background job writing its own file, and auto-advance or picking something
       else to watch meanwhile must not throw it away (2026-10-10). Esc still cancels it. */
    if(enh_running() && !g_enh.is_arch) enhance_cancel();
    /* enhanced outputs live in the LRU cache now — nothing is deleted on navigation.
       The AI toggles are STICKY: they stay on across Next/skip and re-apply to the new video. */
    g_playing_enhanced = 0;
    int filt = pl_filter_active();
    /* skip over unplayable files (bounded) — in a huge library the first file found can be a
       corrupt/sample/unsupported clip; don't get stuck on it. When a filter is active, only
       attempt files that pass it, so Next/Prev/auto-advance never leave the filtered set. */
    int attempts = 0;
    for(int scan = 0; scan < pl_n && attempts < 80; scan++){
        int idx = (i + scan) % pl_n;
        if(filt && !pl_match(idx)) continue;
        attempts++;
        pl_cur = idx;
        close_media();
        if(load_media(pl_items[idx]) == 0){
            g_sess_dirty = 1;                /* remember what was playing for next launch */
            shuffle_consume(idx);            /* no repeats until the whole cycle has played */
            g_orig_w=g_src_w; g_orig_h=g_src_h; g_orig_fps=g_src_fps;  /* remember the ORIGINAL for stats */
            load_subtitles(pl_items[idx]);   /* sibling .srt/.vtt if present (takes precedence) */
            if(g_nsubs > 0 && P.sdec){       /* external subs found → drop the embedded track */
                avcodec_free_context(&P.sdec); P.sstream = -1;
            }
            g_pl_jump = 1;                   /* scroll the playlist panel to this item */
            /* record the play history (not when Prev itself is navigating; skip same-file repeats) */
            if(!g_hist_nav && (g_hist_n==0 || g_hist[g_hist_n-1]!=idx)){
                if(g_hist_n>=512){ memmove(g_hist,g_hist+1,sizeof(int)*511); g_hist_n=511; }
                g_hist[g_hist_n++]=idx;
            }
            if((P.upscale||P.framegen||P.anime) && !enh_running()) enhance_apply();   /* sticky toggles: enhance this one too */
            return;
        }
        fprintf(stderr,"[play] skipping unplayable: %s\n", pl_items[idx]);
    }
    fprintf(stderr,"[play] no playable file found from index %d\n", i);
}
/* advance to the next item (respecting filter + shuffle); returns 1 if it played one */
static int pl_next(void){
    if(pl_n == 0) return 0;
    int i = (g_shuffle && pl_n > 1) ? pl_shuffle_next() : pl_step_match(pl_cur, +1);
    if(i < 0) return 0;
    pl_play_at(i); return 1;
}
/* Prev = go back through the PLAY HISTORY (what you actually watched), so it works with shuffle.
   Falls back to playlist-order stepping when there's no history yet. */
static void pl_prev(void){
    if(pl_n == 0) return;
    if(g_hist_n >= 2){
        g_hist_n--;                          /* drop the current file... */
        int prev = g_hist[g_hist_n-1];       /* ...and replay the one before it */
        if(prev >= 0 && prev < pl_n){ g_hist_nav=1; pl_play_at(prev); g_hist_nav=0; return; }
    }
    int i = pl_step_match(pl_cur, -1);
    if(i >= 0) pl_play_at(i);
}
/* stop = pause and rewind to the start (Stop button / S key) */
static void stop_playback(void){
    if(!P.fmt) return;
    P.paused=1; if(P.audio_dev) SDL_PauseAudioDevice(P.audio_dev,1);
    P.seek_target=0; P.seek_abs=1; P.seek_flag=1;
    /* The pump normally resets these when it consumes flush_video, but it never runs while paused
       and we just paused. Without this the UI clock keeps its old value and, since the bar reads
       max(audio_clock, g_ui_clk), the stale value wins and the bar never returns to the start. */
    SDL_LockMutex(P.clock_mutex); P.audio_clock = 0; SDL_UnlockMutex(P.clock_mutex);
    g_clk_mono = -1e18; g_ui_clk = -1e18;
    g_stopped = 1;
    /* drop the A/B pair: gl_redraw() paints black with no frames, so the stale picture clears at
       once rather than lingering for the frame or two the seek + decode of frame 0 takes. */
    gl_nframes = 0;
    if(P.vstream >= 0){ g_stop_step = 1; g_stop_tries = 0; }
}
/* empty the playlist (current media keeps playing) */
static void pl_clear(void){
    g_sess_dirty = 1;
    for(int i=0;i<pl_n;i++) SDL_free(pl_items[i]);
    pl_n=0; pl_cur=-1; g_pl_selected=-1; pl_scroll=0; g_hist_n=0;
}
/* remove one entry from the playlist */
static void pl_remove(int i){
    if(i<0 || i>=pl_n) return;
    g_sess_dirty = 1;
    SDL_free(pl_items[i]);
    for(int k=i;k<pl_n-1;k++){ pl_items[k]=pl_items[k+1];
        if(pl_dur) pl_dur[k]=pl_dur[k+1]; if(pl_sz) pl_sz[k]=pl_sz[k+1];
        if(pl_sel) pl_sel[k]=pl_sel[k+1]; }
    pl_n--;
    if(pl_cur==i) pl_cur=-1; else if(pl_cur>i) pl_cur--;
    if(g_pl_selected>=pl_n) g_pl_selected=pl_n-1;
    if(pl_n==0) g_pl_selected=-1;
    { int w=0; for(int k=0;k<g_hist_n;k++){        /* keep the play history's indices valid */
          if(g_hist[k]==i) continue;
          g_hist[w++] = g_hist[k]>i ? g_hist[k]-1 : g_hist[k]; }
      g_hist_n=w; }
}
/* move row `from` so it lands immediately BEFORE real index `to` (to==pl_n → end). Identity of the
   playing item / selection / history is tracked by POINTER so they follow the item across the shuffle. */
static void pl_move(int from,int to){
    if(from<0 || from>=pl_n) return;
    if(to<0)to=0; if(to>pl_n)to=pl_n;
    if(to==from || to==from+1) return;                     /* dropped on itself → no-op */
    g_sess_dirty = 1;
    char *curp=(pl_cur>=0&&pl_cur<pl_n)?pl_items[pl_cur]:NULL;
    char *selp=(g_pl_selected>=0&&g_pl_selected<pl_n)?pl_items[g_pl_selected]:NULL;
    char *histp[512]; int hn=g_hist_n<512?g_hist_n:512;
    for(int k=0;k<hn;k++) histp[k]=(g_hist[k]>=0&&g_hist[k]<pl_n)?pl_items[g_hist[k]]:NULL;
    char *item=pl_items[from]; long long sz=pl_sz?pl_sz[from]:0; double du=pl_dur?pl_dur[from]:0; unsigned char se=pl_sel?pl_sel[from]:0;
    for(int k=from;k<pl_n-1;k++){ pl_items[k]=pl_items[k+1]; if(pl_sz)pl_sz[k]=pl_sz[k+1]; if(pl_dur)pl_dur[k]=pl_dur[k+1]; if(pl_sel)pl_sel[k]=pl_sel[k+1]; }
    int dest = to>from ? to-1 : to;
    for(int k=pl_n-1;k>dest;k--){ pl_items[k]=pl_items[k-1]; if(pl_sz)pl_sz[k]=pl_sz[k-1]; if(pl_dur)pl_dur[k]=pl_dur[k-1]; if(pl_sel)pl_sel[k]=pl_sel[k-1]; }
    pl_items[dest]=item; if(pl_sz)pl_sz[dest]=sz; if(pl_dur)pl_dur[dest]=du; if(pl_sel)pl_sel[dest]=se;
    if(curp){ for(int k=0;k<pl_n;k++) if(pl_items[k]==curp){pl_cur=k;break;} }
    if(selp){ for(int k=0;k<pl_n;k++) if(pl_items[k]==selp){g_pl_selected=k;break;} }
    for(int k=0;k<hn;k++) if(histp[k]) for(int j=0;j<pl_n;j++) if(pl_items[j]==histp[k]){ g_hist[k]=j; break; }
}
static int pl_selcount(void){ int n=0; if(pl_sel) for(int i=0;i<pl_n;i++) if(pl_sel[i])n++; return n; }
/* move EVERY selected row (in their current relative order) so the block lands immediately BEFORE real
   index `to` (to==pl_n → end). The moved rows stay selected. Same pointer-tracking as pl_move/pl_sort. */
static void pl_move_selected(int to){
    if(!pl_sel || pl_n<2) return; if(to<0)to=0; if(to>pl_n)to=pl_n;
    g_sess_dirty = 1;
    int *ord=malloc((size_t)pl_n*sizeof(int)); if(!ord) return; int m=0;
    for(int i=0;i<=pl_n;i++){
        if(i==to) for(int s=0;s<pl_n;s++) if(pl_sel[s]) ord[m++]=s;   /* the whole selection, in order */
        if(i<pl_n && !pl_sel[i]) ord[m++]=i;                          /* non-selected keep their order */
    }
    if(m!=pl_n){ free(ord); return; }                                /* safety: must be a permutation */
    char *curp=(pl_cur>=0&&pl_cur<pl_n)?pl_items[pl_cur]:NULL;
    char *selp=(g_pl_selected>=0&&g_pl_selected<pl_n)?pl_items[g_pl_selected]:NULL;
    char *histp[512]; int hn=g_hist_n<512?g_hist_n:512;
    for(int k=0;k<hn;k++) histp[k]=(g_hist[k]>=0&&g_hist[k]<pl_n)?pl_items[g_hist[k]]:NULL;
    char **ni=malloc((size_t)pl_n*sizeof(char*)); unsigned char *nse=malloc((size_t)pl_n);
    long long *ns=pl_sz?malloc((size_t)pl_n*sizeof(long long)):NULL;
    double *nd=pl_dur?malloc((size_t)pl_n*sizeof(double)):NULL;
    if(!ni||!nse){ free(ord); free(ni); free(nse); if(ns)free(ns); if(nd)free(nd); return; }
    for(int i=0;i<pl_n;i++){ int s=ord[i]; ni[i]=pl_items[s]; nse[i]=pl_sel[s]; if(ns)ns[i]=pl_sz[s]; if(nd)nd[i]=pl_dur[s]; }
    memcpy(pl_items,ni,(size_t)pl_n*sizeof(char*)); memcpy(pl_sel,nse,(size_t)pl_n);
    if(ns){ memcpy(pl_sz,ns,(size_t)pl_n*sizeof(long long)); free(ns); }
    if(nd){ memcpy(pl_dur,nd,(size_t)pl_n*sizeof(double)); free(nd); }
    free(ni); free(nse); free(ord);
    if(curp){ for(int k=0;k<pl_n;k++) if(pl_items[k]==curp){pl_cur=k;break;} }
    if(selp){ for(int k=0;k<pl_n;k++) if(pl_items[k]==selp){g_pl_selected=k;break;} }
    for(int k=0;k<hn;k++) if(histp[k]) for(int j=0;j<pl_n;j++) if(pl_items[j]==histp[k]){ g_hist[k]=j; break; }
}
static const char* pl_base(const char*nm){ const char*b=nm; for(const char*q=nm;*q;q++) if(*q=='/'||*q=='\\')b=q+1; return b; }
static int pl_cmp(const void*A,const void*B){
    int ia=*(const int*)A, ib=*(const int*)B, r=0;
    if(g_pl_sort_key==1){ long long a=pl_sz?pl_sz[ia]:0, b=pl_sz?pl_sz[ib]:0; r=a<b?-1:a>b?1:0; }
    else if(g_pl_sort_key==2){ double a=pl_dur?pl_dur[ia]:0, b=pl_dur?pl_dur[ib]:0; r=a<b?-1:a>b?1:0; }
    else if(g_pl_sort_key==3){ double a=g_sort_mt?g_sort_mt[ia]:0, b=g_sort_mt?g_sort_mt[ib]:0; r=a<b?-1:a>b?1:0; }
    else {  /* NATURAL title order: digit runs compare by value, so "Ep 2" < "Ep 10" (not "Ep 10" < "Ep 2") */
        const unsigned char*a=(const unsigned char*)pl_base(pl_items[ia]),*b=(const unsigned char*)pl_base(pl_items[ib]);
        while(*a && *b && r==0){
            if(isdigit(*a) && isdigit(*b)){
                while(*a=='0') a++; while(*b=='0') b++;
                const unsigned char*ea=a,*eb=b; while(isdigit(*ea)) ea++; while(isdigit(*eb)) eb++;
                long la=(long)(ea-a), lb=(long)(eb-b);
                if(la!=lb) r = la<lb?-1:1;                         /* more digits = bigger number */
                else { for(;a<ea;a++,b++) if(*a!=*b){ r=*a<*b?-1:1; break; } a=ea; b=eb; }
            } else { int ca=tolower(*a), cb=tolower(*b); if(ca!=cb) r=ca-cb; else { a++; b++; } }
        }
        if(r==0) r=tolower(*a)-tolower(*b);
    }
    if(r==0) r=ia-ib;                       /* stable tiebreak on original order */
    return r*g_pl_sort_dir;
}
/* sort the whole playlist by key (0=title, 1=size, 2=length, 3=date modified); same key again reverses.
   size/length are lazily probed only for on-screen rows, so fill any missing values first. */
static void pl_sort(int key){
    if(pl_n<2) return;
    if(key==1){ for(int i=0;i<pl_n;i++) if(pl_sz&&pl_sz[i]==-1) pl_sz[i]=pl_file_size(pl_items[i]); }
    else if(key==2){ toast("Sorting by length — probing files…");
        for(int i=0;i<pl_n;i++) if(pl_dur&&pl_dur[i]==-1){ int mk=media_kind(pl_items[i]); pl_dur[i]=(mk==2)?-2:pl_probe_duration(pl_items[i]); } }
    else if(key==3){                       /* date modified: values are only valid for the CURRENT order */
        if(!g_sort_mt_fresh){ double*t=realloc(g_sort_mt,(size_t)pl_n*sizeof(double)); if(!t) return; g_sort_mt=t;
            for(int i=0;i<pl_n;i++) g_sort_mt[i]=pl_file_mtime(pl_items[i]); } }
    g_sort_mt_fresh=0;
    if(g_pl_sort_key==key) g_pl_sort_dir=-g_pl_sort_dir;
    else { g_pl_sort_key=key; g_pl_sort_dir = (key==3) ? -1 : 1; }   /* date: newest first on the first click */
    char *curp=(pl_cur>=0&&pl_cur<pl_n)?pl_items[pl_cur]:NULL;
    char *selp=(g_pl_selected>=0&&g_pl_selected<pl_n)?pl_items[g_pl_selected]:NULL;
    char *histp[512]; int hn=g_hist_n<512?g_hist_n:512;
    for(int k=0;k<hn;k++) histp[k]=(g_hist[k]>=0&&g_hist[k]<pl_n)?pl_items[g_hist[k]]:NULL;
    int *ord=malloc((size_t)pl_n*sizeof(int)); if(!ord) return; for(int i=0;i<pl_n;i++)ord[i]=i;
    qsort(ord,pl_n,sizeof(int),pl_cmp);
    char **ni=malloc((size_t)pl_n*sizeof(char*)); if(!ni){ free(ord); return; }
    long long *ns=pl_sz?malloc((size_t)pl_n*sizeof(long long)):NULL;
    double *nd=pl_dur?malloc((size_t)pl_n*sizeof(double)):NULL;
    unsigned char *nse=pl_sel?malloc((size_t)pl_n):NULL;
    for(int i=0;i<pl_n;i++){ int s=ord[i]; ni[i]=pl_items[s]; if(ns)ns[i]=pl_sz[s]; if(nd)nd[i]=pl_dur[s]; if(nse)nse[i]=pl_sel[s]; }
    memcpy(pl_items,ni,(size_t)pl_n*sizeof(char*));
    if(ns){ memcpy(pl_sz,ns,(size_t)pl_n*sizeof(long long)); free(ns); }
    if(nd){ memcpy(pl_dur,nd,(size_t)pl_n*sizeof(double)); free(nd); }
    if(nse){ memcpy(pl_sel,nse,(size_t)pl_n); free(nse); }
    free(ni); free(ord);
    if(curp){ for(int k=0;k<pl_n;k++) if(pl_items[k]==curp){pl_cur=k;break;} }
    if(selp){ for(int k=0;k<pl_n;k++) if(pl_items[k]==selp){g_pl_selected=k;break;} }
    for(int k=0;k<hn;k++) if(histp[k]) for(int j=0;j<pl_n;j++) if(pl_items[j]==histp[k]){ g_hist[k]=j; break; }
    pl_scroll=0; g_pl_jump=1; g_sess_dirty=1;
}

/* ---------- async playlist workers (spinner while they run) ---------- */
static int worker_probe_sortkey(void *arg){  /* fills g_busy_dur[i] with the sort key for every row */
    (void)arg; int n=pl_n, key=g_busy_key;          /* pl_items is stable: edits are gated while busy */
    for(int i=0;i<n;i++){
        if(key==1)      g_busy_dur[i]=(double)pl_file_size(pl_items[i]);
        else if(key==3) g_busy_dur[i]=pl_file_mtime(pl_items[i]);
        else { int mk=media_kind(pl_items[i]); g_busy_dur[i]=(mk==2)?-2:pl_probe_duration(pl_items[i]); }
    }
    g_busy=BUSY_DONE; return 0;
}
static int worker_scan_folder(void *arg){     /* walks g_scan_root into g_scan (does NOT touch the playlist) */
    (void)arg; g_scan.n=0; scan_into(&g_scan, g_scan_root); g_busy=BUSY_DONE; return 0;
}
/* sort by a key that needs per-file probing (1=size, 2=length, 3=date): probe off-thread with a
   spinner, then the main loop stores the values + sorts. Title (0) and small lists sort immediately. */
static void pl_sort_start(int key){
    if(g_busy) return;
    if(key==0 || pl_n < 24){ pl_sort(key); return; }
    if(key==1){ int miss=0; for(int i=0;i<pl_n;i++) if(pl_sz && pl_sz[i]==-1){ miss=1; break; } if(!miss){ pl_sort(1); return; } }
    if(key==2){ int miss=0; for(int i=0;i<pl_n;i++) if(pl_dur && pl_dur[i]==-1){ miss=1; break; } if(!miss){ pl_sort(2); return; } }
    double *t=realloc(g_busy_dur,(size_t)pl_n*sizeof(double)); if(!t){ pl_sort(key); return; } g_busy_dur=t;
    g_busy_task=BTASK_SORT; g_busy_key=key;
    snprintf(g_busy_label,sizeof g_busy_label,"%s", key==2?"Reading lengths\342\200\246":key==3?"Reading dates\342\200\246":"Reading sizes\342\200\246");
    g_busy=BUSY_WORKING; g_busy_thread=SDL_CreateThread(worker_probe_sortkey,"pl_sortkey",NULL);
    if(!g_busy_thread){ g_busy=BUSY_NONE; pl_sort(key); }
}
/* kick off a folder add: enumerate off-thread, then the main loop appends the results. */
static void pl_folder_add_start(const char *root){
    if(g_busy){ return; }
    snprintf(g_scan_root,sizeof g_scan_root,"%s",root); g_scan.n=0;
    g_busy_task=BTASK_FOLDER; snprintf(g_busy_label,sizeof g_busy_label,"Scanning folder\342\200\246");
    g_busy=BUSY_WORKING; g_busy_thread=SDL_CreateThread(worker_scan_folder,"pl_scan",NULL);
    if(!g_busy_thread){ g_busy=BUSY_NONE; int before=pl_n; add_path_recursive(root);   /* fallback: sync */
        if(pl_n>before && pl_cur<0) g_pl_selected=before; }
}
/* called each main-loop iteration: apply a finished worker's result on the main thread. */
static void pl_busy_poll(void){
    if(g_busy!=BUSY_DONE) return;
    if(g_busy_thread){ SDL_WaitThread(g_busy_thread,NULL); g_busy_thread=NULL; }
    if(g_busy_task==BTASK_SORT){
        int key=g_busy_key;
        if(key==1){ if(pl_sz) for(int i=0;i<pl_n;i++) pl_sz[i]=(long long)g_busy_dur[i]; }
        else if(key==2){ if(pl_dur) for(int i=0;i<pl_n;i++) pl_dur[i]=g_busy_dur[i]; }
        else if(key==3){ double*t=realloc(g_sort_mt,(size_t)pl_n*sizeof(double));
            if(t){ g_sort_mt=t; memcpy(g_sort_mt,g_busy_dur,(size_t)pl_n*sizeof(double)); g_sort_mt_fresh=1; } }
        g_busy=BUSY_NONE; pl_sort(key);          /* values all present → the sort itself is instant */
    } else {                                     /* BTASK_FOLDER */
        int before=pl_n, added=0, found=g_scan.n;
        pl_append_unique(&g_scan,&added);
        plist_free(&g_scan);
        char msg[96];
        if(added) snprintf(msg,sizeof msg,"Added %d file%s%s",added,added==1?"":"s",found>added?" (rest already listed)":"");
        else snprintf(msg,sizeof msg,found?"All %d already in the playlist":"No media found in that folder",found);
        toast(msg);
        if(pl_n>before && pl_cur<0){ g_pl_selected=before; g_pl_jump_sel=1; }
        g_busy=BUSY_NONE;
    }
}
/* ---------- playlist save / session persistence ---------- */
/* write the playlist as UTF-8 .m3u8. session=1 adds #HOGS-SEL / #HOGS-FILTER so the next launch comes
   back with the same row selected and the same media-type chip. Written to <path>.tmp then renamed, so a
   crash mid-write can never leave a truncated session. */
/* the whole file as one malloc'd buffer (cheap: a memcpy of the paths) — so the slow part, the disk
   write, can run on a worker thread for the periodic session save */
static char *pl_m3u_build(int session, size_t *len_out){
    size_t cap = 64 + 2200; for(int i=0;i<pl_n;i++) cap += strlen(pl_items[i]) + 1;
    char *b = malloc(cap); if(!b) return NULL;
    size_t n = 0;
    n += (size_t)snprintf(b+n, cap-n, "#EXTM3U\n");
    if(session){
        int si = (pl_cur>=0 && pl_cur<pl_n) ? pl_cur : g_pl_selected;
        if(si>=0 && si<pl_n) n += (size_t)snprintf(b+n, cap-n, "#HOGS-SEL:%.2048s\n", pl_items[si]);
        n += (size_t)snprintf(b+n, cap-n, "#HOGS-FILTER:%d\n", g_pl_filter);
    }
    for(int i=0;i<pl_n;i++){ size_t l=strlen(pl_items[i]); memcpy(b+n, pl_items[i], l); n += l; b[n++]='\n'; }
    *len_out = n; return b;
}
static int pl_write_buf(const char *path, const char *buf, size_t len){
    char tmp[1200]; snprintf(tmp,sizeof tmp,"%s.tmp",path);
    FILE *f=path_fopen(tmp,L"wb"); if(!f) return 0;
    int ok = fwrite(buf,1,len,f)==len && !ferror(f); if(fclose(f)!=0) ok=0;
    if(!ok){ path_remove(tmp); return 0; }
    wchar_t wt[1300], wp[1300]; path_u8_to_w(tmp,wt,1300); path_u8_to_w(path,wp,1300);
    if(!MoveFileExW(wt,wp,MOVEFILE_REPLACE_EXISTING)){ path_remove(tmp); return 0; }
    return 1;
}
static int pl_write_m3u(const char *path, int session){
    size_t len; char *b = pl_m3u_build(session, &len); if(!b) return 0;
    int ok = pl_write_buf(path, b, len); free(b); return ok;
}
static Uint32 g_sess_saved_at = 0;
static SDL_atomic_t g_sess_writing;    /* a background session write is in flight */
typedef struct { char path[1100]; char *buf; size_t len; } SessJob;
static int sess_write_thread(void *ud){
    SessJob *j = ud;
    Uint64 t0 = SDL_GetPerformanceCounter();
    if(!pl_write_buf(j->path, j->buf, j->len)) fprintf(stderr,"[session] background save failed\n");
    else fprintf(stderr,"[session] saved in background (%u KB, %.1f ms off the main thread)\n", (unsigned)(j->len/1024),
                 (double)(SDL_GetPerformanceCounter()-t0)*1000.0/(double)SDL_GetPerformanceFrequency());
    free(j->buf); free(j); SDL_AtomicSet(&g_sess_writing, 0);
    return 0;
}
/* periodic crash-safety save. A 1.3 MB write + rename (plus the virus scanner on close) on the main
   thread could hitch a frame or two mid-movie; snapshot here, write on a worker (2026-10-08). */
static void sess_save_async(void){
    if(g_nosession || g_sess_temp){ g_sess_dirty=0; return; }
    if(SDL_AtomicGet(&g_sess_writing)) return;             /* previous one still writing: try later */
    SessJob *j = malloc(sizeof *j); if(!j) return;
    sess_file(j->path, sizeof j->path, "hogs_session.m3u8");
    j->buf = pl_m3u_build(1, &j->len); if(!j->buf){ free(j); return; }
    SDL_AtomicSet(&g_sess_writing, 1);
    SDL_Thread *t = SDL_CreateThread(sess_write_thread, "sesssave", j);
    if(!t){ SDL_AtomicSet(&g_sess_writing, 0); free(j->buf); free(j); return; }
    SDL_DetachThread(t);
    g_sess_dirty = 0; g_sess_saved_at = SDL_GetTicks();
}
static void sess_save(void){
    if(g_nosession || g_sess_temp) return;
    while(SDL_AtomicGet(&g_sess_writing)) SDL_Delay(2);    /* never race a background write on the .tmp */
    char p[1100]; sess_file(p,sizeof p,"hogs_session.m3u8");
    if(pl_write_m3u(p,1)) g_sess_dirty=0;
    g_sess_saved_at=SDL_GetTicks();
}
/* restore last session: playlist, selected row, media-type chip. Returns rows restored. */
static int sess_load(void){
    if(g_nosession || g_sess_temp) return 0;
    char p[1100]; sess_file(p,sizeof p,"hogs_session.m3u8");
    PathList L={NULL,0,0}; char sel[2048]=""; int filt=g_pl_filter;
    if(!pl_read_m3u(p,&L,sel,sizeof sel,&filt)){ plist_free(&L); return 0; }
    int added=0; pl_append_unique(&L,&added); plist_free(&L);
    g_pl_filter=filt;
    if(sel[0]) for(int i=0;i<pl_n;i++) if(!SDL_strcasecmp(pl_items[i],sel)){ g_pl_selected=i; g_pl_jump_sel=1; break; }
    g_sess_dirty=0;
    fprintf(stderr,"[session] restored %d entries from %s\n",added,p);
    return added;
}
/* ---------- native file / folder pickers (Add Media) ---------- */
static HWND hogs_hwnd(void){
    SDL_SysWMinfo wm; SDL_VERSION(&wm.version);
    if(P.win && SDL_GetWindowWMInfo(P.win,&wm)) return wm.info.win.window;
    return NULL;
}
static void add_files_dialog(void){
    static wchar_t wbuf[32768]; wbuf[0]=0;                    /* wide so Unicode filenames survive */
    OPENFILENAMEW ofn; ZeroMemory(&ofn,sizeof(ofn));
    ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=hogs_hwnd();
    ofn.lpstrFilter=
        L"Media files\0*.mp4;*.m4v;*.mov;*.mkv;*.webm;*.avi;*.wmv;*.flv;*.mpg;*.mpeg;*.ts;*.m2ts;*.mts;*.ogv;*.3gp;*.divx;*.vob;*.nut;*.y4m;*.rmvb;*.rm;*.ogm;*.asf;*.f4v;*.m2t;*.mpe;*.qt;*.mk3d;*.3g2;*.mka;*.m4b;*.aif;*.aiff;*.wv;*.ac3;*.amr;*.mp3;*.flac;*.wav;*.aac;*.ogg;*.opus;*.m4a;*.wma;*.jpg;*.jpeg;*.png;*.gif;*.bmp;*.webp;*.tif;*.tiff;*.tga;*.jxl;*.avif;*.heic;*.m3u8;*.m3u\0"
        L"All files\0*.*\0\0";
    ofn.lpstrFile=wbuf; ofn.nMaxFile=32768;
    ofn.Flags=OFN_EXPLORER|OFN_ALLOWMULTISELECT|OFN_FILEMUSTEXIST|OFN_HIDEREADONLY|OFN_NOCHANGEDIR;
    int before=pl_n;
    if(GetOpenFileNameW(&ofn)){
        wchar_t *wq=wbuf+wcslen(wbuf)+1;
        if(*wq==0){ char full[2048]; path_w_to_u8(wbuf,full,sizeof(full)); add_path_recursive(full); } /* single: full path */
        else{ char dir[2048]; path_w_to_u8(wbuf,dir,sizeof(dir));         /* multi: dir\0name\0name\0\0 */
            for(; *wq; wq+=wcslen(wq)+1){ char nm[1024]; path_w_to_u8(wq,nm,sizeof(nm));
                char full[2048]; snprintf(full,sizeof(full),"%s\\%s",dir,nm); add_path_recursive(full); } }
    }
    if(pl_n>before && pl_cur<0){ g_pl_selected=before; toast("Added — press Space or Play"); }
}
/* Save the whole playlist (every row, in its current order) to a .m3u8 the user picks. */
static void pl_save_dialog(void){
    if(pl_n==0){ toast("Playlist is empty \342\200\224 nothing to save"); return; }
    wchar_t wf[1040]=L"playlist.m3u8";
    OPENFILENAMEW sfn; ZeroMemory(&sfn,sizeof sfn);
    sfn.lStructSize=sizeof sfn; sfn.hwndOwner=hogs_hwnd();
    sfn.lpstrFilter=L"Playlist (*.m3u8)\0*.m3u8\0Playlist, legacy (*.m3u)\0*.m3u\0All files\0*.*\0";
    sfn.lpstrFile=wf; sfn.nMaxFile=1040; sfn.lpstrTitle=L"Save playlist as…";
    sfn.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR; sfn.lpstrDefExt=L"m3u8";
    if(!GetSaveFileNameW(&sfn)) return;
    char path[2048]; path_w_to_u8(wf,path,sizeof path);
    char msg[96];
    if(pl_write_m3u(path,0)) snprintf(msg,sizeof msg,"Saved %d item%s",pl_n,pl_n==1?"":"s");
    else snprintf(msg,sizeof msg,"Save FAILED \342\200\224 check the folder");
    toast(msg);
}
/* Open a .m3u/.m3u8 and APPEND its entries (rows already listed are not duplicated). Clear first to replace. */
static void pl_load_dialog(void){
    wchar_t wf[1040]; wf[0]=0;
    OPENFILENAMEW ofn; ZeroMemory(&ofn,sizeof ofn);
    ofn.lStructSize=sizeof ofn; ofn.hwndOwner=hogs_hwnd();
    ofn.lpstrFilter=L"Playlists (*.m3u8;*.m3u)\0*.m3u8;*.m3u\0All files\0*.*\0";
    ofn.lpstrFile=wf; ofn.nMaxFile=1040; ofn.lpstrTitle=L"Open playlist";
    ofn.Flags=OFN_FILEMUSTEXIST|OFN_HIDEREADONLY|OFN_NOCHANGEDIR;
    if(!GetOpenFileNameW(&ofn)) return;
    char path[2048]; path_w_to_u8(wf,path,sizeof path);
    PathList L={NULL,0,0};
    if(!pl_read_m3u(path,&L,NULL,0,NULL)){ toast("Could not read that playlist"); return; }
    int added=0, first=pl_append_unique(&L,&added); int total=L.n; plist_free(&L);
    char msg[96]; snprintf(msg,sizeof msg,"Playlist: %d entr%s, %d new",total,total==1?"y":"ies",added); toast(msg);
    if(first>=0 && pl_cur<0){ g_pl_selected=first; g_pl_jump_sel=1; }
}
static void add_folder_dialog(void){
    BROWSEINFOW bi; ZeroMemory(&bi,sizeof(bi));
    bi.hwndOwner=hogs_hwnd();
    bi.lpszTitle=L"Select a folder — all media in it and its subfolders is added";
    bi.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl=SHBrowseForFolderW(&bi);
    if(pidl){
        wchar_t wpath[MAX_PATH];
        if(SHGetPathFromIDListW(pidl,wpath)){ char path[2048]; path_w_to_u8(wpath,path,sizeof(path));
            pl_folder_add_start(path);   /* enumerate off-thread with a spinner; appended when done */ }
        CoTaskMemFree(pidl);
    }
}

/* ---------------- main ---------------- */
static void usage(void){ printf("hogofwar's ultimate media player — drag video/image/audio onto the window, or: hogofwar.exe <file> [file2 ...]\n"); }

/* query the native resolution + refresh of the display the window is on - used for
   true native-resolution rendering in fullscreen and by the stats panel. */
static void update_monitor_mode(void){
    int di = P.win ? SDL_GetWindowDisplayIndex(P.win) : 0; if(di < 0) di = 0;
    SDL_DisplayMode dm;
    if(SDL_GetDesktopDisplayMode(di, &dm) == 0){ g_mon_w = dm.w; g_mon_h = dm.h; g_mon_hz = dm.refresh_rate; }
}

/* FULLSCREEN (2026-10-08): a borderless window ONE PIXEL LARGER than the monitor, instead of SDL's
   FULLSCREEN_DESKTOP. An OpenGL window that exactly covers a monitor gets promoted by the driver/DWM to
   direct "independent flip" scan-out, with FreeSync/VRR engaged on that screen. The user's setup is a
   4K 60Hz FreeSync monitor beside a 144Hz main screen (AMD 7900 XTX); activity on the other monitors
   flips the 4K screen in and out of that mode, which showed as occasional black screens ONLY there and
   ONLY in fullscreen. One pixel of overhang, on a side where no other monitor sits so it is never
   visible, keeps it an ordinary composed window: same picture, no mode switches.
   (An earlier note blamed the monitor's DisplayPort link; the "only when busy on another monitor"
   pattern points at the flip-mode switching instead.) HOGS_FS_EXACT=1 restores the old behaviour. */
static int g_fs_on = 0; static WINDOWPLACEMENT g_fs_wp; static LONG_PTR g_fs_style = 0;
static HWND hogs_hwnd(void);
static int is_fullscreen(void){ return g_fs_on || (SDL_GetWindowFlags(P.win) & SDL_WINDOW_FULLSCREEN_DESKTOP); }
static void toggle_fullscreen(void){
    HWND h = hogs_hwnd();
    if(getenv("HOGS_FS_EXACT") || !h){
        Uint32 fs = SDL_GetWindowFlags(P.win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
        SDL_SetWindowFullscreen(P.win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        return;
    }
    if(!g_fs_on){
        MONITORINFO mi; mi.cbSize = sizeof mi;
        if(!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return;
        g_fs_wp.length = sizeof g_fs_wp; GetWindowPlacement(h, &g_fs_wp);
        g_fs_style = GetWindowLongPtrW(h, GWL_STYLE);
        RECT r = mi.rcMonitor, s;
        s = r; s.top = r.bottom; s.bottom = r.bottom+1;            /* below free? (usual case) */
        if(!MonitorFromRect(&s, MONITOR_DEFAULTTONULL)) r.bottom++;
        else { s = r; s.bottom = r.top; s.top = r.top-1;
            if(!MonitorFromRect(&s, MONITOR_DEFAULTTONULL)) r.top--;
            else { s = r; s.left = r.right; s.right = r.right+1;
                if(!MonitorFromRect(&s, MONITOR_DEFAULTTONULL)) r.right++;
                else { s = r; s.right = r.left; s.left = r.left-1;
                    if(!MonitorFromRect(&s, MONITOR_DEFAULTTONULL)) r.left--; } } }  /* boxed in: exact fit */
        SetWindowLongPtrW(h, GWL_STYLE, (g_fs_style & ~(LONG_PTR)(WS_OVERLAPPEDWINDOW)) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, r.left, r.top, r.right-r.left, r.bottom-r.top,
                     SWP_FRAMECHANGED|SWP_NOOWNERZORDER|SWP_SHOWWINDOW);
        g_fs_on = 1;
        fprintf(stderr,"[fs] borderless %ldx%ld at (%ld,%ld) — monitor %ldx%ld\n", r.right-r.left, r.bottom-r.top,
                r.left, r.top, mi.rcMonitor.right-mi.rcMonitor.left, mi.rcMonitor.bottom-mi.rcMonitor.top);
    } else {
        SetWindowLongPtrW(h, GWL_STYLE, g_fs_style);
        SetWindowPlacement(h, &g_fs_wp);
        SetWindowPos(h, NULL, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOOWNERZORDER|SWP_FRAMECHANGED);
        g_fs_on = 0;
    }
}

/* last-resort crash logger: records the faulting address + which module it lives in
   (our exe? the GPU driver? FFmpeg? SDL?) so we can see WHERE it dies, not just that it did. */
static LONG WINAPI hogs_crash_handler(EXCEPTION_POINTERS *ep){
    void *addr = ep->ExceptionRecord->ExceptionAddress;
    unsigned long code = (unsigned long)ep->ExceptionRecord->ExceptionCode;
    fprintf(stderr,"\n[CRASH] code=0x%08lx addr=%p\n", code, addr);
    if(code==0xC0000005 && ep->ExceptionRecord->NumberParameters>=2)
        fprintf(stderr,"[CRASH] access violation %s addr 0x%p\n",
                ep->ExceptionRecord->ExceptionInformation[0]?"WRITE":"READ",
                (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    HMODULE mod=NULL;
    if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCSTR)addr, &mod) && mod){
        wchar_t wname[MAX_PATH]={0}; GetModuleFileNameW(mod, wname, MAX_PATH);
        char name[MAX_PATH*3]={0}; path_w_to_u8(wname, name, sizeof name);
        fprintf(stderr,"[CRASH] in module %s  (base=%p, offset=0x%llx)\n",
                name, (void*)mod, (unsigned long long)((char*)addr-(char*)mod));
    } else fprintf(stderr,"[CRASH] address not in any known module\n");
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;   /* let the process terminate after logging */
}


/* ---- AI ENHANCE: offline whole-video Real-ESRGAN upscale (U) and/or RIFE frame-gen (G), run by the
   headless hogs_enhance.exe. The player PAUSES with a progress bar, then plays the enhanced result in the
   SAME window. NO separate window, NO fullscreen — the GUI + hotkeys stay live throughout. */
static int file_exists_w(const char *p){ wchar_t w[1300]; int n=MultiByteToWideChar(CP_UTF8,0,p,-1,w,1300);
    return n>0 && GetFileAttributesW(w)!=INVALID_FILE_ATTRIBUTES; }
/* minimal readers for OUR status JSON (not a general parser) */
static int js_num(const char*s,const char*k,double*o){ char kk[48]; snprintf(kk,sizeof kk,"\"%s\":",k);
    const char*p=strstr(s,kk); if(!p) return 0; p+=strlen(kk); while(*p==' ')p++; *o=atof(p); return 1; }
static int js_str(const char*s,const char*k,char*b,int cap){ char kk[48]; snprintf(kk,sizeof kk,"\"%s\":",k);
    const char*p=strstr(s,kk); if(!p) return 0; p+=strlen(kk); while(*p==' ')p++; if(*p!='"')return 0; p++;
    int i=0; while(*p&&*p!='"'&&i<cap-1){ if(*p=='\\'&&p[1])p++; b[i++]=*p++; } b[i]=0; return 1; }
static void enh_delete_out(void){ if(g_enh.out[0]){ path_remove(g_enh.out); g_enh.out[0]=0; } }
/* ---- enhanced-output CACHE: <base>enhcache\e<hash>_<stages>.mkv. Replaying a video or re-toggling
        U/G finds the file instantly instead of re-processing. LRU-evicted to ~50GB (by last-played
        time); the WHOLE cache is wiped when the player exits. ---- */
#define ENH_CACHE_BUDGET (50ULL<<30)                 /* keep at most ~50GB of enhanced videos */
static void enh_wide(const char*u8,wchar_t*w,int cap){ if(MultiByteToWideChar(CP_UTF8,0,u8,-1,w,cap)<=0) w[0]=0; }
static void enh_cache_dir(char*buf,size_t cap){
    snprintf(buf,cap,"%senhcache\\",data_dir());
}
static unsigned long long enh_fnv64(const char*s){
    unsigned long long h=1469598103934665603ULL;
    while(*s){ h^=(unsigned char)*s++; h*=1099511628211ULL; }
    return h;
}
/* The enhancer now upscales the FULL NATIVE source (Real-ESRGAN x4, no downscale-before-upscale),
   so the output no longer depends on the monitor — it's always source x4. enh_target_maxh() is kept
   only so the CLI keeps a --maxh argument (ignored for capping enhancer-side); it's no longer in the
   cache key. The key carries a "_nat1" pipeline tag so pre-native (downscaled) cache entries — which
   used a _h<monitor> suffix — can never be mistaken for a native render. */
static int enh_target_maxh(void){ int h = g_mon_h>0 ? g_mon_h : 1440; return h<1440 ? h : 1440; }
/* MODEL SELECTION (owner policy, official Real-ESRGAN model zoo only, NO downscaling):
   pick the model whose scale gets CLOSEST to the native monitor from the current source.
     anime (T):     realesr-animevideov3-x{2,3,4} — smallest scale that reaches the monitor.
     real footage:  need x2 → RealESRGAN_x2plus   (photographic x2)
                    need x3 → realesr-general-x4v3 (no official x3 for real footage; closest)
                    need x4+→ RealESRGAN_x4plus    (photographic x4; heavy, ~10x slower than general)
                    need x1 (src >= monitor) → no upscale at all (nothing to gain).
   x2plus ships PyTorch-only in the zoo; until a valid ncnn conversion of the OFFICIAL .pth lands in
   vulkan\models as realesrgan-x2plus.param/.bin, the x2 bucket falls back to general-x4v3. */
static int enh_anime_scale(void){
    int sh=g_src_h>0?g_src_h:1080, mh=g_mon_h>0?g_mon_h:1440;
    if(sh*2>=mh) return 2;
    if(sh*3>=mh) return 3;
    return 4;
}
static int enh_real_bucket(void){                    /* 1=restore at native res, 2/3/4 = upscale bucket */
    int sh=g_src_h>0?g_src_h:1080, mh=g_mon_h>0?g_mon_h:1440;
    if(sh>=mh)     return 1;
    if(sh*2>=mh)   return 2;
    if(sh*3>=mh)   return 3;
    return 4;
}
/* v1.44 "FAST AI" POLICY: the compact realesr-general-x4v3 for ALL real-footage buckets.
   MEASURED RATIONALE (v1.43, TNG 448x352 episode): general chain up+fg = 42 fps (faster than
   realtime — press E and WATCH, render outruns playback), vs RealESRGAN_x4plus = ~5 HOURS for the
   same 42-min episode (~10x compute) for ~10% more texture. The plus models (x2plus/x4plus) remain
   in vulkan\models — re-enable per-bucket here if a "Best quality / overnight" tier is ever wanted
   (v1.43 keeps that policy live). NOTE: post-2026-07-07 native-res pipeline — the old "cartoonish"
   look was mostly the downscale-before-upscale bug, not the general model itself. */
static void enh_real_model(const char*base, int bucket, char*name, size_t cap, int*scale,
                           const char**inblob, const char**outblob){
    *inblob="data"; *outblob="output";
    if(g_arch_sel){
        /* ARCHIVE = quality x4 (owner request 2026-10-27): RealESRGAN_x4plus for every source that gets
           upscaled — no more x2plus / general-x4v3 picks by source height. x4plus also runs on the
           GPU-resident chain (x4, data/output), so archive renders multi-worker instead of the 1-worker
           separate RIFE path (~30% GPU). Bucket 1 (source already >= monitor) is a native-res RESTORE,
           not an upscale: keep the lighter general model there (x4plus on a 4K source = 16K inference). */
        (void)base;
        if(bucket==1){ snprintf(name,cap,"realesr-general-x4v3"); *scale=4; return; }
        snprintf(name,cap,"realesrgan-x4plus"); *scale=4; return;
    }
    (void)bucket;
    snprintf(name,cap,"realesr-general-x4v3"); *scale=4;   /* FAST AI default: x1..x4 all general */
}
static void enh_cache_path(char*buf,size_t cap,const char*src,int up,int fg,int anime){
    char dir[1040]; enh_cache_dir(dir,sizeof dir);
    /* model choice is in the key: different model/scale = different output file */
    char tag[4]="";
    if(anime)   snprintf(tag,sizeof tag,"A%d",enh_anime_scale());
    else if(up) snprintf(tag,sizeof tag,"%s%d",g_arch_sel?"Q":"U",enh_real_bucket());  /* Q = quality-x4 archive (was P = mixed x2/x4 plus policy) */
    snprintf(buf,cap,"%se%016llx_%s%s_nat1.mkv",dir,enh_fnv64(src),tag,fg?"G":"");
}
static void enh_cache_touch(const char*path){        /* bump mtime = "played just now" for the LRU */
    wchar_t w[1300]; enh_wide(path,w,1300);
    HANDLE h=CreateFileW(w,FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
    if(h!=INVALID_HANDLE_VALUE){ FILETIME ft; GetSystemTimeAsFileTime(&ft); SetFileTime(h,NULL,NULL,&ft); CloseHandle(h); }
}
/* delete least-recently-played entries until the cache fits the budget (run before each new job).
   Every file counts toward the total even past the 256-entry table (no silent blind spot); a failed
   delete (e.g. the file is OPEN — the one currently playing) does NOT count as freed space; when the
   table drains while still over budget, rescan so previously-untracked files become candidates. */
static void enh_cache_evict(void){
    static struct { unsigned long long mt,sz; wchar_t name[260]; } ent[256];
    char dir[1040]; enh_cache_dir(dir,sizeof dir);
    wchar_t wdir[1300],wpat[1300]; enh_wide(dir,wdir,1300);
    _snwprintf(wpat,1300,L"%se*.mkv",wdir);
    for(int pass=0; pass<4; pass++){
        int n=0; unsigned long long tot=0;
        WIN32_FIND_DATAW fd; HANDLE h=FindFirstFileW(wpat,&fd);
        if(h!=INVALID_HANDLE_VALUE){ do{
            unsigned long long sz=((unsigned long long)fd.nFileSizeHigh<<32)|fd.nFileSizeLow;
            tot+=sz;                                     /* ALL files count toward the budget */
            if(n<256){
                ent[n].mt=((unsigned long long)fd.ftLastWriteTime.dwHighDateTime<<32)|fd.ftLastWriteTime.dwLowDateTime;
                ent[n].sz=sz; wcsncpy(ent[n].name,fd.cFileName,259); ent[n].name[259]=0; n++; }
        } while(FindNextFileW(h,&fd)); FindClose(h); }
        if(tot<=ENH_CACHE_BUDGET) return;
        int deleted=0;
        while(tot>ENH_CACHE_BUDGET && n>0){
            int old=0; for(int i=1;i<n;i++) if(ent[i].mt<ent[old].mt) old=i;
            wchar_t full[1600]; _snwprintf(full,1600,L"%s%s",wdir,ent[old].name);
            if(DeleteFileW(full)){ tot-=ent[old].sz; deleted=1; }   /* locked/open file: skip, try next */
            ent[old]=ent[--n];
        }
        if(!deleted || tot<=ENH_CACHE_BUDGET) return;    /* nothing deletable, or done */
    }
}
static void enh_cache_wipe(void){                    /* quit: empty the cache — videos AND leftovers
    (a cancelled --workers enhance can strand tiny e*.mkv.wN.json worker-progress files: the parent
    is killed before its own cleanup runs, so catch EVERYTHING here, not just *.mkv) */
    char dir[1040]; enh_cache_dir(dir,sizeof dir);
    wchar_t wdir[1300],wpat[1300]; enh_wide(dir,wdir,1300);
    _snwprintf(wpat,1300,L"%se*",wdir);
    WIN32_FIND_DATAW fd; HANDLE h=FindFirstFileW(wpat,&fd);
    if(h!=INVALID_HANDLE_VALUE){ do{
        wchar_t full[1600]; _snwprintf(full,1600,L"%s%s",wdir,fd.cFileName); DeleteFileW(full);
    } while(FindNextFileW(h,&fd)); FindClose(h); }
}
/* load a file directly (not via the playlist) + resume at time t — swaps original <-> enhanced */
static void play_transient(const char *path, double t){
    close_media();
    if(load_media(path)==0){ load_subtitles(path);
        if(t>1.0 && (P.duration<=0 || t<P.duration-0.5)){ P.seek_target=t; P.seek_abs=1; P.seek_flag=1; } }
}
/* shared spawner: build the command line + launch the headless enhancer into job J. */
static int enh_spawn(EnhanceJob *J, const char *inpath, int up, int fg, int anime, const char *outpath, double ss){
    char *bpp=SDL_GetBasePath(); char base[1024]; snprintf(base,sizeof base,"%s",bpp?bpp:""); if(bpp)SDL_free(bpp);
    { char dir[1040]; enh_cache_dir(dir,sizeof dir);        /* ensure the cache dir + make room */
      wchar_t wdir[1300]; enh_wide(dir,wdir,1300); CreateDirectoryW(wdir,NULL); }
    enh_cache_evict();
    snprintf(J->out,sizeof J->out,"%s",outpath);
    snprintf(J->status_path,sizeof J->status_path,"%shogs_enh_%lu.status.json",
             data_dir(),(unsigned long)GetCurrentProcessId());   /* writable data folder, not the program's */
    path_remove(J->status_path); path_remove(J->out);
    const char *MD="vulkan\\models";
    /* AI Upscale = realesr-general-x4v3 (real footage, ~95% of the old RRDB's detail at 9x its
       speed — realtime-class). AnimeUpscale = animevideov3 (fast, cartoonish on live action). */
    /* pick the model per the owner's policy (see enh_real_model/enh_anime_scale above) */
    char upm[64]; int up_sc=4; const char*up_in="data"; const char*up_out="output";
    int bucket = anime ? 0 : enh_real_bucket();
    if(anime){ up_sc=enh_anime_scale(); snprintf(upm,sizeof upm,"realesr-animevideov3-x%d",up_sc); }
    else enh_real_model(base, bucket, upm, sizeof upm, &up_sc, &up_in, &up_out);
    char cmd[4096]; int k=snprintf(cmd,sizeof cmd,
        "\"%shogs_enhance.exe\" --in \"%s\" --out \"%s\" --status \"%s\" --maxh %d"
        " --up-param \"%s%s\\%s.param\" --up-bin \"%s%s\\%s.bin\""
        " --rife-param \"%s%s\\flownet.param\" --rife-bin \"%s%s\\flownet.bin\"",
        base, inpath, J->out, J->status_path, enh_target_maxh(), base,MD,upm, base,MD,upm, base,MD, base,MD);
    if(up && k>0 && k<(int)sizeof cmd) k+=snprintf(cmd+k,sizeof cmd-k," --upscale");
    if(fg && k>0 && k<(int)sizeof cmd) k+=snprintf(cmd+k,sizeof cmd-k," --framegen");
    /* NO downscaling (owner policy): the chosen model's scale IS the output. Tell the enhancer the
       model's scale + input blob, and make its upscale-skip threshold the MONITOR height (so x1 =
       src>=monitor skips, but e.g. 1440p src on a 4K monitor still upscales). */
    if(up_sc!=4 && k>0 && k<(int)sizeof cmd) k+=snprintf(cmd+k,sizeof cmd-k," --up-scale %d", up_sc);
    if(strcmp(up_in,"data") && k>0 && k<(int)sizeof cmd) k+=snprintf(cmd+k,sizeof cmd-k," --up-in %s", up_in);
    if(strcmp(up_out,"output") && k>0 && k<(int)sizeof cmd) k+=snprintf(cmd+k,sizeof cmd-k," --up-out %s", up_out);
    if(bucket==1){
        /* x1 RESTORE (src >= monitor): run the restoration model anyway — it cleans compression
           artifacts/noise even with no resolution to gain — and bring its x4 output back to the
           SOURCE's own native res (never below source). Heaviest config: 4K src = 16K inference. */
        if(k>0 && k<(int)sizeof cmd) k+=snprintf(cmd+k,sizeof cmd-k," --noupscale-above 0 --target-h %d", g_src_h);
    } else if(g_mon_h>0 && k>0 && k<(int)sizeof cmd) k+=snprintf(cmd+k,sizeof cmd-k," --noupscale-above %d", g_mon_h);
    /* play-while-enhancing: progressive rolling-segment render (the enhancer picks its own worker
       count); --ss re-anchors the render to a seek point. */
    if(k>0 && k<(int)sizeof cmd) k+=snprintf(cmd+k,sizeof cmd-k," --progressive");
    if(ss>0.5 && k>0 && k<(int)sizeof cmd) k+=snprintf(cmd+k,sizeof cmd-k," --ss %.3f", ss);
    int wn=MultiByteToWideChar(CP_UTF8,0,cmd,-1,NULL,0); if(wn<=0) return 0;
    wchar_t*wc=(wchar_t*)malloc((size_t)wn*sizeof(wchar_t)); if(!wc) return 0;
    MultiByteToWideChar(CP_UTF8,0,cmd,-1,wc,wn);
    wchar_t wbase[1024]; MultiByteToWideChar(CP_UTF8,0,base,-1,wbase,1024);   /* child cwd = player dir (finds DLLs) */
    STARTUPINFOW si; memset(&si,0,sizeof si); si.cb=sizeof si; memset(&J->pi,0,sizeof J->pi);
    BOOL ok=CreateProcessW(NULL,wc,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,wbase,&si,&J->pi);
    free(wc);
    if(!ok){ fprintf(stderr,"[enh] CreateProcess failed\n"); return 0; }
    J->has_proc=1; J->state=ENH_RUN; J->percent=0; J->last_poll=0; snprintf(J->phase,sizeof J->phase,"Starting");
    J->live=1; J->playing=0; J->waiting=0; J->rendered=0; J->total=0; J->wait_target=-1; J->anchor=ss>0?ss:0;
    return 1;
}
/* launch the FOREGROUND enhancer: inpath -> outpath with the given stages (outpath is a cache key).
   Chaining passes a CACHED upscaled file as inpath with fg only — no re-upscaling.
   anime selects the toon model for the upscale stage (its own button — cartoonish on real footage). */
/* throwaway output for a re-anchored (seeked) live render — never a cache key, wiped on quit */
static void enh_live_path(char*buf,size_t cap){ char dir[1040]; enh_cache_dir(dir,sizeof dir);
    snprintf(buf,cap,"%selive_%lu.mkv",dir,(unsigned long)GetCurrentProcessId()); }
static void enhance_start(const char *inpath, int up, int fg, int anime, const char *outpath){
    if(g_enh.state==ENH_RUN) return;
    /* NOTE: toggles are STICKY — a non-video (image/audio) just skips, keeping the buttons lit
       so the NEXT video auto-enhances. */
    if(pl_cur<0||pl_cur>=pl_n||P.vstream<0||P.is_image) return;
    if(!up && !fg) return;
    /* PLAY-WHILE-ENHANCING: spawn the progressive enhancer at the top and start watching the growing
       output within seconds. anchor=0 → outputs to the CACHE key, so a full watch-through gets cached
       for instant replay. Seeking outside the rendered range later re-anchors to a throwaway path. */
    if(!enh_spawn(&g_enh,inpath,up,fg,anime,outpath,0)){ toast("Could not launch hogs_enhance.exe"); return; }
    snprintf(g_enh.src,sizeof g_enh.src,"%s",inpath); g_enh.r_up=up; g_enh.r_fg=fg; g_enh.r_an=anime;
    g_enh.is_arch=0; g_enh.arch_path[0]=0;   /* archive_start() re-marks the job right after */
    P.paused=1; if(P.audio_dev) SDL_PauseAudioDevice(P.audio_dev,1);
    toast("AI enhancing — playback starts in a few seconds…");
    fprintf(stderr,"[enh] started (live): %s -> %s\n", inpath, outpath);
}
/* RE-ANCHOR: the user seeked outside the rendered range — restart the live render FROM the clicked
   point S (the enhancer emits absolute timestamps from S, so it drops straight onto the timeline).
   The abandoned partial is deleted; the re-anchored render writes to a throwaway path (not cached,
   since a seeked session isn't a full file). Playback resumes from S once its first buffer builds. */
static void enhance_reanchor(double S){
    if(!g_enh.live) return;
    if(g_enh.is_arch){ toast("Archive render \342\200\224 seek limited to the rendered part"); return; }
    int up=g_enh.r_up, fg=g_enh.r_fg, an=g_enh.r_an; char src[1040]; snprintf(src,sizeof src,"%s",g_enh.src);
    if(g_enh.has_proc){ TerminateProcess(g_enh.pi.hProcess,1); WaitForSingleObject(g_enh.pi.hProcess,3000);
        CloseHandle(g_enh.pi.hThread); CloseHandle(g_enh.pi.hProcess); g_enh.has_proc=0; }
    g_enh.state=ENH_IDLE; g_enh.live=g_enh.playing=g_enh.waiting=0; g_enh.wait_target=-1;
    g_live_playing=0; g_live_rendered=0;
    close_media();                                     /* release the old partial so it can be deleted */
    path_remove(g_enh.status_path); enh_delete_out();   /* drop the abandoned [.. up to seek] partial */
    char live[1040]; enh_live_path(live,sizeof live); path_remove(live);
    if(!enh_spawn(&g_enh,src,up,fg,an,live,S)){ toast("Re-anchor failed"); return; }
    snprintf(g_enh.src,sizeof g_enh.src,"%s",src); g_enh.r_up=up; g_enh.r_fg=fg; g_enh.r_an=an;
    P.paused=1; if(P.audio_dev) SDL_PauseAudioDevice(P.audio_dev,1);
    toast("Rendering from here…");
    fprintf(stderr,"[enh] re-anchor at %.1fs\n", S);
}
static void enhance_cancel(void){
    if(g_enh.state!=ENH_RUN) return;
    if(g_enh.has_proc){ TerminateProcess(g_enh.pi.hProcess,1);
        WaitForSingleObject(g_enh.pi.hProcess,3000);   /* let it actually die — else the partial file is
                                                          still open and the remove() below silently fails */
        CloseHandle(g_enh.pi.hThread); CloseHandle(g_enh.pi.hProcess); g_enh.has_proc=0; }
    g_enh.state=ENH_IDLE; g_enh.percent=0; g_enh.phase[0]=0;
    g_enh.live=g_enh.playing=g_enh.waiting=0; g_enh.wait_target=-1; g_live_playing=0; g_live_rendered=0;
    path_remove(g_enh.status_path); enh_delete_out();
    if(g_enh.is_arch){ g_enh.is_arch=0; g_enh.arch_path[0]=0; toast("Archive render cancelled"); return; }  /* playback untouched */
    P.paused=0; if(P.audio_dev) SDL_PauseAudioDevice(P.audio_dev,0);
    toast("AI enhance cancelled");
}
/* seconds of rendered output buffered before live playback starts. Enhancement runs ~1x realtime
   (no margin above playback), so a bigger cushion delays the onset of catch-up "buffering" pauses —
   30s absorbs the low-motion dips where the render briefly drops below realtime. Short clips that
   finish before this just start when done. */
#define PW_HEADSTART 30.0
static void enhance_poll(void){
    if(g_enh.state!=ENH_RUN) return;
    Uint32 now=SDL_GetTicks(); if(now-g_enh.last_poll<250) return; g_enh.last_poll=now;
    char buf[1024]; FILE*f=path_fopen(g_enh.status_path,L"rb");
    if(f){ size_t n=fread(buf,1,sizeof buf-1,f); buf[n]=0; fclose(f);
        double pc; if(js_num(buf,"percent",&pc)) g_enh.percent=(float)pc;
        char ph[64]; if(js_str(buf,"phase",ph,sizeof ph)) snprintf(g_enh.phase,sizeof g_enh.phase,"%s",ph);
        double rr,tt; if(js_num(buf,"rendered",&rr)) g_enh.rendered=rr; if(js_num(buf,"total",&tt)) g_enh.total=tt; }
    /* LIVE: once ~PW_HEADSTART seconds are buffered PAST the anchor, start watching the growing file
       from the anchor (its timestamps are absolute, so play_transient(anchor) is its start). */
    /* ARCHIVE renders in the background and NEVER starts playback by itself (2026-10-10: it used to swap
       to the growing file and unpause after 30s, i.e. "resumes playback during processing"). */
    if(g_enh.live && !g_enh.playing && !g_enh.is_arch &&
       (g_enh.rendered >= g_enh.anchor + PW_HEADSTART || (g_enh.total>0 && g_enh.rendered >= g_enh.total-0.2))){
        play_transient(g_enh.out, g_enh.anchor); g_playing_enhanced=1; g_enh.playing=1;
        if(g_enh.total>0) P.duration=g_enh.total;
        P.paused=0; if(P.audio_dev) SDL_PauseAudioDevice(P.audio_dev,0);
        toast("Playing enhanced — rendering ahead as you watch");
    }
    g_live_playing = (g_enh.live && g_enh.playing); g_live_rendered = g_enh.rendered;   /* mirror for the demux thread */
    if(g_enh.live && g_enh.playing && g_enh.total>0) P.duration=g_enh.total;   /* seek bar spans the whole timeline */
    /* catch-up is now handled by the demux tail-follow (see demux_thread): if playback reaches the
       render head, the queues drain, the video holds its last frame, and audio underruns to silence
       so the master clock stops — everything resumes IN SYNC when more data is appended. No pause,
       no reopen, no drift. (Seeking outside the rendered range is still handled by re-anchor.) */
    DWORD code=STILL_ACTIVE; if(g_enh.has_proc) GetExitCodeProcess(g_enh.pi.hProcess,&code);
    if(code==STILL_ACTIVE) return;
    if(g_enh.has_proc){ CloseHandle(g_enh.pi.hThread); CloseHandle(g_enh.pi.hProcess); g_enh.has_proc=0; }
    int ok = (code==0) && file_exists_w(g_enh.out);
    int was_live=g_enh.live, was_playing=g_enh.playing;
    g_enh.state=ENH_IDLE; g_enh.live=g_enh.playing=g_enh.waiting=0; g_enh.wait_target=-1;
    g_live_playing=0; g_live_rendered=0;
    if(ok && g_enh.is_arch){
        /* ARCHIVE finished: save the remaster to the chosen file and leave playback exactly as it is
           (paused, or whatever the user chose to watch meanwhile). The cache copy stays for instant replay. */
        path_remove(g_enh.status_path);
        wchar_t wsrc[1100],wdst[1100]; path_u8_to_w(g_enh.out,wsrc,1100); path_u8_to_w(g_enh.arch_path,wdst,1100);
        if(CopyFileW(wsrc, wdst, FALSE)){
            char m[1200]; snprintf(m,sizeof m,"Archived \342\234\223 %s", g_enh.arch_path); toast(m);
            fprintf(stderr,"[arch] saved -> %s\n", g_enh.arch_path);
        } else { toast("Archive SAVE FAILED \342\200\224 file kept in cache"); fprintf(stderr,"[arch] copy FAILED -> %s\n", g_enh.arch_path); }
        g_enh.is_arch=0; g_enh.arch_path[0]=0;
        fprintf(stderr,"[enh] done -> %s\n", g_enh.out);
        g_enh.out[0]=0;
        return;
    }
    if(ok){ double t; SDL_LockMutex(P.clock_mutex); t=P.audio_clock; SDL_UnlockMutex(P.clock_mutex);
        path_remove(g_enh.status_path);
        /* finalized now: reopen so the whole timeline is seekable. Live→resume where you're watching;
           offline (or live that finished before buffering)→resume at the original's position / top. */
        play_transient(g_enh.out, (was_live && was_playing) ? t : (was_live?0.0:t));
        g_playing_enhanced=1; P.paused=0; if(P.audio_dev) SDL_PauseAudioDevice(P.audio_dev,0);
        toast((was_live&&was_playing)?"Enhanced render complete":"AI enhanced \342\234\223 — playing the high-quality version");
        fprintf(stderr,"[enh] done -> %s\n", g_enh.out);
        g_enh.out[0]=0;   /* the finished file belongs to the CACHE now — cancel/quit must not delete it */ }
    else { char err[96]="";                                 /* surface the enhancer's own reason if it gave one */
        { FILE*ef=path_fopen(g_enh.status_path,L"rb");
          if(ef){ size_t n=fread(buf,1,sizeof buf-1,ef); buf[n]=0; fclose(ef); js_str(buf,"error",err,sizeof err); } }
        path_remove(g_enh.status_path); enh_delete_out();   /* a failed job's partial output is garbage */
        /* if we were watching a live render that died, fall back to the original at that spot */
        if(was_playing){ double t; SDL_LockMutex(P.clock_mutex); t=P.audio_clock; SDL_UnlockMutex(P.clock_mutex);
            const char*src=(pl_cur>=0&&pl_cur<pl_n)?pl_items[pl_cur]:0; if(src){ play_transient(src,t); g_playing_enhanced=0; } }
        if(g_enh.is_arch){ g_enh.is_arch=0; g_enh.arch_path[0]=0; }   /* archive: leave playback alone */
        else { P.paused=0; if(P.audio_dev) SDL_PauseAudioDevice(P.audio_dev,0); }
        toast(err[0]?err:"AI enhance failed (is the source a normal video?)");
        fprintf(stderr,"[enh] FAILED exit=%lu err=%s\n",(unsigned long)code,err); }
}
/* toggling U/G/T (or the combined E) calls this: play from the cache when we already made this
   version, chain frame-gen onto a cached upscale instead of re-upscaling, otherwise launch a fresh
   job; all off = back to the original. Toggles are sticky across playlist navigation. */
static void enhance_apply(void){
    if(pl_cur<0||pl_cur>=pl_n) return;
    if(enh_running() && g_enh.is_arch){ toast("Archive still rendering \342\200\224 Esc cancels it first"); return; }
    if(enh_running()) enhance_cancel();
    const char *src = pl_items[pl_cur];
    int up=P.upscale, fg=P.framegen, an=P.anime;
    if(!up && !fg && !an){
        if(g_playing_enhanced){
            double t; SDL_LockMutex(P.clock_mutex); t=P.audio_clock; SDL_UnlockMutex(P.clock_mutex);
            play_transient(src, t); g_playing_enhanced=0; toast("Back to original");
        }
        return;
    }
    /* PHASE 6: an already-high-res source gains nothing to enhance — a 4K source x4 = pointless 16K,
       and frame-gen at 4K is heavy/glitchy (owner opted to skip it). If EVERY requested op would be
       skipped by the enhancer, don't launch it at all — just play the original. Thresholds match the
       enhancer's --noupscale-above (1440) / --noframegen-above (1800). */
    {
        int up_eff = (up||an);   /* x1 = native-res RESTORE now, so upscale is always effective */
        int fg_eff = fg;   /* 4K frame-gen is back (RIFE at capped working res) */
        if((up||fg||an) && !up_eff && !fg_eff){
            if(g_playing_enhanced){ double t; SDL_LockMutex(P.clock_mutex); t=P.audio_clock; SDL_UnlockMutex(P.clock_mutex);
                play_transient(src,t); g_playing_enhanced=0; }
            toast("Source already high-res \342\200\224 no enhancement needed");
            return;
        }
    }
    char want[1040]; enh_cache_path(want,sizeof want,src,up,fg,an);
    if(file_exists_w(want)){                                /* cache hit: swap instantly, no processing */
        double t; SDL_LockMutex(P.clock_mutex); t=P.audio_clock; SDL_UnlockMutex(P.clock_mutex);
        enh_cache_touch(want);
        play_transient(want, t); g_playing_enhanced=1; P.paused=0;
        toast("AI enhanced (cached) \342\234\223");
        return;
    }
    if((up||an) && fg){                                     /* chain: frame-gen the cached upscale */
        char haveU[1040]; enh_cache_path(haveU,sizeof haveU,src,up,0,an);
        if(file_exists_w(haveU)){ enh_cache_touch(haveU); enhance_start(haveU,0,1,an,want); return; }
    }
    enhance_start(src,up||an,fg,an,want);
}

/* ================= ARCHIVE (W) =================
   The "keep it forever" remaster: enhance with the PLUS models (v1.43's Best-quality policy —
   RealESRGAN x2plus/x4plus, hours-long) and, when the render finishes, COPY the result to a
   user-chosen file instead of leaving it only in the LRU cache (which evicts / wipes on quit).
   The save-as dialog runs NOW (user present); the copy runs at completion (user may be away). */
/* load playlist row idx PAUSED, showing its first frame — for Archive straight from the playlist
   selection (2026-10-10: it used to demand "play a video first"). Exactly this row: no skipping to the
   next playable one, no sticky-enhance kick-off. 1 = loaded. */
static int pl_load_paused(int idx){
    if(idx<0 || idx>=pl_n) return 0;
    g_playing_enhanced = 0;
    close_media();
    P.paused = 1;                                   /* open_audio_stream opens the device paused */
    pl_cur = idx;
    if(load_media(pl_items[idx]) != 0){ fprintf(stderr,"[arch] cannot open: %s\n", pl_items[idx]); return 0; }
    P.paused = 1; if(P.audio_dev) SDL_PauseAudioDevice(P.audio_dev,1);
    g_orig_w=g_src_w; g_orig_h=g_src_h; g_orig_fps=g_src_fps;
    load_subtitles(pl_items[idx]);
    g_pl_jump = 1; g_sess_dirty = 1;
    g_stop_step = 1; g_stop_tries = 0;              /* decode + show the first frame while paused */
    return 1;
}
static void archive_start(void){
    if(enh_running()){ toast(g_enh.is_arch ? "An archive is already rendering \342\200\224 Esc cancels it"
                                           : "An enhance is already running \342\200\224 Esc cancels it"); return; }
    /* WHICH video: what is playing; otherwise (nothing loaded, or paused/stopped with a different row
       picked in the playlist) the SELECTED row — no need to press Play first. */
    int sel = (g_pl_selected>=0 && g_pl_selected<pl_n) ? g_pl_selected : -1;
    int use_sel = sel>=0 && (!P.fmt || pl_cur<0 || pl_cur>=pl_n || (sel!=pl_cur && (P.paused || g_stopped)));
    if(use_sel){
        if(media_kind(pl_items[sel]) != 0){ toast("Archive: select a video"); return; }
        if(!pl_load_paused(sel)){ toast("Archive: that video could not be opened"); return; }
    }
    if(pl_cur<0||pl_cur>=pl_n||!P.fmt||P.vstream<0||P.is_image){ toast("Archive: select a video in the playlist"); return; }
    /* hold playback while the save dialog is up (the main loop stops presenting under a modal dialog,
       so audio would run on under a frozen picture) — and an archive never resumes it by itself */
    int was_paused = P.paused;
    P.paused=1; if(P.audio_dev) SDL_PauseAudioDevice(P.audio_dev,1);
    const char *src = pl_items[pl_cur];
    /* suggest "<basename> [AI Archive].mkv" next to nothing in particular — user picks the folder */
    const char *bn = src; for(const char*p=src; *p; p++) if(*p=='\\'||*p=='/') bn=p+1;
    char suggest[560]; snprintf(suggest,sizeof suggest,"%s",bn);
    char *dot=strrchr(suggest,'.'); if(dot) *dot=0;
    char fname[1040]; snprintf(fname,sizeof fname,"%s [AI Archive].mkv",suggest);
    wchar_t wfname[1040]; path_u8_to_w(fname,wfname,1040);  /* seed the wide dialog with the suggestion */
    OPENFILENAMEW sfn; ZeroMemory(&sfn,sizeof sfn);
    sfn.lStructSize=sizeof sfn; sfn.hwndOwner=NULL;
    sfn.lpstrFilter=L"Matroska video (*.mkv)\0*.mkv\0All files\0*.*\0";
    sfn.lpstrFile=wfname; sfn.nMaxFile=1040;
    sfn.lpstrTitle=L"Archive enhanced video as…";
    sfn.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST;
    sfn.lpstrDefExt=L"mkv";
    const wchar_t *tout = _wgetenv(L"HOGS_ARCH_OUT");       /* test hook: skip the dialog, save here */
    if(tout && *tout){ wcsncpy(wfname, tout, 1039); wfname[1039]=0; }
    else if(!GetSaveFileNameW(&sfn)){                       /* user cancelled: put playback back */
        if(!was_paused){ P.paused=0; if(P.audio_dev) SDL_PauseAudioDevice(P.audio_dev,0); }
        return;
    }
    path_w_to_u8(wfname,fname,sizeof fname);                /* chosen path back to UTF-8 for storage/copy */
    g_arch_sel=1;                                           /* quality-x4 archive policy + "Q" cache tag */
    char want[1040]; enh_cache_path(want,sizeof want,src,1,1,0);
    if(file_exists_w(want)){                                /* already archived once: just copy */
        g_arch_sel=0;
        wchar_t wsrc[1100],wdst[1100]; path_u8_to_w(want,wsrc,1100); path_u8_to_w(fname,wdst,1100);
        if(CopyFileW(wsrc,wdst,FALSE)){ enh_cache_touch(want); toast("Archived from cache \342\234\223"); }
        else toast("Archive copy FAILED \342\200\224 check the destination");
        if(!was_paused){ P.paused=0; if(P.audio_dev) SDL_PauseAudioDevice(P.audio_dev,0); }   /* instant: nothing to wait for */
        return;
    }
    enhance_start(src,1,1,0,want);                          /* full remaster: upscale + frame-gen */
    g_arch_sel=0;
    if(g_enh.state==ENH_RUN){                               /* started: remember where to save it */
        g_enh.is_arch=1; snprintf(g_enh.arch_path,sizeof g_enh.arch_path,"%s",fname);
        toast("Archive render started (quality x4, slow) \342\200\224 saves when finished");
    }
}

/* ===== THUMBNAIL MODE: `hogofwar.exe --thumb <file> <size>` (2026-10-27) =====
   Called by hogs_thumb.dll (Explorer's thumbnail provider). Windows can't thumbnail HEVC/x265, RMVB, VOB...;
   this grabs a frame with our FFmpeg and writes "HTB1" + int32 w + int32 h + w*h BGRA to stdout, then exits —
   no window, no audio, no log. Decoding runs HERE, in a throwaway process, so a hostile/broken file can never
   crash Explorer (the DLL inside Explorer only spawns this and reads the pipe). Picks the first frame that
   isn't near-black from 12% / 25% / 40% in (past intros), honouring pixel aspect. */
static int thumb_bright(const AVFrame *f){
    if(f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P && f->format != AV_PIX_FMT_YUV422P &&
       f->format != AV_PIX_FMT_YUV444P && f->format != AV_PIX_FMT_NV12) return 255;   /* can't cheaply tell: accept */
    long long sum = 0; int n = 0;
    for(int y = 0; y < f->height; y += 8) for(int x = 0; x < f->width; x += 8){ sum += f->data[0][y*f->linesize[0] + x]; n++; }
    return n ? (int)(sum / n) : 0;
}
static int thumb_main(const char *path, int size){
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);  /* never a crash dialog */
    av_log_set_level(AV_LOG_QUIET);
    if(size < 16) size = 16; if(size > 1024) size = 1024;
    AVFormatContext *fmt = NULL; AVCodecContext *dc = NULL; AVPacket *pk = NULL; AVFrame *fr = NULL, *best = NULL;
    struct SwsContext *sws = NULL; uint8_t *px = NULL; int rc = 1;
    if(avformat_open_input(&fmt, path, NULL, NULL) < 0) return 1;
    if(avformat_find_stream_info(fmt, NULL) < 0) goto done;
    const AVCodec *codec = NULL;
    int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if(vs < 0 || !codec) goto done;
    AVStream *s = fmt->streams[vs];
    if(!(dc = avcodec_alloc_context3(codec)) || avcodec_parameters_to_context(dc, s->codecpar) < 0) goto done;
    dc->thread_count = 2;
    if(avcodec_open2(dc, codec, NULL) < 0) goto done;
    pk = av_packet_alloc(); fr = av_frame_alloc(); best = av_frame_alloc(); if(!pk || !fr || !best) goto done;
    double dur = fmt->duration > 0 ? fmt->duration / (double)AV_TIME_BASE : 0;
    { double fx = ps_duration_fix(fmt, path); if(fx > 0) dur = fx; }   /* VOBs whose timestamps restart */
    int best_b = -1;
    static const double POS[] = { 0.12, 0.25, 0.40, 0.0 };
    for(int k = 0; k < 4 && best_b < 40; k++){
        if(dur > 4){ double t = POS[k] * dur; if(t > 600) t = 600 + k * 60;
            if(av_seek_frame(fmt, -1, (int64_t)(t * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD) < 0 && k) continue;
            avcodec_flush_buffers(dc); }
        else if(k) break;
        int got = 0;
        for(int n = 0; n < 1500 && !got; n++){
            int r = av_read_frame(fmt, pk);
            if(r < 0) avcodec_send_packet(dc, NULL);
            else if(pk->stream_index != vs){ av_packet_unref(pk); continue; }
            else { avcodec_send_packet(dc, pk); av_packet_unref(pk); }
            if(avcodec_receive_frame(dc, fr) == 0) got = 1;
            else if(r < 0) break;
        }
        if(!got) continue;
        int b = thumb_bright(fr);
        if(b > best_b){ AVFrame *t = best; best = fr; fr = t; best_b = b; }
        av_frame_unref(fr);
    }
    if(best_b < 0) goto done;
    AVRational sar = av_guess_sample_aspect_ratio(fmt, s, best);
    double ar = (double)best->width / best->height;
    if(sar.num > 0 && sar.den > 0) ar *= (double)sar.num / sar.den;
    int tw, th;
    if(ar >= 1){ tw = size; th = (int)(size / ar + 0.5); } else { th = size; tw = (int)(size * ar + 0.5); }
    if(tw < 1) tw = 1; if(th < 1) th = 1;
    sws = sws_getContext(best->width, best->height, best->format, tw, th, AV_PIX_FMT_BGRA, SWS_BICUBIC, NULL, NULL, NULL);
    if(!sws || !(px = malloc((size_t)tw * th * 4))) goto done;
    uint8_t *dst[4] = { px, 0, 0, 0 }; int dls[4] = { tw * 4, 0, 0, 0 };
    sws_scale(sws, (const uint8_t * const *)best->data, best->linesize, 0, best->height, dst, dls);
    { HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE); DWORD wr; int32_t hdr[3]; memcpy(hdr, "HTB1", 4); hdr[1] = tw; hdr[2] = th;
      size_t tot = (size_t)tw * th * 4, off = 0;
      if(o && o != INVALID_HANDLE_VALUE && WriteFile(o, hdr, sizeof hdr, &wr, NULL)){
          while(off < tot && WriteFile(o, px + off, (DWORD)(tot - off > 65536 ? 65536 : tot - off), &wr, NULL) && wr) off += wr;
          if(off == tot) rc = 0; } }
done:
    free(px); if(sws) sws_freeContext(sws);
    av_frame_free(&fr); av_frame_free(&best); av_packet_free(&pk); avcodec_free_context(&dc);
    if(fmt) avformat_close_input(&fmt);
    return rc;
}

/* ===== EXPLORER INTEGRATION, per user (2026-10-27) =====
   Every person who runs the player gets — in THEIR OWN HKCU, no admin — the hog logo + a proper name on the
   "Applications\hogofwar.exe" association (what Windows uses once Hogs is the chosen default app), and the
   hogs_thumb.dll thumbnail provider (real picture thumbnails for HEVC/RMVB/VOB that Windows can't do).
   Rewritten only when missing or stale (player folder moved), so normal launches touch nothing.
   DisableProcessIsolation=1: Windows' isolated thumbnail host ignores per-user COM classes (measured
   REGDB_E_CLASSNOTREG); the DLL is safe in-proc because all decoding happens in a `--thumb` child process.
   Undo: unregister_thumbnails.ps1. */
static int reg_str(HKEY root, const wchar_t *key, const wchar_t *name, const wchar_t *val){
    HKEY h; if(RegCreateKeyExW(root, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &h, NULL) != ERROR_SUCCESS) return 0;
    LONG r = RegSetValueExW(h, name, 0, REG_SZ, (const BYTE*)val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
    RegCloseKey(h); return r == ERROR_SUCCESS;
}
static void shell_integration_ensure(void){
    wchar_t self[MAX_PATH]; DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH); if(!n || n >= MAX_PATH) return;
    const wchar_t *bn = self; for(const wchar_t *q = self; *q; q++) if(*q == L'\\') bn = q + 1;
    if(_wcsicmp(bn, L"hogofwar.exe")) return;                 /* test builds never re-point the user's setup */
    wchar_t dir[MAX_PATH]; wcsncpy(dir, self, (size_t)(bn - self)); dir[bn - self] = 0;
    wchar_t dll[MAX_PATH + 32], icon[MAX_PATH + 8], cmd[MAX_PATH + 16];
    _snwprintf(dll, MAX_PATH + 32, L"%lshogs_thumb.dll", dir);
    if(GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) return;
    _snwprintf(icon, MAX_PATH + 8, L"\"%ls\",0", self);
    _snwprintf(cmd, MAX_PATH + 16, L"\"%ls\" \"%%1\"", self);
    const wchar_t *CK = L"Software\\Classes\\CLSID\\{26687DFC-63C4-4F37-8139-5E958C287DB4}";
    const wchar_t *AK = L"Software\\Classes\\Applications\\hogofwar.exe";
    const wchar_t *THK = L"{e357fccd-a995-4576-b01f-234630154e96}";
    wchar_t cur[MAX_PATH + 32] = L"", curi[MAX_PATH + 8] = L""; DWORD cb = sizeof cur, cbi = sizeof curi;
    wchar_t k[300];
    _snwprintf(k, 300, L"%ls\\InprocServer32", CK);
    RegGetValueW(HKEY_CURRENT_USER, k, NULL, RRF_RT_REG_SZ, NULL, cur, &cb);
    _snwprintf(k, 300, L"%ls\\DefaultIcon", AK);
    RegGetValueW(HKEY_CURRENT_USER, k, NULL, RRF_RT_REG_SZ, NULL, curi, &cbi);
    if(!_wcsicmp(cur, dll) && !_wcsicmp(curi, icon)) return;   /* already set up for this folder */
    int ok = 1;
    ok &= reg_str(HKEY_CURRENT_USER, CK, NULL, L"Hogs media player thumbnails");
    { DWORD one = 1; HKEY h; if(RegCreateKeyExW(HKEY_CURRENT_USER, CK, 0, NULL, 0, KEY_SET_VALUE, NULL, &h, NULL) == ERROR_SUCCESS){
        ok &= RegSetValueExW(h, L"DisableProcessIsolation", 0, REG_DWORD, (const BYTE*)&one, sizeof one) == ERROR_SUCCESS; RegCloseKey(h); } else ok = 0; }
    _snwprintf(k, 300, L"%ls\\InprocServer32", CK);
    ok &= reg_str(HKEY_CURRENT_USER, k, NULL, dll) & reg_str(HKEY_CURRENT_USER, k, L"ThreadingModel", L"Apartment");
    ok &= reg_str(HKEY_CURRENT_USER, AK, L"FriendlyAppName", L"Hogs media player");
    _snwprintf(k, 300, L"%ls\\DefaultIcon", AK);           ok &= reg_str(HKEY_CURRENT_USER, k, NULL, icon);
    _snwprintf(k, 300, L"%ls\\shell\\open\\command", AK);  ok &= reg_str(HKEY_CURRENT_USER, k, NULL, cmd);
    _snwprintf(k, 300, L"%ls\\ShellEx\\%ls", AK, THK);      ok &= reg_str(HKEY_CURRENT_USER, k, NULL, L"{26687DFC-63C4-4F37-8139-5E958C287DB4}");
    static const wchar_t *VID[] = { L".mp4",L".mkv",L".avi",L".mov",L".wmv",L".webm",L".m4v",L".flv",L".mpg",L".mpeg",L".ts",
        L".m2ts",L".mts",L".vob",L".rmvb",L".rm",L".ogm",L".ogv",L".3gp",L".divx",L".f4v",L".asf",L".m2t",0 };
    _snwprintf(k, 300, L"%ls\\SupportedTypes", AK);
    for(int i = 0; VID[i]; i++) reg_str(HKEY_CURRENT_USER, k, VID[i], L"");
    /* types Windows has NO thumbnailer for at all get ours too — only where nothing is registered */
    static const wchar_t *NOTH[] = { L".rmvb",L".rm",L".vob",L".flv",L".ogm",L".ogv",L".divx",L".f4v",L".m2t",0 };
    for(int i = 0; NOTH[i]; i++){
        wchar_t ek[128], have[64] = L""; DWORD hb = sizeof have;
        _snwprintf(ek, 128, L"%ls\\ShellEx\\%ls", NOTH[i], THK);
        if(RegGetValueW(HKEY_CLASSES_ROOT, ek, NULL, RRF_RT_REG_SZ, NULL, have, &hb) == ERROR_SUCCESS) continue;
        _snwprintf(ek, 128, L"Software\\Classes\\%ls\\ShellEx\\%ls", NOTH[i], THK);
        reg_str(HKEY_CURRENT_USER, ek, NULL, L"{26687DFC-63C4-4F37-8139-5E958C287DB4}");
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);   /* refresh icons now */
    fprintf(stderr,"[shell] per-user Explorer integration %s (icon + thumbnails -> %ls)\n", ok ? "registered" : "PARTLY failed", dir);
}

int main(int argc, char **argv){
    { int wc = 0; LPWSTR *wv = CommandLineToArgvW(GetCommandLineW(), &wc);   /* before ANY player setup */
      if(wv && wc >= 4 && !wcscmp(wv[1], L"--thumb")){
          char p[2048]; path_w_to_u8(wv[2], p, sizeof p); int sz = _wtoi(wv[3]); LocalFree(wv);
          return thumb_main(p, sz); }
      if(wv) LocalFree(wv); }
    SDL_SetMainReady();
    /* GUI app (-mwindows) has no console. Logs go to a TEMP file (never the player folder) and
       are DELETED on quit — nothing is left on disk. Kept during the session for crash diagnosis. */
    {
        /* GetTempPathW, not …A: a Unicode USERNAME (C:\Users\Wörner\…\Temp) would mangle the ANSI
           form and the log would fail to open or land in the wrong place. */
        wchar_t wtmp[800]; DWORD tn = GetTempPathW(800, wtmp); char tmp[1600]; if(tn) path_w_to_u8(wtmp,tmp,sizeof tmp); else tmp[0]=0;
        /* HOGS_LOG=<path> writes the log there and KEEPS it on quit — the temp log is deleted on
           exit by design, which makes it useless for reporting a bug you only notice at the end.
           _wgetenv so a Unicode HOGS_LOG path survives too. */
        const wchar_t *wlp = _wgetenv(L"HOGS_LOG");
        if(wlp && *wlp){ path_w_to_u8(wlp, g_log_path, sizeof g_log_path); g_log_keep = 1; }
        else snprintf(g_log_path, sizeof g_log_path, "%shogofwar_log.txt", tmp);
        { wchar_t wlog[2200]; path_u8_to_w(g_log_path, wlog, 2200);   /* wide reopen: Unicode temp/log path */
          _wfreopen(wlog, L"w", stderr); _wfreopen(wlog, L"a", stdout); }
        setvbuf(stderr, NULL, _IONBF, 0);  /* unbuffered so the log is current even on crash */
        fprintf(stderr, "[hogofwar] ultimate media player (native) — log start\n");
    }
    SetUnhandledExceptionFilter(hogs_crash_handler);   /* log where a crash happens */
    if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_TIMER) < 0){ fprintf(stderr,"SDL: %s\n", SDL_GetError()); return 1; }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);   /* for the native folder browser dialog */
    /* Multi-monitor fullscreen fix: by default an SDL fullscreen window MINIMIZES when it
       loses focus — on a multi-monitor setup, clicking the other screen made this window go
       black/minimise repeatedly. Keep it up instead. Also stop the OS blanking the display
       during long playback (we don't generate input events while a movie plays). */
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    SDL_DisableScreenSaver();

    memset(&P, 0, sizeof(P));
    P.volume = 0.8; P.clock_mutex = SDL_CreateMutex();
    pq_init(&P.vq); pq_init(&P.aq); pq_init(&P.sq);
    P.aframe = av_frame_alloc(); P.apkt_tmp = av_packet_alloc(); vframe = av_frame_alloc();

    /* request a GL 3.3 core context BEFORE creating the window */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    /* No ALLOW_HIGHDPI: keeps mouse coords == drawable coords so OSD hit-testing is 1:1 */
    P.win = SDL_CreateWindow("Hogs media player V1.45",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
        SDL_WINDOW_RESIZABLE|SDL_WINDOW_OPENGL);
    if(P.win && getenv("HOGS_WINPOS")){ int x=0,y=0; if(sscanf(getenv("HOGS_WINPOS"),"%d,%d",&x,&y)==2) SDL_SetWindowPosition(P.win,x,y); }  /* test: pick a monitor */

    /* Try the OpenGL output path. If anything fails,
       fall back to the SDL_Renderer path so playback still works. */
    if(gl_init()){
        use_gl = 1;
        const char *ver = (const char*)glGetString(GL_VERSION);
        const char *ren = (const char*)glGetString(GL_RENDERER);
        fprintf(stderr, "[GL] OpenGL output active — %s | %s\n", ver?ver:"?", ren?ren:"?");
    } else {
        if(gl_ctx){ SDL_GL_DeleteContext(gl_ctx); gl_ctx = NULL; }
        P.ren = SDL_CreateRenderer(P.win, -1, SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
        if(!P.ren) P.ren = SDL_CreateRenderer(P.win, -1, 0);
        fprintf(stderr, "[GL] falling back to SDL_Renderer (no shaders)\n");
    }

    update_monitor_mode();
    fprintf(stderr, "[monitor] native %dx%d @ %d Hz — upscale + frame-gen target\n", g_mon_w, g_mon_h, g_mon_hz);
    /* The lossless FFv1 decoder the user asked for is built into the bundled libavcodec
       (same library family HandBrake ships); confirm it resolved so FFv1 .mkv/.avi/.mov/.nut play. */
    { const AVCodec *ffv1 = avcodec_find_decoder(AV_CODEC_ID_FFV1);
      fprintf(stderr, "[codec] lossless FFv1 decoder: %s\n", ffv1 ? "available" : "NOT AVAILABLE"); }

    SDL_StartTextInput();   /* keep text input live so the playlist search box receives SDL_TEXTINPUT */

    P.vstream = P.astream = -1;
    if(argc <= 1) usage();
    /* The CRT's argv is ANSI (system codepage) — Unicode paths on the command line (drag-and-drop,
       "Open with", accented folder names) arrive mangled and won't open. Re-derive the arguments
       from the WIDE command line and convert to UTF-8, which is what everything downstream expects. */
    /* SESSION: last run's playlist (order, selected row, media-type chip) and the pinned exclude words come
       back first, so reopening the player is zero clicks back to where you were. Files passed on the command
       line are then merged in WITHOUT duplicating rows that the session already has. */
    g_nosession = getenv("HOGS_NOSESSION") != NULL;
    if(!g_nosession) shell_integration_ensure();   /* this user's Explorer icon + thumbnails (no-op when current) */
    /* OPENED WITH FILES (double-click in Explorer / "Open with", 2026-10-27): the user wants JUST what
       they opened — not their whole saved 11k-row session with it. Such a launch is a temporary playlist:
       the saved session is neither loaded nor overwritten. Hide-words + search text still load and save
       (2026-10-28), and the opened rows are exempt from them (pl_add_exempt) so the file you double-clicked
       is never hidden. */
    { int wargc=0; LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
      if(wargv){ if(wargc > 1){ g_sess_temp = 1; fprintf(stderr,"[session] opened with %d file(s): temporary playlist, saved session untouched (hide-words + search still apply)\n", wargc-1); }
                 LocalFree(wargv); } }
    excl_load();
    srch_text_load();                      /* the search box as it was left (typed -words included) */
    sess_load();
    int arg_first = -1;
    { int wargc=0; LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
      g_pl_add_exempt = g_sess_temp;       /* what you opened is shown even if a hide-word matches it */
      if(wargv){ for(int i=1;i<wargc;i++){ char a[2048]; path_w_to_u8(wargv[i],a,sizeof(a)); int f=add_path_recursive(a); if(arg_first<0) arg_first=f; } LocalFree(wargv); }
      else for(int i=1;i<argc;i++){ int f=add_path_recursive(argv[i]); if(arg_first<0) arg_first=f; }   /* fallback: raw argv */
      g_pl_add_exempt = 0; }
    /* nothing auto-plays: media loads SELECTED and waits for Play/Space (user request) */
    theme_load();                          /* restore the OSD theme chosen last run */
    if(getenv("HOGS_VIZ")) g_viz_mode = atoi(getenv("HOGS_VIZ")) & 3;   /* test: force visualizer mode */
    show_playlist = 1;                     /* always up on load; hides once playback starts */
    if(arg_first >= 0){ g_pl_selected = arg_first; g_pl_jump_sel = 1; toast("Media loaded — press Space or Play"); }
    else if(g_pl_selected >= 0 && g_pl_selected < pl_n){
        char m[120]; snprintf(m,sizeof m,"Welcome back \342\200\224 %d item%s, last pick selected (Space plays)",pl_n,pl_n==1?"":"s"); toast(m); }
    else if(pl_n > 0){ g_pl_selected = 0; toast("Media loaded — press Space or Play"); }
    if(pl_sel && g_pl_selected >= 0 && g_pl_selected < pl_n) pl_sel[g_pl_selected] = 1;
    int start_idx = (g_pl_selected >= 0 && g_pl_selected < pl_n) ? g_pl_selected : 0;
    { int vis=0; for(int i=0;i<pl_n;i++) if(pl_match(i)) vis++;
      fprintf(stderr,"[playlist] %d rows, %d visible after filters, %d exclude word(s), selected=%d\n",pl_n,vis,g_excl_n,g_pl_selected); }
    /* test hook: HOGS_AUTOENHANCE=1 auto-plays the first item and triggers a live E-enhance on start
       (so the play-while-enhance path can be exercised without synthetic keypresses). */
    if(getenv("HOGS_AUTOENHANCE") && pl_n > 0){ pl_play_at(start_idx); P.upscale=1; P.framegen=1; enhance_apply(); }
    /* test hook: HOGS_AUTOPLAY=1 plays the first item, and HOGS_AUTOSTOP_MS=<n> fires stop_playback()
       n ms later — the transport/clock paths are otherwise unreachable headlessly, because Windows
       refuses synthetic keystrokes into a background OpenGL window. */
    if(getenv("HOGS_AUTOPLAY") && pl_n > 0) pl_play_at(start_idx);
    const char *autostop = getenv("HOGS_AUTOSTOP_MS");
    Uint32 autostop_at = autostop ? SDL_GetTicks() + (Uint32)atoi(autostop) : 0;
    /* test hook: HOGS_AUTOSEEK_MS=<n> HOGS_AUTOSEEK_FRAC=<0..1> performs the same absolute seek the
       seek-bar click does, so post-seek A/V sync can be measured headlessly. */
    const char *autoseek = getenv("HOGS_AUTOSEEK_MS");
    Uint32 autoseek_at = autoseek ? SDL_GetTicks() + (Uint32)atoi(autoseek) : 0;
    double autoseek_frac = getenv("HOGS_AUTOSEEK_FRAC") ? atof(getenv("HOGS_AUTOSEEK_FRAC")) : 0.1;
    int autoseek_n = getenv("HOGS_AUTOSEEK_N") ? atoi(getenv("HOGS_AUTOSEEK_N")) : 1, autoseek_n_done = 0;

    Uint32 last_title = 0, eof_since = 0;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main_thread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    { SDL_Thread *hw = SDL_CreateThread(hang_watch_thread, "hangwatch", NULL); if(hw) SDL_DetachThread(hw); }
    while(!P.quit){
        g_heartbeat = SDL_GetTicks(); if(!g_heartbeat) g_heartbeat = 1;
        /* test hook: HOGS_AUTONEXT="<ms>,<count>" presses N (next) every <ms>, <count> times */
        { static int an_left = -1; static Uint32 an_ms = 0, an_at = 0;
          if(an_left < 0){ const char *e = getenv("HOGS_AUTONEXT"); unsigned a=0,b=0; if(e) sscanf(e,"%u,%u",&a,&b);
              an_ms = a; an_left = (int)b; an_at = SDL_GetTicks() + 3000; }
          if(an_left > 0 && SDL_GetTicks() >= an_at){ an_left--; an_at = SDL_GetTicks() + an_ms;
              fprintf(stderr,"[test] AUTONEXT (%d left)\n", an_left); pl_next(); } }
        /* test hooks: HOGS_SHOWKEYS=1 opens the hotkeys panel; HOGS_DONATE_TEST=1 presses Copy at 3s */
        { static int sk = -1, dt = -1;
          if(sk < 0){ sk = getenv("HOGS_SHOWKEYS") ? 1 : 0; if(sk) g_show_keys = 1; }
          if(dt < 0) dt = getenv("HOGS_DONATE_TEST") ? 1 : 0;
          if(dt == 1 && SDL_GetTicks() > 3000){ dt = 0; donate_copy(); fprintf(stderr,"[test] DONATE copy -> %s\n", HOGS_BTC); } }
        /* test hook: HOGS_TESTHANG_MS=<n> blocks the main thread for 6s at n ms (proves the [HANG] dump) */
        { static int th = -1; if(th < 0){ const char *e = getenv("HOGS_TESTHANG_MS"); th = e ? atoi(e) : 0; }
          if(th > 0 && SDL_GetTicks() >= (Uint32)th){ th = 0; g_stage = "test hang (SDL_Delay)"; SDL_Delay(6000); g_stage = "main loop"; } }
        pl_busy_poll();          /* apply a finished background folder-scan / length-probe on the main thread */
        srch_text_autosave();    /* search box text -> hogs_search.txt once typing pauses */
        if(g_sess_dirty && !g_busy && SDL_GetTicks()-g_sess_saved_at > 8000) sess_save_async();   /* crash-safe session */
        SDL_Event e;
        while(SDL_PollEvent(&e)){
            if(e.type == SDL_QUIT) P.quit=1;
            else if(e.type == SDL_DROPFILE){
                char *f = e.drop.file;
                int before = pl_n;
                add_path_recursive(f);      /* file → add; folder → expand all subfolders */
                SDL_free(f);
                /* no autoplay: select the first new item; Space/Play starts it */
                if(pl_n > before && pl_cur < 0){ g_pl_selected = before; toast("Added — press Space or Play"); }
            }
            else if(e.type == SDL_KEYDOWN){
                if(g_pl_search_active){   /* typing in the playlist search box — shortcuts are suspended */
                    SDL_Keycode k=e.key.keysym.sym; SDL_Keymod km=SDL_GetModState();
                    int sh=(km&KMOD_SHIFT)!=0, ct=(km&KMOD_CTRL)!=0;
                    if(g_pl_caret > g_pl_search_len) g_pl_caret = g_pl_search_len;
                    if(g_pl_anchor > g_pl_search_len || g_pl_anchor < 0) g_pl_anchor = g_pl_caret;
                    if(ct && k==SDLK_a){ g_pl_anchor=0; g_pl_caret=g_pl_search_len; }          /* select all text */
                    else if(ct && (k==SDLK_c || k==SDLK_x)){                                   /* copy / cut */
                        if(srch_has_sel()){ int a,b; srch_sel_range(&a,&b); char t[sizeof g_pl_search];
                            memcpy(t,g_pl_search+a,(size_t)(b-a)); t[b-a]=0; SDL_SetClipboardText(t);
                            if(k==SDLK_x) srch_del_sel(); } }
                    else if(ct && k==SDLK_v){                                                  /* paste (one line) */
                        char *t=SDL_GetClipboardText();
                        if(t){ for(char *q=t;*q;q++) if(*q=='\r'||*q=='\n'||*q=='\t') *q=' '; srch_insert(t); SDL_free(t); } }
                    else if(k==SDLK_BACKSPACE) srch_erase(1);
                    else if(k==SDLK_DELETE)    srch_erase(0);
                    else if(k==SDLK_LEFT){
                        if(!sh && srch_has_sel()){ int a,b; srch_sel_range(&a,&b); g_pl_caret=a; }
                        else g_pl_caret = u8_prev_at(g_pl_search, g_pl_caret);
                        if(!sh) g_pl_anchor=g_pl_caret; }
                    else if(k==SDLK_RIGHT){
                        if(!sh && srch_has_sel()){ int a,b; srch_sel_range(&a,&b); g_pl_caret=b; }
                        else g_pl_caret = u8_next_at(g_pl_search, g_pl_caret, g_pl_search_len);
                        if(!sh) g_pl_anchor=g_pl_caret; }
                    else if(k==SDLK_HOME){ g_pl_caret = 0;                if(!sh) g_pl_anchor=g_pl_caret; }
                    else if(k==SDLK_END){  g_pl_caret = g_pl_search_len;  if(!sh) g_pl_anchor=g_pl_caret; }
                    else if(k==SDLK_RETURN||k==SDLK_KP_ENTER){
                        /* Enter pins any -words as persistent exclude chips; otherwise it plays the first match */
                        if(!srch_pin_negatives()){ for(int i=0;i<pl_n;i++) if(pl_match(i)){ g_pl_selected=i; pl_play_at(i); break; } } }
                    else if(k==SDLK_ESCAPE){ g_pl_search_active=0; g_pl_anchor=g_pl_caret; }
                    osd_visible=1; osd_last_active=SDL_GetTicks();
                } else if((SDL_GetModState()&KMOD_CTRL) && (e.key.keysym.sym==SDLK_a || e.key.keysym.sym==SDLK_f ||
                                                             e.key.keysym.sym==SDLK_s || e.key.keysym.sym==SDLK_o ||
                                                             e.key.keysym.sym==SDLK_b)){
                    SDL_Keycode k=e.key.keysym.sym;
                    if(k==SDLK_a && pl_sel && !g_busy){         /* Ctrl+A: select every row currently shown */
                        int n=0, first=-1;
                        for(int i=0;i<pl_n;i++){ int m=pl_match(i); pl_sel[i]=(unsigned char)m; if(m){ n++; if(first<0) first=i; } }
                        if(first>=0) g_pl_selected=first;
                        show_playlist=1;
                        char msg[96]; snprintf(msg,sizeof msg,"Selected %d item%s  (Del removes \302\267 drag moves)",n,n==1?"":"s"); toast(msg);
                    }
                    else if(k==SDLK_f){ show_playlist=1; g_pl_search_active=1; g_pl_anchor=0; g_pl_caret=g_pl_search_len; }  /* Ctrl+F: search */
                    else if(k==SDLK_s){ if(!g_busy) pl_save_dialog(); }   /* Ctrl+S: save playlist */
                    else if(k==SDLK_o){ if(!g_busy) pl_load_dialog(); }   /* Ctrl+O: open playlist */
                    else if(k==SDLK_b){ donate_copy(); }                     /* Ctrl+B: copy the Bitcoin donation address */
                    osd_visible=1; osd_last_active=SDL_GetTicks();
                } else switch(e.key.keysym.sym){
                    case SDLK_ESCAPE:{   /* Esc — cancel a running enhance, else leave fullscreen / close overlays */
                        if(enh_running()) enhance_cancel();
                        else if(is_fullscreen()) toggle_fullscreen();
                        else if(g_ctx_open) g_ctx_open=0;
                        else if(g_show_keys) g_show_keys=0;
                        else if(g_show_stats) g_show_stats=0;
                        else if(show_playlist) show_playlist=0;
                    } break;
                    case SDLK_SPACE:
                        if(!P.fmt && pl_n>0){ int s=g_pl_selected>=0?g_pl_selected:0; pl_play_at(s); } /* idle: play the selection */
                        else { P.paused=!P.paused; g_stopped=0; if(P.audio_dev) SDL_PauseAudioDevice(P.audio_dev,P.paused); }
                        break;
                    case SDLK_s: stop_playback(); break;
                    case SDLK_y: theme_cycle((SDL_GetModState()&KMOD_SHIFT)?-1:+1); break;  /* Shift+Y = back */
                    case SDLK_m: P.muted=!P.muted; break;
                    case SDLK_c: P.fill=!P.fill; break;            /* crop-to-fill / letterbox */
                    case SDLK_f: toggle_fullscreen(); break;
                    case SDLK_LEFT:  P.seek_req=-10; P.seek_flag=1; g_stopped=0; break;
                    case SDLK_RIGHT:
                        if(g_enh.live && g_enh.playing){   /* live: past the render head → re-anchor there */
                            double c; SDL_LockMutex(P.clock_mutex); c=P.audio_clock; SDL_UnlockMutex(P.clock_mutex);
                            if(c+10 > g_enh.rendered-0.3){ enhance_reanchor(c+10); break; }
                        }
                        P.seek_req=+10; P.seek_flag=1; g_stopped=0; break;
                    case SDLK_UP:    P.volume=P.volume<VOL_MAX-0.05?P.volume+0.05:VOL_MAX;
                        { char vb[48]; snprintf(vb,sizeof vb,"Volume %d%%%s",(int)(P.volume*100+0.5),P.volume>1.0?"  (boost)":""); toast(vb); } break;
                    case SDLK_DOWN:  P.volume=P.volume>0.05?P.volume-0.05:0.0;
                        { char vb[48]; snprintf(vb,sizeof vb,"Volume %d%%%s",(int)(P.volume*100+0.5),P.volume>1.0?"  (boost)":""); toast(vb); } break;
                    case SDLK_n: pl_next(); break;
                    case SDLK_p: pl_prev(); break;
                    case SDLK_r: g_repeat = !g_repeat; break;
                    case SDLK_h: g_shuffle = !g_shuffle; if(g_shuffle) g_bag_valid=0; break; /* fresh bag */
                    case SDLK_u: P.upscale=!P.upscale; if(P.upscale)P.anime=0; enhance_apply(); break; /* ✨ AI Upscale (real footage) */
                    case SDLK_g: P.framegen=!P.framegen; enhance_apply(); break; /* 🎞 AI Frame-Gen (RIFE, whole video) */
                    case SDLK_e: { int on=!(P.upscale&&P.framegen);              /* ⚡ both in one click */
                                   P.upscale=P.framegen=on; if(on)P.anime=0; enhance_apply(); } break;
                    case SDLK_t: P.anime=!P.anime; if(P.anime)P.upscale=0; enhance_apply(); break; /* 🎨 AnimeUpscale (toon model) */
                    case SDLK_d: P.light=!P.light; toast(P.light?"Light Enhancement ON (shader)":"Light Enhancement off"); break; /* 💡 v141 trio */
                    case SDLK_w: archive_start(); break;                                           /* 📼 Archive remaster */
                    case SDLK_l: show_playlist=!show_playlist; osd_visible=1; osd_last_active=SDL_GetTicks(); break;
                    case SDLK_a: switch_audio_track(+1); break;   /* cycle audio track */
                    case SDLK_b: g_subs_enabled=!g_subs_enabled; break; /* subtitles on/off */
                    case SDLK_v: g_viz_mode=(g_viz_mode+1)%4; break; /* bars/wave/ripple/matrix */
                    case SDLK_i: g_show_stats=!g_show_stats; osd_visible=1; osd_last_active=SDL_GetTicks(); break;
                    case SDLK_k: case SDLK_SLASH: case SDLK_F1:
                        g_show_keys=!g_show_keys; osd_visible=1; osd_last_active=SDL_GetTicks(); break;
                    case SDLK_DELETE: if(!g_busy) {         /* remove ALL selected rows (or the anchor) */
                        int any=0;
                        if(pl_sel) for(int i=pl_n-1;i>=0;i--) if(pl_sel[i]){ pl_remove(i); any=1; }
                        if(!any && g_pl_selected>=0) pl_remove(g_pl_selected);
                        g_pl_selected=-1;
                    } break;
                }
            }
            else if(e.type == SDL_TEXTINPUT){
                if(g_pl_search_active){
                    if(g_pl_caret > g_pl_search_len) g_pl_caret = g_pl_search_len;
                    srch_insert(e.text.text);          /* at the caret, not the end */
                    osd_visible=1; osd_last_active=SDL_GetTicks();
                }
            }
            else if(e.type == SDL_MOUSEMOTION){
                g_mouse_x = e.motion.x; g_mouse_y = e.motion.y;
                if(g_pl_resizing){   /* dragging the playlist's left edge → set its width */
                    g_pl_w = g_win_w - e.motion.x;
                    if(g_pl_w < 300) g_pl_w = 300; if(g_pl_w > g_win_w*0.92f) g_pl_w = g_win_w*0.92f;
                }
                if(g_pl_sb_drag && pl_sb_y1 > pl_sb_y0){   /* dragging the playlist scrollbar thumb */
                    float f = ((float)e.motion.y - pl_sb_y0) / (pl_sb_y1 - pl_sb_y0);
                    if(f<0)f=0; if(f>1)f=1;
                    pl_scroll = f * (g_pl_rows_tot - g_pl_rows_vis);   /* clamped in the draw pass */
                }
                if(g_srch_drag && (e.motion.state & SDL_BUTTON_LMASK))   /* drag-select text in the search box */
                    g_pl_caret = srch_caret_at_x((float)e.motion.x);
                /* a held-left-button move past a small threshold on an armed row starts a reorder drag */
                if(g_pl_drag_idx>=0 && !g_pl_dragging && (e.motion.state & SDL_BUTTON_LMASK) &&
                   fabsf((float)e.motion.y - g_pl_drag_y0) > 6.0f) g_pl_dragging=1;
                osd_visible = 1; osd_last_active = SDL_GetTicks();
            }
            else if(e.type == SDL_MOUSEBUTTONUP){
                if(!g_busy && g_pl_dragging && g_pl_drag_idx>=0 && g_pl_drop_at>=0){
                    /* dragging a row that's part of a multi-selection moves the WHOLE selection; otherwise
                       just the one row (its selection was set on mouse-down). */
                    if(pl_sel && pl_sel[g_pl_drag_idx] && pl_selcount()>1) pl_move_selected(g_pl_drop_at);
                    else pl_move(g_pl_drag_idx, g_pl_drop_at);
                }
                else if(g_pl_click_pending>=0 && pl_sel){   /* plain click on a selection, no drag → collapse to single */
                    memset(pl_sel,0,(size_t)pl_n); pl_sel[g_pl_click_pending]=1; g_pl_selected=g_pl_click_pending;
                }
                g_pl_resizing = 0; g_pl_sb_drag = 0; g_pl_dragging = 0; g_pl_drag_idx = -1; g_pl_drop_at = -1; g_pl_click_pending = -1;
                g_srch_drag = 0;
            }
            else if(e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT){
                g_mouse_x=e.button.x; g_mouse_y=e.button.y;
                g_ctx_open=1; g_ctx_x=(float)e.button.x; g_ctx_y=(float)e.button.y;
                osd_visible=1; osd_last_active=SDL_GetTicks();
            }
            else if(e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT){
                int mx = e.button.x, my = e.button.y;
                g_mouse_x = mx; g_mouse_y = my;
                int handled = 0;
                g_pl_search_active = 0;   /* any click defocuses the search box (B_SEARCH re-focuses below) */
                /* grab the playlist's left edge to resize it (drag to widen and read long names) —
                   the grab zone matches the THICK visual handle */
                if(show_playlist && !g_ctx_open){
                    float edge = g_win_w - g_pl_w;
                    if(mx >= edge-14 && mx <= edge+6){ g_pl_resizing = 1; handled = 1; }
                    /* the playlist scrollbar: click the track to jump, then drag */
                    else if(g_pl_rows_tot > g_pl_rows_vis &&
                            mx >= pl_sb_x0-4 && mx <= pl_sb_x1+4 && my >= pl_sb_y0 && my <= pl_sb_y1){
                        float f = ((float)my - pl_sb_y0) / (pl_sb_y1 - pl_sb_y0);
                        if(f<0)f=0; if(f>1)f=1;
                        pl_scroll = f * (g_pl_rows_tot - g_pl_rows_vis);
                        g_pl_sb_drag = 1; handled = 1;
                    }
                }
                /* context menu (drawn on top) gets first crack; any click dismisses it */
                if(g_ctx_open){
                    for(int c=0;c<g_ctx_n;c++){
                        CtxItem *it=&g_ctx[c];
                        if(mx>=it->x && mx<=it->x+it->w && my>=it->y && my<=it->y+it->h){
                            if(it->kind==1) set_audio_stream(it->val);
                            else if(it->kind==2) g_subs_enabled=0;
                            else if(it->kind==3) g_subs_enabled=1;
                            break;
                        }
                    }
                    g_ctx_open=0; handled=1;
                }
                int wasVisible = osd_visible;
                osd_visible = 1; osd_last_active = SDL_GetTicks();
                /* seek bar */
                if(!handled && wasVisible && seek_x1 > seek_x0 && mx>=seek_x0 && mx<=seek_x1 && my>=seek_y0 && my<=seek_y1 && P.duration>0){
                    double frac=(mx-seek_x0)/(seek_x1-seek_x0); if(frac<0)frac=0; if(frac>1)frac=1;
                    double tgt=frac*P.duration; g_stopped=0;
                    /* live render: click ANYWHERE. Inside the rendered range → instant seek. Outside
                       it (ahead of the head, or before where this render started) → RE-ANCHOR: restart
                       rendering from the click so enhanced footage begins there, not a long wait. */
                    if(g_enh.live && g_enh.playing && (tgt > g_enh.rendered - 0.3 || tgt < g_enh.anchor - 0.3)){
                        enhance_reanchor(tgt);
                    } else { P.seek_target=tgt; P.seek_abs=1; P.seek_flag=1; }
                    handled=1;
                }
                /* buttons / playlist rows (only act if the OSD was already visible) */
                if(wasVisible && !handled){
                    for(int b=0;b<osd_nbtn;b++){
                        OsdBtn *B=&osd_btn[b];
                        if(mx>=B->x && mx<=B->x+B->w && my>=B->y && my<=B->y+B->h){
                            int id=B->id;
                            if(id>=B_PLITEM_BASE){
                                int idx=id-B_PLITEM_BASE;
                                SDL_Keymod km=SDL_GetModState();
                                g_pl_drag_idx=idx; g_pl_drag_y0=(float)my; g_pl_click_pending=-1;   /* arm a reorder drag */
                                if(e.button.clicks>=2){ g_pl_selected=idx; pl_play_at(idx); } /* double-click = play */
                                else if((km&KMOD_SHIFT) && g_pl_selected>=0 && pl_sel){       /* range select */
                                    int a=g_pl_selected<idx?g_pl_selected:idx, b=g_pl_selected<idx?idx:g_pl_selected;
                                    if(!(km&KMOD_CTRL)) memset(pl_sel,0,(size_t)pl_n);
                                    for(int s=a;s<=b && s<pl_n;s++) pl_sel[s]=1;
                                }
                                else if((km&KMOD_CTRL) && pl_sel){                             /* toggle this row */
                                    pl_sel[idx]^=1;
                                    if(pl_sel[idx]) g_pl_selected=idx;                        /* selected → new anchor */
                                    else if(g_pl_selected==idx){                              /* deselected the anchor → */
                                        g_pl_selected=-1;                                     /* move anchor to another */
                                        for(int s=0;s<pl_n;s++) if(pl_sel[s]){ g_pl_selected=s; break; } /* still-selected row */
                                    }
                                }
                                else {                                                        /* plain click */
                                    if(pl_sel && pl_sel[idx] && pl_selcount()>1){
                                        g_pl_click_pending=idx;   /* on a multi-selection: keep it (a drag moves all);
                                                                     collapse to just this row on mouse-UP if no drag */
                                    } else {                      /* select just this row */
                                        if(pl_sel) memset(pl_sel,0,(size_t)pl_n);
                                        if(pl_sel) pl_sel[idx]=1; g_pl_selected=idx;
                                    }
                                }
                            }
                            else if(id>=B_EXCL_BASE && id<B_EXCL_BASE+EXCL_MAX){ excl_remove(id-B_EXCL_BASE); }  /* chip click */
                            else switch(id){
                                case B_PREV: pl_prev(); break;
                                case B_NEXT: pl_next(); break;
                                case B_PLAY:      /* = Spacebar: start the selection if idle, else resume */
                                    if(!P.fmt && pl_n>0){ int s=g_pl_selected>=0?g_pl_selected:0; pl_play_at(s); }
                                    else if(P.fmt){ P.paused=0; g_stopped=0; if(P.audio_dev) SDL_PauseAudioDevice(P.audio_dev,0); }
                                    break;
                                case B_PAUSE2:
                                    if(P.fmt){ P.paused=1; g_stopped=0; if(P.audio_dev) SDL_PauseAudioDevice(P.audio_dev,1); }
                                    break;
                                case B_STOP: stop_playback(); break;
                                case B_THEME: theme_cycle((SDL_GetModState()&KMOD_SHIFT)?-1:+1); break;
                                case B_U: P.upscale=!P.upscale; if(P.upscale)P.anime=0; enhance_apply(); break;
                                case B_G: P.framegen=!P.framegen; enhance_apply(); break;
                                case B_ENH: { int on=!(P.upscale&&P.framegen);
                                              P.upscale=P.framegen=on; if(on)P.anime=0; enhance_apply(); } break;
                                case B_D: P.anime=!P.anime; if(P.anime)P.upscale=0; enhance_apply(); break;
                                case B_LIGHT: P.light=!P.light; toast(P.light?"Light Enhancement ON (shader)":"Light Enhancement off"); break;
                                case B_ARCH: archive_start(); break;
                                case B_FILL: P.fill=!P.fill; break;
                                case B_R: g_repeat=!g_repeat; break;
                                case B_H: g_shuffle=!g_shuffle; if(g_shuffle) g_bag_valid=0; break;
                                case B_AUDIO: switch_audio_track(+1); break;
                                case B_SUBS: g_subs_enabled=!g_subs_enabled; break;
                                case B_STATS: g_show_stats=!g_show_stats; break;
                                case B_KEYS: g_show_keys=!g_show_keys; break;
                                case B_DONATE: donate_copy(); break;
                                case B_LIST: show_playlist=!show_playlist; break;
                                case B_ADDF: if(!g_busy) add_files_dialog(); break;
                                case B_ADDD: if(!g_busy) add_folder_dialog(); break;
                                case B_CLEAR: if(!g_busy) pl_clear(); break;
                                case B_SEARCH: {                             /* focus; click = caret, drag = select,
                                                                                double-click = word, triple = all */
                                    g_pl_search_active=1;
                                    int c = srch_caret_at_x((float)mx);
                                    if(e.button.clicks>=3){ g_pl_anchor=0; g_pl_caret=g_pl_search_len; }
                                    else if(e.button.clicks==2) srch_select_word(c);
                                    else { g_pl_caret=c; if(!(SDL_GetModState()&KMOD_SHIFT)) g_pl_anchor=c; g_srch_drag=1; }
                                } break;
                                case B_SAVEPL: if(!g_busy) pl_save_dialog(); break;
                                case B_LOADPL: if(!g_busy) pl_load_dialog(); break;
                                case B_FALL: g_pl_filter=0; pl_scroll=0; break;
                                case B_FVID: g_pl_filter=1; pl_scroll=0; break;
                                case B_FAUD: g_pl_filter=2; pl_scroll=0; break;
                                case B_FIMG: g_pl_filter=3; pl_scroll=0; break;
                                case B_SORTNAME: if(!g_busy) pl_sort(0); break;
                                case B_SORTSIZE: if(!g_busy) pl_sort_start(1); break;   /* per-file probes run off-thread (spinner) */
                                case B_SORTLEN:  if(!g_busy) pl_sort_start(2); break;
                                case B_SORTDATE: if(!g_busy) pl_sort_start(3); break;
                            }
                            handled=1; break;
                        }
                    }
                }
                /* double-click on the video area → toggle fullscreen */
                if(!handled && e.button.clicks>=2){
                    toggle_fullscreen();
                    handled=1;
                }
            }
            else if(e.type == SDL_MOUSEWHEEL){
                int over_pl = show_playlist && g_mouse_x >= g_win_w - g_pl_w;   /* hovering the panel? */
                if(over_pl) pl_scroll -= e.wheel.y * 3;        /* scroll the playlist */
                else {                                          /* otherwise the wheel is a volume knob */
                    P.volume += e.wheel.y * 0.05;
                    if(P.volume < 0) P.volume = 0; if(P.volume > VOL_MAX) P.volume = VOL_MAX;
                    { char vb[48]; snprintf(vb,sizeof vb,"Volume %d%%%s",(int)(P.volume*100+0.5),P.volume>1.0?"  (boost)":""); toast(vb); }
                }
                osd_visible = 1; osd_last_active = SDL_GetTicks();
            }
        }
        /* OSD auto-hide after idle — while media is up, also hide the mouse cursor and the playlist
           panel so watching gets a fully clean screen; mouse movement / L brings them back. */
        if(osd_visible && SDL_GetTicks() - osd_last_active > 2500) osd_visible = 0;
        { int media = (P.fmt!=NULL) || g_live_playing;
          /* NOTE: this used to clear show_playlist on the OSD's visible->hidden edge, which DISMISSED
             the panel (scroll, selection and open-state lost) rather than hiding it. draw_osd already
             hides it with the OSD while KEEPING show_playlist set, so it comes straight back on the
             next mouse move — that is what the comment there always promised. Don't clear it here. */
          SDL_ShowCursor((osd_visible || !media) ? SDL_ENABLE : SDL_DISABLE); }

        if(autostop_at && SDL_GetTicks() >= autostop_at){ autostop_at = 0; stop_playback();
            fprintf(stderr,"[test] HOGS_AUTOSTOP_MS fired -> stop_playback()\n"); }
        /* test hook: HOGS_AUTOPAUSE_MS=<n> HOGS_AUTORESUME_MS=<m> pauses at n ms and resumes at m ms,
           exactly like Space — reproduces "out of sync after a long pause" headlessly. */
        { static Uint32 ap_at = 1, ar_at = 1;
          if(ap_at == 1){ const char *e = getenv("HOGS_AUTOPAUSE_MS");  ap_at = e ? (Uint32)atoi(e) : 0; }
          if(ar_at == 1){ const char *e = getenv("HOGS_AUTORESUME_MS"); ar_at = e ? (Uint32)atoi(e) : 0; }
          if(ap_at > 1 && SDL_GetTicks() >= ap_at && P.fmt){ ap_at = 0; P.paused = 1; if(P.audio_dev) SDL_PauseAudioDevice(P.audio_dev,1);
              fprintf(stderr,"[test] AUTOPAUSE\n"); }
          if(ar_at > 1 && !ap_at && SDL_GetTicks() >= ar_at && P.fmt){ ar_at = 0; P.paused = 0; g_stopped = 0; if(P.audio_dev) SDL_PauseAudioDevice(P.audio_dev,0);
              fprintf(stderr,"[test] AUTORESUME\n");
              if(getenv("HOGS_RESUME_RACE")) SDL_Delay(60); } }   /* let the audio callback run before the pump (the race) */
        /* test hooks: HOGS_AUTOARCH_MS=<n> presses Archive (W) at n ms (with HOGS_ARCH_OUT=<file> instead
           of the save dialog); HOGS_AUTOPLAYAT_MS=<n> plays the selected row at n ms (navigation mid-archive) */
        { static Uint32 ar = 1, pa = 1;
          if(ar == 1){ const char *e = getenv("HOGS_AUTOARCH_MS"); ar = e ? (Uint32)atoi(e) : 0; }
          if(pa == 1){ const char *e = getenv("HOGS_AUTOPLAYAT_MS"); pa = e ? (Uint32)atoi(e) : 0; }
          if(ar > 1 && SDL_GetTicks() >= ar){ ar = 0; fprintf(stderr,"[test] AUTOARCH\n"); archive_start(); }
          if(pa > 1 && SDL_GetTicks() >= pa){ pa = 0; fprintf(stderr,"[test] AUTOPLAYAT row %d\n", g_pl_selected); pl_play_at(g_pl_selected); } }
        /* test hooks: HOGS_AUTOFS_MS=<n> toggles fullscreen at n ms (HOGS_WINPOS=x,y picks the monitor);
           HOGS_SEARCH_TYPE=<text> types it into the search box, one character per 250 ms, with the
           playlist open — the "typing in search lags playback" scenario, measured with HOGS_PROF. */
        { static Uint32 fs_at[2] = {1,0}; static int fs_i = 0;      /* "n" or "n,m": toggle at n (and back at m) */
          if(fs_at[0] == 1){ const char *e = getenv("HOGS_AUTOFS_MS"); unsigned a=0,b=0;
              if(e) sscanf(e, "%u,%u", &a, &b); fs_at[0] = a; fs_at[1] = b; }
          if(fs_i < 2 && fs_at[fs_i] > 1 && SDL_GetTicks() >= fs_at[fs_i]){ fs_i++; toggle_fullscreen();
              fprintf(stderr,"[test] AUTOFS -> %s\n", is_fullscreen() ? "fullscreen" : "windowed"); } }
        { static const char *st = (const char*)1; static Uint32 st_next = 0;
          if(st == (const char*)1){ st = getenv("HOGS_SEARCH_TYPE"); st_next = SDL_GetTicks() + 3000; }
          if(st && *st && SDL_GetTicks() >= st_next){
              char c[2] = { *st++, 0 }; show_playlist = 1; g_pl_search_active = 1;
              if(c[0] == '|') srch_pin_negatives();    /* '|' = press Enter (pin the exclusions) */
              else srch_insert(c);
              st_next = SDL_GetTicks() + 250;
              fprintf(stderr,"[test] SEARCH_TYPE now \"%s\"\n", g_pl_search); } }
        if(autoseek_at && SDL_GetTicks() >= autoseek_at && P.fmt && P.duration > 0){
            /* HOGS_AUTOSEEK_N>1 repeats the seek every 3s, walking to a different spot each time —
               reproduces "it gets worse the more I seek". */
            static const double FR[6] = {0.33,0.66,0.20,0.80,0.45,0.10};
            double frac = (autoseek_n_done==0) ? autoseek_frac : FR[autoseek_n_done % 6];
            double tgt = frac * P.duration; g_stopped = 0;
            P.seek_target = tgt; P.seek_abs = 1; P.seek_flag = 1;
            autoseek_n_done++;
            fprintf(stderr,"[test] AUTOSEEK #%d -> %.2fs (frac %.3f)\n", autoseek_n_done, tgt, frac);
            autoseek_at = (autoseek_n_done < autoseek_n) ? SDL_GetTicks()+3000 : 0;
        }

        enhance_poll();   /* advance a running AI enhance job -> progress bar; swap to the result on done */

        if(!P.paused) drain_subtitles();   /* decode any queued embedded-subtitle packets → cues */

        if(use_gl){
            SDL_GL_GetDrawableSize(P.win, &g_win_w, &g_win_h);
            double oclk; SDL_LockMutex(P.clock_mutex); oclk = P.audio_clock; SDL_UnlockMutex(P.clock_mutex); if(g_ui_clk > oclk) oclk = g_ui_clk;
            if(!P.paused && P.fmt) gl_video_pump();   /* advance + render video (no swap) */
            /* STOPPED: pump once the seek has landed (demux sets flush_video, then clears seek_flag)
               so the frame we rewound to is decoded and shown. Retry while the packet queue refills —
               the first pump can find it empty — and cap the tries so a file that yields no frame at
               all (EOF'd container) settles on black instead of pumping every vsync forever. */
            else if(P.fmt && g_stop_step && !P.seek_flag && !(g_vd.run && P.flush_video)){
                gl_video_pump();
                if(gl_nframes >= 1 || ++g_stop_tries > 120) g_stop_step = 0;
            }
            else if(!P.fmt && !g_live_playing) draw_idle();   /* nothing loaded → ambient idle screen */
            else gl_redraw();                          /* paused video: re-present last frame */
            /* Advance the monotonic bar follower toward the true position (oclk) before drawing.
               Snap on init / pause / seek / new file (any >1s discontinuity); otherwise coast at
               1.0x wall and pull toward truth at 5%/frame, clamped to never reverse. */
            { static double bar_wall=0; double nowt=av_gettime()/1000000.0;
              if(g_bar_clk < -1e17 || P.paused || fabs(oclk - g_bar_clk) > 1.0){ g_bar_clk = oclk; bar_wall = nowt; }
              else { double dt = nowt - bar_wall; bar_wall = nowt;
                     if(dt < 0) dt = 0; if(dt > 0.25) dt = 0.25;
                     double coast = g_bar_clk + dt;                 /* 1.0x wall advance */
                     double next  = coast + (oclk - coast) * 0.05;  /* gentle pull toward truth */
                     if(next > oclk + 0.5) next = oclk + 0.5;       /* don't run away ahead of truth */
                     if(next < g_bar_clk) next = g_bar_clk;         /* NEVER reverse — plateau instead */
                     g_bar_clk = next; } }
            /* CLKLOG: fine-grained (per-frame, ~20ms throttle) trace of the value the SEEK BAR draws
               (oclk) next to the raw audio clock and the smoothed projection, to see pulse shape. */
            if(getenv("HOGS_CLKLOG") && !P.paused && P.fmt){
                static Uint32 clk_t0=0; Uint32 cn=SDL_GetTicks();
                if(cn-clk_t0>=20){ clk_t0=cn;
                    double ac; SDL_LockMutex(P.clock_mutex); ac=P.audio_clock; SDL_UnlockMutex(P.clock_mutex);
                    fprintf(stderr,"[clk] t=%u oclk=%.4f audio=%.4f ui=%.4f bar=%.4f\n", cn, oclk, ac, g_ui_clk, g_bar_clk); } }
            draw_osd(oclk);                            /* control bar on top */
            if(g_busy) draw_spinner();                 /* busy overlay while a playlist worker runs */
            SDL_GL_SwapWindow(P.win);                  /* vsync paces the whole loop */
            if(!P.fmt && !g_live_playing) SDL_Delay(18);  /* idle screen: cap ~30fps, spare the GPU */
        } else {
            if(!P.paused && P.fmt) video_step();       /* SDL fallback presents internally */
            else SDL_Delay(16);
        }
        /* measure on-screen present rate (vsync) AND the real video frame rate (new frames shown) */
        { static Uint32 ft0=0; static int fc=0; static int vf0=0; Uint32 tn=SDL_GetTicks(); fc++;
          if(tn-ft0>=500){ if(ft0){ g_disp_fps=fc*1000.0/(tn-ft0);
              /* RAW presented-frame rate over this ~500ms window. Counting distinct frames in a fixed
                 wall window has ±1-frame quantization: a 29.97fps source lands 14 or 15 frames per
                 window = 28 or 30, occasionally 26/32 — so the number swings ±3fps and the STUTTERING
                 flag (< 0.85*target) false-trips on perfectly smooth playback. The rate is a noisy
                 estimate of a steady quantity, so LOW-PASS it: an EMA removes the zero-mean quantization
                 while a SUSTAINED real drop (a true stutter) still pulls it down within ~1s. */
              double raw_vfps=(g_vid_frames-vf0)*1000.0/(tn-ft0);
              g_vid_fps = (g_vid_fps>0) ? g_vid_fps*0.6 + raw_vfps*0.4 : raw_vfps;
              if(getenv("HOGS_FPSLOG")){ double _c,_u; SDL_LockMutex(P.clock_mutex); _c=P.audio_clock; SDL_UnlockMutex(P.clock_mutex); _u=g_ui_clk;
                /* av = video pts MINUS the master (audio) clock. Negative = video behind audio. This is
                   the A/V sync error, measured where it actually matters: the frame being presented.
                   uiclk = what the SEEK BAR reads (max of audio_clock and the smoothed projection). */
                /* adec-v = last DECODED audio position minus the presented video pts: unlike av it is not
                   slaved to the master clock, so a latched wall-pacing desync shows up here (normally a
                   small constant lead = the audio buffer). */
                fprintf(stderr,"[fps] vid=%.1f raw=%.1f disp=%.1f clk=%.2f uiclk=%.2f vpts=%.2f av=%+.3f adec-v=%+.3f stall=%d creep=%d swrdelay=%lld (%.3fs) vq=%d aq=%d paused=%d astream=%d\n",
                        g_vid_fps, raw_vfps, g_disp_fps, _c, _u, gl_ptsB, gl_ptsB - _c, P.aclk_probe - gl_ptsB,
                        P.audio_stalled, P.aclk_creep,
                        (long long)g_swr_delay, P.out_rate? (double)g_swr_delay/P.out_rate : 0.0,
                        P.vq.nb_packets, P.aq.nb_packets, P.paused, P.astream); } }
            ft0=tn; fc=0; vf0=g_vid_frames; } }
        if(P.paused){ /* don't run EOF logic while paused */ }

        /* still images: show for 10s then advance (slideshow); pausing holds it indefinitely. */
        { static Uint32 image_since=0; static int image_cur=-1;
          if(P.fmt && P.is_image){
              if(image_cur != pl_cur){ image_cur = pl_cur; image_since = SDL_GetTicks(); }
              if(!P.paused && SDL_GetTicks() - image_since > 10000){
                  image_cur = -1;
                  if(pl_n > 1 || g_repeat){ pl_next(); }   /* advance; stall-watchdog retries if nothing loads */
                  else P.paused = 1;       /* lone image → hold after 10s */
              }
          } else image_cur = -1;
        }

        /* auto-advance at end of file (demux hit EOF and all packets are consumed).
           A still image is exempt here — its 10s timer above handles advancing. During a LIVE
           progressive render, hitting the current end means "caught up to the render head", NOT
           end-of-video — enhance_poll holds/resumes it, so never auto-advance here. */
        if(P.fmt && !P.is_image && P.eof && P.vq.nb_packets == 0 && P.aq.nb_packets == 0
           && !(g_enh.live && g_enh.playing)){
            if(!eof_since) eof_since = SDL_GetTicks();
            double clk; SDL_LockMutex(P.clock_mutex); clk = P.audio_clock; SDL_UnlockMutex(P.clock_mutex); if(g_ui_clk > clk) clk = g_ui_clk;
            int nearEnd = (P.duration <= 0) || (clk >= P.duration - 0.4);
            if(nearEnd || (SDL_GetTicks() - eof_since > 700)){   /* timer covers a plateaued clock */
                eof_since = 0;
                /* HONEST FAILURE (the ".wmv plays very weird" report): a stream that ends MINUTES
                   before its declared duration is a damaged or DRM-protected file (old WM-DRM wmv:
                   ffmpeg reads the ~5s clear header, then hits the encrypted payload and EOFs —
                   measured on Sons of Anarchy S02 wmvs). Say so instead of silently restart-looping. */
                if(!nearEnd && P.duration > 60 && clk < P.duration - 60){
                    toast("File ended early \342\200\224 likely DRM-protected or damaged (.wmv WM-DRM?)");
                    fprintf(stderr,"[open] premature end at %.1fs of %.0fs — corrupt or DRM-protected\n",clk,P.duration);
                }
                if(pl_n > 1 || g_repeat){ pl_next(); }   /* advance; stall-watchdog retries if nothing loads */
                else P.paused = 1;       /* single item, no repeat → stop at end */
            }
        } else eof_since = 0;

        /* STALL WATCHDOG — the "leave it running all night" safety net: force-advance if a loaded,
           non-image, non-paused file makes NO progress (no new video frames AND the clock isn't moving)
           for ~20s; and if NO media is loaded (e.g. a failed load) keep retrying the playlist every 1.5s.
           Catches frozen decoders, hung files, and load failures from ANY cause. */
        { static Uint32 prog_t=0, nomedia_t=0; static int prog_vf=-1; static double prog_clk=-1;
          Uint32 nowt = SDL_GetTicks();
          if(!P.fmt && g_enh.live){                     /* live enhance mid re-anchor: media briefly
                                                           closed while the new render buffers — wait */
              prog_t=0; nomedia_t=0;
          } else if(!P.fmt){                            /* nothing loaded → keep trying the playlist —
                                                           but ONLY after the user started playback once
                                                           (fresh loads must WAIT for Play/Space) */
              if(pl_n>0 && g_hist_n>0){ if(!nomedia_t) nomedia_t=nowt; else if(nowt-nomedia_t>1500){ nomedia_t=0; pl_next(); } }
              prog_t=0; prog_vf=-1; prog_clk=-1;
          } else if(P.paused || P.is_image || enh_running()){   /* paused / slideshow / enhancing: not a stall */
              prog_t=nowt; nomedia_t=0;
          } else {
              nomedia_t=0;
              double c; SDL_LockMutex(P.clock_mutex); c=P.audio_clock; SDL_UnlockMutex(P.clock_mutex);
              /* Reset when the clock MOVES AT ALL (either direction) — not only when it exceeds a
                 high-water mark. The old `c > prog_clk+0.05` never reset when a NEW file started
                 (its clock begins at 0, below the previous file's prog_clk), so healthy tracks were
                 force-advanced at exactly 20s until the clock climbed back past the stale watermark.
                 fabs() re-baselines on the file change / seek and still fires only on a truly frozen clock. */
              if(!prog_t || g_vid_frames!=prog_vf || fabs(c-prog_clk)>0.05){ prog_vf=g_vid_frames; prog_clk=c; prog_t=nowt; }
              else if(nowt-prog_t>20000){               /* 20s of zero progress → assume stuck, skip on */
                  fprintf(stderr,"[watchdog] playback stalled ~20s — force-advancing to next file\n");
                  prog_t=nowt; pl_next();
              }
          }
        }

        Uint32 now = SDL_GetTicks();
        if(now - last_title > 500 && P.fmt){
            last_title = now;
            update_monitor_mode();   /* pick up display changes / the window moving to another monitor */
            double clk; SDL_LockMutex(P.clock_mutex); clk=P.audio_clock; SDL_UnlockMutex(P.clock_mutex);
            const char *nm = (pl_cur>=0 && pl_cur<pl_n) ? pl_items[pl_cur] : "";
            const char *base = nm; for(const char *q=nm; *q; q++) if(*q=='/'||*q=='\\') base=q+1;
            char t[512];
            snprintf(t,sizeof(t),"Hogs media player V1.45 · [%d/%d] %s  —  %.0f:%05.2f / %.0f:%05.2f%s%s%s%s",
                     pl_cur+1, pl_n, base, clk/60, fmod(clk,60), P.duration/60, fmod(P.duration,60),
                     P.is_image?"  [image]":"", P.paused?"  (paused)":"", g_repeat?"  R":"", g_shuffle?"  H":"");
            SDL_SetWindowTitle(P.win, t);
        }
    }

    P.quit=1;
    if(!g_busy) sess_save();              /* remember the playlist + selection for next launch */
    if(g_srch_ver != g_srch_saved_ver) srch_text_save();
    if(enh_running()) enhance_cancel();   /* kill the enhancer, don't orphan it (deletes its partial file) */
    close_media();                        /* CLOSE FIRST: a playing enhanced file is held open — deleting
                                             it would fail and it would survive the wipe */
    enh_cache_wipe();                     /* delete ALL cached enhanced videos on exit (user setting) */
    if(g_hw_device) av_buffer_unref(&g_hw_device);
    if(gl_ctx) SDL_GL_DeleteContext(gl_ctx);
    SDL_Quit();
    if(g_log_path[0]){ fclose(stderr); fclose(stdout); if(!g_log_keep) path_remove(g_log_path); }  /* leave no log behind unless HOGS_LOG asked */
    return 0;
}
