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

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"

#define Tag "AudioSync"

#define ServerPort        11000
#define ClientPort        11001
#define InitialProbeCount 700
#define ProbeRingSize     1024
#define DeltaRingSize     96
#define MaxSkew           0.0004
#define ProbeIntervalNs   25000000L

#define PtRegister 0x01
#define PtSyncReq  0x02
#define PtSyncAck  0x03
#define PtFire     0x04
#define PtSyncInfo 0x05

typedef struct __attribute__((packed)) {
    uint8_t Type;
    uint64_t T1;
} SyncReqPkt;

typedef struct __attribute__((packed)) {
    uint8_t Type;
    uint64_t T1;
    uint64_t T2;
    uint64_t T3;
} SyncAckPkt;

typedef struct __attribute__((packed)) {
    uint8_t Type;
    uint64_t FireAtPcUs;
} FirePkt;

typedef struct __attribute__((packed)) {
    uint8_t Type;
    int64_t OffsetUs;
    int64_t RttUs;
    int32_t SampleCount;
    float SigmaUs;
    float SkewPpm;
    float HwRateHz;
} SyncInfoPkt;

typedef struct { double LocalUs; double Rtt; double Offset; } SyncSample;
typedef struct { double MonoUs; double DeltaUs; } DeltaSample;

static float* PcmBuf          = NULL;
static int    PcmFrames       = 0;
static int    PcmChannels     = 2;
static int    PcmSampleRate   = 44100;
static int    StreamChannels  = 2;
static int    StreamSampleRate = 48000;

static AAudioStream* AudioStream = NULL;

static _Atomic int      Running      = 0;
static _Atomic int      FireReady    = 0;
static _Atomic uint32_t FireEpoch    = 0;
static _Atomic double   OutputTrimUs = 0;
static _Atomic uint64_t LastFirePcUs = 0;

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

static int CompareDouble(const void* A, const void* B) {
    double Da = *(const double*)A;
    double Db = *(const double*)B;
    return (Da > Db) - (Da < Db);
}

static uint64_t GetMonotonicMicroseconds(void) {
    struct timespec Ts;
    clock_gettime(CLOCK_MONOTONIC, &Ts);
    return (uint64_t)Ts.tv_sec * 1000000ULL + (uint64_t)Ts.tv_nsec / 1000ULL;
}

static double GetMonotonicMicrosecondsExact(void) {
    struct timespec Ts;
    clock_gettime(CLOCK_MONOTONIC, &Ts);
    return (double)Ts.tv_sec * 1000000.0 + (double)Ts.tv_nsec / 1000.0;
}

static double GetRealtimeMicrosecondsExact(void) {
    struct timespec Ts;
    clock_gettime(CLOCK_REALTIME, &Ts);
    return (double)Ts.tv_sec * 1000000.0 + (double)Ts.tv_nsec / 1000.0;
}

static DeltaSample     DeltaRing[DeltaRingSize];
static int             DeltaHead = 0;
static int             DeltaCount = 0;
static double          DeltaAnchorUs = 0.0;
static double          DeltaBaseUs = 0.0;
static double          DeltaSkew = 0.0;
static int             DeltaReady = 0;
static pthread_mutex_t DeltaLock = PTHREAD_MUTEX_INITIALIZER;

static void RefitDeltaModel(void) {
    if (DeltaCount < 2) {
        if (DeltaCount == 1) {
            DeltaAnchorUs = DeltaRing[0].MonoUs;
            DeltaBaseUs   = DeltaRing[0].DeltaUs;
            DeltaSkew     = 0.0;
            DeltaReady    = 1;
        }
        return;
    }
    double MinX = DeltaRing[0].MonoUs, MaxX = DeltaRing[0].MonoUs, Anchor = 0.0;
    for (int I = 0; I < DeltaCount; I++) {
        double X = DeltaRing[I].MonoUs;
        if (X < MinX) MinX = X;
        if (X > MaxX) MaxX = X;
        Anchor += X;
    }
    Anchor /= (double)DeltaCount;
    double Sx = 0.0, Sy = 0.0, Sxx = 0.0, Sxy = 0.0;
    for (int I = 0; I < DeltaCount; I++) {
        double X = DeltaRing[I].MonoUs - Anchor;
        double Y = DeltaRing[I].DeltaUs;
        Sx += X; Sy += Y; Sxx += X * X; Sxy += X * Y;
    }
    double N = (double)DeltaCount;
    double Den = N * Sxx - Sx * Sx;
    double B = 0.0;
    if ((MaxX - MinX) > 3000000.0 && Den > 0.0) {
        B = (N * Sxy - Sx * Sy) / Den;
        if (B >  MaxSkew) B =  MaxSkew;
        if (B < -MaxSkew) B = -MaxSkew;
    }
    DeltaAnchorUs = Anchor;
    DeltaBaseUs   = (Sy - B * Sx) / N;
    DeltaSkew     = B;
    DeltaReady    = 1;
}

