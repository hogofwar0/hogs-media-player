// hogs_enhance — HEADLESS offline enhancer (no window). Decodes a video, runs Real-ESRGAN upscale
// and/or RIFE frame-gen (reusing ncnn_bridge), and writes an H.265 .mkv. Progress -> a status JSON file.
// The player launches this, pauses, shows a progress bar, then plays the result. NO SDL, NO Vulkan present.
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
}
#include "ncnn_bridge.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <ctime>
#include <vector>
#include <windows.h>   /* --workers: CreateProcess + job object (children die with the parent) */
#include <pdh.h>       /* --perflog: read GPU Engine / memory perf counters (Phase 0 instrumentation) */
#include <pdhmsg.h>    /* PDH_MORE_DATA, PDH_CSTATUS_VALID_DATA */
#include <dxgi.h>      /* vendor-neutral total-VRAM query (AMD/NVIDIA/Intel) for the Phase-1 budget */

static const char *g_status=0;
static void write_status(int pct,const char*phase,int done,const char*err){
    if(!g_status) return;
    FILE*f=fopen(g_status,"wb"); if(!f) return;
    fprintf(f,"{\"percent\":%d,\"phase\":\"%s\",\"done\":%d,\"error\":\"%s\"}\n",
            pct,phase?phase:"",done,err?err:"");
    fclose(f);
}
// progressive-mode status: adds "rendered"/"total" seconds so the player knows how much CONTIGUOUS
// in-order output already exists (its safe playback + seek horizon) and the full timeline length.
static void write_status_prog(int pct,const char*phase,int done,double rendered,double total){
    if(!g_status) return;
    FILE*f=fopen(g_status,"wb"); if(!f) return;
    fprintf(f,"{\"percent\":%d,\"phase\":\"%s\",\"done\":%d,\"error\":\"\",\"rendered\":%.3f,\"total\":%.3f}\n",
            pct,phase?phase:"",done,rendered,total);
    fclose(f);
}
// ---- Phase-0 instrumentation: --perflog CSV of REAL resource use (not Task Manager) ----
// Reads the Windows GPU Engine + Adapter Memory perf counters via PDH (Vulkan compute shows on
// the "compute" engine, invisible on TM's default "3D" graph) + host RAM via GlobalMemoryStatusEx.
// Only the PARENT orchestrator (progressive / --workers) samples; children stay silent.
struct PerfLogger {
    FILE* f=0; PDH_HQUERY q=0;
    PDH_HCOUNTER cCompute=0,cVcn=0,cVcnE=0,cVcnD=0,cCopy=0,cDed=0,cShared=0;
    ULONGLONG t0=0,last=0;
    // sum all wildcard-instance values of one formatted counter (GPU exposes one per engine/process)
    static double sum_counter(PDH_HCOUNTER c){
        DWORD sz=0,n=0; if(!c) return 0;
        PDH_STATUS st=PdhGetFormattedCounterArrayA(c,PDH_FMT_DOUBLE,&sz,&n,0);
        if(st!=PDH_MORE_DATA || !sz) return 0;
        PDH_FMT_COUNTERVALUE_ITEM_A* buf=(PDH_FMT_COUNTERVALUE_ITEM_A*)malloc(sz);
        if(!buf) return 0;
        double s=0;
        if(PdhGetFormattedCounterArrayA(c,PDH_FMT_DOUBLE,&sz,&n,buf)==ERROR_SUCCESS)
            for(DWORD i=0;i<n;i++) if(buf[i].FmtValue.CStatus==PDH_CSTATUS_VALID_DATA) s+=buf[i].FmtValue.doubleValue;
        free(buf); return s;
    }
    void init(const char* path){
        if(!path) return;
        f=fopen(path,"wb"); if(!f) return;
        fprintf(f,"t_sec,workers_live,compute_pct,vcn_pct,copy_pct,vram_used_MB,vram_shared_MB,ram_used_MB,ram_avail_MB,progress\n");
        fflush(f);
        if(PdhOpenQueryA(0,0,&q)!=ERROR_SUCCESS){ q=0; return; }
        // English counter names -> locale-independent; wildcards expand to all instances
        PdhAddEnglishCounterA(q,"\\GPU Engine(*engtype_Compute*)\\Utilization Percentage",0,&cCompute);
        PdhAddEnglishCounterA(q,"\\GPU Engine(*engtype_Video Codec Engine*)\\Utilization Percentage",0,&cVcn);  // AMD
        PdhAddEnglishCounterA(q,"\\GPU Engine(*engtype_VideoEncode*)\\Utilization Percentage",0,&cVcnE);        // NVIDIA/Intel
        PdhAddEnglishCounterA(q,"\\GPU Engine(*engtype_VideoDecode*)\\Utilization Percentage",0,&cVcnD);        // NVIDIA/Intel
        PdhAddEnglishCounterA(q,"\\GPU Engine(*engtype_Copy*)\\Utilization Percentage",0,&cCopy);
        PdhAddEnglishCounterA(q,"\\GPU Adapter Memory(*)\\Dedicated Usage",0,&cDed);
        PdhAddEnglishCounterA(q,"\\GPU Adapter Memory(*)\\Shared Usage",0,&cShared);
        PdhCollectQueryData(q);                    // prime (rate counters need two samples)
        t0=GetTickCount64(); last=0;
    }
    // call often; writes at most ~1/sec. workers_live + a free-form progress value for context.
    void tick(int workers_live,double progress){
        if(!f) return;
        ULONGLONG now=GetTickCount64();
        if(last && now-last<950) return;           // throttle to ~1 Hz
        last=now;
        double comp=0,vcn=0,copy=0,ded=0,shr=0;
        if(q && PdhCollectQueryData(q)==ERROR_SUCCESS){
            comp=sum_counter(cCompute); copy=sum_counter(cCopy);
            vcn=sum_counter(cVcn)+sum_counter(cVcnE)+sum_counter(cVcnD);   // AMD codec engine OR NVIDIA/Intel enc+dec
            ded=sum_counter(cDed); shr=sum_counter(cShared);
        }
        MEMORYSTATUSEX ms; ms.dwLength=sizeof ms; GlobalMemoryStatusEx(&ms);
        double ram_used=(double)(ms.ullTotalPhys-ms.ullAvailPhys)/(1024.0*1024.0);
        double ram_avail=(double)ms.ullAvailPhys/(1024.0*1024.0);
        fprintf(f,"%.1f,%d,%.1f,%.1f,%.1f,%.0f,%.0f,%.0f,%.0f,%.3f\n",
                (now-t0)/1000.0,workers_live,comp,vcn,copy,
                ded/(1024.0*1024.0),shr/(1024.0*1024.0),ram_used,ram_avail,progress);
        fflush(f);
    }
    void close(){ if(q) PdhCloseQuery(q); q=0; if(f) fclose(f); f=0; }
};

// One-shot: total dedicated VRAM in use across the adapter (MB), for the Phase-1 worker probe.
// Returns -1 if unavailable (caller falls back to a fixed worker count).
static double gpu_vram_used_mb(){
    PDH_HQUERY q; if(PdhOpenQueryA(0,0,&q)!=ERROR_SUCCESS) return -1;
    PDH_HCOUNTER c;
    if(PdhAddEnglishCounterA(q,"\\GPU Adapter Memory(*)\\Dedicated Usage",0,&c)!=ERROR_SUCCESS){ PdhCloseQuery(q); return -1; }
    PdhCollectQueryData(q); Sleep(60); PdhCollectQueryData(q);   // gauge: two collects for a stable read
    double s=PerfLogger::sum_counter(c);
    PdhCloseQuery(q);
    return s<=0 ? -1 : s/(1024.0*1024.0);
}
// Vendor-neutral TOTAL VRAM of the largest (discrete) GPU, in MB, via DXGI — works on AMD/NVIDIA/
// Intel. So the Phase-1 budget adapts to the actual card instead of a hardcoded 24GB. -1 if unknown.
static double gpu_total_vram_mb(){
    IDXGIFactory1* fac=0;
    if(FAILED(CreateDXGIFactory1(IID_IDXGIFactory1,(void**)&fac)) || !fac) return -1;
    IDXGIAdapter1* ad=0; double best=-1;
    for(UINT i=0; fac->EnumAdapters1(i,&ad)!=DXGI_ERROR_NOT_FOUND; i++){
        DXGI_ADAPTER_DESC1 d;
        if(SUCCEEDED(ad->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)){
            double mb=(double)d.DedicatedVideoMemory/(1024.0*1024.0);   // physical VRAM (0 for pure-shared iGPU)
            if(mb>best) best=mb;
        }
        ad->Release(); ad=0;
    }
    fac->Release();
    return best;
}

#define FAIL(msg) do{ fprintf(stderr,"[enh] %s\n",msg); write_status(0,"error",1,msg); return 3; }while(0)

