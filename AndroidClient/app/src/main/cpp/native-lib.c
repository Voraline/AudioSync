#define _GNU_SOURCE
#include <jni.h>
#include <aaudio/AAudio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>                  /* sqrt() for sigma filtering */
#include <android/log.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

#define Tag  "AudioSync"
#define Logi(...) __android_log_print(ANDROID_LOG_INFO,  Tag, __VA_ARGS__)
#define Loge(...) __android_log_print(ANDROID_LOG_ERROR, Tag, __VA_ARGS__)

#define ServerPort     11000
#define ClientPort     11001
#define MaxSyncSamples 1000

#define PtRegister 0x01
#define PtSyncReq  0x02
#define PtSyncAck  0x03
#define PtFire     0x04

typedef struct __attribute__((packed)) { uint8_t Type; uint64_t T1; }                           SyncReqPkt;
typedef struct __attribute__((packed)) { uint8_t Type; uint64_t T1; uint64_t T2; uint64_t T3; } SyncAckPkt;
typedef struct __attribute__((packed)) { uint8_t Type; uint64_t FireAtPcUs; }                   FirePkt;
typedef struct { int64_t Rtt; int64_t Offset; } SyncSample;

static float*   PcmBuf        = NULL;
static int      PcmFrames     = 0;
static int      PcmChannels   = 2;
static int      PcmSampleRate = 44100;

static _Atomic int      Running       = 0;
static _Atomic int      FireReady     = 0;
static _Atomic int64_t  ClockOffsetUs = 0;
static _Atomic uint64_t LastFirePcUs  = 0;


static int                Sock = -1;
static char               SrvIp[64];
static struct sockaddr_in SrvAddr;

/* ── Rolling-probe ring buffer ───────────────────────────────────────────────
 * Stores the last 32 probe results so DoRollingProbe can filter by RTT
 * instead of applying each raw sample directly to ClockOffsetUs.          */
#define ProbeRingSize 64
static SyncSample ProbeRing[ProbeRingSize];
static int        ProbeRingHead = 0;   /* next write position              */
static int        ProbeRingFull = 0;   /* 1 once the buffer has wrapped    */

/* qsort comparator — sort SyncSample ascending by RTT */
static int CmpRtt(const void* A, const void* B) {
    int64_t Ra = ((const SyncSample*)A)->Rtt;
    int64_t Rb = ((const SyncSample*)B)->Rtt;
    return (Ra > Rb) - (Ra < Rb);
}

static uint64_t NowUs(void) {
    struct timespec Ts;
    clock_gettime(CLOCK_MONOTONIC, &Ts);
    return (uint64_t)Ts.tv_sec * 1000000ULL + (uint64_t)Ts.tv_nsec / 1000ULL;
}

static AAudioStream* AudioStream = NULL;