static void SampleRealtimeToMonotonicDelta(void) {
    double BestUncert = 1e18, BestDelta = 0.0, BestMono = 0.0;
    for (int I = 0; I < 24; I++) {
        double M1 = GetMonotonicMicrosecondsExact();
        double R  = GetRealtimeMicrosecondsExact();
        double M2 = GetMonotonicMicrosecondsExact();
        double Uncert = (M2 - M1) * 0.5;
        if (Uncert < 0.0 || Uncert > 1e6) continue;
        if (Uncert < BestUncert) {
            BestUncert = Uncert;
            BestMono   = (M1 + M2) * 0.5;
            BestDelta  = BestMono - R;
        }
    }
    if (BestUncert > 1e17) return;
    pthread_mutex_lock(&DeltaLock);
    DeltaRing[DeltaHead].MonoUs  = BestMono;
    DeltaRing[DeltaHead].DeltaUs = BestDelta;
    DeltaHead = (DeltaHead + 1) % DeltaRingSize;
    if (DeltaCount < DeltaRingSize) DeltaCount++;
    RefitDeltaModel();
    pthread_mutex_unlock(&DeltaLock);
}

static void ResetDeltaModel(void) {
    pthread_mutex_lock(&DeltaLock);
    DeltaHead = 0;
    DeltaCount = 0;
    DeltaReady = 0;
    pthread_mutex_unlock(&DeltaLock);
    for (int I = 0; I < 6; I++) {
        SampleRealtimeToMonotonicDelta();
        struct timespec Ts = {0, 2000000};
        nanosleep(&Ts, NULL);
    }
    pthread_mutex_lock(&DeltaLock);
    double D = DeltaBaseUs, S = DeltaSkew;
    pthread_mutex_unlock(&DeltaLock);
    NativeLog(ANDROID_LOG_INFO, "RealToMono: delta=%.1f us  skew=%.2f ppm", D, S * 1e6);
}

static double EvaluateRealToMonoDelta(double AtMonoUs) {
    pthread_mutex_lock(&DeltaLock);
    double R = DeltaReady ? DeltaBaseUs + DeltaSkew * (AtMonoUs - DeltaAnchorUs) : 0.0;
    int Ready = DeltaReady;
    pthread_mutex_unlock(&DeltaLock);
    return Ready ? R : 0.0;
}

static int EnableKernelReceiveTimestamps(int Fd) {
    int Flags = SOF_TIMESTAMPING_RX_SOFTWARE
              | SOF_TIMESTAMPING_SOFTWARE
              | SOF_TIMESTAMPING_OPT_CMSG
              | SOF_TIMESTAMPING_OPT_TSONLY;
    int Ret = setsockopt(Fd, SOL_SOCKET, SO_TIMESTAMPING, &Flags, sizeof(Flags));
    if (Ret < 0) NativeLog(ANDROID_LOG_INFO, "SO_TIMESTAMPING unavailable - using userspace T4 fallback");
    return (Ret == 0);
}

static ssize_t ReceiveWithTimestamp(int Fd, void* Buf, size_t Len, double* T4Out) {
    struct iovec Iov = { .iov_base = Buf, .iov_len = Len };
    uint8_t CtrlBuf[CMSG_SPACE(sizeof(struct timespec) * 3)];
    struct msghdr Msg;
    memset(&Msg, 0, sizeof(Msg));
    Msg.msg_iov        = &Iov;
    Msg.msg_iovlen     = 1;
    Msg.msg_control    = CtrlBuf;
    Msg.msg_controllen = sizeof(CtrlBuf);

    ssize_t N = recvmsg(Fd, &Msg, 0);
    double  UserNow = GetMonotonicMicrosecondsExact();
    *T4Out = UserNow;

    if (N > 0) {
        for (struct cmsghdr* Cm = CMSG_FIRSTHDR(&Msg); Cm; Cm = CMSG_NXTHDR(&Msg, Cm)) {
            if (Cm->cmsg_level == SOL_SOCKET && Cm->cmsg_type == SCM_TIMESTAMPING) {
                struct timespec* Ts = (struct timespec*)CMSG_DATA(Cm);
                if (Ts[0].tv_sec != 0 || Ts[0].tv_nsec != 0) {
                    double RealUs = (double)Ts[0].tv_sec * 1000000.0 + (double)Ts[0].tv_nsec / 1000.0;
                    double Mono   = RealUs + EvaluateRealToMonoDelta(UserNow);
                    if (Mono <= UserNow + 1000.0 && Mono > UserNow - 2000000.0) *T4Out = Mono;
                }
                break;
            }
        }
    }
    return N;
}

