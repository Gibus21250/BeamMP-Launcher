// Copyright (c) 2019-present Anonymous275.
// BeamMP Launcher code is not in the public domain and is not free software.
// One must be granted explicit permission by the copyright holder in order to modify or distribute any part of the source or binaries.
// Anything else is prohibited. Modified works may not be published and have be upstreamed to the official repository.
///
/// Created by Anonymous275 on 7/25/2020
///
#include "Network/network.hpp"
#include <zlib.h>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#elif defined(__linux__)
#include "linuxfixes.h"
#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "Logger.h"
#include <charconv>
#include <mutex>
#include <string>
#include <thread>

std::chrono::time_point<std::chrono::high_resolution_clock> PingStart, PingEnd;
bool GConnected = false;
bool CServer = true;
SOCKET CSocket = INVALID_SOCKET;
SOCKET GSocket = INVALID_SOCKET;
int ClientID = -1;

int KillSocket(uint64_t Dead) {
    if (Dead == INVALID_SOCKET) {
        debug("Kill invalid socket got, returning...");
        return 0;
    }
    shutdown(Dead, SD_BOTH);
    int a = closesocket(Dead);
    if (a != 0 && !shuttingdown) {
        warn("Failed to close socket!");
    }
    return a;
}

bool CheckBytes(uint32_t Bytes) {
    if (Bytes == 0) {
        debug("(Proxy) Connection closing");
        return false;
    } else if (Bytes < 0) {
        debug("(Proxy) send failed with error: " + std::to_string(WSAGetLastError()));
        return false;
    }
    return true;
}

void GameSend(std::string_view Data) {
    static std::mutex Lock;
    std::scoped_lock Guard(Lock);
    if (TCPTerminate || !GConnected || CSocket == -1)
        return;
    int32_t Size, Temp, Sent;
    Size = int32_t(Data.size());
    Sent = 0;
#ifdef DEBUG
    if (Size > 1000) {
        debug("Launcher -> game (" + std::to_string(Size) + ")");
    }
#endif
    do {
        if (Sent > -1) {
            Temp = send(CSocket, &Data[Sent], Size - Sent, 0);
        }
        if (!CheckBytes(Temp))
            return;
        Sent += Temp;
    } while (Sent < Size);
    // send separately to avoid an allocation for += "\n"
    Temp = send(CSocket, "\n", 1, 0);
    if (!CheckBytes(Temp)) {
        return;
    }
}
void ServerSend(std::string Data, bool Rel) {
    if (Terminate || Data.empty())
        return;
    if (Data.find("Zp") != std::string::npos && Data.size() > 500) {
        abort();
    }
    char C = 0;
    bool Ack = false;
    int DLen = int(Data.length());
    if (DLen > 3)
        C = Data.at(0);
    if (C == 'O' || C == 'T')
        Ack = true;
    if (C == 'N' || C == 'W' || C == 'Y' || C == 'V' || C == 'E' || C == 'C')
        Rel = true;
    if (compressBound(Data.size()) > 1024)
        Rel = true;
    if (Ack || Rel) {
        if (Ack || DLen > 1000)
            SendLarge(Data);
        else
            TCPSend(Data, TCPSock);
    } else
        UDPSend(Data);

    if (DLen > 1000) {
        debug("(Launcher->Server) Bytes sent: " + std::to_string(Data.length()) + " : "
            + Data.substr(0, 10)
            + Data.substr(Data.length() - 10));
    } else if (C == 'Z') {
        // debug("(Game->Launcher) : " + Data);
    }
}

void NetReset() {
    debug("Network reset called");
    TCPTerminate = false;
    GConnected = false;
    Terminate = false;
    UlStatus = "Ulstart";
    MStatus = " ";

    if (UDPSock != INVALID_SOCKET) {
        debug("Terminating UDP Socket : " + std::to_string(TCPSock));
        KillSocket(UDPSock);
    }
    UDPSock = -1;
    if (TCPSock != INVALID_SOCKET) {
        debug("Terminating TCP Socket : " + std::to_string(TCPSock));
        KillSocket(TCPSock);
    }
    TCPSock = -1;
    if (GSocket != INVALID_SOCKET) {
        debug("Terminating GTCP Socket : " + std::to_string(GSocket));
        KillSocket(GSocket);
    }
    GSocket = -1;
}

