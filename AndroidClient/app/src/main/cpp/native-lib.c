#define _GNU_SOURCE
#include <jni.h>
#include <aaudio/AAudio.h>
#include <sys/socket.h>
#include <sys/time.h>
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
#include <stdarg.h>
#include <math.h>
#include <android/log.h>
#include <linux/net_tstamp.h>
#include <linux/errqueue.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

#define Tag  "AudioSync"
#define Logi(...) NativeLog(ANDROID_LOG_INFO, __VA_ARGS__)
#define Loge(...) NativeLog(ANDROID_LOG_ERROR, __VA_ARGS__)

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
static int      StreamChannels = 2;
static int      StreamSampleRate = 48000;

static _Atomic int      Running       = 0;
static _Atomic int      FireReady     = 0;
static _Atomic int64_t  ClockOffsetUs = 0;
static _Atomic int64_t  OutputTrimUs  = 0;
static _Atomic uint64_t LastFirePcUs  = 0;


static int                Sock = -1;
static char               SrvIp[64];
static struct sockaddr_in SrvAddr;
static JavaVM*            AppVm = NULL;
static jobject            ConsoleSink = NULL;
static jmethodID          ConsoleLogMethod = NULL;
static pthread_mutex_t    ConsoleLock = PTHREAD_MUTEX_INITIALIZER;

static void NativeLog(int Priority, const char* Fmt, ...) {
    char Msg[512];
    va_list Args;
    va_start(Args, Fmt);
    vsnprintf(Msg, sizeof(Msg), Fmt, Args);
    va_end(Args);
    __android_log_write(Priority, Tag, Msg);
    if (AppVm == NULL) return;
    JNIEnv* Env = NULL;
    int Attached = 0;
    jint State = (*AppVm)->GetEnv(AppVm, (void**)&Env, JNI_VERSION_1_6);
    if (State == JNI_EDETACHED) {
        if ((*AppVm)->AttachCurrentThread(AppVm, &Env, NULL) != JNI_OK) return;
        Attached = 1;
    } else if (State != JNI_OK) {
        return;
    }
    jobject LocalSink = NULL;
    jmethodID Method = NULL;
    pthread_mutex_lock(&ConsoleLock);
    if (ConsoleSink != NULL && ConsoleLogMethod != NULL) {
        LocalSink = (*Env)->NewLocalRef(Env, ConsoleSink);
        Method = ConsoleLogMethod;
    }
    pthread_mutex_unlock(&ConsoleLock);
    if (LocalSink != NULL && Method != NULL) {
        jstring Text = (*Env)->NewStringUTF(Env, Msg);
        if (Text != NULL) {
            (*Env)->CallVoidMethod(Env, LocalSink, Method, Text);
            (*Env)->DeleteLocalRef(Env, Text);
        }
        if ((*Env)->ExceptionCheck(Env)) (*Env)->ExceptionClear(Env);
        (*Env)->DeleteLocalRef(Env, LocalSink);
    }
    if (Attached) (*AppVm)->DetachCurrentThread(AppVm);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* Vm, void* Reserved) {
    (void)Reserved;
    AppVm = Vm;
    return JNI_VERSION_1_6;
}

#define ProbeRingSize 128
static SyncSample ProbeRing[ProbeRingSize];
static int        ProbeRingHead = 0;
static int        ProbeRingFull = 0;

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

static uint64_t NowRealUs(void) {
    struct timespec Ts;
    clock_gettime(CLOCK_REALTIME, &Ts);
    return (uint64_t)Ts.tv_sec * 1000000ULL + (uint64_t)Ts.tv_nsec / 1000ULL;
}

static int64_t RealToMonoDeltaUs   = 0;
static int     RealToMonoDeltaInit = 0;

static void InitRealToMonoDelta(void) {
    if (RealToMonoDeltaInit) return;
    int64_t Deltas[8];
    for (int I = 0; I < 8; I++) {
        uint64_t R = NowRealUs();
        uint64_t M = NowUs();
        Deltas[I] = (int64_t)M - (int64_t)R;
    }
    for (int I = 1; I < 8; I++) {
        int64_t K = Deltas[I]; int J = I - 1;
        while (J >= 0 && Deltas[J] > K) { Deltas[J+1] = Deltas[J]; J--; }
        Deltas[J+1] = K;
    }
    RealToMonoDeltaUs   = Deltas[4];
    RealToMonoDeltaInit = 1;
    Logi("RealToMonoDelta: %lld us", (long long)RealToMonoDeltaUs);
}