static _Atomic uint32_t ClockSeq = 0;
static double ClockAnchorUs = 0.0;
static double ClockBaseUs   = 0.0;
static double ClockSkew     = 0.0;

static void PublishClockModel(double AnchorUs, double BaseUs, double Skew) {
    uint32_t S = atomic_load_explicit(&ClockSeq, memory_order_relaxed);
    atomic_store_explicit(&ClockSeq, S + 1, memory_order_release);
    atomic_thread_fence(memory_order_release);
    ClockAnchorUs = AnchorUs;
    ClockBaseUs   = BaseUs;
    ClockSkew     = Skew;
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&ClockSeq, S + 2, memory_order_release);
}

static void EvaluateClockModel(double LocalUs, double* OffsetOut, double* SkewOut) {
    uint32_t S0, S1;
    double A, B, K;
    do {
        S0 = atomic_load_explicit(&ClockSeq, memory_order_acquire);
        A = ClockAnchorUs;
        B = ClockBaseUs;
        K = ClockSkew;
        atomic_thread_fence(memory_order_acquire);
        S1 = atomic_load_explicit(&ClockSeq, memory_order_acquire);
    } while ((S0 & 1u) || S0 != S1);
    *OffsetOut = B + K * (LocalUs - A);
    *SkewOut   = K;
}

static SyncSample ProbeRing[ProbeRingSize];
static int    ProbeHead = 0;
static int    ProbeCount = 0;
static double ModelSigmaUs = 0.0;
static double ModelSemUs = 0.0;
static double ModelMinRttUs = 0.0;
static double ModelSkewPpm = 0.0;
static int    ModelUsedCount = 0;
static int    ModelReady = 0;

static void ResetProbeRing(void) {
    ProbeHead = 0;
    ProbeCount = 0;
    ModelReady = 0;
}

static void AddProbeSample(double T1, double T2, double T3, double T4) {
    double Rtt = (T4 - T1) - (T3 - T2);
    if (Rtt < 0.0) return;
    ProbeRing[ProbeHead].LocalUs = (T1 + T4) * 0.5;
    ProbeRing[ProbeHead].Rtt     = Rtt;
    ProbeRing[ProbeHead].Offset  = ((T2 - T1) + (T3 - T4)) * 0.5;
    ProbeHead = (ProbeHead + 1) % ProbeRingSize;
    if (ProbeCount < ProbeRingSize) ProbeCount++;
}

static double FitXs[ProbeRingSize];
static double FitYs[ProbeRingSize];
static double FitBase[ProbeRingSize];
static double FitWs[ProbeRingSize];
static double FitRes[ProbeRingSize];
static double FitMag[ProbeRingSize];

