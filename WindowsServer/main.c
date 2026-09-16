#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <timeapi.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

#ifndef SIO_TIMESTAMPING
#define SIO_TIMESTAMPING _WSAIOW(IOC_VENDOR, 28)
typedef struct _TIMESTAMPING_CONFIG {
    UINT32 Flags;
    UINT32 TxTimestampsBufferCount;
} TIMESTAMPING_CONFIG, *PTIMESTAMPING_CONFIG;
#define TIMESTAMPING_FLAG_RX 0x1
#define TIMESTAMPING_FLAG_TX 0x2
#endif

#ifndef SO_TIMESTAMP
#define SO_TIMESTAMP 0x300A
#endif

#define ServerPort 11000
#define ClientPort 11001
#define MaxClients 64
#define PtRegister 0x01
#define PtSyncReq  0x02
#define PtSyncAck  0x03
#define PtFire     0x04
#define PtSyncInfo 0x05

#pragma pack(push,1)
typedef struct {
    uint8_t Type;
    uint64_t T1;
} SyncReqPkt;

typedef struct {
    uint8_t Type;
    uint64_t T1;
    uint64_t T2;
    uint64_t T3;
} SyncAckPkt;

typedef struct {
    uint8_t Type;
    uint64_t FireAtPcUs;
} FirePkt;

typedef struct {
    uint8_t Type;
    int64_t OffsetUs;
    int64_t RttUs;
    int32_t SampleCount;
    float SigmaUs;
    float SkewPpm;
    float HwRateHz;
} SyncInfoPkt;
#pragma pack(pop)

typedef struct {
    struct sockaddr_in Addr;
    char    Ip[32];
    int      HasSyncInfo;
    int64_t  LastOffsetUs;
    int64_t  LastRttUs;
    float    LastSigmaUs;
    float    LastSkewPpm;
    float    LastHwRateHz;
    uint64_t LastReportUs;
} Client;

static SOCKET Sock;
static Client Clients[MaxClients];
static int ClientCount = 0;
static CRITICAL_SECTION ClientLock;

static void PrintCancellationForPair(int IndexA, int IndexB) {
    double SigmaA = (double)Clients[IndexA].LastSigmaUs;
    double SigmaB = (double)Clients[IndexB].LastSigmaUs;
    double DeltaUs = sqrt(SigmaA * SigmaA + SigmaB * SigmaB);
    double SkewDiff = fabs((double)Clients[IndexA].LastSkewPpm - (double)Clients[IndexB].LastSkewPpm);
    double RateA = (double)Clients[IndexA].LastHwRateHz;
    double RateB = (double)Clients[IndexB].LastHwRateHz;
    double RateDiffPpm = 0.0;
    if (RateA > 1.0 && RateB > 1.0) RateDiffPpm = fabs(RateA - RateB) / RateB * 1e6;
    if (DeltaUs <= 0.0) {
        printf("      Device %d vs Device %d: mismatch=0.0 us (no cancellation)\n", IndexA + 1, IndexB + 1);
        return;
    }
    double DeltaSec = DeltaUs / 1000000.0;
    double CancelHz = 1.0 / (2.0 * DeltaSec);
    printf("      Device %d vs Device %d: est. mismatch=%.2f us -> first cancel at %.0f Hz  "
           "(clock skew diff=%.2f ppm, dac rate diff=%.2f ppm)\n",
           IndexA + 1, IndexB + 1, DeltaUs, CancelHz, SkewDiff, RateDiffPpm);
}

static void EvaluateDelayMismatches(void) {
    int SyncedIdx[MaxClients];
    int SyncedCount = 0;
    for (int I = 0; I < ClientCount; I++) {
        if (Clients[I].HasSyncInfo) SyncedIdx[SyncedCount++] = I;
    }
    if (SyncedCount < 2) return;
    printf("    Delay mismatch analysis:\n");
    for (int I = 0; I < SyncedCount; I++) {
        for (int J = I + 1; J < SyncedCount; J++) {
            PrintCancellationForPair(SyncedIdx[I], SyncedIdx[J]);
        }
    }
}

static int64_t QpcFrequency(void) {
    static LARGE_INTEGER F;
    static int Init = 0;
    if (!Init) { QueryPerformanceFrequency(&F); Init = 1; }
    return F.QuadPart;
}