static uint64_t RealTsToMonoUs(const struct timespec* Ts) {
    uint64_t RealUs = (uint64_t)Ts->tv_sec * 1000000ULL
                    + (uint64_t)Ts->tv_nsec / 1000ULL;
    return (uint64_t)((int64_t)RealUs + RealToMonoDeltaUs);
}

static int EnableKernelRxTs(int Fd) {
    int Flags = SOF_TIMESTAMPING_RX_SOFTWARE
              | SOF_TIMESTAMPING_SOFTWARE
              | SOF_TIMESTAMPING_OPT_CMSG
              | SOF_TIMESTAMPING_OPT_TSONLY;
    int Ret = setsockopt(Fd, SOL_SOCKET, SO_TIMESTAMPING, &Flags, sizeof(Flags));
    if (Ret < 0) Logi("SO_TIMESTAMPING unavailable - using userspace T4 fallback");
    return (Ret == 0);
}

static ssize_t RecvWithTs(int Fd, void* Buf, size_t Len, uint64_t* T4Out) {
    struct iovec Iov = { .iov_base = Buf, .iov_len = Len };
    uint8_t CtrlBuf[CMSG_SPACE(sizeof(struct timespec) * 3)];
    struct msghdr Msg;
    memset(&Msg, 0, sizeof(Msg));
    Msg.msg_iov        = &Iov;
    Msg.msg_iovlen     = 1;
    Msg.msg_control    = CtrlBuf;
    Msg.msg_controllen = sizeof(CtrlBuf);

    ssize_t N  = recvmsg(Fd, &Msg, 0);
    *T4Out     = NowUs();

    if (N > 0) {
        for (struct cmsghdr* Cm = CMSG_FIRSTHDR(&Msg); Cm; Cm = CMSG_NXTHDR(&Msg, Cm)) {
            if (Cm->cmsg_level == SOL_SOCKET && Cm->cmsg_type == SCM_TIMESTAMPING) {
                struct timespec* Ts = (struct timespec*)CMSG_DATA(Cm);
                if (Ts[0].tv_sec != 0 || Ts[0].tv_nsec != 0)
                    *T4Out = RealTsToMonoUs(&Ts[0]);
                break;
            }
        }
    }
    return N;
}

static AAudioStream* AudioStream = NULL;

static float SourceSample(int64_t Frame, int Channel) {
    if (Frame < 0 || Frame >= PcmFrames || PcmBuf == NULL) return 0.0f;
    if (PcmChannels <= 1) Channel = 0;
    else if (Channel >= PcmChannels) Channel = PcmChannels - 1;
    return PcmBuf[Frame * PcmChannels + Channel];
}

static float InterpolateSource(double Frame, int Channel) {
    if (Frame < 0.0 || Frame >= (double)(PcmFrames - 2)) return 0.0f;
    int64_t F = (int64_t)Frame;
    float Frac = (float)(Frame - (double)F);
    float P0 = SourceSample(F - 1, Channel);
    float P1 = SourceSample(F, Channel);
    float P2 = SourceSample(F + 1, Channel);
    float P3 = SourceSample(F + 2, Channel);
    float C0 = P1;
    float C1 = 0.5f * (P2 - P0);
    float C2 = P0 - 2.5f * P1 + 2.0f * P2 - 0.5f * P3;
    float C3 = 0.5f * (P3 - P0) + 1.5f * (P1 - P2);
    return C0 + Frac * (C1 + Frac * (C2 + Frac * C3));
}

static float RenderSample(double SourceFrame, int OutChannel) {
    if (StreamChannels == 1 && PcmChannels >= 2) {
        return 0.5f * (InterpolateSource(SourceFrame, 0) + InterpolateSource(SourceFrame, 1));
    }
    int Channel = (PcmChannels == 1) ? 0 : OutChannel;
    return InterpolateSource(SourceFrame, Channel);
}