static aaudio_data_callback_result_t AudioCallback(AAudioStream* St, void* U, void* Data, int32_t NumFrames) {
    (void)U;
    float* Out = (float*)Data;
    if (!atomic_load_explicit(&FireReady, memory_order_acquire)) {
        memset(Out, 0, (size_t)NumFrames * PcmChannels * sizeof(float));
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    int64_t HwFramePos = 0, HwPresentNs = 0;
    int64_t WritePresentsUs;
    if (AAudioStream_getTimestamp(St, CLOCK_MONOTONIC, &HwFramePos, &HwPresentNs) == AAUDIO_OK) {
        int64_t FramesWritten = AAudioStream_getFramesWritten(St);
        int64_t AheadFrames   = FramesWritten - HwFramePos;
        WritePresentsUs = HwPresentNs / 1000LL + AheadFrames * 1000000LL / PcmSampleRate;
    } else {
        WritePresentsUs = (int64_t)NowUs();
    }
    double CurrentServerTimeUs = (double)WritePresentsUs + (double)atomic_load_explicit(&ClockOffsetUs, memory_order_relaxed);
    double TargetFirePcUs      = (double)atomic_load_explicit(&LastFirePcUs, memory_order_relaxed);
    double ExactF = (CurrentServerTimeUs - TargetFirePcUs) * (double)PcmSampleRate / 1000000.0;
    for (int32_t I = 0; I < NumFrames; I++) {
        double SampleIdx = ExactF + (double)I;
        if (SampleIdx < 0.0 || SampleIdx >= (double)(PcmFrames - 2)) {
            for (int C = 0; C < PcmChannels; C++) *Out++ = 0.0f;
        } else {
            int64_t F = (int64_t)SampleIdx;
            float Frac = (float)(SampleIdx - (double)F);
            for (int C = 0; C < PcmChannels; C++) {
                float P0 = (F - 1 >= 0) ? PcmBuf[(F - 1) * PcmChannels + C] : 0.0f;
                float P1 = PcmBuf[F * PcmChannels + C];
                float P2 = PcmBuf[(F + 1) * PcmChannels + C];
                float P3 = PcmBuf[(F + 2) * PcmChannels + C];
                float C0 = P1;
                float C1 = 0.5f * (P2 - P0);
                float C2 = P0 - 2.5f * P1 + 2.0f * P2 - 0.5f * P3;
                float C3 = 0.5f * (P3 - P0) + 1.5f * (P1 - P2);
                *Out++ = C0 + Frac * (C1 + Frac * (C2 + Frac * C3));
            }
        }
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void DoClockSync(void) {
    /* Pin to big cores at SCHED_FIFO so the OS cannot preempt us mid-measurement.
     * A preemption between sendto() and NowUs() inflates RTT and skews the offset. */
    struct sched_param Sp;
    Sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &Sp);
    cpu_set_t Cpus;
    CPU_ZERO(&Cpus);
    int NumCpus = sysconf(_SC_NPROCESSORS_ONLN);
    for (int I = NumCpus / 2; I < NumCpus; I++) CPU_SET(I, &Cpus);
    sched_setaffinity(0, sizeof(Cpus), &Cpus);

    int SyncSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (SyncSock < 0) return;
    int Tos = 0x10, Prio = 6;
    setsockopt(SyncSock, IPPROTO_IP, IP_TOS,      &Tos,  sizeof(Tos));
    setsockopt(SyncSock, SOL_SOCKET, SO_PRIORITY,  &Prio, sizeof(Prio));
    struct timeval Tv = {0, 200000};
    setsockopt(SyncSock, SOL_SOCKET, SO_RCVTIMEO,  &Tv,   sizeof(Tv));

    /* 512 samples * 20 ms apart = ~10.24 seconds of collection.
     * 20 ms gap lets the WiFi TX queue drain between probes so consecutive
     * samples are statistically independent — critical for valid filtering. */
    static SyncSample Samples[MaxSyncSamples];  /* static: keeps off the stack */
    int SampleCount = 0;
    uint8_t RxBuf[64];
    struct timespec Gap = {0, 10000000};         /* 10 ms between probes */

    for (int I = 0; I < MaxSyncSamples; I++) {
        SyncReqPkt Req;
        Req.Type    = PtSyncReq;
        uint64_t T1 = NowUs();
        Req.T1      = T1;
        sendto(SyncSock, &Req, sizeof(Req), 0, (struct sockaddr*)&SrvAddr, sizeof(SrvAddr));
        ssize_t  N  = recv(SyncSock, RxBuf, sizeof(RxBuf), 0);
        uint64_t T4 = NowUs();
        nanosleep(&Gap, NULL);                   /* always wait, even on failure */
        if (N < (ssize_t)sizeof(SyncAckPkt)) continue;
        SyncAckPkt* Ack = (SyncAckPkt*)RxBuf;
        if (Ack->Type != PtSyncAck || Ack->T1 != T1) continue;
        Samples[SampleCount].Rtt    = (int64_t)(T4 - T1);
        /* NTP/Cristian offset formula: corrects for asymmetric one-way delays */
        Samples[SampleCount].Offset = ((int64_t)Ack->T2 - (int64_t)T1
                                     + (int64_t)Ack->T3 - (int64_t)T4) / 2;
        SampleCount++;
    }
    close(SyncSock);
    if (SampleCount == 0) return;

    /* ── Step 1: sort all samples by RTT ascending ──────────────────────────
     * Low RTT = packet spent minimal time in WiFi queues = more symmetric
     * delay = more accurate offset.  High RTT = queuing asymmetry unknown. */
    qsort(Samples, (size_t)SampleCount, sizeof(SyncSample), CmpRtt);

    /* ── Step 2: take the 10 lowest-RTT samples (absolute count, not %) ────
     * With 6000 probes these 10 sit at the 0.17th percentile — far more
     * selective than any percentage approach.  More total samples means more
     * chances to catch a packet where BOTH directions were unqueued, so the
     * absolute minimum RTT keeps falling as sample count grows.
     * The sigma filter in Step 4 removes any remaining noisy offsets.      */
    int TopN = 10;
    if (TopN > SampleCount) TopN = SampleCount;  /* safety: if very few probes succeeded */

    /* ── Step 3: compute mean + standard deviation of their offsets ─────────
     * Even among low-RTT samples there can be flukes where one direction was
     * unusually fast. Sigma filtering removes those outliers. */
    double Sum = 0.0;
    for (int I = 0; I < TopN; I++) Sum += (double)Samples[I].Offset;
    double Mean = Sum / (double)TopN;

    double Var = 0.0;
    for (int I = 0; I < TopN; I++) {
        double D = (double)Samples[I].Offset - Mean;
        Var += D * D;
    }
    double Sigma = (TopN > 1) ? sqrt(Var / (double)(TopN - 1)) : 0.0;

    /* ── Step 4: reject outliers beyond 0.8 sigma, recompute mean ───────────
     * 0.8 sigma is tight but with only ~20 carefully-selected low-RTT samples
     * the spread is already very small; any remaining outlier is a bad packet. */
    double FinalSum   = 0.0;
    int    FinalCount = 0;
    for (int I = 0; I < TopN; I++) {
        double D = (double)Samples[I].Offset - Mean;
        if (D < 0.0) D = -D;
        if (Sigma < 1.0 || D <= 0.8 * Sigma) {
            FinalSum += (double)Samples[I].Offset;
            FinalCount++;
        }
    }
    if (FinalCount == 0) { FinalSum = Mean; FinalCount = 1; }

    int64_t FinalOffset = (int64_t)(FinalSum / (double)FinalCount);
    atomic_store_explicit(&ClockOffsetUs, FinalOffset, memory_order_release);

    /* Reset the rolling-probe ring buffer — its old offsets are now stale
     * relative to the freshly computed absolute offset. */
    ProbeRingHead = 0;
    ProbeRingFull = 0;

    Logi("ClockSync: collected=%d  filtered=%d  final=%d  "
         "offset=%lld us  minRTT=%lld us  sigma=%.1f us",
         SampleCount, TopN, FinalCount,
         (long long)FinalOffset,
         (long long)Samples[0].Rtt,
         Sigma);
}

static void DoRollingProbe(int SSock) {
    SyncReqPkt Req;
    Req.Type    = PtSyncReq;
    uint64_t T1 = NowUs();
    Req.T1      = T1;
    sendto(SSock, &Req, sizeof(Req), 0, (struct sockaddr*)&SrvAddr, sizeof(SrvAddr));
    uint8_t RxBuf[64];
    ssize_t  N  = recv(SSock, RxBuf, sizeof(RxBuf), 0);
    uint64_t T4 = NowUs();
    if (N < (ssize_t)sizeof(SyncAckPkt)) return;
    SyncAckPkt* Ack = (SyncAckPkt*)RxBuf;
    if (Ack->Type != PtSyncAck || Ack->T1 != T1) return;

    /* Store this probe in the ring buffer */
    ProbeRing[ProbeRingHead].Rtt    = (int64_t)(T4 - T1);
    ProbeRing[ProbeRingHead].Offset = ((int64_t)Ack->T2 - (int64_t)T1
                                     + (int64_t)Ack->T3 - (int64_t)T4) / 2;
    ProbeRingHead = (ProbeRingHead + 1) % ProbeRingSize;
    if (ProbeRingHead == 0) ProbeRingFull = 1;

    int Count = ProbeRingFull ? ProbeRingSize : ProbeRingHead;

    if (Count < 4) {
        /* Ring not full enough to filter yet — apply a small direct step so
         * we still track the clock in the first few seconds after connect. */
        int64_t CurOff = atomic_load_explicit(&ClockOffsetUs, memory_order_relaxed);
        int64_t Diff   = ProbeRing[(ProbeRingHead + ProbeRingSize - 1) % ProbeRingSize].Offset - CurOff;
        if (Diff >  500) Diff =  500;
        if (Diff < -500) Diff = -500;
        atomic_store_explicit(&ClockOffsetUs, CurOff + Diff, memory_order_release);
        return;
    }

    /* Copy and sort the ring buffer by RTT, take bottom 10%.
     * With ring size 64 that is ~6 samples — the very lowest-RTT probes.
     * Same principle as DoClockSync: lowest RTT = most symmetric delay
     * = most accurate offset estimate.                                    */
    SyncSample Tmp[ProbeRingSize];
    memcpy(Tmp, ProbeRing, (size_t)Count * sizeof(SyncSample));
    qsort(Tmp, (size_t)Count, sizeof(SyncSample), CmpRtt);

    int BestN = Count * 10 / 100;   /* bottom 10% of 64 = ~6 samples */
    if (BestN < 2) BestN = 2;

    /* Sigma filter on the offset values of those best-N samples.
     * A low-RTT packet can still carry a noisy offset (e.g., server jitter);
     * rejecting offsets beyond 1.0σ of the best-N set removes those.      */
    double OffSum = 0.0;
    for (int I = 0; I < BestN; I++) OffSum += (double)Tmp[I].Offset;
    double OffMean = OffSum / (double)BestN;
    double OffVar  = 0.0;
    for (int I = 0; I < BestN; I++) {
        double D = (double)Tmp[I].Offset - OffMean;
        OffVar += D * D;
    }
    double OffSigma = (BestN > 1) ? sqrt(OffVar / (double)(BestN - 1)) : 0.0;
    double FSum = 0.0; int FC = 0;
    for (int I = 0; I < BestN; I++) {
        double D = (double)Tmp[I].Offset - OffMean;
        if (D < 0.0) D = -D;
        if (OffSigma < 1.0 || D <= 1.0 * OffSigma) { FSum += (double)Tmp[I].Offset; FC++; }
    }
    if (FC == 0) { FSum = OffMean; FC = 1; }
    int64_t TargetOffset = (int64_t)(FSum / (double)FC);

    /* Apply 15% of the error per probe — converges in ~20 probes (~0.6 s
     * at 30 ms interval). Hard cap at ±100 us: a burst of bad packets can
     * only corrupt the offset by 100 us per step instead of 200 us.       */
    int64_t CurOff = atomic_load_explicit(&ClockOffsetUs, memory_order_relaxed);
    int64_t Step   = (TargetOffset - CurOff) * 15 / 100;
    if (Step >  100) Step =  100;
    if (Step < -100) Step = -100;
    atomic_store_explicit(&ClockOffsetUs, CurOff + Step, memory_order_release);
}

static void* KeepAliveThread(void* U) {
    (void)U;
    /* Pin this thread to SCHED_FIFO so the OS cannot preempt us between
     * clock_gettime(T1) and sendto() during rolling probes.  Without this,
     * a context switch inflates the apparent RTT and biases the offset.    */
    struct sched_param KaSp;
    KaSp.sched_priority = sched_get_priority_max(SCHED_FIFO);
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &KaSp);
    uint8_t Ping  = PtRegister;
    int Cycle = 0;
    int SSock = socket(AF_INET, SOCK_DGRAM, 0);
    int Tos = 0x10, Prio = 6;
    setsockopt(SSock, IPPROTO_IP, IP_TOS,      &Tos,  sizeof(Tos));
    setsockopt(SSock, SOL_SOCKET, SO_PRIORITY,  &Prio, sizeof(Prio));
    struct timeval Tv = {0, 25000};   /* 25 ms timeout — half the probe interval */
    setsockopt(SSock, SOL_SOCKET, SO_RCVTIMEO, &Tv,  sizeof(Tv));
    while (atomic_load_explicit(&Running, memory_order_relaxed)) {
        sendto(Sock, &Ping, 1, 0, (struct sockaddr*)&SrvAddr, sizeof(SrvAddr));
        DoRollingProbe(SSock);
        /* 30 ms between probes (was 50 ms) — tighter tracking of clock drift.
         * WiFi clocks drift ~1–2 ppm so 30 ms gives ~0.06 us drift per interval,
         * well within the 100 us step cap in DoRollingProbe.                     */
        struct timespec Ts = {0, 30000000};
        nanosleep(&Ts, NULL);
        /* Full re-sync every 10000 probes = 10000 * 30 ms = ~5 minutes.
         * DoClockSync now takes ~60 seconds so we run it infrequently;
         * the rolling probe maintains sub-100 us accuracy between syncs.   */
        if (++Cycle >= 10000) {
            Cycle = 0;
            DoClockSync();
        }
    }
    close(SSock);
    return NULL;
}

JNIEXPORT jstring JNICALL Java_com_audiosync_app_MainActivity_NativeDecodeMp3(JNIEnv* Env, jobject Obj, jbyteArray Mp3Data) {
    (void)Obj;
    jsize Len = (*Env)->GetArrayLength(Env, Mp3Data);
    jbyte* Raw = (*Env)->GetByteArrayElements(Env, Mp3Data, NULL);
    mp3dec_t Dec;
    mp3dec_frame_info_t Info;
    mp3d_sample_t Smp[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_init(&Dec);
    size_t PcmCap = (size_t)Len * 4;
    float* Pcm    = (float*)malloc(PcmCap * sizeof(float));
    size_t PcmLen = 0;
    const uint8_t* Ptr  = (const uint8_t*)Raw;
    int Left = (int)Len;
    int Channels = 0, SampleRate = 0;
    while (Left > 0) {
        int N = mp3dec_decode_frame(&Dec, Ptr, Left, Smp, &Info);
        if (Info.frame_bytes == 0) break;
        Ptr  += Info.frame_bytes;
        Left -= Info.frame_bytes;
        if (N == 0) continue;
        if (Channels == 0) { Channels = Info.channels; SampleRate = Info.hz; }
        size_t Add = (size_t)(N * Info.channels);
        if (PcmLen + Add > PcmCap) {
            PcmCap = (PcmLen + Add) * 2;
            Pcm    = (float*)realloc(Pcm, PcmCap * sizeof(float));
        }
        for (size_t I = 0; I < Add; I++) {
            Pcm[PcmLen + I] = Smp[I] / 32768.0f;
        }
        PcmLen += Add;
    }
    (*Env)->ReleaseByteArrayElements(Env, Mp3Data, Raw, JNI_ABORT);
    if (Channels == 0) {
        free(Pcm);
        return (*Env)->NewStringUTF(Env, "ERROR:No frames decoded");
    }
    free(PcmBuf);
    PcmBuf        = Pcm;
    PcmFrames     = (int)(PcmLen / (size_t)Channels);
    PcmChannels   = Channels;
    PcmSampleRate = SampleRate;
    atomic_store(&FireReady, 0);
    Logi("Decoded: %d frames  %d ch  %d hz", PcmFrames, PcmChannels, PcmSampleRate);
    char Msg[64];
    float Secs = (float)PcmFrames / SampleRate;
    snprintf(Msg, sizeof(Msg), "OK:%d:%d:%.1f", Channels, SampleRate, Secs);
    return (*Env)->NewStringUTF(Env, Msg);
}

JNIEXPORT void JNICALL Java_com_audiosync_app_MainActivity_NativeConnect(JNIEnv* Env, jobject Obj, jstring IpStr) {
    (void)Obj;
    atomic_store_explicit(&Running, 0, memory_order_release);
    if (Sock != -1) { close(Sock); Sock = -1; }
    atomic_store(&FireReady, 0);
    const char* Ip = (*Env)->GetStringUTFChars(Env, IpStr, NULL);
    strncpy(SrvIp, Ip, sizeof(SrvIp) - 1);
    (*Env)->ReleaseStringUTFChars(Env, IpStr, Ip);
    Sock = socket(AF_INET, SOCK_DGRAM, 0);
    int Reuse = 1, BufSz = 65536, Tos = 0x10, Prio = 6;
    setsockopt(Sock, SOL_SOCKET, SO_REUSEADDR, &Reuse, sizeof(Reuse));
    setsockopt(Sock, SOL_SOCKET, SO_RCVBUF,    &BufSz, sizeof(BufSz));
    setsockopt(Sock, IPPROTO_IP, IP_TOS,       &Tos,   sizeof(Tos));
    setsockopt(Sock, SOL_SOCKET, SO_PRIORITY,  &Prio,  sizeof(Prio));
    struct sockaddr_in BindAddr;
    memset(&BindAddr, 0, sizeof(BindAddr));
    BindAddr.sin_family      = AF_INET;
    BindAddr.sin_port        = htons(ClientPort);
    BindAddr.sin_addr.s_addr = INADDR_ANY;
    if (bind(Sock, (struct sockaddr*)&BindAddr, sizeof(BindAddr)) < 0) {
        Loge("bind failed"); close(Sock); Sock = -1; return;
    }
    struct ip_mreq Mreq;
    Mreq.imr_multiaddr.s_addr = inet_addr("224.0.0.100");
    Mreq.imr_interface.s_addr = INADDR_ANY;
    setsockopt(Sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &Mreq, sizeof(Mreq));
    memset(&SrvAddr, 0, sizeof(SrvAddr));
    SrvAddr.sin_family = AF_INET;
    SrvAddr.sin_port   = htons(ServerPort);
    inet_pton(AF_INET, SrvIp, &SrvAddr.sin_addr);
    DoClockSync();
    atomic_store_explicit(&Running, 1, memory_order_release);
    pthread_t Rt;
    pthread_attr_t Ra;
    pthread_attr_init(&Ra);
    pthread_attr_setdetachstate(&Ra, PTHREAD_CREATE_DETACHED);
    pthread_create(&Rt, &Ra, KeepAliveThread, NULL);
    pthread_attr_destroy(&Ra);
}

JNIEXPORT void JNICALL Java_com_audiosync_app_MainActivity_NativeStartReceiveLoop(JNIEnv* Env, jobject Obj) {
    (void)Env; (void)Obj;
    if (PcmFrames == 0 || Sock == -1) return;
    atomic_store_explicit(&LastFirePcUs, 0, memory_order_release);
    AAudioStreamBuilder* Bld;
    AAudio_createStreamBuilder(&Bld);
    AAudioStreamBuilder_setFormat(Bld,          AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(Bld,    PcmChannels);
    AAudioStreamBuilder_setSampleRate(Bld,      PcmSampleRate);
    AAudioStreamBuilder_setPerformanceMode(Bld, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(Bld,     AAUDIO_SHARING_MODE_EXCLUSIVE);
    AAudioStream* Probe;
    AAudioStreamBuilder_openStream(Bld, &Probe);
    int32_t Burst = AAudioStream_getFramesPerBurst(Probe);
    AAudioStream_close(Probe);
    AAudioStreamBuilder_delete(Bld);
    AAudio_createStreamBuilder(&Bld);
    AAudioStreamBuilder_setFormat(Bld,                AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(Bld,          PcmChannels);
    AAudioStreamBuilder_setSampleRate(Bld,            PcmSampleRate);
    AAudioStreamBuilder_setPerformanceMode(Bld,       AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(Bld,           AAUDIO_SHARING_MODE_EXCLUSIVE);
    AAudioStreamBuilder_setUsage(Bld,                 AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(Bld,           AAUDIO_CONTENT_TYPE_SONIFICATION);
    AAudioStreamBuilder_setDataCallback(Bld,          AudioCallback, NULL);
    AAudioStreamBuilder_setFramesPerDataCallback(Bld, Burst);
    AAudioStream* St;
    AAudioStreamBuilder_openStream(Bld, &St);
    AAudioStreamBuilder_delete(Bld);
    AudioStream = St;
    AAudioStream_setBufferSizeInFrames(St, Burst);
    AAudioStream_requestStart(St);
    struct timespec StabilizeTs = {0, 30000000};
    nanosleep(&StabilizeTs, NULL);
    struct sched_param Sp;
    Sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &Sp);
    cpu_set_t Cpus;
    CPU_ZERO(&Cpus);
    int NumCpus = sysconf(_SC_NPROCESSORS_ONLN);
    for (int I = NumCpus / 2; I < NumCpus; I++) CPU_SET(I, &Cpus);
    sched_setaffinity(0, sizeof(Cpus), &Cpus);
    uint8_t RxBuf[32];
    while (atomic_load_explicit(&Running, memory_order_relaxed)) {
        ssize_t N = recv(Sock, RxBuf, sizeof(RxBuf), 0);
        if (N < 1) break;
        if (RxBuf[0] != PtFire || N < (ssize_t)sizeof(FirePkt)) continue;
        FirePkt* Fp = (FirePkt*)RxBuf;
        if (Fp->FireAtPcUs == atomic_load_explicit(&LastFirePcUs, memory_order_relaxed)) continue;
        atomic_store_explicit(&LastFirePcUs, Fp->FireAtPcUs, memory_order_release);
        atomic_store_explicit(&FireReady, 1, memory_order_release);
        Logi("Fire Received! Target PC Time: %lld us", (long long)Fp->FireAtPcUs);
    }
    AAudioStream_requestStop(St);
    AAudioStream_close(St);
}

JNIEXPORT void JNICALL Java_com_audiosync_app_MainActivity_NativeDisconnect(JNIEnv* Env, jobject Obj) {
    (void)Env; (void)Obj;
    atomic_store_explicit(&Running,   0, memory_order_release);
    atomic_store_explicit(&FireReady, 0, memory_order_release);
    if (Sock != -1) { shutdown(Sock, SHUT_RDWR); close(Sock); Sock = -1; }
}