static uint64_t QpcTicksToMicroseconds(int64_t Ticks) {
    int64_t Freq = QpcFrequency();
    if (Freq <= 0) return 0;
    int64_t Secs = Ticks / Freq;
    int64_t Rem  = Ticks % Freq;
    return (uint64_t)(Secs * 1000000LL + (Rem * 1000000LL) / Freq);
}

static uint64_t GetMonotonicMicroseconds(void) {
    LARGE_INTEGER T;
    QueryPerformanceCounter(&T);
    return QpcTicksToMicroseconds(T.QuadPart);
}

static double GetMonotonicMicrosecondsExact(void) {
    LARGE_INTEGER T;
    QueryPerformanceCounter(&T);
    int64_t Freq = QpcFrequency();
    if (Freq <= 0) return 0.0;
    int64_t Secs = T.QuadPart / Freq;
    int64_t Rem  = T.QuadPart % Freq;
    return (double)Secs * 1000000.0 + ((double)Rem * 1000000.0) / (double)Freq;
}

static LPFN_WSARECVMSG WSARecvMsgPtr   = NULL;
static int             SioTimestamping = 0;
static int             PlainSoTimestamp = 0;

#define DeltaRingSize 96
#define MaxSkew 0.0004

typedef struct { double QpcUs; double DeltaUs; } DeltaSample;

static DeltaSample DeltaRing[DeltaRingSize];
static int         DeltaHead = 0;
static int         DeltaCount = 0;
static double      DeltaAnchorUs = 0.0;
static double      DeltaBaseUs = 0.0;
static double      DeltaSkew = 0.0;
static int         DeltaReady = 0;
static double      DeltaNextSampleUs = 0.0;

static double GetFileTimeMicroseconds(void) {
    FILETIME Ft;
    GetSystemTimePreciseAsFileTime(&Ft);
    uint64_t Raw = ((uint64_t)Ft.dwHighDateTime << 32) | (uint64_t)Ft.dwLowDateTime;
    return (double)Raw / 10.0;
}