void AutoPing() {
    while (!Terminate) {
        ServerSend("p", false);
        PingStart = std::chrono::high_resolution_clock::now();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void ParserAsync(std::string_view Data) {

    if (Data.empty())
        return;
    char Code = Data.at(0), SubCode = 0;
    if (Data.length() > 1)
        SubCode = Data.at(1);
    switch (Code) {
    case 'p':
        PingEnd = std::chrono::high_resolution_clock::now();
        if (PingStart > PingEnd)
            ping = 0;
        else
            ping = int(std::chrono::duration_cast<std::chrono::milliseconds>(PingEnd - PingStart).count());
        return;
    case 'M':
        MStatus = Data;
        UlStatus = "Uldone";
        return;
    default:
        break;
    }
    GameSend(Data);
}

void ServerParser(std::string_view Data) {
    ParserAsync(Data);
}

void NetMain(const std::string& IP, int Port) {
    std::thread Ping(AutoPing);
    Ping.detach();
    UDPClientMain(IP, Port);
    CServer = true;
    Terminate = true;
    info("Connection Terminated!");
}

void TCPGameServer(const std::string& IP, int Port) {

    struct sockaddr_storage loopBackTcp { };

    GSocket = initSocket("127.0.0.1", DEFAULT_PORT + 1, SOCK_STREAM, &loopBackTcp); 

    if (GSocket == INVALID_SOCKET) {
        return;
    }

    int iRes = bind(GSocket, (sockaddr*)&loopBackTcp, sizeof(sockaddr_storage));
    if (iRes == SOCKET_ERROR) {
        neterror("(Proxy) LoopBack TCP bind failed!");
        KillSocket(GSocket);
        return;
    }

    iRes = listen(GSocket, SOMAXCONN);
    if (iRes == SOCKET_ERROR) {
        neterror("(Proxy) LoopBack TCP listen failed!");
        KillSocket(GSocket);
        return;
    }

    while (!TCPTerminate && GSocket != INVALID_SOCKET) {
        debug("MAIN LOOP OF GAME SERVER");
        GConnected = false;
        if (!CServer) {
            warn("Connection still alive terminating");
            NetReset();
            TCPTerminate = true;
            Terminate = true;
            break;
        }
        if (CServer) {
            std::thread Client(TCPClientMain, IP, Port);
            Client.detach();
        }

        CSocket = accept(GSocket, nullptr, nullptr);
        if (CSocket == -1) {
            debug("(Proxy) accept failed with error: " + std::to_string(WSAGetLastError()));
            break;
        }
        debug("(Proxy) Game Connected!");
        GConnected = true;
        if (CServer) {
            std::thread t1(NetMain, IP, Port);
            t1.detach();
            CServer = false;
        }
        int32_t Size, Temp, Rcv;
        char Header[10] = { 0 };

        // Read byte by byte until '>' is rcved then get the size and read based on it
        do {
            Rcv = 0;

            do {
                Temp = recv(CSocket, &Header[Rcv], 1, 0);
                if (Temp < 1 || TCPTerminate)
                    break;
            } while (Header[Rcv++] != '>');
            if (Temp < 1 || TCPTerminate)
                break;
            if (std::from_chars(Header, &Header[Rcv], Size).ptr[0] != '>') {
                debug("(Game) Invalid lua Header -> " + std::string(Header, Rcv));
                break;
            }
            std::string Ret(Size, 0);
            Rcv = 0;
            do {
                Temp = recv(CSocket, &Ret[Rcv], Size - Rcv, 0);
                if (Temp < 1)
                    break;
                Rcv += Temp;
            } while (Rcv < Size && !TCPTerminate);
            if (Temp < 1 || TCPTerminate)
                break;

            ServerSend(Ret, false);

        } while (Temp > 0 && !TCPTerminate);
        if (Temp == 0)
            debug("(Proxy) Connection closing");
        else
            debug("(Proxy) recv failed error : " + std::to_string(WSAGetLastError()));
    }
    TCPTerminate = true;
    GConnected = false;
    Terminate = true;
    if (CSocket != SOCKET_ERROR)
        KillSocket(CSocket);
    debug("END OF GAME SERVER");
}