static int RefitClockModel(void) {
    if (ProbeCount < 6) return 0;

    double MinRtt = 1e18;
    for (int I = 0; I < ProbeCount; I++)
        if (ProbeRing[I].Rtt < MinRtt) MinRtt = ProbeRing[I].Rtt;

    double Gate = MinRtt + MinRtt * 0.25 + 300.0;
    int    N = 0;
    double Anchor = 0.0, MinX = 0.0, MaxX = 0.0;

    for (int I = 0; I < ProbeCount; I++) {
        if (ProbeRing[I].Rtt > Gate) continue;
        double X = ProbeRing[I].LocalUs;
        double Excess = (ProbeRing[I].Rtt - MinRtt) / 60.0;
        FitXs[N]   = X;
        FitYs[N]   = ProbeRing[I].Offset;
        FitBase[N] = 1.0 / (1.0 + Excess * Excess);
        FitWs[N]   = FitBase[N];
        if (N == 0) { MinX = X; MaxX = X; }
        else { if (X < MinX) MinX = X; if (X > MaxX) MaxX = X; }
        Anchor += X;
        N++;
    }
    if (N < 6) return 0;
    Anchor /= (double)N;

    double Span = MaxX - MinX;
    int    AllowSkew = (Span > 4000000.0 && N >= 24);
    double A = 0.0, B = 0.0, Scale = 1.0, Neff = (double)N;

    for (int Iter = 0; Iter < 3; Iter++) {
        double Sw = 0.0, Sx = 0.0, Sy = 0.0, Sxx = 0.0, Sxy = 0.0, Sww = 0.0;
        for (int I = 0; I < N; I++) {
            double X = FitXs[I] - Anchor;
            double W = FitWs[I];
            Sw += W; Sww += W * W;
            Sx += W * X; Sy += W * FitYs[I];
            Sxx += W * X * X; Sxy += W * X * FitYs[I];
        }
        if (Sw <= 1e-9) return 0;
        if (AllowSkew) {
            double Den = Sw * Sxx - Sx * Sx;
            B = (Den > 0.0) ? (Sw * Sxy - Sx * Sy) / Den : 0.0;
            if (B >  MaxSkew) B =  MaxSkew;
            if (B < -MaxSkew) B = -MaxSkew;
        } else {
            B = 0.0;
        }
        A = (Sy - B * Sx) / Sw;
        Neff = (Sww > 0.0) ? (Sw * Sw) / Sww : (double)N;

        for (int I = 0; I < N; I++) {
            FitRes[I] = FitYs[I] - (A + B * (FitXs[I] - Anchor));
            FitMag[I] = fabs(FitRes[I]);
        }
        qsort(FitMag, (size_t)N, sizeof(double), CompareDouble);
        Scale = 1.4826 * FitMag[N / 2];
        if (Scale < 1.0) Scale = 1.0;

        for (int I = 0; I < N; I++) {
            double U = FitRes[I] / (4.0 * Scale);
            double Rw = (U * U < 1.0) ? (1.0 - U * U) * (1.0 - U * U) : 0.0;
            FitWs[I] = FitBase[I] * Rw;
        }
    }

    PublishClockModel(Anchor, A, B);
    ModelSigmaUs   = Scale;
    ModelSemUs     = (Neff > 1.0) ? Scale / sqrt(Neff) : Scale;
    ModelMinRttUs  = MinRtt;
    ModelSkewPpm   = B * 1e6;
    ModelUsedCount = N;
    ModelReady     = 1;
    return 1;
}

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

static double HwSw = 0.0, HwSx = 0.0, HwSy = 0.0, HwSxx = 0.0, HwSxy = 0.0;
static double HwFrameOrigin = 0.0, HwTimeOrigin = 0.0;
static double HwSlopeUsPerFrame = 0.0;
static double HwInterceptUs = 0.0;
static int    HwInit = 0;
static int    HwFitCount = 0;
static _Atomic double HwMeasuredRateHz = 0.0;

static void ResetHardwareClock(void) {
    HwSw = HwSx = HwSy = HwSxx = HwSxy = 0.0;
    HwFrameOrigin = HwTimeOrigin = 0.0;
    HwSlopeUsPerFrame = 1000000.0 / (double)StreamSampleRate;
    HwInterceptUs = 0.0;
    HwInit = 0;
    HwFitCount = 0;
    atomic_store_explicit(&HwMeasuredRateHz, 0.0, memory_order_release);
}

static void UpdateHardwareClock(double FramePos, double PresentUs) {
    double Nominal = 1000000.0 / (double)StreamSampleRate;
    if (!HwInit) {
        HwFrameOrigin     = FramePos;
        HwTimeOrigin      = PresentUs;
        HwSlopeUsPerFrame = Nominal;
        HwInterceptUs     = 0.0;
        HwInit            = 1;
    }
    double X = FramePos - HwFrameOrigin;
    double Y = PresentUs - HwTimeOrigin;
    if (HwFitCount > 64) {
        double Predicted = HwInterceptUs + HwSlopeUsPerFrame * X;
        if (fabs(Y - Predicted) > 4000.0) return;
    }
    const double Lambda = 0.9997;
    HwSw  = HwSw  * Lambda + 1.0;
    HwSx  = HwSx  * Lambda + X;
    HwSy  = HwSy  * Lambda + Y;
    HwSxx = HwSxx * Lambda + X * X;
    HwSxy = HwSxy * Lambda + X * Y;
    if (HwFitCount < 1 << 30) HwFitCount++;
    double Den = HwSw * HwSxx - HwSx * HwSx;
    if (HwFitCount >= 16 && Den > 1e-6) {
        double Slope = (HwSw * HwSxy - HwSx * HwSy) / Den;
        if (Slope > Nominal * 0.95 && Slope < Nominal * 1.05) {
            HwSlopeUsPerFrame = Slope;
            HwInterceptUs     = (HwSy - Slope * HwSx) / HwSw;
            atomic_store_explicit(&HwMeasuredRateHz, 1000000.0 / Slope, memory_order_relaxed);
        }
    } else {
        HwInterceptUs = (HwSy - HwSlopeUsPerFrame * HwSx) / (HwSw > 0.0 ? HwSw : 1.0);
    }
}

static double PredictPresentUs(double FramePos) {
    if (!HwInit) return 0.0;
    return HwTimeOrigin + HwInterceptUs + HwSlopeUsPerFrame * (FramePos - HwFrameOrigin);
}