static void RefitFileTimeToQpcDelta(void) {
    if (DeltaCount < 1) return;
    if (DeltaCount < 2) {
        DeltaAnchorUs = DeltaRing[0].QpcUs;
        DeltaBaseUs   = DeltaRing[0].DeltaUs;
        DeltaSkew     = 0.0;
        DeltaReady    = 1;
        return;
    }
    double MinX = DeltaRing[0].QpcUs, MaxX = DeltaRing[0].QpcUs, Anchor = 0.0;
    for (int I = 0; I < DeltaCount; I++) {
        double X = DeltaRing[I].QpcUs;
        if (X < MinX) MinX = X;
        if (X > MaxX) MaxX = X;
        Anchor += X;
    }
    Anchor /= (double)DeltaCount;
    double Sx = 0.0, Sy = 0.0, Sxx = 0.0, Sxy = 0.0;
    for (int I = 0; I < DeltaCount; I++) {
        double X = DeltaRing[I].QpcUs - Anchor;
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

static void SampleFileTimeToQpcDelta(void) {
    double BestUncert = 1e18, BestDelta = 0.0, BestQpc = 0.0;
    for (int I = 0; I < 24; I++) {
        double Q1 = GetMonotonicMicrosecondsExact();
        double Ft = GetFileTimeMicroseconds();
        double Q2 = GetMonotonicMicrosecondsExact();
        double Uncert = (Q2 - Q1) * 0.5;
        if (Uncert < 0.0 || Uncert > 1e6) continue;
        if (Uncert < BestUncert) {
            BestUncert = Uncert;
            BestQpc    = (Q1 + Q2) * 0.5;
            BestDelta  = BestQpc - Ft;
        }
    }
    if (BestUncert > 1e17) return;
    DeltaRing[DeltaHead].QpcUs   = BestQpc;
    DeltaRing[DeltaHead].DeltaUs = BestDelta;
    DeltaHead = (DeltaHead + 1) % DeltaRingSize;
    if (DeltaCount < DeltaRingSize) DeltaCount++;
    RefitFileTimeToQpcDelta();
    DeltaNextSampleUs = BestQpc + 500000.0;
}

static void InitializeFileTimeToQpcDelta(void) {
    if (DeltaReady) return;
    for (int I = 0; I < 6; I++) {
        SampleFileTimeToQpcDelta();
        Sleep(2);
    }
}

static double EvaluateFileTimeToQpcDelta(double AtQpcUs) {
    if (!DeltaReady) return 0.0;
    if (AtQpcUs > DeltaNextSampleUs) SampleFileTimeToQpcDelta();
    return DeltaBaseUs + DeltaSkew * (AtQpcUs - DeltaAnchorUs);
}

static int EnableKernelReceiveTimestamps(SOCKET S) {
    GUID Guid = WSAID_WSARECVMSG;
    DWORD Bytes = 0;
    if (WSAIoctl(S, SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &Guid, sizeof(Guid), &WSARecvMsgPtr, sizeof(WSARecvMsgPtr),
                 &Bytes, NULL, NULL) != 0) {
        printf("  [TS] WSARecvMsg extension lookup failed, WSAGetLastError=%d\n", WSAGetLastError());
        WSARecvMsgPtr = NULL;
        return 0;
    }

    TIMESTAMPING_CONFIG Cfg;
    Cfg.Flags = TIMESTAMPING_FLAG_RX;
    Cfg.TxTimestampsBufferCount = 0;
    DWORD Ret = 0;
    if (WSAIoctl(S, SIO_TIMESTAMPING, &Cfg, sizeof(Cfg), NULL, 0, &Ret, NULL, NULL) == 0) {
        SioTimestamping = 1;
        return 1;
    }
    printf("  [TS] SIO_TIMESTAMPING (hardware) unsupported by this NIC/driver, WSAGetLastError=%d\n", WSAGetLastError());

    BOOL TsOn = TRUE;
    if (setsockopt(S, SOL_SOCKET, SO_TIMESTAMP, (char*)&TsOn, sizeof(TsOn)) == 0) {
        PlainSoTimestamp = 1;
        InitializeFileTimeToQpcDelta();
        return 1;
    }
    printf("  [TS] SO_TIMESTAMP also unsupported, WSAGetLastError=%d\n", WSAGetLastError());

    return 0;
}

static int ReceiveWithTimestamp(SOCKET S, char* Buf, int Len, struct sockaddr_in* From, uint64_t* TsOut) {
    if (WSARecvMsgPtr != NULL && (SioTimestamping || PlainSoTimestamp)) {
        WSABUF Wb;
        Wb.buf = Buf;
        Wb.len = (ULONG)Len;
        char CtrlBuf[64];
        WSAMSG Msg;
        memset(&Msg, 0, sizeof(Msg));
        Msg.name          = (struct sockaddr*)From;
        Msg.namelen       = sizeof(*From);
        Msg.lpBuffers     = &Wb;
        Msg.dwBufferCount = 1;
        Msg.Control.buf   = CtrlBuf;
        Msg.Control.len   = sizeof(CtrlBuf);

        DWORD N = 0;
        int Rc = WSARecvMsgPtr(S, &Msg, &N, NULL, NULL);
        double FallbackNow = GetMonotonicMicrosecondsExact();
        if (Rc != 0) return -1;

        for (WSACMSGHDR* Cm = WSA_CMSG_FIRSTHDR(&Msg); Cm; Cm = WSA_CMSG_NXTHDR(&Msg, Cm)) {
            if (Cm->cmsg_level == SOL_SOCKET && Cm->cmsg_type == SO_TIMESTAMP) {
                ULONGLONG Raw = *(ULONGLONG*)WSA_CMSG_DATA(Cm);
                if (Raw > 0) {
                    if (SioTimestamping) {
                        *TsOut = QpcTicksToMicroseconds((int64_t)Raw);
                        return (int)N;
                    } else {
                        double FtUs = (double)Raw / 10.0;
                        double Stamped = FtUs + EvaluateFileTimeToQpcDelta(FallbackNow);
                        if (Stamped <= FallbackNow + 1000.0 && Stamped > FallbackNow - 2000000.0)
                            *TsOut = (uint64_t)Stamped;
                        else
                            *TsOut = (uint64_t)FallbackNow;
                        return (int)N;
                    }
                }
                break;
            }
        }
        *TsOut = (uint64_t)FallbackNow;
        return (int)N;
    }

    int Fl = sizeof(*From);
    int N = recvfrom(S, Buf, Len, 0, (struct sockaddr*)From, &Fl);
    *TsOut = GetMonotonicMicroseconds();
    return N;
}

static DWORD WINAPI ListenerThread(void* Unused) {
    (void)Unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    DWORD_PTR ProcMask, SysMask;
    if (GetProcessAffinityMask(GetCurrentProcess(), &ProcMask, &SysMask)) {
        DWORD_PTR Pinned = 0;
        for (int Bit = (int)(sizeof(DWORD_PTR) * 8) - 1; Bit >= 0; Bit--) {
            if (ProcMask & ((DWORD_PTR)1 << Bit)) { Pinned = (DWORD_PTR)1 << Bit; break; }
        }
        if (Pinned) SetThreadAffinityMask(GetCurrentThread(), Pinned);
    }
    EnableKernelReceiveTimestamps(Sock);
    InitializeFileTimeToQpcDelta();
    uint8_t Buf[64];
    struct sockaddr_in From;
    while (1) {
        uint64_t T2 = 0;
        int N = ReceiveWithTimestamp(Sock, (char*)Buf, sizeof(Buf), &From, &T2);
        if (N < 1) continue;
        if (Buf[0] == PtRegister) {
            struct sockaddr_in Ca = From;
            Ca.sin_port = htons(ClientPort);
            EnterCriticalSection(&ClientLock);
            int Found = 0;
            for (int I = 0; I < ClientCount; I++) {
                if (Clients[I].Addr.sin_addr.s_addr == Ca.sin_addr.s_addr) { Found = 1; break; }
            }
            if (!Found && ClientCount < MaxClients) {
                Clients[ClientCount].Addr = Ca;
                inet_ntop(AF_INET, &Ca.sin_addr, Clients[ClientCount].Ip, 32);
                printf("  + Device: %s  (total: %d)\n", Clients[ClientCount].Ip, ClientCount + 1);
                ClientCount++;
            }
            LeaveCriticalSection(&ClientLock);
        } else if (Buf[0] == PtSyncReq && N >= (int)sizeof(SyncReqPkt)) {
            SyncReqPkt* Req = (SyncReqPkt*)Buf;
            SyncAckPkt Ack;
            Ack.Type = PtSyncAck;
            Ack.T1   = Req->T1;
            Ack.T2   = T2;
            Ack.T3   = GetMonotonicMicroseconds();
            sendto(Sock, (char*)&Ack, sizeof(Ack), 0, (struct sockaddr*)&From, sizeof(From));
        } else if (Buf[0] == PtSyncInfo && N >= (int)sizeof(SyncInfoPkt)) {
            SyncInfoPkt* Info = (SyncInfoPkt*)Buf;
            struct sockaddr_in Ca = From;
            Ca.sin_port = htons(ClientPort);
            EnterCriticalSection(&ClientLock);
            int Idx = -1;
            for (int I = 0; I < ClientCount; I++) {
                if (Clients[I].Addr.sin_addr.s_addr == From.sin_addr.s_addr) { Idx = I; break; }
            }
            if (Idx == -1 && ClientCount < MaxClients) {
                Idx = ClientCount;
                Clients[Idx].Addr = Ca;
                inet_ntop(AF_INET, &Ca.sin_addr, Clients[Idx].Ip, 32);
                printf("  + Device: %s  (total: %d)\n", Clients[Idx].Ip, ClientCount + 1);
                ClientCount++;
            }
            if (Idx != -1) {
                int WasSynced = Clients[Idx].HasSyncInfo;
                uint64_t NowUs = GetMonotonicMicroseconds();
                Clients[Idx].HasSyncInfo  = 1;
                Clients[Idx].LastOffsetUs = Info->OffsetUs;
                Clients[Idx].LastRttUs    = Info->RttUs;
                Clients[Idx].LastSigmaUs  = Info->SigmaUs;
                Clients[Idx].LastSkewPpm  = Info->SkewPpm;
                Clients[Idx].LastHwRateHz = Info->HwRateHz;
                if (!WasSynced || NowUs - Clients[Idx].LastReportUs > 4000000ULL) {
                    Clients[Idx].LastReportUs = NowUs;
                    printf("  = Device %s: offset=%+lld us  minRTT=%lld us  samples=%d  "
                           "precision=%.2f us  skew=%+.2f ppm  dac=%.2f Hz\n",
                           Clients[Idx].Ip,
                           (long long)Info->OffsetUs,
                           (long long)Info->RttUs,
                           Info->SampleCount,
                           Info->SigmaUs,
                           Info->SkewPpm,
                           Info->HwRateHz);
                    EvaluateDelayMismatches();
                }
            }
            LeaveCriticalSection(&ClientLock);
        }
    }
    return 0;
}

int main(void) {
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
    InitializeCriticalSection(&ClientLock);
    WSADATA Wsa;
    WSAStartup(MAKEWORD(2,2), &Wsa);
    Sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int BufSz = 65536, Tos = 0xB8, Ttl = 255;
    setsockopt(Sock, SOL_SOCKET, SO_SNDBUF, (char*)&BufSz, 4);
    setsockopt(Sock, SOL_SOCKET, SO_RCVBUF, (char*)&BufSz, 4);
    setsockopt(Sock, IPPROTO_IP, IP_TOS, (char*)&Tos, sizeof(Tos));
    setsockopt(Sock, IPPROTO_IP, IP_MULTICAST_TTL, (char*)&Ttl, sizeof(Ttl));
    struct sockaddr_in Ba;
    memset(&Ba, 0, sizeof(Ba));
    Ba.sin_family = AF_INET;
    Ba.sin_port   = htons(ServerPort);
    bind(Sock, (struct sockaddr*)&Ba, sizeof(Ba));
    CreateThread(NULL, 0, ListenerThread, NULL, 0, NULL);
    Sleep(50);
    printf("RX timestamp mode = %s\n",
           SioTimestamping ? "kernel SIO_TIMESTAMPING" :
           PlainSoTimestamp ? "SO_TIMESTAMP fallback" : "userspace timestamp fallback");
    printf("AudioSync Server\n");
    printf("Listening on port %d. Press ENTER to fire all devices.\n\n", ServerPort);
    while (1) {
        getchar();
        Client LocalClients[MaxClients];
        int LocalCount;
        EnterCriticalSection(&ClientLock);
        if (ClientCount == 0) {
            printf("No devices connected.\n");
            LeaveCriticalSection(&ClientLock);
            continue;
        }
        LocalCount = ClientCount;
        memcpy(LocalClients, Clients, (size_t)ClientCount * sizeof(Client));
        LeaveCriticalSection(&ClientLock);
        printf("\n  Device clock offsets:\n");
        for (int I = 0; I < LocalCount; I++) {
            if (LocalClients[I].HasSyncInfo) {
                printf("    Device %d (%s): offset=%+lld us  rtt=%lld us  precision=%.2f us  skew=%+.2f ppm\n",
                       I + 1, LocalClients[I].Ip,
                       (long long)LocalClients[I].LastOffsetUs,
                       (long long)LocalClients[I].LastRttUs,
                       LocalClients[I].LastSigmaUs,
                       LocalClients[I].LastSkewPpm);
            } else {
                printf("    Device %d (%s): not synced yet\n", I + 1, LocalClients[I].Ip);
            }
        }
        FirePkt Fp;
        Fp.Type       = PtFire;
        Fp.FireAtPcUs = GetMonotonicMicroseconds() + 500000ULL;
        struct sockaddr_in McAddr;
        memset(&McAddr, 0, sizeof(McAddr));
        McAddr.sin_family      = AF_INET;
        McAddr.sin_port        = htons(ClientPort);
        inet_pton(AF_INET, "224.0.0.100", &McAddr.sin_addr);
        for (int R = 0; R < 4; R++) {
            sendto(Sock, (char*)&Fp, sizeof(Fp), 0, (struct sockaddr*)&McAddr, sizeof(McAddr));
            for (int I = 0; I < LocalCount; I++) {
                sendto(Sock, (char*)&Fp, sizeof(Fp), 0, (struct sockaddr*)&LocalClients[I].Addr, sizeof(struct sockaddr_in));
            }
            if (R < 3) Sleep(20);
        }
        printf("Fired %d device(s)!\n", LocalCount);
    }
    timeEndPeriod(1);
    return 0;
}