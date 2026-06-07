#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <timeapi.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

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

static DWORD WINAPI ListenerThread(void* Unused) {
    (void)Unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    uint8_t Buf[64];
    struct sockaddr_in From;
    while (1) {
        int Fl = sizeof(From);
        int N = recvfrom(Sock, (char*)Buf, sizeof(Buf), 0, (struct sockaddr*)&From, &Fl);
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
            Ack.T2 = Ack.T3 = NowUs();
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