static double   PlayFrame = 0.0;
static int      PlayInit = 0;
static double   PllIntegral = 0.0;
static uint32_t CallbackFireEpoch = 0;

static aaudio_data_callback_result_t AudioCallback(AAudioStream* St, void* U, void* Data, int32_t NumFrames) {
    (void)U;
    float* Out = (float*)Data;

    uint32_t Epoch = atomic_load_explicit(&FireEpoch, memory_order_acquire);
    if (Epoch != CallbackFireEpoch) {
        CallbackFireEpoch = Epoch;
        PlayInit    = 0;
        PllIntegral = 0.0;
    }

    if (!atomic_load_explicit(&FireReady, memory_order_acquire)) {
        memset(Out, 0, (size_t)NumFrames * StreamChannels * sizeof(float));
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    int64_t HwFramePos = 0, HwPresentNs = 0;
    double  FramesWritten = (double)AAudioStream_getFramesWritten(St);
    if (AAudioStream_getTimestamp(St, CLOCK_MONOTONIC, &HwFramePos, &HwPresentNs) == AAUDIO_OK
        && HwFramePos > 0 && HwPresentNs > 0) {
        UpdateHardwareClock((double)HwFramePos, (double)HwPresentNs / 1000.0);
    }

    double WritePresentsUs;
    if (HwInit && HwFitCount >= 16) {
        WritePresentsUs = PredictPresentUs(FramesWritten);
    } else {
        WritePresentsUs = GetMonotonicMicrosecondsExact()
                        + (double)AAudioStream_getBufferSizeInFrames(St) * HwSlopeUsPerFrame;
    }

    double OffsetUs = 0.0, SkewValue = 0.0;
    EvaluateClockModel(WritePresentsUs, &OffsetUs, &SkewValue);

    double ServerNowUs    = WritePresentsUs + OffsetUs;
    double TargetFirePcUs = (double)atomic_load_explicit(&LastFirePcUs, memory_order_relaxed)
                          + atomic_load_explicit(&OutputTrimUs, memory_order_relaxed);
    double IdealFrame = (ServerNowUs - TargetFirePcUs) * (double)PcmSampleRate / 1000000.0;

    double ReseekLimit = (double)PcmSampleRate * 0.02;
    double Error = IdealFrame - PlayFrame;
    if (!PlayInit || fabs(Error) > ReseekLimit || !isfinite(PlayFrame)) {
        PlayFrame   = IdealFrame;
        PllIntegral = 0.0;
        PlayInit    = 1;
        Error       = 0.0;
    }

    double NominalStep = (double)PcmSampleRate * HwSlopeUsPerFrame / 1000000.0 * (1.0 + SkewValue);
    double Dt          = (double)NumFrames * HwSlopeUsPerFrame / 1000000.0;
    double Corr        = Error / (0.4 * (double)StreamSampleRate);
    if (Corr >  0.02) Corr =  0.02;
    if (Corr < -0.02) Corr = -0.02;
    PllIntegral += Corr * Dt / 3.0;
    if (PllIntegral >  0.002) PllIntegral =  0.002;
    if (PllIntegral < -0.002) PllIntegral = -0.002;

    double Step = NominalStep + Corr + PllIntegral;
    double StepMin = NominalStep * 0.97;
    double StepMax = NominalStep * 1.03;
    if (Step < StepMin) Step = StepMin;
    if (Step > StepMax) Step = StepMax;

    double SourceFrame = PlayFrame;
    for (int32_t I = 0; I < NumFrames; I++) {
        for (int C = 0; C < StreamChannels; C++) *Out++ = RenderSample(SourceFrame, C);
        SourceFrame += Step;
    }
    PlayFrame = SourceFrame;
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void ApplyRealtimeScheduling(void) {
    struct sched_param Sp;
    Sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &Sp);
    cpu_set_t Cpus;
    CPU_ZERO(&Cpus);
    int NumCpus = sysconf(_SC_NPROCESSORS_ONLN);
    for (int I = NumCpus / 2; I < NumCpus; I++) CPU_SET(I, &Cpus);
    sched_setaffinity(0, sizeof(Cpus), &Cpus);
}

static void SendSyncInfo(void) {
    if (Sock == -1 || !ModelReady) return;
    double Offset = 0.0, Skew = 0.0;
    EvaluateClockModel(GetMonotonicMicrosecondsExact(), &Offset, &Skew);
    SyncInfoPkt Info;
    Info.Type        = PtSyncInfo;
    Info.OffsetUs    = (int64_t)Offset;
    Info.RttUs       = (int64_t)ModelMinRttUs;
    Info.SampleCount = (int32_t)ProbeCount;
    Info.SigmaUs     = (float)ModelSemUs;
    Info.SkewPpm     = (float)ModelSkewPpm;
    Info.HwRateHz    = (float)atomic_load_explicit(&HwMeasuredRateHz, memory_order_relaxed);
    sendto(Sock, &Info, sizeof(Info), 0, (struct sockaddr*)&SrvAddr, sizeof(SrvAddr));
}

static void SynchronizeClock(void) {
    ApplyRealtimeScheduling();

    int SyncSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (SyncSock < 0) return;
    int Tos = 0xB8, Prio = 6;
    setsockopt(SyncSock, IPPROTO_IP, IP_TOS,      &Tos,  sizeof(Tos));
    setsockopt(SyncSock, SOL_SOCKET, SO_PRIORITY, &Prio, sizeof(Prio));
    struct timeval Tv = {0, 120000};
    setsockopt(SyncSock, SOL_SOCKET, SO_RCVTIMEO, &Tv,   sizeof(Tv));

    ResetDeltaModel();
    ResetProbeRing();

    int SioActive = EnableKernelReceiveTimestamps(SyncSock);
    NativeLog(ANDROID_LOG_INFO, "ClockSync: RX timestamp mode = %s",
              SioActive ? "kernel SO_TIMESTAMPING" : "userspace fallback");

    uint8_t RxBuf[64];
    struct timespec Gap = {0, 7000000};
    int Accepted = 0;

    for (int I = 0; I < InitialProbeCount; I++) {
        if ((I % 90) == 0) SampleRealtimeToMonotonicDelta();
        SyncReqPkt Req;
        Req.Type    = PtSyncReq;
        double T1   = GetMonotonicMicrosecondsExact();
        Req.T1      = (uint64_t)T1;
        sendto(SyncSock, &Req, sizeof(Req), 0, (struct sockaddr*)&SrvAddr, sizeof(SrvAddr));
        double  T4 = 0.0;
        ssize_t N  = ReceiveWithTimestamp(SyncSock, RxBuf, sizeof(RxBuf), &T4);
        nanosleep(&Gap, NULL);
        if (N < (ssize_t)sizeof(SyncAckPkt)) continue;
        SyncAckPkt* Ack = (SyncAckPkt*)RxBuf;
        if (Ack->Type != PtSyncAck || Ack->T1 != Req.T1) continue;
        AddProbeSample(T1, (double)Ack->T2, (double)Ack->T3, T4);
        Accepted++;
    }
    close(SyncSock);

    if (!RefitClockModel()) {
        NativeLog(ANDROID_LOG_ERROR, "ClockSync failed: accepted=%d", Accepted);
        return;
    }

    double Offset = 0.0, Skew = 0.0;
    EvaluateClockModel(GetMonotonicMicrosecondsExact(), &Offset, &Skew);
    NativeLog(ANDROID_LOG_INFO,
              "ClockSync: accepted=%d  used=%d  offset=%+.1f us  minRTT=%.1f us  "
              "sigma=%.1f us  precision=%.2f us  skew=%.2f ppm",
              Accepted, ModelUsedCount, Offset, ModelMinRttUs,
              ModelSigmaUs, ModelSemUs, ModelSkewPpm);

    SendSyncInfo();
}

static void UpdateRollingProbe(int SSock, int* RefitTick) {
    SyncReqPkt Req;
    Req.Type  = PtSyncReq;
    double T1 = GetMonotonicMicrosecondsExact();
    Req.T1    = (uint64_t)T1;
    sendto(SSock, &Req, sizeof(Req), 0, (struct sockaddr*)&SrvAddr, sizeof(SrvAddr));
    uint8_t RxBuf[64];
    double  T4 = 0.0;
    ssize_t N  = ReceiveWithTimestamp(SSock, RxBuf, sizeof(RxBuf), &T4);
    if (N < (ssize_t)sizeof(SyncAckPkt)) return;
    SyncAckPkt* Ack = (SyncAckPkt*)RxBuf;
    if (Ack->Type != PtSyncAck || Ack->T1 != Req.T1) return;
    AddProbeSample(T1, (double)Ack->T2, (double)Ack->T3, T4);
    (*RefitTick)++;
    if (ProbeCount < 64 || (*RefitTick % 4) == 0) RefitClockModel();
}

static void* KeepAliveThread(void* U) {
    (void)U;
    ApplyRealtimeScheduling();
    uint8_t Ping = PtRegister;
    int SSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (SSock < 0) return NULL;
    int Tos = 0xB8, Prio = 6;
    setsockopt(SSock, IPPROTO_IP, IP_TOS,      &Tos,  sizeof(Tos));
    setsockopt(SSock, SOL_SOCKET, SO_PRIORITY, &Prio, sizeof(Prio));
    struct timeval Tv = {0, 22000};
    setsockopt(SSock, SOL_SOCKET, SO_RCVTIMEO, &Tv, sizeof(Tv));
    EnableKernelReceiveTimestamps(SSock);
    int RefitTick = 0;
    int Tick = 0;
    while (atomic_load_explicit(&Running, memory_order_relaxed)) {
        if ((Tick % 40) == 0) SampleRealtimeToMonotonicDelta();
        sendto(SSock, &Ping, 1, 0, (struct sockaddr*)&SrvAddr, sizeof(SrvAddr));
        UpdateRollingProbe(SSock, &RefitTick);
        if ((Tick % 80) == 79) {
            SendSyncInfo();
            double Offset = 0.0, Skew = 0.0;
            EvaluateClockModel(GetMonotonicMicrosecondsExact(), &Offset, &Skew);
            NativeLog(ANDROID_LOG_INFO,
                      "Sync: offset=%+.1f us  skew=%.2f ppm  minRTT=%.1f us  "
                      "sigma=%.1f us  precision=%.2f us  hw=%.3f Hz",
                      Offset, ModelSkewPpm, ModelMinRttUs, ModelSigmaUs, ModelSemUs,
                      atomic_load_explicit(&HwMeasuredRateHz, memory_order_relaxed));
        }
        Tick++;
        struct timespec Ts = {0, ProbeIntervalNs};
        nanosleep(&Ts, NULL);
    }
    close(SSock);
    return NULL;
}

JNIEXPORT jstring JNICALL Java_com_audiosync_app_MainActivity_NativeDecodeMp3(JNIEnv* Env, jobject Obj, jbyteArray Mp3Data) {
    (void)Obj;
    jsize Len = (*Env)->GetArrayLength(Env, Mp3Data);
    jbyte* Raw = (*Env)->GetByteArrayElements(Env, Mp3Data, NULL);
    drmp3_config Config = {0};
    drmp3_uint64 FrameCount = 0;
    float* Pcm = Raw == NULL ? NULL : drmp3_open_memory_and_read_pcm_frames_f32(
        Raw, (size_t)Len, &Config, &FrameCount, NULL);
    (*Env)->ReleaseByteArrayElements(Env, Mp3Data, Raw, JNI_ABORT);
    if (Pcm == NULL || Config.channels == 0 || Config.sampleRate == 0) {
        drmp3_free(Pcm, NULL);
        return (*Env)->NewStringUTF(Env, "ERROR:No frames decoded");
    }
    drmp3_free(PcmBuf, NULL);
    PcmBuf        = Pcm;
    PcmFrames     = (int)FrameCount;
    PcmChannels   = (int)Config.channels;
    PcmSampleRate = (int)Config.sampleRate;
    atomic_store(&FireReady, 0);
    NativeLog(ANDROID_LOG_INFO, "Decoded: %d frames  %d ch  %d hz", PcmFrames, PcmChannels, PcmSampleRate);
    char Msg[64];
    float Secs = (float)PcmFrames / PcmSampleRate;
    snprintf(Msg, sizeof(Msg), "OK:%d:%d:%.1f", PcmChannels, PcmSampleRate, Secs);
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
    NativeLog(ANDROID_LOG_INFO, "Console attached");
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

JNIEXPORT void JNICALL Java_com_audiosync_app_MainActivity_NativeSetOutputTrimUs(JNIEnv* Env, jobject Obj, jdouble TrimUs) {
    (void)Env; (void)Obj;
    atomic_store_explicit(&OutputTrimUs, (double)TrimUs, memory_order_release);
}

JNIEXPORT void JNICALL Java_com_audiosync_app_MainActivity_NativeConnect(JNIEnv* Env, jobject Obj, jstring IpStr) {
    (void)Obj;
    atomic_store_explicit(&Running, 0, memory_order_release);
    struct timespec Settle = {0, 60000000};
    nanosleep(&Settle, NULL);
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
        NativeLog(ANDROID_LOG_ERROR, "bind failed");
        close(Sock);
        Sock = -1;
        return;
    }
    struct ip_mreq Mreq;
    Mreq.imr_multiaddr.s_addr = inet_addr("224.0.0.100");
    Mreq.imr_interface.s_addr = INADDR_ANY;
    setsockopt(Sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &Mreq, sizeof(Mreq));
    memset(&SrvAddr, 0, sizeof(SrvAddr));
    SrvAddr.sin_family = AF_INET;
    SrvAddr.sin_port   = htons(ServerPort);
    inet_pton(AF_INET, SrvIp, &SrvAddr.sin_addr);
    NativeLog(ANDROID_LOG_INFO, "Connecting to %s", SrvIp);
    SynchronizeClock();
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
    NativeLog(ANDROID_LOG_INFO, "Opening audio stream");
    atomic_store_explicit(&LastFirePcUs, 0, memory_order_release);
    AAudioStreamBuilder* Bld;
    AAudio_createStreamBuilder(&Bld);
    AAudioStreamBuilder_setFormat(Bld,          AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(Bld,    2);
    AAudioStreamBuilder_setPerformanceMode(Bld, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(Bld,     AAUDIO_SHARING_MODE_EXCLUSIVE);
    AAudioStream* Probe;
    aaudio_result_t ProbeResult = AAudioStreamBuilder_openStream(Bld, &Probe);
    int UsedExclusive = 1;
    if (ProbeResult != AAUDIO_OK) {
        NativeLog(ANDROID_LOG_INFO, "Exclusive stream unavailable (%d), falling back to shared mode", (int)ProbeResult);
        UsedExclusive = 0;
        AAudioStreamBuilder_setSharingMode(Bld, AAUDIO_SHARING_MODE_SHARED);
        ProbeResult = AAudioStreamBuilder_openStream(Bld, &Probe);
        if (ProbeResult != AAUDIO_OK) {
            NativeLog(ANDROID_LOG_ERROR, "Failed to open probe audio stream (%d)", (int)ProbeResult);
            AAudioStreamBuilder_delete(Bld);
            return;
        }
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
    AAudioStreamBuilder_setSharingMode(Bld,           UsedExclusive ? AAUDIO_SHARING_MODE_EXCLUSIVE : AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setUsage(Bld,                 AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(Bld,           AAUDIO_CONTENT_TYPE_SONIFICATION);
    AAudioStreamBuilder_setDataCallback(Bld,          AudioCallback, NULL);
    AAudioStreamBuilder_setFramesPerDataCallback(Bld, Burst);
    AAudioStream* St;
    aaudio_result_t OpenResult = AAudioStreamBuilder_openStream(Bld, &St);
    if (OpenResult != AAUDIO_OK && UsedExclusive) {
        NativeLog(ANDROID_LOG_INFO, "Exclusive stream open failed (%d), retrying shared", (int)OpenResult);
        AAudioStreamBuilder_setSharingMode(Bld, AAUDIO_SHARING_MODE_SHARED);
        OpenResult = AAudioStreamBuilder_openStream(Bld, &St);
    }
    if (OpenResult != AAUDIO_OK) {
        NativeLog(ANDROID_LOG_ERROR, "Failed to open audio stream (%d)", (int)OpenResult);
        AAudioStreamBuilder_delete(Bld);
        return;
    }
    AAudioStreamBuilder_delete(Bld);
    AudioStream = St;
    StreamChannels = AAudioStream_getChannelCount(St);
    StreamSampleRate = AAudioStream_getSampleRate(St);
    if (StreamChannels <= 0) StreamChannels = 2;
    if (StreamSampleRate <= 0) StreamSampleRate = PcmSampleRate;
    ResetHardwareClock();
    PlayInit    = 0;
    PllIntegral = 0.0;
    AAudioStream_setBufferSizeInFrames(St, Burst * 2);
    AAudioStream_requestStart(St);
    NativeLog(ANDROID_LOG_INFO, "Audio ready: burst=%d stream=%dch %dhz source=%dch %dhz",
              Burst, StreamChannels, StreamSampleRate, PcmChannels, PcmSampleRate);
    ApplyRealtimeScheduling();
    uint8_t RxBuf[32];
    while (atomic_load_explicit(&Running, memory_order_relaxed)) {
        ssize_t N = recv(Sock, RxBuf, sizeof(RxBuf), 0);
        if (N < 1) break;
        if (RxBuf[0] != PtFire || N < (ssize_t)sizeof(FirePkt)) continue;
        FirePkt* Fp = (FirePkt*)RxBuf;
        if (Fp->FireAtPcUs == atomic_load_explicit(&LastFirePcUs, memory_order_relaxed)) continue;
        atomic_store_explicit(&LastFirePcUs, Fp->FireAtPcUs, memory_order_release);
        atomic_fetch_add_explicit(&FireEpoch, 1, memory_order_release);
        atomic_store_explicit(&FireReady, 1, memory_order_release);
        NativeLog(ANDROID_LOG_INFO, "Fire received: target PC time: %lld us", (long long)Fp->FireAtPcUs);
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
    NativeLog(ANDROID_LOG_INFO, "Disconnected");
}