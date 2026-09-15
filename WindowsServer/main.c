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
} SyncInfoPkt;
#pragma pack(pop)

typedef struct {
    struct sockaddr_in Addr;
    char    Ip[32];
    int     HasSyncInfo;
    int64_t LastOffsetUs;
    int64_t LastRttUs;
    float   LastSigmaUs;
} Client;

static SOCKET Sock;
static Client Clients[MaxClients];
static int ClientCount = 0;
static CRITICAL_SECTION ClientLock;

static void PrintCancellationForPair(int IndexA, int IndexB) {
    float SigmaA = Clients[IndexA].LastSigmaUs;
    float SigmaB = Clients[IndexB].LastSigmaUs;
    double DeltaUs = sqrt((double)SigmaA * (double)SigmaA + (double)SigmaB * (double)SigmaB);
    if (DeltaUs <= 0.0) {
        printf("      Device %d vs Device %d: mismatch=0.0 us (no cancellation)\n", IndexA + 1, IndexB + 1);
        return;
    }
    double DeltaSec = DeltaUs / 1000000.0;
    double CancelHz = 1.0 / (2.0 * DeltaSec);
    printf("      Device %d vs Device %d: est. mismatch=%.1f us -> first cancel at %.1f Hz\n",
           IndexA + 1, IndexB + 1, DeltaUs, CancelHz);
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

static uint64_t GetMonotonicMicroseconds(void) {
    static LARGE_INTEGER F;
    static int Init = 0;
    if (!Init) { QueryPerformanceFrequency(&F); Init = 1; }
    LARGE_INTEGER T;
    QueryPerformanceCounter(&T);
    return (uint64_t)(T.QuadPart * 1000000LL / F.QuadPart);
}

static LPFN_WSARECVMSG WSARecvMsgPtr   = NULL;
static int             SioTimestamping = 0;
static int             PlainSoTimestamp = 0;
static int64_t         FileTimeToQpcUsDelta = 0;
static int             FileTimeToQpcInit = 0;

static void InitializeFileTimeToQpcDelta(void) {
    if (FileTimeToQpcInit) return;
    int64_t Deltas[8];
    for (int I = 0; I < 8; I++) {
        FILETIME Ft;
        GetSystemTimePreciseAsFileTime(&Ft);
        uint64_t FtUs = ((uint64_t)Ft.dwHighDateTime << 32 | Ft.dwLowDateTime) / 10ULL;
        uint64_t QpcUs = GetMonotonicMicroseconds();
        Deltas[I] = (int64_t)QpcUs - (int64_t)FtUs;
    }
    for (int I = 1; I < 8; I++) {
        int64_t K = Deltas[I]; int J = I - 1;
        while (J >= 0 && Deltas[J] > K) { Deltas[J+1] = Deltas[J]; J--; }
        Deltas[J+1] = K;
    }
    FileTimeToQpcUsDelta = Deltas[4];
    FileTimeToQpcInit    = 1;
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
        uint64_t FallbackNow = GetMonotonicMicroseconds();
        if (Rc != 0) return -1;

        for (WSACMSGHDR* Cm = WSA_CMSG_FIRSTHDR(&Msg); Cm; Cm = WSA_CMSG_NXTHDR(&Msg, Cm)) {
            if (Cm->cmsg_level == SOL_SOCKET && Cm->cmsg_type == SO_TIMESTAMP) {
                ULONGLONG Raw = *(ULONGLONG*)WSA_CMSG_DATA(Cm);
                if (Raw > 0) {
                    if (SioTimestamping) {
                        static LARGE_INTEGER F;
                        static int FInit = 0;
                        if (!FInit) { QueryPerformanceFrequency(&F); FInit = 1; }
                        uint64_t Us = Raw * 1000000ULL / (uint64_t)F.QuadPart;
                        *TsOut = Us;
                        return (int)N;
                    } else {
                        uint64_t FtUs = Raw / 10ULL;
                        *TsOut = (uint64_t)((int64_t)FtUs + FileTimeToQpcUsDelta);
                        return (int)N;
                    }
                }
                break;
            }
        }
        *TsOut = FallbackNow;
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
                Clients[Idx].HasSyncInfo  = 1;
                Clients[Idx].LastOffsetUs = Info->OffsetUs;
                Clients[Idx].LastRttUs    = Info->RttUs;
                Clients[Idx].LastSigmaUs  = Info->SigmaUs;
                printf("  = Device %s synced: offset=%+lld us  minRTT=%lld us  samples=%d  precision=%.1f us\n",
                       Clients[Idx].Ip,
                       (long long)Info->OffsetUs,
                       (long long)Info->RttUs,
                       Info->SampleCount,
                       Info->SigmaUs);
                EvaluateDelayMismatches();
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
                printf("    Device %d (%s): offset=%+lld us  rtt=%lld us\n",
                       I + 1, LocalClients[I].Ip,
                       (long long)LocalClients[I].LastOffsetUs,
                       (long long)LocalClients[I].LastRttUs);
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
