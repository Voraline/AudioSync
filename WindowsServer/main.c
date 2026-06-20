#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <timeapi.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

/* ---- SIO_TIMESTAMPING support (Windows 10 1809+ / Server 2019+) ----
   Not always present in older SDK headers, so define the bits we need
   ourselves and probe for support at runtime. Falls back cleanly to
   userspace NowUs() on anything that doesn't support it. */
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

#pragma pack(push,1)
typedef struct { uint8_t Type; uint64_t T1; } SyncReqPkt;
typedef struct { uint8_t Type; uint64_t T1; uint64_t T2; uint64_t T3; } SyncAckPkt;
typedef struct { uint8_t Type; uint64_t FireAtPcUs; } FirePkt;
#pragma pack(pop)

typedef struct { struct sockaddr_in Addr; char Ip[32]; } Client;

static SOCKET Sock;
static Client Clients[MaxClients];
static int ClientCount = 0;
static CRITICAL_SECTION ClientLock;

static uint64_t NowUs(void) {
    static LARGE_INTEGER F;
    static int Init = 0;
    if (!Init) { QueryPerformanceFrequency(&F); Init = 1; }
    LARGE_INTEGER T;
    QueryPerformanceCounter(&T);
    return (uint64_t)(T.QuadPart * 1000000LL / F.QuadPart);
}

static LPFN_WSARECVMSG WSARecvMsgPtr   = NULL;
static int             SioTimestamping = 0; /* RX cmsg is QPC-domain */
static int             PlainSoTimestamp = 0; /* RX cmsg is FILETIME-domain, needs calibration */
static int64_t         FileTimeToQpcUsDelta = 0;
static int             FileTimeToQpcInit = 0;

/* FILETIME (100ns ticks since 1601) -> our QPC-derived NowUs() domain.
   Same idea as the Android client's RealToMonoDeltaUs: sample both
   clocks back-to-back several times and take the median delta to
   cancel out scheduling noise in the calibration itself. */
static void InitFileTimeToQpcDelta(void) {
    if (FileTimeToQpcInit) return;
    int64_t Deltas[8];
    for (int I = 0; I < 8; I++) {
        FILETIME Ft;
        GetSystemTimePreciseAsFileTime(&Ft);
        uint64_t FtUs = ((uint64_t)Ft.dwHighDateTime << 32 | Ft.dwLowDateTime) / 10ULL;
        uint64_t QpcUs = NowUs();
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

static int EnableKernelRxTs(SOCKET S) {
    GUID Guid = WSAID_WSARECVMSG;
    DWORD Bytes = 0;
    if (WSAIoctl(S, SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &Guid, sizeof(Guid), &WSARecvMsgPtr, sizeof(WSARecvMsgPtr),
                 &Bytes, NULL, NULL) != 0) {
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

    BOOL TsOn = TRUE;
    if (setsockopt(S, SOL_SOCKET, SO_TIMESTAMP, (char*)&TsOn, sizeof(TsOn)) == 0) {
        PlainSoTimestamp = 1;
        InitFileTimeToQpcDelta();
        return 1;
    }

    return 0;
}

/* Receives a datagram and returns the best available timestamp for when
   it arrived at the kernel, falling back to a NowUs() call taken
   immediately after the syscall returns if no kernel timestamp is
   present in this packet's control data. */
static int RecvWithTs(SOCKET S, char* Buf, int Len, struct sockaddr_in* From, uint64_t* TsOut) {
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
        uint64_t FallbackNow = NowUs();
        if (Rc != 0) return -1;

        for (WSACMSGHDR* Cm = WSA_CMSG_FIRSTHDR(&Msg); Cm; Cm = WSA_CMSG_NXTHDR(&Msg, Cm)) {
            if (Cm->cmsg_level == SOL_SOCKET && Cm->cmsg_type == SO_TIMESTAMP) {
                ULONGLONG Raw = *(ULONGLONG*)WSA_CMSG_DATA(Cm);
                if (Raw > 0) {
                    if (SioTimestamping) {
                        /* SIO_TIMESTAMPING delivers QPC ticks directly. */
                        LARGE_INTEGER F;
                        QueryPerformanceFrequency(&F);
                        uint64_t Us = Raw * 1000000ULL / (uint64_t)F.QuadPart;
                        *TsOut = Us;
                        return (int)N;
                    } else {
                        /* Plain SO_TIMESTAMP delivers FILETIME (100ns
                           ticks since 1601). Convert to our QPC domain
                           using the calibrated offset. */
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
    *TsOut = NowUs();
    return N;
}

static DWORD WINAPI ListenerThread(void* Unused) {
    (void)Unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    /* Pin to a fixed core so the response path doesn't get bounced
       between cores mid-handling (cache + scheduler jitter), mirroring
       the affinity pinning the Android client does for its sync thread.
       Use the last logical core, leaving core 0 free for system/ISR work. */
    DWORD_PTR ProcMask, SysMask;
    if (GetProcessAffinityMask(GetCurrentProcess(), &ProcMask, &SysMask)) {
        DWORD_PTR Pinned = 0;
        for (int Bit = (int)(sizeof(DWORD_PTR) * 8) - 1; Bit >= 0; Bit--) {
            if (ProcMask & ((DWORD_PTR)1 << Bit)) { Pinned = (DWORD_PTR)1 << Bit; break; }
        }
        if (Pinned) SetThreadAffinityMask(GetCurrentThread(), Pinned);
    }
    EnableKernelRxTs(Sock);
    uint8_t Buf[64];
    struct sockaddr_in From;
    while (1) {
        uint64_t T2 = 0;
        int N = RecvWithTs(Sock, (char*)Buf, sizeof(Buf), &From, &T2);
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
            Ack.T2   = T2;       /* kernel/driver RX timestamp, not post-hoc */
            Ack.T3   = NowUs();  /* captured immediately before the send */
            sendto(Sock, (char*)&Ack, sizeof(Ack), 0, (struct sockaddr*)&From, sizeof(From));
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
        FirePkt Fp;
        Fp.Type       = PtFire;
        Fp.FireAtPcUs = NowUs() + 500000ULL;
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