static aaudio_data_callback_result_t AudioCallback(AAudioStream* St, void* U, void* Data, int32_t NumFrames) {
    (void)U;
    float* Out = (float*)Data;
    if (!atomic_load_explicit(&FireReady, memory_order_acquire)) {
        memset(Out, 0, (size_t)NumFrames * StreamChannels * sizeof(float));
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    int64_t HwFramePos = 0, HwPresentNs = 0;
    int64_t WritePresentsUs;
    if (AAudioStream_getTimestamp(St, CLOCK_MONOTONIC, &HwFramePos, &HwPresentNs) == AAUDIO_OK) {
        int64_t FramesWritten = AAudioStream_getFramesWritten(St);
        int64_t AheadFrames   = FramesWritten - HwFramePos;
        if (AheadFrames < 0) AheadFrames = 0;
        WritePresentsUs = HwPresentNs / 1000LL + AheadFrames * 1000000LL / StreamSampleRate;
    } else {
        WritePresentsUs = (int64_t)NowUs();
    }
    double CurrentServerTimeUs = (double)WritePresentsUs + (double)atomic_load_explicit(&ClockOffsetUs, memory_order_relaxed);
    double TargetFirePcUs      = (double)atomic_load_explicit(&LastFirePcUs, memory_order_relaxed)
                               + (double)atomic_load_explicit(&OutputTrimUs, memory_order_relaxed);
    double SourceFrame = (CurrentServerTimeUs - TargetFirePcUs) * (double)PcmSampleRate / 1000000.0;
    double SourceStep = (double)PcmSampleRate / (double)StreamSampleRate;
    for (int32_t I = 0; I < NumFrames; I++) {
        for (int C = 0; C < StreamChannels; C++) *Out++ = RenderSample(SourceFrame, C);
        SourceFrame += SourceStep;
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void DoClockSync(void) {
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
    int Tos = 0xB8, Prio = 6;
    setsockopt(SyncSock, IPPROTO_IP, IP_TOS,      &Tos,  sizeof(Tos));
    setsockopt(SyncSock, SOL_SOCKET, SO_PRIORITY,  &Prio, sizeof(Prio));
    struct timeval Tv = {0, 200000};
    setsockopt(SyncSock, SOL_SOCKET, SO_RCVTIMEO,  &Tv,   sizeof(Tv));
    InitRealToMonoDelta();
    EnableKernelRxTs(SyncSock);

    static SyncSample Samples[MaxSyncSamples];
    int SampleCount = 0;
    uint8_t RxBuf[64];
    struct timespec Gap = {0, 10000000};

    for (int I = 0; I < MaxSyncSamples; I++) {
        SyncReqPkt Req;
        Req.Type    = PtSyncReq;
        uint64_t T1 = NowUs();
        Req.T1      = T1;
        sendto(SyncSock, &Req, sizeof(Req), 0, (struct sockaddr*)&SrvAddr, sizeof(SrvAddr));
        uint64_t T4 = 0;
        ssize_t  N  = RecvWithTs(SyncSock, RxBuf, sizeof(RxBuf), &T4);
        nanosleep(&Gap, NULL);
        if (N < (ssize_t)sizeof(SyncAckPkt)) continue;
        SyncAckPkt* Ack = (SyncAckPkt*)RxBuf;
        if (Ack->Type != PtSyncAck || Ack->T1 != T1) continue;
        Samples[SampleCount].Rtt    = (int64_t)(T4 - T1);
        Samples[SampleCount].Offset = ((int64_t)Ack->T2 - (int64_t)T1
                                     + (int64_t)Ack->T3 - (int64_t)T4) / 2;
        SampleCount++;
    }
    close(SyncSock);
    if (SampleCount == 0) return;

    qsort(Samples, (size_t)SampleCount, sizeof(SyncSample), CmpRtt);

    int TopN = 10;
    if (TopN > SampleCount) TopN = SampleCount;

    double Sum = 0.0;
    for (int I = 0; I < TopN; I++) Sum += (double)Samples[I].Offset;
    double Mean = Sum / (double)TopN;

    double Var = 0.0;
    for (int I = 0; I < TopN; I++) {
        double D = (double)Samples[I].Offset - Mean;
        Var += D * D;
    }
    double Sigma = (TopN > 1) ? sqrt(Var / (double)(TopN - 1)) : 0.0;

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
    uint64_t T4 = 0;
    ssize_t  N  = RecvWithTs(SSock, RxBuf, sizeof(RxBuf), &T4);
    if (N < (ssize_t)sizeof(SyncAckPkt)) return;
    SyncAckPkt* Ack = (SyncAckPkt*)RxBuf;
    if (Ack->Type != PtSyncAck || Ack->T1 != T1) return;

    ProbeRing[ProbeRingHead].Rtt    = (int64_t)(T4 - T1);
    ProbeRing[ProbeRingHead].Offset = ((int64_t)Ack->T2 - (int64_t)T1
                                     + (int64_t)Ack->T3 - (int64_t)T4) / 2;
    ProbeRingHead = (ProbeRingHead + 1) % ProbeRingSize;
    if (ProbeRingHead == 0) ProbeRingFull = 1;

    int Count = ProbeRingFull ? ProbeRingSize : ProbeRingHead;

    if (Count < 10) {
        int64_t CurOff = atomic_load_explicit(&ClockOffsetUs, memory_order_relaxed);
        int64_t Diff   = ProbeRing[(ProbeRingHead + ProbeRingSize - 1) % ProbeRingSize].Offset - CurOff;
        if (Diff >  500) Diff =  500;
        if (Diff < -500) Diff = -500;
        atomic_store_explicit(&ClockOffsetUs, CurOff + Diff, memory_order_release);
        return;
    }

    SyncSample Tmp[ProbeRingSize];
    memcpy(Tmp, ProbeRing, (size_t)Count * sizeof(SyncSample));
    qsort(Tmp, (size_t)Count, sizeof(SyncSample), CmpRtt);

    int BestN = Count * 8 / 100;
    if (BestN < 2) BestN = 2;

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
        if (OffSigma < 1.0 || D <= 0.8 * OffSigma) { FSum += (double)Tmp[I].Offset; FC++; }
    }
    if (FC == 0) { FSum = OffMean; FC = 1; }
    int64_t TargetOffset = (int64_t)(FSum / (double)FC);

    int64_t CurOff = atomic_load_explicit(&ClockOffsetUs, memory_order_relaxed);
    int64_t Step   = (TargetOffset - CurOff) * 15 / 100;
    if (Step >  100) Step =  100;
    if (Step < -100) Step = -100;
    atomic_store_explicit(&ClockOffsetUs, CurOff + Step, memory_order_release);
}

static void* KeepAliveThread(void* U) {
    (void)U;
    struct sched_param KaSp;
    KaSp.sched_priority = sched_get_priority_max(SCHED_FIFO);
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &KaSp);
    uint8_t Ping  = PtRegister;
    int Cycle = 0;
    int SSock = socket(AF_INET, SOCK_DGRAM, 0);
    int Tos = 0xB8, Prio = 6;
    setsockopt(SSock, IPPROTO_IP, IP_TOS,      &Tos,  sizeof(Tos));
    setsockopt(SSock, SOL_SOCKET, SO_PRIORITY,  &Prio, sizeof(Prio));
    struct timeval Tv = {0, 25000};
    setsockopt(SSock, SOL_SOCKET, SO_RCVTIMEO, &Tv,  sizeof(Tv));
    EnableKernelRxTs(SSock);
    while (atomic_load_explicit(&Running, memory_order_relaxed)) {
        sendto(Sock, &Ping, 1, 0, (struct sockaddr*)&SrvAddr, sizeof(SrvAddr));
        DoRollingProbe(SSock);
        struct timespec Ts = {0, 30000000};
        nanosleep(&Ts, NULL);
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

JNIEXPORT void JNICALL Java_com_audiosync_app_MainActivity_NativeSetConsoleSink(JNIEnv* Env, jobject Obj) {
    (*Env)->GetJavaVM(Env, &AppVm);
    jclass Cls = (*Env)->GetObjectClass(Env, Obj);
    jmethodID Method = (*Env)->GetMethodID(Env, Cls, "NativeConsoleLog", "(Ljava/lang/String;)V");
    pthread_mutex_lock(&ConsoleLock);
    if (ConsoleSink != NULL) (*Env)->DeleteGlobalRef(Env, ConsoleSink);
    ConsoleSink = (*Env)->NewGlobalRef(Env, Obj);
    ConsoleLogMethod = Method;
    pthread_mutex_unlock(&ConsoleLock);
    (*Env)->DeleteLocalRef(Env, Cls);
    Logi("Console attached");
}

JNIEXPORT void JNICALL Java_com_audiosync_app_MainActivity_NativeClearConsoleSink(JNIEnv* Env, jobject Obj) {
    (void)Obj;
    pthread_mutex_lock(&ConsoleLock);
    if (ConsoleSink != NULL) {
        (*Env)->DeleteGlobalRef(Env, ConsoleSink);
        ConsoleSink = NULL;
    }
    ConsoleLogMethod = NULL;
    pthread_mutex_unlock(&ConsoleLock);
}

JNIEXPORT void JNICALL Java_com_audiosync_app_MainActivity_NativeSetOutputTrimUs(JNIEnv* Env, jobject Obj, jint TrimUs) {
    (void)Env; (void)Obj;
    atomic_store_explicit(&OutputTrimUs, (int64_t)TrimUs, memory_order_release);
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
    int Reuse = 1, BufSz = 65536, Tos = 0xB8, Prio = 6;
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
    Logi("Connecting to %s", SrvIp);
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
    Logi("Opening audio stream");
    atomic_store_explicit(&LastFirePcUs, 0, memory_order_release);
    AAudioStreamBuilder* Bld;
    AAudio_createStreamBuilder(&Bld);
    AAudioStreamBuilder_setFormat(Bld,          AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(Bld,    2);
    AAudioStreamBuilder_setPerformanceMode(Bld, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(Bld,     AAUDIO_SHARING_MODE_EXCLUSIVE);
    AAudioStream* Probe;
    if (AAudioStreamBuilder_openStream(Bld, &Probe) != AAUDIO_OK) {
        AAudioStreamBuilder_delete(Bld);
        return;
    }
    int32_t Burst = AAudioStream_getFramesPerBurst(Probe);
    int32_t NativeRate = AAudioStream_getSampleRate(Probe);
    AAudioStream_close(Probe);
    AAudioStreamBuilder_delete(Bld);
    AAudio_createStreamBuilder(&Bld);
    AAudioStreamBuilder_setFormat(Bld,                AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(Bld,          2);
    if (NativeRate > 0) AAudioStreamBuilder_setSampleRate(Bld, NativeRate);
    AAudioStreamBuilder_setPerformanceMode(Bld,       AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(Bld,           AAUDIO_SHARING_MODE_EXCLUSIVE);
    AAudioStreamBuilder_setUsage(Bld,                 AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(Bld,           AAUDIO_CONTENT_TYPE_SONIFICATION);
    AAudioStreamBuilder_setDataCallback(Bld,          AudioCallback, NULL);
    AAudioStreamBuilder_setFramesPerDataCallback(Bld, Burst);
    AAudioStream* St;
    if (AAudioStreamBuilder_openStream(Bld, &St) != AAUDIO_OK) {
        AAudioStreamBuilder_delete(Bld);
        return;
    }
    AAudioStreamBuilder_delete(Bld);
    AudioStream = St;
    StreamChannels = AAudioStream_getChannelCount(St);
    StreamSampleRate = AAudioStream_getSampleRate(St);
    if (StreamChannels <= 0) StreamChannels = 2;
    if (StreamSampleRate <= 0) StreamSampleRate = PcmSampleRate;
    AAudioStream_setBufferSizeInFrames(St, Burst * 2);
    AAudioStream_requestStart(St);
    Logi("Audio ready: burst=%d stream=%dch %dhz source=%dch %dhz", Burst, StreamChannels, StreamSampleRate, PcmChannels, PcmSampleRate);
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
    AudioStream = NULL;
}

JNIEXPORT void JNICALL Java_com_audiosync_app_MainActivity_NativeDisconnect(JNIEnv* Env, jobject Obj) {
    (void)Env; (void)Obj;
    atomic_store_explicit(&Running,   0, memory_order_release);
    atomic_store_explicit(&FireReady, 0, memory_order_release);
    if (Sock != -1) { shutdown(Sock, SHUT_RDWR); close(Sock); Sock = -1; }
    Logi("Disconnected");
}