int main(int argc,char**argv){
    setvbuf(stdout,NULL,_IONBF,0);
    const char *in=0,*out=0,*upP=0,*upB=0,*rfP=0,*rfB=0; int do_up=0,do_fg=0,maxh=1440,tilestats=0,deltastats=0;
    int up_scale=4; const char *up_in="data", *up_out="output";  // model-agnostic: --up-scale/--up-in/--up-out (ESRGAN=4/data/output; CUGAN up2x=2/in0/out0)
    double seg_ss=-1, seg_to=-1;                 // segment window (worker mode): process [ss, to) seconds
    int seampreroll=0, noaudio=0, workers=1;     // seampreroll: k>0 segment pre-rolls 1 frame before its start so RIFE makes the real cross-seam midpoint
    int progressive=0;                           // play-while-enhancing: rolling segments + in-order incremental append
    const char* perflog=0;                        // --perflog <file>: parent samples GPU/RAM counters -> CSV (Phase 0)
    int workers_force=0;                           // --workers-force N (or env HOGS_WORKERS): bypass the auto probe (Phase 1)
    int noupscale_above=1440;                      // --noupscale-above N: skip x4 upscale when src height >= N (Phase 6); 0=never skip
    int noframegen_above=0;                        // --noframegen-above N: skip frame-gen when src height >= N; 0(default)=never skip (4K frame-gen is back via rife_maxh)
    int rife_maxh=1440;                            // --rife-maxh N: run RIFE at <=N height (flow doesn't need 8MP); midpoints Lanczos-up to full res. 0=native-res RIFE
    int target_h=0;                                // --target-h N: cap the UPSCALED output height to N (monitor native), Lanczos-downscaling the x4 result; 0=keep full x4
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--in")&&i+1<argc) in=argv[++i];
        else if(!strcmp(argv[i],"--out")&&i+1<argc) out=argv[++i];
        else if(!strcmp(argv[i],"--status")&&i+1<argc) g_status=argv[++i];
        else if(!strcmp(argv[i],"--upscale")) do_up=1;
        else if(!strcmp(argv[i],"--framegen")) do_fg=1;
        else if(!strcmp(argv[i],"--maxh")&&i+1<argc) maxh=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--noupscale-above")&&i+1<argc) noupscale_above=atoi(argv[++i]); // Phase 6
        else if(!strcmp(argv[i],"--noframegen-above")&&i+1<argc) noframegen_above=atoi(argv[++i]); // Phase 6 (4K frame-gen)
        else if(!strcmp(argv[i],"--rife-maxh")&&i+1<argc) rife_maxh=atoi(argv[++i]);              // RIFE working-res cap (0=native)
        else if(!strcmp(argv[i],"--target-h")&&i+1<argc) target_h=atoi(argv[++i]);                 // native-res target: cap upscaled output height
        else if(!strcmp(argv[i],"--up-param")&&i+1<argc) upP=argv[++i];
        else if(!strcmp(argv[i],"--up-bin")&&i+1<argc)   upB=argv[++i];
        else if(!strcmp(argv[i],"--up-scale")&&i+1<argc) up_scale=atoi(argv[++i]);   // model output scale (2/3/4)
        else if(!strcmp(argv[i],"--up-in")&&i+1<argc)    up_in=argv[++i];            // model input blob name
        else if(!strcmp(argv[i],"--up-out")&&i+1<argc)   up_out=argv[++i];           // model output blob name
        else if(!strcmp(argv[i],"--rife-param")&&i+1<argc) rfP=argv[++i];
        else if(!strcmp(argv[i],"--rife-bin")&&i+1<argc)   rfB=argv[++i];
        else if(!strcmp(argv[i],"--tilestats")) tilestats=1;   // decode-only DELTA-reuse measurement
        else if(!strcmp(argv[i],"--deltastats")) deltastats=1; // DELTA D1: per-pixel event measurement
        else if(!strcmp(argv[i],"--workers")&&i+1<argc) workers=atoi(argv[++i]); // parallel segment workers
        else if(!strcmp(argv[i],"--progressive")) progressive=1;                 // play-while-enhancing orchestrator
        else if(!strcmp(argv[i],"--ss")&&i+1<argc) seg_ss=atof(argv[++i]);       // worker: segment start (s)
        else if(!strcmp(argv[i],"--to")&&i+1<argc) seg_to=atof(argv[++i]);       // worker: segment end (s, exclusive)
        else if(!strcmp(argv[i],"--seampreroll")) seampreroll=1;                 // worker k>0: pre-roll 1 frame for the cross-seam midpoint
        else if(!strcmp(argv[i],"--noaudio")) noaudio=1;                         // worker: parent muxes the audio
        else if(!strcmp(argv[i],"--perflog")&&i+1<argc) perflog=argv[++i];        // parent: resource CSV (Phase 0)
        else if(!strcmp(argv[i],"--workers-force")&&i+1<argc) workers_force=atoi(argv[++i]); // force worker count (Phase 1)
    }
    if(workers<1) workers=1; if(workers>8) workers=8;
    if(!in||!out){ fprintf(stderr,"usage: hogs_enhance --in v --out v.mkv --status s [--upscale][--framegen] --up-param.. --rife-param..\n"); return 1; }
    if(!do_up&&!do_fg){ do_up=1; }               // nothing selected -> at least upscale
    write_status(0,"Starting",0,"");

    // ---- open input, find video (+ audio) ----
    AVFormatContext*ifmt=0;
    if(avformat_open_input(&ifmt,in,0,0)<0) FAIL("cannot open input");
    if(avformat_find_stream_info(ifmt,0)<0) FAIL("no stream info");
    int vs=av_find_best_stream(ifmt,AVMEDIA_TYPE_VIDEO,-1,-1,0,0); if(vs<0) FAIL("no video stream");
    int as=av_find_best_stream(ifmt,AVMEDIA_TYPE_AUDIO,-1,-1,0,0);
    if(noaudio) as=-1;                           // worker: the parent muxes audio at concat time
    AVCodecParameters*vpar=ifmt->streams[vs]->codecpar;
    const AVCodec*vdec=avcodec_find_decoder(vpar->codec_id); if(!vdec) FAIL("no video decoder");
    AVCodecContext*vdc=avcodec_alloc_context3(vdec); avcodec_parameters_to_context(vdc,vpar);
    vdc->thread_count=0;
    if(avcodec_open2(vdc,vdec,0)<0) FAIL("cannot open video decoder");

    // source fps + total frames (for progress + output timing)
    AVRational sfr = ifmt->streams[vs]->avg_frame_rate;
    if(!(sfr.num>0&&sfr.den>0)) sfr=(AVRational){24,1};
    double sfps = (double)sfr.num/sfr.den;
    long total = ifmt->streams[vs]->nb_frames;
    if(total<=0 && ifmt->duration>0) total=(long)(ifmt->duration/(double)AV_TIME_BASE*sfps);
    if(total<=0) total=1;

    // ---- --tilestats: measurement pass for the DELTA tile-transcoder idea. Decode only (no AI,
    //      no encode): how many 32x32 tiles at the U-pipeline's capped res are unchanged between
    //      consecutive frames, bucketed by mean-abs-diff — tells us the grain floor + reuse %.
    if(tilestats){
        int sW=vpar->width, sH=vpar->height;
        int tH = sH>360?360:sH, tW=(int)((double)sW*tH/sH+0.5); tW&=~1; tH&=~1;
        const int TS=32; int gx=tW/TS, gy=tH/TS;
        long hist[8]={0}; const double bounds[7]={1,2,3,4,6,8,12};
        long ntiles=0, nframes=0, skip=1500, maxf=6000, ndup=0;
        unsigned char*cur=(unsigned char*)malloc((size_t)tW*tH*3), *prv=(unsigned char*)malloc((size_t)tW*tH*3);
        if(!cur||!prv) FAIL("oom");
        SwsContext*sw=0; int have_prev=0;
        AVPacket*tp=av_packet_alloc(); AVFrame*fq=av_frame_alloc();
        while(av_read_frame(ifmt,tp)>=0 && nframes<maxf){
            if(tp->stream_index==vs && avcodec_send_packet(vdc,tp)==0){
                while(avcodec_receive_frame(vdc,fq)==0 && nframes<maxf){
                    if(skip>0){ skip--; continue; }
                    sw=sws_getCachedContext(sw,fq->width,fq->height,(AVPixelFormat)fq->format,tW,tH,AV_PIX_FMT_RGB24,SWS_BILINEAR,0,0,0);
                    uint8_t*d[1]={cur}; int l[1]={tW*3};
                    sws_scale(sw,(const uint8_t*const*)fq->data,fq->linesize,0,fq->height,d,l);
                    if(have_prev){
                        long fsum=0;
                        for(int ty=0;ty<gy;ty++) for(int tx=0;tx<gx;tx++){
                            long s=0;
                            for(int y=0;y<TS;y++){
                                const unsigned char*a=cur+((size_t)(ty*TS+y)*tW+tx*TS)*3;
                                const unsigned char*b=prv+((size_t)(ty*TS+y)*tW+tx*TS)*3;
                                for(int x=0;x<TS*3;x++) s+=abs((int)a[x]-(int)b[x]);
                            }
                            double mad=(double)s/(TS*TS*3); fsum+=s;
                            int bi=7; for(int k=0;k<7;k++) if(mad<bounds[k]){bi=k;break;}
                            hist[bi]++; ntiles++;
                        }
                        if(gx>0&&gy>0&&(double)fsum/((double)gx*gy*TS*TS*3)<1.0) ndup++;
                        nframes++;
                    }
                    { unsigned char*t=cur; cur=prv; prv=t; have_prev=1; }
                }
            }
            av_packet_unref(tp);
        }
        av_packet_free(&tp); av_frame_free(&fq);
        printf("[tilestats] %s\n", in);
        printf("[tilestats] %dx%d capped, %dx%d tiles, %ld frame-pairs, whole-frame dups=%ld (%.1f%%)\n",
               tW,tH,gx,gy,nframes,ndup,nframes?100.0*ndup/nframes:0.0);
        const char*lbl[8]={"<1  ","1-2 ","2-3 ","3-4 ","4-6 ","6-8 ","8-12",">=12"};
        long cum=0;
        for(int k=0;k<8;k++){ cum+=hist[k];
            printf("[tilestats] mad %s : %5.1f%%   (reusable if thresh here: %5.1f%%)\n",
                   lbl[k], ntiles?100.0*hist[k]/ntiles:0.0, ntiles?100.0*cum/ntiles:0.0); }
        free(cur); free(prv); if(sw) sws_freeContext(sw);
        avcodec_free_context(&vdc); avformat_close_input(&ifmt);
        return 0;
    }
    // ---- --deltastats: DELTA phase D1 — PER-PIXEL event measurement (design doc §4).
    //      A pixel FIRES an event when its color changes by more than √2 per channel (max of
    //      |Δr|,|Δg|,|Δb|; 8-bit units) vs the previous frame — the noise/meaning boundary.
    //      Decode-only at the U-pipeline's capped res. Reports, per the D1 gate:
    //        · per-pixel event coverage (mean/median %) → validates the 75-85% unchanged estimate
    //        · dup frames (zero events), per-pixel |Δ| histogram → where the grain floor sits
    //        · coverage after receptive-field dilation r=10 (realesr) and r=32 (RIFE) → the
    //          fraction of model compute D2's masked inference can actually skip
    //        · 10th-percentile frame MAD (grain floor) → auto gate recommendation (√2 vs 2√2)
    if(deltastats){
        int sW=vpar->width, sH=vpar->height;
        int tH = sH>360?360:sH, tW=(int)((double)sW*tH/sH+0.5); tW&=~1; tH&=~1;
        const size_t NPX=(size_t)tW*tH;
        unsigned char*cur=(unsigned char*)malloc(NPX*3), *prv=(unsigned char*)malloc(NPX*3);
        unsigned char*mask=(unsigned char*)malloc(NPX);          // 1 = event fired at this pixel
        unsigned char*dil =(unsigned char*)malloc(NPX);          // scratch: dilated mask
        int*psum=(int*)malloc(((size_t)(tW>tH?tW:tH)+1)*sizeof(int));
        if(!cur||!prv||!mask||!dil||!psum) FAIL("oom");
        // per-pixel |Δ| histogram (max channel): 0,1,2,3,4-5,6-7,8-11,>=12 — √2 sits between 1 and 2
        long long hist[8]={0}; const int hedge[7]={1,2,3,4,6,8,12};
        double*cov=(double*)malloc(sizeof(double)*8192);         // per-frame event coverage % (√2 gate)
        double*fmad=(double*)malloc(sizeof(double)*8192);        // per-frame mean MAD (grain floor)
        // GATE SWEEP: analog grain fires isolated single-pixel events that dilation then inflates
        // to near-full coverage (measured: Voyager 92-99% processed at a raw √2 gate). So each
        // gate is evaluated raw AND with isolated-event suppression (a 3x3 opening: an event
        // survives only if >=2 of its 8 neighbours also fired — real motion is spatially
        // coherent, grain is not). D2's mask builder uses whichever gate the report recommends.
        enum { NG=4 };
        static const double gateval[NG]={1.4142135624, 2.8284271248, 4.0, 6.0};
        static const int    gmin[NG]  ={2, 3, 5, 7};             // integer mx>=gmin ⇔ mx > gate
        static const char*  gname[NG] ={"√2  (1.41)","2√2 (2.83)","4.0       ","6.0       "};
        double gfire[NG]={0}, gsupp[NG]={0}, gd10[NG]={0}, gd32[NG]={0};
        unsigned char*mask2=(unsigned char*)malloc(NPX);         // suppressed mask
        unsigned char*diffb=(unsigned char*)malloc(NPX);         // per-pixel max-channel |Δ| (clamped 255)
        if(!mask2||!diffb) FAIL("oom");
        // skip the intro/titles on long sources; short clips are analysed whole
        long nframes=0, ndup=0, skip=total>3000?1500:0, maxf=6000;
        // separable box dilation of the binary mask by radius r (prefix sums; O(N) per pass)
        auto dilate=[&](const unsigned char*src, unsigned char*dst, int r)->double{
            // horizontal into dst
            for(int y=0;y<tH;y++){
                const unsigned char*s=src+(size_t)y*tW; unsigned char*d=dst+(size_t)y*tW;
                psum[0]=0; for(int x=0;x<tW;x++) psum[x+1]=psum[x]+s[x];
                for(int x=0;x<tW;x++){ int a=x-r; if(a<0)a=0; int b=x+r+1; if(b>tW)b=tW;
                    d[x]=(psum[b]-psum[a])>0; }
            }
            // vertical in place (column prefix sums, one column at a time via a row buffer)
            long on=0;
            for(int x=0;x<tW;x++){
                psum[0]=0; for(int y=0;y<tH;y++) psum[y+1]=psum[y]+dst[(size_t)y*tW+x];
                for(int y=0;y<tH;y++){ int a=y-r; if(a<0)a=0; int b=y+r+1; if(b>tH)b=tH;
                    int v=(psum[b]-psum[a])>0; on+=v; dst[(size_t)y*tW+x]=(unsigned char)v; }
            }
            return (double)on/NPX;
        };
        SwsContext*sw=0; int have_prev=0;
        AVPacket*tp=av_packet_alloc(); AVFrame*fq=av_frame_alloc();
        while(av_read_frame(ifmt,tp)>=0 && nframes<maxf){
            if(tp->stream_index==vs && avcodec_send_packet(vdc,tp)==0){
                while(avcodec_receive_frame(vdc,fq)==0 && nframes<maxf){
                    if(skip>0){ skip--; continue; }
                    sw=sws_getCachedContext(sw,fq->width,fq->height,(AVPixelFormat)fq->format,tW,tH,AV_PIX_FMT_RGB24,SWS_BILINEAR,0,0,0);
                    uint8_t*d[1]={cur}; int l[1]={tW*3};
                    sws_scale(sw,(const uint8_t*const*)fq->data,fq->linesize,0,fq->height,d,l);
                    if(have_prev){
                        long long fired=0, sum=0;
                        for(size_t i=0;i<NPX;i++){
                            int dr=abs((int)cur[i*3  ]-(int)prv[i*3  ]);
                            int dg=abs((int)cur[i*3+1]-(int)prv[i*3+1]);
                            int db=abs((int)cur[i*3+2]-(int)prv[i*3+2]);
                            int mx=dr>dg?dr:dg; if(db>mx)mx=db;
                            sum+=dr+dg+db;
                            diffb[i]=(unsigned char)(mx>255?255:mx);
                            int bi=7; for(int k=0;k<7;k++) if(mx<hedge[k]){bi=k;break;}
                            hist[bi]++;
                            // the √2 gate: integer mx>=2 ⇔ mx > 1.414
                            fired+=(mx>=2);
                        }
                        double c=100.0*fired/NPX, fm=(double)sum/(NPX*3);
                        if(nframes<8192){ cov[nframes]=c; fmad[nframes]=fm; }
                        if(fired==0) ndup++;
                        for(int g=0; g<NG; g++){
                            int thr=gmin[g]; long long f=0;
                            for(size_t i=0;i<NPX;i++){ int ev=(diffb[i]>=thr); mask[i]=(unsigned char)ev; f+=ev; }
                            gfire[g]+=(double)f/NPX;
                            if(!f){ continue; }                  // dup at this gate: nothing to dilate
                            // 3x3 opening: keep an event only if >=2 of its 8 neighbours fired
                            long long s=0;
                            for(int y=0;y<tH;y++){
                                const unsigned char*m0=mask+(size_t)(y>0?y-1:0)*tW,
                                                   *m1=mask+(size_t)y*tW,
                                                   *m2=mask+(size_t)(y<tH-1?y+1:y)*tW;
                                unsigned char*o=mask2+(size_t)y*tW;
                                for(int x=0;x<tW;x++){
                                    int xl=x>0?x-1:0, xr=x<tW-1?x+1:x;
                                    int nb=m0[xl]+m0[x]+m0[xr]+m1[xl]+m1[xr]+m2[xl]+m2[x]+m2[xr];
                                    int v=(m1[x] && nb>=2); o[x]=(unsigned char)v; s+=v;
                                }
                            }
                            gsupp[g]+=(double)s/NPX;
                            if(s){ gd10[g]+=dilate(mask2,dil,10); gd32[g]+=dilate(mask2,dil,32); }
                        }
                        nframes++;
                    }
                    { unsigned char*t=cur; cur=prv; prv=t; have_prev=1; }
                }
            }
            av_packet_unref(tp);
        }
        av_packet_free(&tp); av_frame_free(&fq);
        long n=nframes>8192?8192:nframes;
        // medians / percentiles (insertion-free: qsort)
        auto cmpd=[](const void*a,const void*b)->int{ double x=*(const double*)a,y=*(const double*)b; return x<y?-1:x>y?1:0; };
        qsort(cov,n,sizeof(double),cmpd); qsort(fmad,n,sizeof(double),cmpd);
        double covsum=0; for(long i=0;i<n;i++) covsum+=cov[i];
        long long ntot=0; for(int k=0;k<8;k++) ntot+=hist[k];
        printf("[deltastats] %s\n", in);
        printf("[deltastats] %dx%d capped, %ld frame-pairs analysed (√2 gate, per-pixel max-channel Δ)\n",tW,tH,nframes);
        printf("[deltastats] dup frames (zero events)        : %ld (%.1f%%)\n",ndup,nframes?100.0*ndup/nframes:0.0);
        printf("[deltastats] event coverage  mean %.1f%%  median %.1f%%  p90 %.1f%%  → unchanged %.1f%%\n",
               n?covsum/n:0.0, n?cov[n/2]:0.0, n?cov[(long)(n*0.9)]:0.0, n?100.0-covsum/n:0.0);
        const char*lbl[8]={"0   ","1   ","2   ","3   ","4-5 ","6-7 ","8-11",">=12"};
        long long cum=0;
        for(int k=0;k<8;k++){ cum+=hist[k];
            printf("[deltastats] |Δ|max %s : %5.1f%%   (cum %5.1f%%)%s\n",
                   lbl[k], ntot?100.0*hist[k]/ntot:0.0, ntot?100.0*cum/ntot:0.0,
                   k==1?"   <- √2 gate: events are everything BELOW this line's cum":""); }
        // gate sweep: processed% (after suppression + dilation) = the fraction of the frame D2's
        // masked inference must still run. Pick the smallest gate whose r=10 processed% is <=60.
        printf("[deltastats] gate sweep (fired → after 3x3 suppression → processed after dilation):\n");
        int best=-1;
        for(int g=0; g<NG; g++){
            double fi=100.0*gfire[g]/(nframes?nframes:1), su=100.0*gsupp[g]/(nframes?nframes:1);
            double d10=100.0*gd10[g]/(nframes?nframes:1), d32=100.0*gd32[g]/(nframes?nframes:1);
            printf("[deltastats]   gate %s : fired %5.1f%%  supp %5.1f%%  →  r10 %5.1f%% (skip %4.1f%%)  r32 %5.1f%% (skip %4.1f%%)\n",
                   gname[g], fi, su, d10, 100.0-d10, d32, 100.0-d32);
            if(best<0 && d10<=60.0) best=g;
        }
        double p10=n?fmad[(long)(n*0.10)]:0.0;
        printf("[deltastats] grain floor (p10 frame MAD)     : %.2f\n", p10);
        if(best>=0) printf("[deltastats] RECOMMENDED gate for D2         : %s (first gate with <=60%% upscale coverage)\n", gname[best]);
        else        printf("[deltastats] RECOMMENDED gate for D2         : none <=60%% — high-motion/grain source, masked inference won't pay here\n");
        free(cur);free(prv);free(mask);free(mask2);free(diffb);free(dil);free(psum);free(cov);free(fmad);
        if(sw) sws_freeContext(sw);
        avcodec_free_context(&vdc); avformat_close_input(&ifmt);
        return 0;
    }
    // ---- PHASE-6 skip decisions FIRST (before ofr / output dims), so the output framerate and
    //      timestamp base reflect the FINAL do_up/do_fg. (A 4K source whose frame-gen is skipped must
    //      NOT declare a 2x framerate.) These only need the source dims, known since the stream open.
    int srcW=vpar->width, srcH=vpar->height;
    // don't x4 an already-high-res source: a 4K source -> 16K is 16x the GPU compute + file size for a
    // resolution no display shows. Skip the upscale model (NOT downscale-then-upscale, so no "cartoonish"
    // regression). In the parent orchestrator this just drops --upscale from child commands (do_up=0).
    if(do_up && noupscale_above>0 && srcH>=noupscale_above){
        fprintf(stderr,"[enh] source %dx%d already >= %dp — skipping x4 upscale (would have been %dx%d)\n",
                srcW,srcH,noupscale_above,srcW*4,srcH*4);
        do_up=0;
    }
    // frame-gen on an already-4K source: RIFE at native 4K is heavy (~7fps), needs single-worker to
    // avoid GPU-contention tile glitches, and doubling a 24fps film is low value / "soap opera". Skipped.
    if(do_fg && noframegen_above>0 && srcH>=noframegen_above){
        fprintf(stderr,"[enh] source %dx%d already >= %dp — skipping frame-gen (heavy/low-value at 4K)\n",
                srcW,srcH,noframegen_above);
        do_fg=0;
    }
    // frame-gen has NO fps ceiling — the model runs whenever it's asked. (Owner policy: don't fence
    // the AI. The degenerate "nothing selected" case was already turned into upscale above.)
    // RATIONAL output framerate (29.97 stays 30000/1001 — integer rounding drifts A/V ~3.6s/hour)
    AVRational ofr = do_fg ? av_mul_q(sfr,(AVRational){2,1}) : sfr;
    // ---- A/V SYNC: output frames carry the REAL source timestamps (PTS passthrough), NOT a synthetic
    //      CFR counter at 2x avg_frame_rate. The old counter assumed avg_frame_rate == the true content
    //      rate; when it was off by even ~0.1% (common in DVD/telecine rips whose containers store a
    //      nominal rate) the video timeline slid away from the audio a few seconds over a feature —
    //      "audio creeps ahead of the picture". Anchoring every emitted frame to its source frame's
    //      best_effort_timestamp makes the enhanced video span EXACTLY the source video's timeline,
    //      so it stays locked to the (stream-copied) audio no matter what the metadata claims.
    AVRational svtb = ifmt->streams[vs]->time_base;   // SOURCE frame timestamps live in this base
    // OUTPUT timestamp base = one tick per OUTPUT frame period (av_inv_q(ofr)). This is >=2x finer than
    // a coarse source base — e.g. AVI/DivX where 1 tick == 1 SOURCE frame — so frame-gen midpoints
    // (prev+cur)/2 always land on a distinct integer tick. (Using the source base directly made AVI
    // midpoints collide; the monotonic guard then stretched each segment to ~2x its true length, so
    // segments overlapped at concat → "non monotonically increasing dts" → progressive enhance failed.)
    AVRational otb = av_inv_q(ofr);
    // fallback OUTPUT ticks per source frame, for the rare frame that decodes with no pts
    int64_t src_frame_ticks = (int64_t)llround((1.0/(sfps>0?sfps:24.0)) / av_q2d(otb));
    if(src_frame_ticks<1) src_frame_ticks=1;
    // segment worker: progress total covers only this worker's window
    double src_dur = ifmt->duration>0 ? ifmt->duration/(double)AV_TIME_BASE : 0;
    if(seg_ss>=0 || seg_to>0){
        double a = seg_ss>0?seg_ss:0, b = (seg_to>0 && (src_dur<=0 || seg_to<src_dur)) ? seg_to : src_dur;
        if(b>a) { total=(long)((b-a)*sfps); if(total<=0) total=1; }
    }

    // ---- DIMS: the model still sees the FULL NATIVE source (no downscale-before-upscale — that caused
    //      the "cartoonish" hallucination). procW/procH = what the AI produces (native x4). Then the
    //      FINAL output is Lanczos-downscaled to the monitor's native height (--target-h) so we don't
    //      emit a pointless 8K file the display can't show (which is slow, huge, and looks worse after
    //      the player shrinks it). Downscale applies ONLY to an actual UPSCALE that OVERSHOOTS the
    //      target — a high-res passthrough is left at its own resolution, never reduced below source.
    if(up_scale<2)up_scale=2; if(up_scale>4)up_scale=4;
    int sc = do_up?up_scale:1;   // model scale (2/3/4); do_up/do_fg finalized above (Phase-6 skips)
    int capW=srcW, capH=srcH;
    capW&=~1; capH&=~1;
    int procW=capW*sc, procH=capH*sc;                   // AI output dims (native x scale)
    int outW=procW, outH=procH;                         // final encoded dims (may be downscaled below)
    if(do_up && target_h>0 && procH>target_h){
        outH=target_h&~1;
        outW=((int)((double)procW*outH/procH+0.5))&~1;
        fprintf(stderr,"[enh] target-res: AI %dx%d -> Lanczos-downscaled to %dx%d (monitor native)\n",procW,procH,outW,outH);
    }

    // ================= PLAY-WHILE-ENHANCING (progressive orchestrator) =================
    // Same GPU-saturating 3-process parallelism as --workers, but over MANY short (~30s) segments
    // instead of 3 giant thirds, and the output is appended IN ORDER the instant each next segment
    // finishes — so the file grows front-to-back and the player can watch it while it renders.
    // The first segment is short (~15s) to minimise the initial buffer wait; after that the 3
    // concurrent renderers (~1.4x realtime aggregate) stay ahead of 1x playback so stalls are rare.
    // Status carries "rendered"/"total" seconds = the player's safe play + seek horizon.
    /* --ss A on a --progressive job = re-anchor: render [A, end] with ABSOLUTE output timestamps
       starting at A, so the player (which seeks the render to wherever you click) can play it on
       the real movie timeline with no offset math. A defaults to 0. */
    double A = seg_ss>0 ? seg_ss : 0.0;
    if(progressive && seg_to<0 && (src_dur - A) > 5.0){
        int conc=3;   // Phase 1: concurrent worker count — finalized by the probe/override below
        // RAMPED segment schedule (from the anchor A): tiny first segments so the player can start
        // within seconds and the buffer fills before playback drains it, then 30s steady-state.
        std::vector<double> bnd; bnd.push_back(A);
        const double ramp[]={8,12,20}; double t=A;
        for(int i=0;i<3 && t<src_dur-1.0;i++){ t+=ramp[i]; if(t<src_dur-1.0) bnd.push_back(t); }
        for(t=bnd.back()+30.0; t<src_dur-1.0; t+=30.0) bnd.push_back(t);
        bnd.push_back(src_dur);
        int nseg=(int)bnd.size()-1;
        if(nseg<2){ progressive=0; }                 // too short after all → fall through to normal
        else {
            fprintf(stderr,"[enh] progressive: %d segments (ramped→30s) over %.0fs (concurrency decided below)\n",nseg,src_dur);
            HANDLE job=CreateJobObjectA(0,0);
            if(job){ JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl; memset(&jl,0,sizeof jl);
                jl.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                SetInformationJobObject(job,JobObjectExtendedLimitInformation,&jl,sizeof jl);
                AssignProcessToJobObject(job,GetCurrentProcess()); }
            char exe[1024]; GetModuleFileNameA(0,exe,sizeof exe);
            std::vector<int> state(nseg,0);          // 0=pending 1=running 2=done 3=appended
            std::vector<PROCESS_INFORMATION> pi(nseg); for(int i=0;i<nseg;i++) memset(&pi[i],0,sizeof(PROCESS_INFORMATION));
            auto segout=[&](int k,char*b,int cap){ snprintf(b,cap,"%s.s%d.mkv",out,k); };
            auto segsta=[&](int k,char*b,int cap){ snprintf(b,cap,"%s.s%d.json",out,k); };
            auto spawn=[&](int k)->bool{
                char wo[1100],ws[1100]; segout(k,wo,sizeof wo); segsta(k,ws,sizeof ws);
                char cmd[4096]; int cl=0;
                cl+=snprintf(cmd+cl,sizeof(cmd)-cl,"\"%s\" --in \"%s\" --out \"%s\" --status \"%s\" --maxh %d --noaudio",
                             exe,in,wo,ws,maxh);
                if(do_up) cl+=snprintf(cmd+cl,sizeof(cmd)-cl," --upscale");
                if(do_fg) cl+=snprintf(cmd+cl,sizeof(cmd)-cl," --framegen");
                if(target_h>0) cl+=snprintf(cmd+cl,sizeof(cmd)-cl," --target-h %d",target_h);
                /* children must inherit the skip thresholds: their defaults (1440/1800) could otherwise
                   re-skip an upscale the parent approved (e.g. 1440p source on a 4K monitor). */
                cl+=snprintf(cmd+cl,sizeof(cmd)-cl," --noupscale-above %d --noframegen-above %d --rife-maxh %d",noupscale_above,noframegen_above,rife_maxh);
                if(upP&&upB) cl+=snprintf(cmd+cl,sizeof(cmd)-cl," --up-param \"%s\" --up-bin \"%s\" --up-scale %d --up-in %s --up-out %s",upP,upB,up_scale,up_in,up_out);
                if(rfP&&rfB) cl+=snprintf(cmd+cl,sizeof(cmd)-cl," --rife-param \"%s\" --rife-bin \"%s\"",rfP,rfB);
                cl+=snprintf(cmd+cl,sizeof(cmd)-cl," --ss %.3f --to %.3f",bnd[k],bnd[k+1]);
                // seam overlap: every segment after the first pre-rolls ONE frame before its start so
                // RIFE can generate the real midpoint bridging it to the previous segment (the cross-seam
                // frame that would otherwise be missing → the mid-scene motion lurch at 8s/20s/...).
                if(k>0 && do_fg) cl+=snprintf(cmd+cl,sizeof(cmd)-cl," --seampreroll");
                STARTUPINFOA si; memset(&si,0,sizeof si); si.cb=sizeof si;
                if(!CreateProcessA(0,cmd,0,0,FALSE,CREATE_NO_WINDOW,0,0,&si,&pi[k])) return false;
                state[k]=1; return true;
            };
            // ---- growing output muxer (header written lazily once segment 0 is ready) ----
            AVFormatContext*om=0; AVStream*cov=0; int coa=-1, hdr=0, failed=0, started=0, appended=0;
            long long cum_frames=0; double last_vsec=A;   // last appended video pts (s) = the play/seek horizon
            int64_t last_vdts=INT64_MIN;                   // strictly-increasing dts guard across all seams
            if(A>0 && as>=0) av_seek_frame(ifmt,-1,(int64_t)(A*AV_TIME_BASE),AVSEEK_FLAG_BACKWARD);  // audio from the anchor
            AVPacket*apk=av_packet_alloc(); int a_have=0, a_eof=(as<0);
            int64_t ast=(as>=0)?ifmt->streams[as]->start_time:0;
            AVRational atb=(as>=0)?ifmt->streams[as]->time_base:(AVRational){1,1000};
            auto a_next=[&](void){ a_have=0;
                while(!a_eof && av_read_frame(ifmt,apk)>=0){
                    if(apk->stream_index!=as){ av_packet_unref(apk); continue; }
                    if(ast!=AV_NOPTS_VALUE && ast>0){ if(apk->pts!=AV_NOPTS_VALUE)apk->pts-=ast; if(apk->dts!=AV_NOPTS_VALUE)apk->dts-=ast; }
                    /* anchored render: the BACKWARD seek overshoots to a keyframe before A — drop the
                       pre-anchor audio so audio starts with the video, not ~seconds ahead of it. */
                    if(A>0 && apk->pts!=AV_NOPTS_VALUE && apk->pts*av_q2d(atb) < A-0.03){ av_packet_unref(apk); continue; }
                    a_have=1; return; }
                a_eof=1; };
            // append one finished segment file's video (+ interleave source audio up to it), in order
            auto append_seg=[&](int k)->int{
                char wo[1100]; segout(k,wo,sizeof wo);
                AVFormatContext*sk=0;
                if(avformat_open_input(&sk,wo,0,0)<0) return -1;
                if(avformat_find_stream_info(sk,0)<0){ avformat_close_input(&sk); return -1; }
                int kv=av_find_best_stream(sk,AVMEDIA_TYPE_VIDEO,-1,-1,0,0);
                if(kv<0){ avformat_close_input(&sk); return -1; }
                if(!hdr){                             // lazily create the output from segment 0's params
                    if(avformat_alloc_output_context2(&om,0,"matroska",out)<0||!om){ avformat_close_input(&sk); return -1; }
                    om->flush_packets=1;              // keep the growing file promptly tail-readable
                    cov=avformat_new_stream(om,0);
                    avcodec_parameters_copy(cov->codecpar,sk->streams[kv]->codecpar);
                    cov->codecpar->codec_tag=0; cov->time_base=sk->streams[kv]->time_base;
                    if(as>=0){ AVStream*oa=avformat_new_stream(om,0);
                        avcodec_parameters_copy(oa->codecpar,ifmt->streams[as]->codecpar);
                        oa->codecpar->codec_tag=0; oa->time_base=ifmt->streams[as]->time_base; coa=oa->index; }
                    if(!(om->oformat->flags&AVFMT_NOFILE) && avio_open(&om->pb,out,AVIO_FLAG_WRITE)<0){ avformat_close_input(&sk); return -1; }
                    if(avformat_write_header(om,0)<0){ avformat_close_input(&sk); return -1; }
                    if(as>=0) a_next();
                    hdr=1;
                }
                // segment files already carry ABSOLUTE source timestamps (re-anchored render → they
                // start at A), so append them verbatim — no per-segment offset, no drift accumulation.
                AVPacket*vpk=av_packet_alloc(); long long segf=0; int rr=0;
                while(av_read_frame(sk,vpk)>=0){
                    if(vpk->stream_index!=kv){ av_packet_unref(vpk); continue; }
                    av_packet_rescale_ts(vpk,sk->streams[kv]->time_base,cov->time_base);
                    // seam guard: ms-rounding can tie/invert a segment's first dts vs the prior
                    // segment's last — nudge it up so the muxer never sees non-monotonic dts.
                    if(vpk->dts!=AV_NOPTS_VALUE){
                        if(vpk->dts<=last_vdts){ int64_t sh=last_vdts+1-vpk->dts;
                            vpk->dts+=sh; if(vpk->pts!=AV_NOPTS_VALUE) vpk->pts+=sh; }
                        last_vdts=vpk->dts; }
                    double vsec = vpk->pts!=AV_NOPTS_VALUE ? vpk->pts*av_q2d(cov->time_base) : 0;
                    if(vsec>last_vsec) last_vsec=vsec;
                    while(a_have && apk->pts!=AV_NOPTS_VALUE && apk->pts*av_q2d(atb)<=vsec){
                        AVPacket*ac=av_packet_clone(apk); ac->stream_index=coa;
                        av_packet_rescale_ts(ac,atb,om->streams[coa]->time_base);
                        if(av_interleaved_write_frame(om,ac)<0) rr=-1;
                        av_packet_free(&ac); av_packet_unref(apk); a_next();
                    }
                    vpk->stream_index=cov->index;
                    if(av_interleaved_write_frame(om,vpk)<0){ rr=-1; av_packet_unref(vpk); break; }
                    segf++;
                }
                av_packet_free(&vpk); avformat_close_input(&sk);
                cum_frames+=segf;
                av_write_frame(om,0); if(om->pb) avio_flush(om->pb);   // flush so the tail is readable NOW
                return rr;
            };
            // ---- Phase 1: decide concurrent worker count (probe-then-scale, VRAM-capped) ----
            // Measured: 1 stream ~50% GPU at any res; ~2-3 saturate compute; VRAM ~1GB per source
            // megapixel. So VRAM is the real limiter (4K needs 2, not 3). Override: --workers-force
            // N or env HOGS_WORKERS bypasses the probe.
            int force = workers_force>0 ? workers_force : 0;
            { const char*fe=getenv("HOGS_WORKERS"); if(!force && fe) force=atoi(fe); }
            if(force>0){
                conc = force<1?1:(force>8?8:force);
                fprintf(stderr,"[enh] worker count FORCED to %d\n",conc);
                for(int k=0;k<nseg && k<conc && !failed;k++){ if(!spawn(k)){failed=1;break;} started=k+1; }
            } else {
                // VRAM budget = 90% of the card's ACTUAL total VRAM (vendor-neutral DXGI query), so
                // this adapts to any GPU (8GB NVIDIA, 4GB Intel, 24GB AMD...). Fallback 8000MB is
                // conservative (won't overcommit an unknown card); escape hatch stays --workers-force.
                double total_vram = gpu_total_vram_mb();
                const double VRAM_BUDGET_MB = total_vram>512 ? total_vram*0.90 : 8000.0;
                const int    COMPUTE_CAP=3;          // beyond ~2-3 streams just queues (compute-bound)
                fprintf(stderr,"[enh] GPU total VRAM=%.0fMB -> budget %.0fMB\n",total_vram,VRAM_BUDGET_MB);
                double base_vram = gpu_vram_used_mb();               // VRAM in use before any worker
                if(!spawn(0)){ failed=1; }
                else {
                    started=1;
                    double peak=0;                                   // watch worker 0 reach steady VRAM, stay alive
                    /* 2026-10-27: was a fixed 6s window. RealESRGAN_x4plus (archive) is still loading at 6s —
                       the probe saw 667MB/worker, launched 3, and the real ~19.5GB each overran the 24GB card:
                       "progressive enhance failed". Now wait (up to 60s) until usage has clearly risen AND
                       held steady for 4s. */
                    int stable=0;
                    for(int s=0;s<120 && !failed;s++){
                        Sleep(500);
                        DWORD ec=STILL_ACTIVE; GetExitCodeProcess(pi[0].hProcess,&ec);
                        if(ec!=STILL_ACTIVE){ if(ec!=0) failed=1; break; }  // finished/crashed during probe
                        double v=gpu_vram_used_mb();
                        if(v>peak+50){ peak=v; stable=0; } else stable++;
                        if(s>=11 && stable>=8 && (base_vram<=0 || peak>base_vram+300)) break;
                    }
                    double per = (base_vram>0 && peak>base_vram) ? peak-base_vram : 2700.0; // fallback ~1080p footprint
                    if(per<300) per=300;
                    double budget = base_vram>0 ? VRAM_BUDGET_MB-base_vram : VRAM_BUDGET_MB;
                    /* FRAME-GEN ONLY is not compute-bound (measured 2026-10-27, 720p, 4-min clip: 1 worker 30% GPU
                       0.9x realtime; 3 -> 36% 1.4x; 6 -> 40% 1.7x) — per-frame CPU conversions + synchronous small
                       RIFE submits dominate, so more parallel segments keep helping. Upscaling jobs saturate the
                       GPU (88-93% measured) and stay at 3. 1080p 400s: 6 workers 42% GPU/64% CPU, 8 -> 47%/72% (+9%);
                       beyond that the CPU (HEVC decode + per-frame conversions) is the limit. */
                    int cap = (do_fg && !do_up) ? 8 : COMPUTE_CAP;
                    conc = (int)(budget/per); if(conc<1)conc=1; if(conc>cap)conc=cap;
                    // High-res FRAME-GEN safety: heavy RIFE at native 4K run by MULTIPLE concurrent
                    // workers corrupts GPU tiles (black rectangles on midpoints — measured: 1 worker
                    // Correctness before speed; override: --workers-force.
                    if(do_fg){
                        // STANDALONE-RIFE CONTENTION: multiple concurrent worker processes each running
                        // the separate ncnn_rife path corrupt GPU tiles (hard black rectangles on
                        // midpoints) — measured at native 4K AND at capped 1440p working res; single
                        // worker is always clean. Only the GPU-resident CHAIN (up+fg, x4 data/output,
                        // output <=4K — RIFE recorded in the same command buffer) is contention-safe
                        // multi-worker. So: if this job will run standalone RIFE, force 1 worker.
                        /* 2026-10-27 re-measure: FRAME-GEN ONLY with 3 and 6 concurrent workers produced exactly the
                           same frame count as 1 worker (11578 frames, 4-min 720p) — zero black midpoints caught by
                           the guard. (Cause of the old corruption unconfirmed — the NULL pack8-pipeline fix in
                           warp.cpp is a likely candidate.) So
                           frame-gen-only runs multi-worker again (the black-midpoint guard stays as the safety net).
                           Upscale + standalone RIFE (non-x4 models) is not re-measured: still 1 worker. */
                        int chain_ok = do_up && up_scale==4 && capH*4<=4400;   // any x4 ESRGAN-family model (blob names passed through)
                        if(do_up && !chain_ok && conc>1){ conc=1;
                            fprintf(stderr,"[enh] upscale + standalone-RIFE framegen: 1 worker (multi-worker unverified for this path)\n"); }
                    }
                    fprintf(stderr,"[enh] probe: base=%.0fMB peak=%.0fMB per-worker=%.0fMB budget=%.0fMB -> %d concurrent\n",
                            base_vram,peak,per,budget,conc);
                    for(int k=started;k<nseg && k<conc && !failed;k++){ if(!spawn(k)){failed=1;break;} started=k+1; }
                }
            }
            char ph[80]; snprintf(ph,sizeof ph,"%s%s%s ·live", do_up?"Upscaling":"", (do_up&&do_fg)?"+":"", do_fg?"Frame-gen":"");
            PerfLogger plog; plog.init(perflog);   // Phase-0: resource CSV (no-op if --perflog absent)
            while(!failed && appended<nseg){
                for(int k=0;k<started;k++) if(state[k]==1){
                    DWORD ec=STILL_ACTIVE; GetExitCodeProcess(pi[k].hProcess,&ec);
                    if(ec!=STILL_ACTIVE){ CloseHandle(pi[k].hProcess); CloseHandle(pi[k].hThread);
                        if(ec!=0) failed=1; else state[k]=2;
                        if(!failed && started<nseg){ if(!spawn(started)) failed=1; else started++; } }
                }
                while(!failed && appended<nseg && state[appended]==2){
                    if(append_seg(appended)){ failed=1; break; }
                    char sf[1100]; segout(appended,sf,sizeof sf); remove(sf);
                    char sj[1100]; segsta(appended,sj,sizeof sj); remove(sj);
                    state[appended]=3; appended++;
                    double rendered = last_vsec;   // absolute time available = last appended video pts
                    write_status_prog(appended*100/nseg>99?99:appended*100/nseg, ph,
                                      appended>=nseg?0:0, rendered, src_dur);
                }
                { int live=0; for(int k=0;k<started;k++) if(state[k]==1) live++;
                  plog.tick(live,last_vsec); }
                if(!failed && appended<nseg) Sleep(150);
            }
            plog.close();
            // finalize: trailing audio + trailer
            if(!failed && hdr){
                while(a_have){ AVPacket*ac=av_packet_clone(apk); ac->stream_index=coa;
                    av_packet_rescale_ts(ac,atb,om->streams[coa]->time_base);
                    if(av_interleaved_write_frame(om,ac)<0){ failed=1; av_packet_free(&ac); break; }
                    av_packet_free(&ac); av_packet_unref(apk); a_next(); }
                if(!failed) av_write_trailer(om);
            }
            if(om){ if(!(om->oformat->flags&AVFMT_NOFILE)&&om->pb) avio_closep(&om->pb); avformat_free_context(om); }
            av_packet_free(&apk);
            for(int k=0;k<nseg;k++){                  // reap survivors + sweep temps on failure/cancel
                if(state[k]==1){ TerminateProcess(pi[k].hProcess,1); WaitForSingleObject(pi[k].hProcess,5000);
                                 CloseHandle(pi[k].hProcess); CloseHandle(pi[k].hThread); }
                char sf[1100]; segout(k,sf,sizeof sf); remove(sf);
                char sj[1100]; segsta(k,sj,sizeof sj); remove(sj);
            }
            avformat_close_input(&ifmt);
            if(failed){ write_status(0,"error",1,"progressive enhance failed"); remove(out); fprintf(stderr,"[enh] FAILED (progressive)\n"); return 3; }
            write_status_prog(100,"Done",1,src_dur,src_dur);
            fprintf(stderr,"[enh] done -> %s (progressive, %d segments, %lld frames)\n",out,nseg,cum_frames);
            return 0;
        }
    }

    // ================= PARALLEL SEGMENT WORKERS (DELTA D2b-3) =================
    // One GPU inference stream leaves a 7900 XTX mostly idle (small kernels, serial submits).
    // --workers N splits the file into N time-segments, runs N copies of this exe on them
    // concurrently (each its own Vulkan queue + encoder), then concatenates the segment videos
    // losslessly and muxes the source audio back in. Seams: each segment after the first emits
    // its first frame twice (--seamfill) so the 2x frame-gen cadence stays exact — equivalent
    // to a scene-cut duplicate, invisible. Children live in a kill-on-close job object, so the
    // player cancelling the parent still reaps every worker.
    if(workers>1 && seg_ss<0 && seg_to<0){
        if(src_dur < 20.0*workers) workers = src_dur>40 ? (int)(src_dur/20) : 1;  // short file: fewer/none
        if(workers>1){
            fprintf(stderr,"[enh] %d parallel segment workers over %.0fs\n",workers,src_dur);
            HANDLE job=CreateJobObjectA(0,0);
            if(job){ JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl; memset(&jl,0,sizeof jl);
                jl.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                SetInformationJobObject(job,JobObjectExtendedLimitInformation,&jl,sizeof jl);
                AssignProcessToJobObject(job,GetCurrentProcess()); }   // children inherit → die with us
            char exe[1024]; GetModuleFileNameA(0,exe,sizeof exe);
            double seg=src_dur/workers;
            PROCESS_INFORMATION pis[8]; memset(pis,0,sizeof pis);
            char wout[8][1100], wst[8][1100];
            int spawn_fail=0;
            for(int k=0;k<workers;k++){
                snprintf(wout[k],sizeof wout[k],"%s.w%d.mkv",out,k);
                snprintf(wst[k], sizeof wst[k], "%s.w%d.json",out,k);
                char cmd[4096]; int cl=0;
                cl+=snprintf(cmd+cl,sizeof(cmd)-cl,"\"%s\" --in \"%s\" --out \"%s\" --status \"%s\" --maxh %d --noaudio",
                             exe,in,wout[k],wst[k],maxh);
                if(do_up) cl+=snprintf(cmd+cl,sizeof(cmd)-cl," --upscale");
                if(do_fg) cl+=snprintf(cmd+cl,sizeof(cmd)-cl," --framegen");
                if(target_h>0) cl+=snprintf(cmd+cl,sizeof(cmd)-cl," --target-h %d",target_h);
                /* children must inherit the skip thresholds: their defaults (1440/1800) could otherwise
                   re-skip an upscale the parent approved (e.g. 1440p source on a 4K monitor). */
                cl+=snprintf(cmd+cl,sizeof(cmd)-cl," --noupscale-above %d --noframegen-above %d --rife-maxh %d",noupscale_above,noframegen_above,rife_maxh);
                if(upP&&upB) cl+=snprintf(cmd+cl,sizeof(cmd)-cl," --up-param \"%s\" --up-bin \"%s\" --up-scale %d --up-in %s --up-out %s",upP,upB,up_scale,up_in,up_out);
                if(rfP&&rfB) cl+=snprintf(cmd+cl,sizeof(cmd)-cl," --rife-param \"%s\" --rife-bin \"%s\"",rfP,rfB);
                cl+=snprintf(cmd+cl,sizeof(cmd)-cl," --ss %.3f",k*seg);
                if(k<workers-1) cl+=snprintf(cmd+cl,sizeof(cmd)-cl," --to %.3f",(k+1)*seg);
                // seam overlap: pre-roll one frame in every segment after the first so RIFE can make
                // the real cross-seam midpoint (see the progressive path for the full rationale).
                if(k>0 && do_fg) cl+=snprintf(cmd+cl,sizeof(cmd)-cl," --seampreroll");
                STARTUPINFOA si; memset(&si,0,sizeof si); si.cb=sizeof si;
                if(!CreateProcessA(0,cmd,0,0,FALSE,CREATE_NO_WINDOW,0,0,&si,&pis[k])){ spawn_fail=1; break; }
            }
            int failed=spawn_fail;
            if(!spawn_fail){
                // poll worker status files -> aggregate progress; watch for early worker death
                char ph[80]; snprintf(ph,sizeof ph,"%s%s%s ·%dx", do_up?"Upscaling":"", (do_up&&do_fg)?"+":"", do_fg?"Frame-gen":"", workers);
                PerfLogger plog; plog.init(perflog);   // Phase-0: resource CSV (no-op if --perflog absent)
                for(;;){
                    int done=0, agg=0;
                    for(int k=0;k<workers;k++){
                        DWORD ec=STILL_ACTIVE; GetExitCodeProcess(pis[k].hProcess,&ec);
                        if(ec!=STILL_ACTIVE){ done++; if(ec!=0) failed=1; agg+=100; continue; }
                        FILE*f=fopen(wst[k],"rb");
                        if(f){ char buf[256]; size_t n=fread(buf,1,255,f); buf[n]=0; fclose(f);
                               int p=0; if(sscanf(buf,"{\"percent\":%d",&p)==1) agg+=p; }
                    }
                    write_status(agg/workers>99?99:agg/workers, ph, 0, "");
                    plog.tick(workers-done, agg/(double)workers);
                    if(done==workers || failed) break;
                    Sleep(300);
                }
                plog.close();
                for(int k=0;k<workers;k++){                     // reap everything (kill leftovers on failure)
                    if(failed) TerminateProcess(pis[k].hProcess,1);
                    WaitForSingleObject(pis[k].hProcess,failed?5000:INFINITE);
                    DWORD ec=1; GetExitCodeProcess(pis[k].hProcess,&ec); if(ec!=0) failed=1;
                    CloseHandle(pis[k].hProcess); CloseHandle(pis[k].hThread);
                }
            }
            // ---- lossless concat of the segment videos + source audio ----
            if(!failed){
                write_status(99,"Joining",0,"");
                AVFormatContext*om=0; AVFormatContext*s0=0;
                int cvs=-1, r=0;
                if(avformat_open_input(&s0,wout[0],0,0)<0 || avformat_find_stream_info(s0,0)<0) r=-1;
                if(!r){ cvs=av_find_best_stream(s0,AVMEDIA_TYPE_VIDEO,-1,-1,0,0); if(cvs<0) r=-1; }
                if(!r && (avformat_alloc_output_context2(&om,0,"matroska",out)<0 || !om)) r=-1;
                AVStream*cov=0; int coa=-1;
                if(!r){
                    cov=avformat_new_stream(om,0);
                    avcodec_parameters_copy(cov->codecpar,s0->streams[cvs]->codecpar);
                    cov->codecpar->codec_tag=0; cov->time_base=s0->streams[cvs]->time_base;
                    if(as>=0){ AVStream*oa=avformat_new_stream(om,0);
                        avcodec_parameters_copy(oa->codecpar,ifmt->streams[as]->codecpar);
                        oa->codecpar->codec_tag=0; oa->time_base=ifmt->streams[as]->time_base; coa=oa->index; }
                    if(!(om->oformat->flags&AVFMT_NOFILE) && avio_open(&om->pb,out,AVIO_FLAG_WRITE)<0) r=-1;
                    if(!r && avformat_write_header(om,0)<0) r=-1;
                }
                avformat_close_input(&s0);
                // audio iterator over the SOURCE (parent never read it, so it's still at the top)
                AVPacket*apk=av_packet_alloc(); int a_have=0, a_eof=(as<0);
                int64_t ast = (as>=0)?ifmt->streams[as]->start_time:0;
                AVRational atb = (as>=0)?ifmt->streams[as]->time_base:(AVRational){1,1000};
                auto a_next=[&](void){
                    a_have=0;
                    while(!a_eof && av_read_frame(ifmt,apk)>=0){
                        if(apk->stream_index!=as){ av_packet_unref(apk); continue; }
                        if(ast!=AV_NOPTS_VALUE && ast>0){ if(apk->pts!=AV_NOPTS_VALUE)apk->pts-=ast; if(apk->dts!=AV_NOPTS_VALUE)apk->dts-=ast; }
                        a_have=1; return;
                    }
                    a_eof=1;
                };
                if(!a_eof) a_next();
                // Each segment file already carries ABSOLUTE source timestamps (PTS passthrough), so
                // segments are contiguous on the real timeline — just copy them through, no per-segment
                // frame-count offset. (max_b_frames=0 keeps pts==dts, so the joins stay monotonic.)
                AVPacket*vpk=av_packet_alloc(); int64_t last_vdts=INT64_MIN;
                for(int k=0;k<workers && !r;k++){
                    AVFormatContext*sk=0;
                    if(avformat_open_input(&sk,wout[k],0,0)<0){ r=-1; break; }
                    int kv=av_find_best_stream(sk,AVMEDIA_TYPE_VIDEO,-1,-1,0,0);
                    if(kv<0){ avformat_close_input(&sk); r=-1; break; }
                    while(av_read_frame(sk,vpk)>=0){
                        if(vpk->stream_index!=kv){ av_packet_unref(vpk); continue; }
                        av_packet_rescale_ts(vpk,sk->streams[kv]->time_base,cov->time_base);
                        if(vpk->dts!=AV_NOPTS_VALUE){          // strictly-increasing dts across seams
                            if(vpk->dts<=last_vdts){ int64_t sh=last_vdts+1-vpk->dts;
                                vpk->dts+=sh; if(vpk->pts!=AV_NOPTS_VALUE) vpk->pts+=sh; }
                            last_vdts=vpk->dts; }
                        double vsec = vpk->pts!=AV_NOPTS_VALUE ? vpk->pts*av_q2d(cov->time_base) : 0;
                        // interleave: source audio up to this video timestamp
                        while(a_have && apk->pts!=AV_NOPTS_VALUE && apk->pts*av_q2d(atb)<=vsec){
                            AVPacket*ac=av_packet_clone(apk); ac->stream_index=coa;
                            av_packet_rescale_ts(ac,atb,om->streams[coa]->time_base);
                            if(av_interleaved_write_frame(om,ac)<0) r=-1;
                            av_packet_free(&ac); av_packet_unref(apk); a_next();
                        }
                        vpk->stream_index=cov->index;
                        if(av_interleaved_write_frame(om,vpk)<0){ r=-1; av_packet_unref(vpk); break; }
                    }
                    avformat_close_input(&sk);
                }
                while(!r && a_have){                             // trailing audio after the last video packet
                    AVPacket*ac=av_packet_clone(apk); ac->stream_index=coa;
                    av_packet_rescale_ts(ac,atb,om->streams[coa]->time_base);
                    if(av_interleaved_write_frame(om,ac)<0) r=-1;
                    av_packet_free(&ac); av_packet_unref(apk); a_next();
                }
                av_packet_free(&vpk); av_packet_free(&apk);
                if(om){ if(!r) av_write_trailer(om);
                        if(!(om->oformat->flags&AVFMT_NOFILE)&&om->pb) avio_closep(&om->pb);
                        avformat_free_context(om); }
                if(r) failed=1;
            }
            for(int k=0;k<workers;k++){ remove(wout[k]); remove(wst[k]); }   // segment temps
            avformat_close_input(&ifmt);
            if(failed){ write_status(0,"error",1,"a segment worker failed"); fprintf(stderr,"[enh] FAILED (workers)\n"); remove(out); return 3; }
            write_status(100,"Done",1,"");
            fprintf(stderr,"[enh] done -> %s (%d workers)\n",out,workers);
            return 0;
        }
    }


    // ---- output muxer + H.265 video encoder + audio stream copy ----
    AVFormatContext*ofmt=0;
    if(avformat_alloc_output_context2(&ofmt,0,"matroska",out)<0||!ofmt) FAIL("alloc output ctx");
    // HARDWARE encoder first, whichever GPU the machine has (100+fps at 1440p vs ~15fps CPU libx265 —
    // the difference between "faster than the video" and "much slower"). Try AMD → NVIDIA → Intel,
    // then CPU libx265 as the universal fallback. Any that can't open (wrong GPU / option mismatch)
    // is skipped, so this runs on any Windows box. AI UPSCALE (ncnn Vulkan) already runs on any GPU.
    int gop=(int)(av_q2d(ofr)+0.5); if(gop<1)gop=24;     // ~1s GOP: snappy seeking in the enhanced file
    AVCodecContext*vec=0; const AVCodec*venc=0;
    const char* tryenc[] = { "hevc_amf", "hevc_nvenc", "hevc_qsv", "libx265", 0 };
    for(int t=0; tryenc[t] && !vec; t++){
        const char*name = tryenc[t];
        const AVCodec*c=avcodec_find_encoder_by_name(name);
        if(!c && !strcmp(name,"libx265")) c=avcodec_find_encoder(AV_CODEC_ID_HEVC);   // any HEVC as last resort
        if(!c) continue;
        AVCodecContext*e=avcodec_alloc_context3(c);
        e->width=outW; e->height=outH; e->pix_fmt=AV_PIX_FMT_YUV420P;
        e->sample_aspect_ratio=vpar->sample_aspect_ratio;  // preserve SAR (anamorphic sources display correctly)
        e->time_base=otb; e->framerate=ofr;              // timestamps = real source PTS (passthrough, rescaled)
        e->gop_size=gop;
        e->max_b_frames=0;                               // no reordering: pts==dts, so absolute-PTS
                                                         // segment concat can't produce non-monotonic dts
        if(ofmt->oformat->flags & AVFMT_GLOBALHEADER) e->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
        if(!strcmp(name,"hevc_amf")){                      // AMD AMF
            av_opt_set(e->priv_data,"usage","transcoding",0); av_opt_set(e->priv_data,"quality","quality",0);
            av_opt_set(e->priv_data,"rc","cqp",0); av_opt_set(e->priv_data,"qp_i","22",0); av_opt_set(e->priv_data,"qp_p","24",0);
        } else if(!strcmp(name,"hevc_nvenc")){             // NVIDIA NVENC
            av_opt_set(e->priv_data,"preset","p5",0); av_opt_set(e->priv_data,"tune","hq",0);
            av_opt_set(e->priv_data,"rc","constqp",0); av_opt_set(e->priv_data,"qp","23",0);
        } else if(!strcmp(name,"hevc_qsv")){               // Intel Quick Sync
            av_opt_set(e->priv_data,"preset","medium",0); e->global_quality=23; e->flags|=AV_CODEC_FLAG_QSCALE;
        } else {                                           // CPU libx265
            av_opt_set(e->priv_data,"preset","medium",0); av_opt_set(e->priv_data,"crf","20",0);
            char xp[80]; snprintf(xp,sizeof xp,"keyint=%d:min-keyint=%d:no-open-gop=1",gop,gop/2>1?gop/2:1);
            av_opt_set(e->priv_data,"x265-params",xp,0);
        }
        if(avcodec_open2(e,c,0)==0){ vec=e; venc=c; fprintf(stderr,"[enh] encoder: %s\n",name); }
        else avcodec_free_context(&e);
    }
    if(!vec) FAIL("no H.265 encoder (amf/nvenc/qsv/libx265)");
    AVStream*ovs=avformat_new_stream(ofmt,0); avcodec_parameters_from_context(ovs->codecpar,vec); ovs->time_base=vec->time_base;
    int oas=-1;
    if(as>=0){   // copy the audio stream through (no re-encode)
        AVStream*oa=avformat_new_stream(ofmt,0);
        avcodec_parameters_copy(oa->codecpar,ifmt->streams[as]->codecpar);
        oa->codecpar->codec_tag=0; oa->time_base=ifmt->streams[as]->time_base; oas=oa->index;
    }
    if(!(ofmt->oformat->flags&AVFMT_NOFILE) && avio_open(&ofmt->pb,out,AVIO_FLAG_WRITE)<0) FAIL("cannot open output file");
    if(avformat_write_header(ofmt,0)<0) FAIL("cannot write header");

    // ---- ncnn (Vulkan compute; no window) ----
    NcnnVkHandles hh; if(ncnn_vk_init(&hh)) FAIL("ncnn init failed");
    // GPU-RESIDENT CHAIN for combined mode: the upscaled frame stays on the GPU as RIFE's input —
    // no CPU round-trip between the nets. Needs the SOURCE padded to /16 (x4 output is then /64,
    // RIFE v4.26's alignment); the pad is replicated edge pixels, cropped away at encode time.
    int use_chain=0, BW=capW, BH=capH;
    // The GPU-resident chain hardcodes ESRGAN x4 + data/output + /16→/64 alignment, so only use it for
    // that model. Other models (CUGAN x2/x3, custom blobs) take the separate upscale→RIFE path.
    // (also: chain RIFEs at the FULL 4x res on-GPU with no way to cap it — beyond ~4K output that is
    // prohibitively slow/VRAM-hungry (x1-restore on a 4K source = 16K RIFE), so route those through
    // the separate path where --rife-maxh applies.)
    // 2026-10-27: the upscaler's blob names + scale are passed through to the chain, and the source pad
    // is /(64/scale) so the upscaled frame is /64. Archive is now always RealESRGAN_x4plus (x4) -> chain.
    int esrgan_x4 = (up_scale==4 && capH*4<=4400);   // x2 support is plumbed (pad/ts) but untested — keep x4 only
    if(do_up && do_fg && esrgan_x4){
        use_chain = (ncnn_chain_init(upP,upB,rfP,rfB,up_in,up_out,up_scale)==0);
        int al = 64/up_scale;
        if(use_chain){ BW=(capW+al-1)/al*al; BH=(capH+al-1)/al*al; fprintf(stderr,"[enh] GPU-resident chain on (%dx%d padded, x%d %s/%s)\n",BW,BH,up_scale,up_in,up_out); }
        else fprintf(stderr,"[enh] chain init failed — falling back to the separate-pass path\n");
    } else if(do_up && do_fg){
        fprintf(stderr,"[enh] non-ESRGAN model (%s, x%d) — using separate upscale→RIFE path\n",up_in,up_scale);
    }

    AVPacket*pkt=av_packet_alloc(); AVFrame*fr=av_frame_alloc();
    SwsContext*sws_dn=0, *sws_rgb2yuv=0;
    SwsContext*sws_rd=0, *sws_ru=0;                      // RIFE working-res down (GPU thread) / spare
    unsigned char*rifeA=0,*rifeB=0;                      // reusable RIFE input pair at working res
    SwsContext*w_sws_mu=0; unsigned char*w_midfull=0;    // WRITER-side midpoint upscale (QT_MIDS)
    unsigned char*prevUp=0; int64_t prevUp_pts=AV_NOPTS_VALUE; int UW=0,UH=0; long emitted=0, decoded=0;
    int seam_skip_prev=seampreroll;   // separate-pass: skip the pre-roll anchor's base emit (once)
    long n_interp=0, n_faildup=0;                        // frame-gen diagnostics
    // NO fences around the models (owner policy). Every frame is upscaled and every pair is RIFE'd; no
    // duplicate-skipping, no scene-cut special-casing, no inspecting/gating/vetoing what goes into or
    // comes out of the models. Genuinely repeated SOURCE frames still look held (RIFE of a frame against
    // its own copy returns that frame — correct, and the source's doing).
    long chain_sent=0;                                   // chain frames submitted (first has no mid)
    time_t t_start=time(0);
    AVFrame*yf=av_frame_alloc(); yf->format=AV_PIX_FMT_YUV420P; yf->width=outW; yf->height=outH;
    av_frame_get_buffer(yf,32);
    fprintf(stderr,"[enh] %dx%d src -> %dx%d out (up=%d fg=%d) %.2f->%.2ffps, ~%ld frames\n",
            srcW,srcH,outW,outH,do_up,do_fg,sfps,av_q2d(ofr),total);

    // ---- deinterlacer: yadif with deint=interlaced (progressive frames pass through untouched).
    //      Old DVDs are interlaced; combed fields fed to the AI models = serrated upscale artifacts
    //      and impossible-motion glitches in RIFE. Graph is built lazily from the first frame's props.
    AVFilterGraph*fg_graph=0; AVFilterContext*fg_src=0,*fg_sink=0; int deint_ok=0, deint_tried=0;
    AVFrame*ff=av_frame_alloc();
    auto deint_init=[&](AVFrame*f)->int{
        char args[256];
        fg_graph=avfilter_graph_alloc(); if(!fg_graph) return -1;
        AVRational tb=ifmt->streams[vs]->time_base;
        snprintf(args,sizeof args,"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                 f->width,f->height,f->format,tb.num,tb.den,
                 f->sample_aspect_ratio.num?f->sample_aspect_ratio.num:1,
                 f->sample_aspect_ratio.den?f->sample_aspect_ratio.den:1);
        if(avfilter_graph_create_filter(&fg_src,avfilter_get_by_name("buffer"),"in",args,0,fg_graph)<0) return -1;
        if(avfilter_graph_create_filter(&fg_sink,avfilter_get_by_name("buffersink"),"out",0,0,fg_graph)<0) return -1;
        AVFilterInOut*out_io=avfilter_inout_alloc(),*in_io=avfilter_inout_alloc();
        out_io->name=av_strdup("in");  out_io->filter_ctx=fg_src;  out_io->pad_idx=0; out_io->next=0;
        in_io ->name=av_strdup("out"); in_io ->filter_ctx=fg_sink; in_io ->pad_idx=0; in_io ->next=0;
        int r=avfilter_graph_parse_ptr(fg_graph,"yadif=mode=send_frame:parity=auto:deint=interlaced",&in_io,&out_io,0);
        avfilter_inout_free(&in_io); avfilter_inout_free(&out_io);
        if(r<0) return -1;
        if(avfilter_graph_config(fg_graph,0)<0) return -1;
        return 0;
    };
    { char ph[64]; snprintf(ph,sizeof ph,"%s%s%s", do_up?"Upscaling":"", (do_up&&do_fg)?"+":"", do_fg?"Frame-gen":""); write_status(0,ph,0,""); }

    // ---- PIPELINED ENCODER: color-convert + H.265 encode + mux run on a WORKER thread so the
    //      GPU/decoder never wait on them. A bounded queue carries video frames AND audio packets
    //      in production order (single consumer = interleaving preserved, no libav locking needed).
    // QT_CHAIN carries the GPU chain's raw output holder: the writer does the ~20ms/frame
    // denormalize+to_pixels there, OVERLAPPED with the next frame's GPU inference (this was the
    // serial stall behind "GPU 2%, CPU 10% during processing").
    enum { QCAP=6, QT_VIDEO=0, QT_AUDIO=1, QT_STOP=2, QT_CHAIN=3, QT_UP=4, QT_MIDS=5 };  // QT_UP: pure-upscale raw holder (Phase 2); QT_MIDS: small RIFE midpoint, writer upscales
    struct QItem { int type; unsigned char*rgb; AVPacket*pkt; void*holder; int64_t pts; int w,h; };
    QItem q[QCAP]; int qh=0,qt=0,qn=0;
    std::mutex qm; std::condition_variable cv_push, cv_pop;
    std::atomic<int> enc_err(0);
    // worker-side: one RGB frame -> yuv -> encoder -> muxer (stridePx >= outW; sws crops the pad)
    int64_t last_pts_written = INT64_MIN;                // guard: keep the encoder input strictly monotone
    // SEAM-GLITCH FIX: a --seampreroll worker's 1.5-frame pre-roll window can catch TWO anchor frames
    // (timing-phase dependent); the old "skip the first base emit once" then let the extra anchor
    // through as a DUPLICATE of the previous segment's last frame → the ~every-30s (and 8s/20s ramp)
    // frame jump. Hard rule instead: NOTHING with a pts earlier than ~half a frame before this
    // segment's start is ever emitted — pre-roll frames are RIFE anchors only, never output.
    int64_t emit_floor_pts = INT64_MIN;
    if(seampreroll && seg_ss>0)
        emit_floor_pts = (int64_t)llround((seg_ss - 0.5/(sfps>0?sfps:24.0)) / av_q2d(otb));
    // fraction of near-black pixels in an RGB buffer (sampled every 4px for speed). Used to catch
    // RIFE midpoints that came back corrupt as hard black rectangles — a measured GPU failure mode.
    auto img_blackfrac=[&](const unsigned char*rgb,int stridePx,int w,int h)->double{
        long tot=0,blk=0;
        for(int y=0;y<h;y+=4) for(int x=0;x<w;x+=4){
            const unsigned char*p=rgb+((size_t)y*stridePx + x)*3;
            int m=p[0]; if(p[1]>m)m=p[1]; if(p[2]>m)m=p[2];
            tot++; if(m<16) blk++;
        }
        return tot? (double)blk/(double)tot : 0.0;
    };
    // mean absolute luma difference between two same-size frames, 0..1. Large ⇒ a scene CUT (or huge
    // motion): RIFE interpolates a melted ghost of two unrelated frames across it, so the midpoint is
    // dropped. Calibrated on this content: cut≈0.22, normal motion≈0.02 → threshold 0.13 separates them.
    auto frame_diff=[&](const unsigned char*a,const unsigned char*b,int stridePx,int w,int h)->double{
        long tot=0; double sum=0;
        for(int y=0;y<h;y+=4) for(int x=0;x<w;x+=4){
            const unsigned char*pa=a+((size_t)y*stridePx+x)*3, *pb=b+((size_t)y*stridePx+x)*3;
            int la=(pa[0]*77+pa[1]*150+pa[2]*29)>>8, lb=(pb[0]*77+pb[1]*150+pb[2]*29)>>8;
            int d=la-lb; if(d<0)d=-d; sum+=d; tot++;
        }
        return tot? sum/(double)tot/255.0 : 0.0;
    };
    const double MID_CUT_DIFF = 0.13;   // frame_diff above this ⇒ scene cut ⇒ drop the midpoint
    auto encode_one=[&](const unsigned char*rgb, int stridePx, int64_t pts)->int{
        if(pts < emit_floor_pts) return 0;               // pre-segment anchor/midpoint: silently drop
        // input is the AI content procW x procH (row stride stridePx, which may be a wider /16-padded
        // chain buffer — sws crops it); output is outW x outH. When they differ, this ONE pass does the
        // Lanczos downscale to the monitor-native target AND the RGB->YUV convert together (no 8K file).
        int down = (procW!=outW || procH!=outH);
        sws_rgb2yuv=sws_getCachedContext(sws_rgb2yuv,procW,procH,AV_PIX_FMT_RGB24,outW,outH,AV_PIX_FMT_YUV420P,
                                         down?SWS_LANCZOS:SWS_BILINEAR,0,0,0);
        const uint8_t*sd[1]={rgb}; int sl[1]={stridePx*3};
        av_frame_make_writable(yf);
        sws_scale(sws_rgb2yuv,sd,sl,0,procH,yf->data,yf->linesize);
        if(pts<=last_pts_written) pts=last_pts_written+1; // midpoints/dups never collide with the base frame
        last_pts_written=pts;
        yf->pts=pts; emitted++;
        if(avcodec_send_frame(vec,yf)<0) return -1;
        AVPacket*op=av_packet_alloc(); int r=0, rr;
        while((rr=avcodec_receive_packet(vec,op))==0){
            op->stream_index=ovs->index; av_packet_rescale_ts(op,vec->time_base,ovs->time_base);
            if(av_interleaved_write_frame(ofmt,op)<0){ r=-1; }
            av_packet_unref(op);
        }
        if(rr!=AVERROR(EAGAIN)) r=-1;   // real mid-stream encoder failure — don't report "Done" on a broken file
        av_packet_free(&op); return r;
    };
    std::thread enc_thr([&]{
        int64_t wprev_pts=AV_NOPTS_VALUE;    // pts of the previously emitted BASE (source) frame
        int wfirst=1;                        // first base frame emits alone (no preceding midpoint)
        double last_base_bf=0.0;             // near-black fraction of the last real (base) frame — the
                                             // reference for spotting a spuriously-black RIFE midpoint
        unsigned char* prev_cup=0; int prev_uw=0;   // previous chain frame, kept for the scene-cut check
        for(;;){
            std::unique_lock<std::mutex> lk(qm);
            cv_pop.wait(lk,[&]{return qn>0;});
            QItem it=q[qh]; qh=(qh+1)%QCAP; qn--;
            lk.unlock(); cv_push.notify_one();
            if(it.type==QT_STOP) break;
            if(it.type==QT_AUDIO){
                if(av_interleaved_write_frame(ofmt,it.pkt)<0) enc_err=1;
                av_packet_free(&it.pkt); continue;
            }
            if(it.type==QT_CHAIN){
                // finish (denormalize + to_pixels) HERE, while the GPU runs the next frame.
                // Frame N sits at its REAL source pts (it.pts); the midpoint(N-1,N) sits halfway
                // between the two source frames' pts — so the doubled stream stays glued to the
                // source (and audio) timeline instead of a synthetic 2x-CFR grid.
                unsigned char*cmid=0; int uw=0,uh=0;
                unsigned char*cup=ncnn_chain_finish(it.holder,&cmid,&uw,&uh);
                if(!cup){ enc_err=1; if(cmid)free(cmid); continue; }
                double cup_bf=img_blackfrac(cup,uw,procW,procH);
                if(!wfirst){
                    int64_t midpts=(wprev_pts!=AV_NOPTS_VALUE)?(wprev_pts+it.pts)/2:it.pts;
                    // RIFE's midpoint, verbatim — UNLESS it came back as a spurious black rectangle
                    // (measured GPU corruption). A real midpoint of two non-black frames is never mostly
                    // black, so if it is far blacker than both neighbours, DROP it: the real timestamps
                    // leave a sub-frame gap that looks identical on screen, instead of a black flash.
                    if(cmid){
                        double mbf=img_blackfrac(cmid,uw,procW,procH);
                        int corrupt = (mbf>0.35 && mbf-cup_bf>0.30 && mbf-last_base_bf>0.30);
                        // scene-cut guard: RIFE across a cut is a melted cross-scene ghost — drop it.
                        int cut = (prev_cup && prev_uw==uw) ? (frame_diff(prev_cup,cup,uw,procW,procH)>MID_CUT_DIFF) : 0;
                        if(cut) fprintf(stderr,"[enh] dropped cross-cut RIFE midpoint (chain)\n");
                        if(corrupt) fprintf(stderr,"[enh] dropped black RIFE midpoint (blackfrac %.2f vs frame %.2f)\n",mbf,cup_bf);
                        if(cut||corrupt){ n_faildup++; }
                        else { if(encode_one(cmid,uw,midpts)) enc_err=1; n_interp++; }
                    }
                    else n_faildup++;
                }
                if(cmid) free(cmid);
                last_base_bf=cup_bf;
                // pre-roll anchor (first frame of a --seampreroll segment): the previous segment already
                // emitted this exact frame, so skip its base emit — it was only RIFE's anchor here.
                if(!(wfirst && seampreroll)){ if(encode_one(cup,uw,it.pts)) enc_err=1; }
                if(prev_cup) free(prev_cup);
                prev_cup=cup; prev_uw=uw;               // retain for next frame's scene-cut check (don't free cup)
                wfirst=0; wprev_pts=it.pts;
                continue;
            }
            if(it.type==QT_UP){
                // PURE UPSCALE (no frame-gen): do the denormalize+to_pixels HERE (398MB@4K),
                // overlapped with the GPU thread running the next frame. Emits at the real pts.
                int uw=0,uh=0;
                unsigned char* up=ncnn_upscale_raw_finish(it.holder,&uw,&uh);
                if(!up){ enc_err=1; continue; }
                if(encode_one(up,uw,it.pts)) enc_err=1;
                free(up);
                continue;
            }
            if(it.type==QT_MIDS){
                // small RIFE midpoint (--rife-maxh): Lanczos-up to full res HERE, overlapped with the
                // GPU thread's next pair — keeping the expensive upscale off the inference thread.
                // (Black-midpoint rejection already happened on the producer thread, which has both
                //  bracketing real frames in hand.)
                if(!w_midfull) w_midfull=(unsigned char*)malloc((size_t)procW*procH*3);
                if(w_midfull){
                    w_sws_mu=sws_getCachedContext(w_sws_mu,it.w,it.h,AV_PIX_FMT_RGB24,procW,procH,AV_PIX_FMT_RGB24,SWS_LANCZOS,0,0,0);
                    const uint8_t*sm[1]={it.rgb}; int lm[1]={it.w*3}; uint8_t*dm[1]={w_midfull}; int lo[1]={procW*3};
                    sws_scale(w_sws_mu,sm,lm,0,it.h,dm,lo);
                    if(encode_one(w_midfull,procW,it.pts)) enc_err=1; else n_interp++;
                } else enc_err=1;
                free(it.rgb);
                continue;
            }
            if(encode_one(it.rgb,procW,it.pts)) enc_err=1;
            free(it.rgb);
        }
        if(prev_cup) free(prev_cup);                        // release the retained scene-cut reference
        avcodec_send_frame(vec,0);                          // final encoder flush (writer thread owns it)
        AVPacket*op=av_packet_alloc(); int rr;
        while((rr=avcodec_receive_packet(vec,op))==0){
            op->stream_index=ovs->index; av_packet_rescale_ts(op,vec->time_base,ovs->time_base);
            if(av_interleaved_write_frame(ofmt,op)<0) enc_err=1;
            av_packet_unref(op);
        }
        if(rr!=AVERROR_EOF) enc_err=1;   // a flush-time failure drops the buffered tail — surface it
        av_packet_free(&op);
    });
    // producer-side: copy the AI frame (procW x procH) into the queue; the writer's encode_one does the
    // (optional) Lanczos downscale to the target. stridePx = buffer row width in PIXELS; when it exceeds
    // procW (the GPU-chain's /16 pad) the copy crops each row back to the real content width.
    auto encode_rgb=[&](const unsigned char*rgb,int stridePx,int64_t pts)->int{
        if(enc_err) return -1;
        std::unique_lock<std::mutex> lk(qm);
        cv_push.wait(lk,[&]{return qn<QCAP;});
        QItem*it=&q[qt]; it->type=QT_VIDEO; it->pkt=0; it->pts=pts;
        size_t N=(size_t)procW*procH*3;
        it->rgb=(unsigned char*)malloc(N); if(!it->rgb) return -1;
        if(stridePx<=procW) memcpy(it->rgb,rgb,N);
        else for(int y=0;y<procH;y++)
            memcpy(it->rgb+(size_t)y*procW*3, rgb+(size_t)y*stridePx*3, (size_t)procW*3);
        qt=(qt+1)%QCAP; qn++;
        lk.unlock(); cv_pop.notify_one(); return 0;
    };
    auto enqueue_audio=[&](AVPacket*ap){                    // takes ownership of ap
        std::unique_lock<std::mutex> lk(qm);
        cv_push.wait(lk,[&]{return qn<QCAP;});
        QItem*it=&q[qt]; it->type=QT_AUDIO; it->rgb=0; it->pkt=ap;
        qt=(qt+1)%QCAP; qn++;
        lk.unlock(); cv_pop.notify_one();
    };
    auto enqueue_chain=[&](int type, void*hold, int64_t pts)->int{  // QT_CHAIN
        if(enc_err) return -1;
        std::unique_lock<std::mutex> lk(qm);
        cv_push.wait(lk,[&]{return qn<QCAP;});
        QItem*it=&q[qt]; it->type=type; it->rgb=0; it->pkt=0; it->holder=hold; it->pts=pts;
        qt=(qt+1)%QCAP; qn++;
        lk.unlock(); cv_pop.notify_one(); return 0;
    };
    auto enqueue_mids=[&](unsigned char*small_mid,int w,int h,int64_t pts)->int{  // QT_MIDS (takes ownership)
        if(enc_err){ free(small_mid); return -1; }
        std::unique_lock<std::mutex> lk(qm);
        cv_push.wait(lk,[&]{return qn<QCAP;});
        QItem*it=&q[qt]; it->type=QT_MIDS; it->rgb=small_mid; it->pkt=0; it->holder=0; it->pts=pts; it->w=w; it->h=h;
        qt=(qt+1)%QCAP; qn++;
        lk.unlock(); cv_pop.notify_one(); return 0;
    };
    auto enqueue_stop=[&](void){
        std::unique_lock<std::mutex> lk(qm);
        cv_push.wait(lk,[&]{return qn<QCAP;});
        q[qt].type=QT_STOP; q[qt].rgb=0; q[qt].pkt=0;
        qt=(qt+1)%QCAP; qn++;
        lk.unlock(); cv_pop.notify_one();
    };
    // ---- DECODE PRODUCER STAGE: decode + deinterlace + cap/pad/diff run on their own thread,
    //      handing prepared source-res RGB (+ dup/cut flags) to the GPU thread through a small
    //      queue. Third pipeline stage after GPU-inference (main) and convert+encode (writer).
    struct PrepItem { unsigned char*rgb; int stop; int64_t pts; };
    enum { PCAP=4 };
    PrepItem prepq[PCAP]; int pph=0,ppt=0,ppn=0;
    std::mutex pm; std::condition_variable pcv_push, pcv_pop;
    auto prep_push=[&](PrepItem it){
        std::unique_lock<std::mutex> lk(pm);
        pcv_push.wait(lk,[&]{return ppn<PCAP;});
        prepq[ppt]=it; ppt=(ppt+1)%PCAP; ppn++;
        lk.unlock(); pcv_pop.notify_one();
    };
    auto prep_pop=[&](void)->PrepItem{
        std::unique_lock<std::mutex> lk(pm);
        pcv_pop.wait(lk,[&]{return ppn>0;});
        PrepItem it=prepq[pph]; pph=(pph+1)%PCAP; ppn--;
        lk.unlock(); pcv_push.notify_one(); return it;
    };
    // producer side of one decoded source frame: cap -> pad -> dup/cut diff -> queue
    long prep_count=0;                 // fallback frame index for sources that decode with no pts
    auto prep=[&](AVFrame*f)->int{
        // real source timestamp for this frame, rescaled SOURCE base -> OUTPUT base; rides the pipeline
        // to the encoder (A/V sync). The rescale is what makes coarse AVI/DivX per-frame timestamps
        // fine enough for frame-gen midpoints.
        int64_t spts = f->best_effort_timestamp;
        if(spts==AV_NOPTS_VALUE) spts=f->pts;
        if(spts!=AV_NOPTS_VALUE) spts=av_rescale_q(spts,svtb,otb);
        else spts=prep_count*src_frame_ticks;                       // synthesize a monotone timeline
        prep_count++;
        // cap + convert to packed RGB at BW x BH (chain mode pads to /16 with replicated edges)
        sws_dn=sws_getCachedContext(sws_dn,f->width,f->height,(AVPixelFormat)f->format,capW,capH,AV_PIX_FMT_RGB24,SWS_BILINEAR,0,0,0);
        unsigned char*rgb=(unsigned char*)malloc((size_t)BW*BH*3); if(!rgb) return -1;
        uint8_t*dd[1]={rgb}; int dl[1]={BW*3};
        sws_scale(sws_dn,(const uint8_t*const*)f->data,f->linesize,0,f->height,dd,dl);
        if(BW>capW || BH>capH){                          // fill the pad by replicating edges
            for(int y=0;y<capH;y++){ unsigned char*row=rgb+(size_t)y*BW*3;
                for(int x=capW;x<BW;x++){ row[x*3]=row[(capW-1)*3]; row[x*3+1]=row[(capW-1)*3+1]; row[x*3+2]=row[(capW-1)*3+2]; } }
            for(int y=capH;y<BH;y++) memcpy(rgb+(size_t)y*BW*3, rgb+(size_t)(capH-1)*BW*3, (size_t)BW*3);
        }
        // No scene-cut detection: the model sees every frame and RIFE runs on every pair. Nothing
        // going into or coming out of the model is inspected, gated, or altered.
        PrepItem it; it.rgb=rgb; it.stop=0; it.pts=spts;
        prep_push(it);
        return 0;
    };
    // GPU/consumer side of one prepared frame (upscale -> frame-gen emit)
    auto body=[&](unsigned char*rgb, int64_t pts)->int{
        // PIPELINED chain path (U+G): the GPU pass runs here; the CPU post-processing + the
        // mid/frame emission order all live on the writer thread, so the GPU never waits for
        // float->RGB conversion or H.265 encode of the previous frame. Every frame is inferred and
        // every pair is RIFE'd — no scene-cut skipping, no output vetoing.
        if(use_chain){
            int want = chain_sent;                       // a previous frame is on-GPU -> always RIFE it
            void*hold = ncnn_chain_frame_raw(rgb,BW,BH,want); free(rgb);
            if(!hold) return -1;
            chain_sent=1;
            return enqueue_chain(QT_CHAIN,hold,pts);
        }
        // PURE UPSCALE (do_up, no frame-gen): run only the GPU extract here and hand the raw [0,1]
        // holder to the writer, which does the 398MB@4K denormalize+to_pixels overlapped with the
        // next frame's GPU pass. This reclaims the ~50% single-stream bubble (Phase 2).
        if(do_up && !do_fg){
            void* hold = ncnn_upscale_cached_raw(rgb,capW,capH,upP,upB,up_in,up_out); free(rgb);
            if(!hold) return -1;
            return enqueue_chain(QT_UP,hold,pts);
        }
        unsigned char*up=0; int uw,uh; unsigned char*chain_mid=0;
        if(do_up){                                       // always run the model — no reuse/skip
            up=ncnn_upscale_cached(rgb,capW,capH,upP,upB,up_in,up_out,&uw,&uh); free(rgb);
            if(!up) return -1;
        }
        else { up=rgb; uw=capW; uh=capH; }
        UW=uw; UH=uh;
        if(do_fg){
            if(prevUp){
                unsigned char*mid=0;
                // RIFE WORKING RES: optical flow doesn't need 8+ megapixels. Above --rife-maxh the
                // pair is Lanczos-downscaled for RIFE and the midpoint Lanczos-upscaled back — base
                // frames stay untouched native; only the 1/48s-flash midpoints carry the softness.
                // This is what makes 4K (and 16K-restore) frame-gen fast AND multi-worker safe (the
                // native-4K RIFE contention black-boxes never appear at <=1440p working res).
                int RW=UW, RH=UH;
                if(rife_maxh>0 && UH>rife_maxh){ RH=rife_maxh&~1; RW=((int)((double)UW*RH/UH+0.5))&~1; }
                unsigned char*mid_small=0;               // capped-res midpoint (writer upscales it)
                if(RW!=UW || RH!=UH){
                    if(!rifeA){ rifeA=(unsigned char*)malloc((size_t)RW*RH*3); rifeB=(unsigned char*)malloc((size_t)RW*RH*3); }
                    if(rifeA && rifeB){
                        // BILINEAR down for the flow inputs (motion estimation doesn't care) — cheap
                        sws_rd=sws_getCachedContext(sws_rd,UW,UH,AV_PIX_FMT_RGB24,RW,RH,AV_PIX_FMT_RGB24,SWS_BILINEAR,0,0,0);
                        const uint8_t*sa[1]={prevUp}; int la[1]={UW*3}; uint8_t*da[1]={rifeA}; int ld[1]={RW*3};
                        sws_scale(sws_rd,sa,la,0,UH,da,ld);
                        const uint8_t*sb[1]={up}; uint8_t*db[1]={rifeB};
                        sws_scale(sws_rd,sb,la,0,UH,db,ld);
                        int mw,mh; mid_small=ncnn_rife(rifeA,rifeB,RW,RH,0.5f,rfP,rfB,&mw,&mh);
                        if(mid_small && (mw!=RW||mh!=RH)){ free(mid_small); mid_small=0; }
                    }
                } else {
                    int mw,mh;                           // RIFE every pair; use whatever it returns
                    mid=ncnn_rife(prevUp,up,UW,UH,0.5f,rfP,rfB,&mw,&mh);
                    if(mid && (mw!=UW||mh!=UH)){ free(mid); mid=0; }     // only a size mismatch is unusable
                }
                int64_t midpts=(prevUp_pts!=AV_NOPTS_VALUE)?(prevUp_pts+pts)/2:pts;
                // pre-roll anchor's base is skipped once (prev segment already emitted it); its midpoint
                // with this frame IS the cross-seam bridge we pre-rolled to make.
                if(!seam_skip_prev){ if(encode_rgb(prevUp,UW,prevUp_pts)) { free(up); if(mid)free(mid); if(mid_small)free(mid_small); return -1; } }
                seam_skip_prev=0;
                // RIFE occasionally returns a spurious black rectangle (measured GPU corruption). A real
                // midpoint of two non-black frames is never mostly black, so if the midpoint is far
                // blacker than BOTH bracketing real frames, DROP it — the real timestamps leave a
                // sub-frame gap that looks identical on screen, instead of a flashing black box.
                auto mid_is_black=[&](const unsigned char*buf,int strd,int mw,int mh)->int{
                    double mbf=img_blackfrac(buf,strd,mw,mh); if(mbf<=0.35) return 0;
                    double pf=img_blackfrac(prevUp,UW,UW,UH), cf=img_blackfrac(up,UW,UW,UH);
                    if(mbf>pf+0.30 && mbf>cf+0.30){ fprintf(stderr,"[enh] dropped black RIFE midpoint (blackfrac %.2f vs %.2f/%.2f)\n",mbf,pf,cf); return 1; }
                    return 0;
                };
                /* scene-cut guard: if the two REAL frames are wildly different, RIFE's midpoint is a
                   melted cross-cut ghost — drop it (a held frame for 1/48s reads as clean). */
                double pairdiff=frame_diff(prevUp,up,UW,UW,UH);
                int cut = pairdiff>MID_CUT_DIFF;
                if(cut) fprintf(stderr,"[enh] dropped cross-cut RIFE midpoint (framediff %.3f)\n",pairdiff);
                if(mid){
                    if(cut || mid_is_black(mid,UW,UW,UH)){ free(mid); n_faildup++; }
                    else { int e=encode_rgb(mid,UW,midpts); free(mid); n_interp++; if(e){ free(up); return -1; } }
                }
                else if(mid_small){                      // Lanczos-up happens on the WRITER (QT_MIDS)
                    if(cut || mid_is_black(mid_small,RW,RW,RH)){ free(mid_small); n_faildup++; }
                    else if(enqueue_mids(mid_small,RW,RH,midpts)){ free(up); return -1; }
                }
                else n_faildup++;   // RIFE gave nothing — emit nothing (no inserted duplicate); real
                                    // timestamps leave a one-slot gap, visually identical to a hold
                free(prevUp); prevUp=up; prevUp_pts=pts;
            } else {             // first frame: hold as the RIFE anchor
                prevUp=up; prevUp_pts=pts;
            }
        } else {
            if(encode_rgb(up,UW,pts)){ free(up); return -1; } free(up);
        }
        if(chain_mid) free(chain_mid);   // safety: never leak an unused midpoint
        return 0;
    };

    std::atomic<int> rc(0); int last_pct=-1;
    std::atomic<int> seg_end(0);   // segment worker hit its --to boundary: stop reading
    if(seg_ss>0.01) av_seek_frame(ifmt,-1,(int64_t)(seg_ss*AV_TIME_BASE),AVSEEK_FLAG_BACKWARD);
    // one decoded frame -> (optional yadif deinterlace) -> prep(); also drives progress.
    // Runs on the DECODE thread; failures raise rc, which both threads watch.
    auto handle_frame=[&](AVFrame*f)->void{
        // segment worker: drop the keyframe-seek pre-roll; stop at the (exclusive) end boundary.
        // A boundary frame lands in exactly ONE worker: t>=to-eps dropped here, and the next
        // worker (whose ss==to) keeps t>=ss-eps.
        if(seg_ss>=0 || seg_to>0){
            int64_t bt=f->best_effort_timestamp;
            if(bt!=AV_NOPTS_VALUE){
                double t=bt*av_q2d(ifmt->streams[vs]->time_base);
                // --seampreroll keeps the ONE frame just before ss as a non-emitted RIFE anchor, so the
                // first real frame can be interpolated against it → the true cross-seam midpoint.
                double preroll = seampreroll ? 1.5/(sfps>0?sfps:24.0) : 0.0;
                if(seg_ss>0 && t < seg_ss-1e-6-preroll) return;
                if(seg_to>0 && t>=seg_to-1e-6){ seg_end=1; return; }
            }
        }
        if(!deint_tried){ deint_tried=1;
            // Only deinterlace ACTUALLY-interlaced content. Progressive sources (modern 4K/HD like
            // 10-bit HDR WEB-DLs) don't need it, and forcing them through the yadif buffersrc — whose
            // graph is fixed from frame 1 — throws "changing video frame properties on the fly" when
            // later frames carry different colorspace/range metadata, which can corrupt frames (black
            // blocks). Interlaced DVDs still get deinterlaced.
            int is_interlaced = (f->flags & AV_FRAME_FLAG_INTERLACED) != 0;
            deint_ok = is_interlaced ? (deint_init(f)==0) : 0;
            if(!is_interlaced) fprintf(stderr,"[enh] progressive source — deinterlacer skipped\n");
            else if(!deint_ok)  fprintf(stderr,"[enh] deinterlacer unavailable, frames pass through\n"); }
        if(deint_ok && av_buffersrc_add_frame(fg_src,f)>=0){
            while(av_buffersink_get_frame(fg_sink,ff)>=0){
                if(prep(ff)) rc=1;
                av_frame_unref(ff);
            }
        } else if(prep(f)) rc=1;
        decoded++;
        int pct=(int)(decoded*100/(total>0?total:1)); if(pct>99)pct=99;
        if(pct!=last_pct){ last_pct=pct;
            char ph[64]; snprintf(ph,sizeof ph,"%s%s%s", do_up?"Upscaling":"", (do_up&&do_fg)?"+":"", do_fg?"Frame-gen":"");
            write_status(pct,ph,0,""); }
        if(decoded%200==0){ long el=(long)(time(0)-t_start); if(el<1)el=1;
            fprintf(stderr,"[enh] %ld/%ld frames, %.2f fps, interp=%ld held=%ld\n",
                    decoded,total,(double)decoded/el,n_interp,n_faildup); }
    };
    std::thread dec_thr([&]{
        while(av_read_frame(ifmt,pkt)>=0){
            if(pkt->stream_index==vs){
                for(;;){   // EAGAIN-safe send: drain frames and resend instead of dropping the packet
                    int sr=avcodec_send_packet(vdc,pkt);
                    while(avcodec_receive_frame(vdc,fr)==0) handle_frame(fr);
                    if(sr!=AVERROR(EAGAIN)) { if(sr<0&&sr!=AVERROR_EOF) fprintf(stderr,"[enh] decode err %d\n",sr); break; }
                }
            } else if(pkt->stream_index==as && oas>=0){    // copy audio through (via the writer thread)
                AVPacket*ap=av_packet_clone(pkt);
                int64_t ast=ifmt->streams[as]->start_time; // normalize away a container start offset (MPEG-TS)
                if(ast!=AV_NOPTS_VALUE && ast>0){ if(ap->pts!=AV_NOPTS_VALUE)ap->pts-=ast; if(ap->dts!=AV_NOPTS_VALUE)ap->dts-=ast; }
                ap->stream_index=oas; av_packet_rescale_ts(ap,ifmt->streams[as]->time_base,ofmt->streams[oas]->time_base);
                enqueue_audio(ap);
            }
            av_packet_unref(pkt);
            if(rc || seg_end) break;
        }
        // flush decoder (+ the deinterlacer's buffered frame)
        avcodec_send_packet(vdc,0);
        while(!rc && avcodec_receive_frame(vdc,fr)==0) handle_frame(fr);
        if(!rc && deint_ok && av_buffersrc_add_frame(fg_src,0)>=0)
            while(av_buffersink_get_frame(fg_sink,ff)>=0){ if(prep(ff)) rc=1; av_frame_unref(ff); }
        PrepItem stop; stop.rgb=0; stop.stop=1;
        prep_push(stop);
    });
    // GPU/consumer loop: keeps draining after an error so the decode thread can't jam on a full queue.
    for(;;){
        PrepItem it=prep_pop();
        if(it.stop) break;
        if(rc){ free(it.rgb); continue; }
        if(body(it.rgb,it.pts)) rc=1;
    }
    dec_thr.join();
    // frame-gen: emit the final held frame at its own source pts (last frame of the timeline)
    if(!rc && do_fg && prevUp){ if(encode_rgb(prevUp,UW,prevUp_pts)) rc=1; }
    if(prevUp) free(prevUp);
    // stop + drain the writer thread (it flushes the encoder itself), then finalize the container
    enqueue_stop();
    enc_thr.join();
    if(enc_err) rc=1;
    av_write_trailer(ofmt);

    // cleanup
    if(ofmt&&!(ofmt->oformat->flags&AVFMT_NOFILE)) avio_closep(&ofmt->pb);
    avcodec_free_context(&vec); avformat_free_context(ofmt);
    avcodec_free_context(&vdc); avformat_close_input(&ifmt);
    av_frame_free(&fr); av_frame_free(&yf); av_frame_free(&ff); av_packet_free(&pkt);
    if(fg_graph) avfilter_graph_free(&fg_graph);
    if(sws_dn) sws_freeContext(sws_dn); if(sws_rgb2yuv) sws_freeContext(sws_rgb2yuv);
    if(sws_rd) sws_freeContext(sws_rd); if(sws_ru) sws_freeContext(sws_ru);
    if(w_sws_mu) sws_freeContext(w_sws_mu);
    free(rifeA); free(rifeB); free(w_midfull);
    ncnn_chain_close(); ncnn_upscale_gpu_close(); ncnn_rife_close(); ncnn_vk_shutdown();

    if(rc){ write_status(last_pct<0?0:last_pct,"error",1,"processing failed"); fprintf(stderr,"[enh] FAILED\n"); return 3; }
    // frame-gen ran but EVERY midpoint failed -> the model/GPU is broken; don't pretend it worked
    if(do_fg && decoded>2 && n_interp==0 && n_faildup>0){
        write_status(last_pct<0?0:last_pct,"error",1,"frame-gen failed (RIFE produced no frames)");
        fprintf(stderr,"[enh] FAILED: RIFE produced 0 midpoints (%ld attempts)\n",n_faildup); return 3; }
    write_status(100,"Done",1,"");
    { long el=(long)(time(0)-t_start); if(el<1)el=1;
      fprintf(stderr,"[enh] done -> %s (%ld frames in %lds, %.2f fps; interp=%ld held=%ld)\n",
              out,emitted,el,(double)emitted/el,n_interp,n_faildup); }
    return 0;
}
