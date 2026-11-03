// SPDX-License-Identifier: GPL-3.0-only
// See network.h.
#include "network.h"
#include "steam_net.h"
#include "steam_lobby.h"
#include "../qa/automation.h"
#include <cstdio>
#include <string>
#include <io.h>
#include <fcntl.h>
#include <mstcpip.h> // SIO_KEEPALIVE_VALS
#include <algorithm> // std::min - NOMINMAX is defined project-wide, so windows.h's min/max macros are gone

#pragma comment(lib, "ws2_32.lib")

void Network_Bind(const TsmHost* host)
{
    TsmBind(host);
    SteamNet_Bind(host);
    SteamLobby_Bind(host);
}

bool NetworkInit()
{
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        Logf("republicmp  NETWORK: WSAStartup failed, code=%d", result);
        return false;
    }
    Logf("republicmp  NETWORK: WinSock initialized (%d.%d)",
         LOBYTE(wsaData.wVersion), HIBYTE(wsaData.wVersion));
    SteamNet_Init(); // optional: without Steam, links are TCP only
    return true;
}

// Windows SO_RCVTIMEO/SO_SNDTIMEO take a DWORD (ms), not a struct timeval.
static bool NetworkSetSocketTimeouts(SOCKET sock, DWORD timeoutMs)
{
    if (sock == INVALID_SOCKET) {
        Logf("republicmp  NETWORK: NetworkSetSocketTimeouts() got INVALID_SOCKET");
        return false;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs)) == SOCKET_ERROR) {
        Logf("republicmp  NETWORK: setsockopt(SO_RCVTIMEO) failed, WSAGetLastError=%d", WSAGetLastError());
        return false;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs)) == SOCKET_ERROR) {
        Logf("republicmp  NETWORK: setsockopt(SO_SNDTIMEO) failed, WSAGetLastError=%d", WSAGetLastError());
        return false;
    }
    return true;
}

// TCP keepalive: the join connection sits in a blocking recv for minutes with
// nothing to read (host saving, client loading), so a silently dead link (VPN
// drop, crashed peer machine) would never be noticed. Probes after 5s idle,
// every 1s; Windows gives up after 10 unanswered ones -> ~15s to an error.
static void NetworkEnableKeepalive(SOCKET sock)
{
    tcp_keepalive ka = {};
    ka.onoff = 1;
    ka.keepalivetime = 5000;
    ka.keepaliveinterval = 1000;
    DWORD returned = 0;
    if (WSAIoctl(sock, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0, &returned, nullptr, nullptr) == SOCKET_ERROR)
        Logf("republicmp  NETWORK: SIO_KEEPALIVE_VALS failed, WSAGetLastError=%d", WSAGetLastError());
}

NetLink NetworkListen(unsigned short port, int backlog)
{
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        Logf("republicmp  NETWORK: socket() failed, WSAGetLastError=%d", WSAGetLastError());
        return INVALID_SOCKET;
    }
    BOOL reuse = TRUE;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR || listen(listenSock, backlog) == SOCKET_ERROR) {
        Logf("republicmp  NETWORK: listening on :%u failed, WSAGetLastError=%d", port, WSAGetLastError());
        closesocket(listenSock);
        return INVALID_SOCKET;
    }
    Logf("republicmp  NETWORK: listening on :%u", port);
    SteamNet_Listen(listenSock, port);
    return listenSock;
}

NetLink NetworkAcceptReady(NetLink listenSock, DWORD waitMs, DWORD ioTimeoutMs)
{
    if (listenSock == INVALID_SOCKET) return INVALID_SOCKET;
    // Steam players queue beside the TCP ones: look at both in short steps.
    const DWORD pollStepMs = 50;
    for (DWORD waited = 0;; waited += pollStepMs) {
        NetLink steam = SteamNet_Accept(listenSock, ioTimeoutMs);
        if (steam != NET_NO_LINK) return steam;
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSock, &readSet);
        DWORD stepMs = std::min(pollStepMs, waitMs - std::min(waited, waitMs));
        timeval wait = { 0, (long)stepMs * 1000 };
        int r = select(0, &readSet, nullptr, nullptr, &wait);
        if (r > 0) break;
        if (r == SOCKET_ERROR || waited >= waitMs) return INVALID_SOCKET;
    }
    sockaddr_in clientAddr = {};
    int clientAddrLen = sizeof(clientAddr);
    SOCKET clientSock = accept(listenSock, (sockaddr*)&clientAddr, &clientAddrLen);
    if (clientSock == INVALID_SOCKET) {
        Logf("republicmp  NETWORK: accept() failed, WSAGetLastError=%d", WSAGetLastError());
        return INVALID_SOCKET;
    }
    char clientIp[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, sizeof(clientIp));
    Logf("republicmp  NETWORK: client connected, %s:%u", clientIp, ntohs(clientAddr.sin_port));
    if (!NetworkSetSocketTimeouts(clientSock, ioTimeoutMs)) {
        closesocket(clientSock);
        return INVALID_SOCKET;
    }
    NetworkEnableKeepalive(clientSock);
    return clientSock;
}

void NetworkSetTimeouts(NetLink sock, DWORD timeoutMs)
{
    if (SteamNet_IsLink(sock)) return SteamNet_SetTimeouts(sock, timeoutMs);
    if (!NetworkSetSocketTimeouts(sock, timeoutMs)) NetworkShutdown(sock);
}

void NetworkSetSendTimeout(NetLink sock, DWORD timeoutMs)
{
    if (SteamNet_IsLink(sock)) return; // LAN-only policy; do not shorten Steam's idle receive
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs)) == SOCKET_ERROR) {
        Logf("republicmp  NETWORK: couldn't set send deadline, WSAGetLastError=%d", WSAGetLastError());
        NetworkShutdown(sock);
    }
}

void NetworkPeerAddress(NetLink sock, char* out, size_t outSize)
{
    if (SteamNet_IsLink(sock)) return SteamNet_PeerAddress(sock, out, outSize);
    sockaddr_in addr = {};
    int len = sizeof(addr);
    out[0] = 0;
    if (sock != INVALID_SOCKET && getpeername(sock, (sockaddr*)&addr, &len) == 0)
        inet_ntop(AF_INET, &addr.sin_addr, out, outSize);
}

NetLink NetworkConnect(const char* hostIp, unsigned short port, DWORD connectTimeoutMs, DWORD ioTimeoutMs, volatile bool* cancelFlag)
{
    port = QaAutomation_ConnectPort(hostIp, port);
    unsigned long long steamId;
    if (SteamNet_ParseId(hostIp, &steamId))
        return SteamNet_Connect(steamId, port, connectTimeoutMs, ioTimeoutMs, cancelFlag);
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        Logf("republicmp  NETWORK: socket() failed, WSAGetLastError=%d", WSAGetLastError());
        return INVALID_SOCKET;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, hostIp, &addr.sin_addr) != 1) {
        Logf("republicmp  NETWORK: couldn't parse IP [%s]", hostIp);
        closesocket(sock);
        return INVALID_SOCKET;
    }

    Logf("republicmp  NETWORK: connecting to %s:%u (timeout %u ms)...", hostIp, port, connectTimeoutMs);

    // Standard non-blocking connect-with-timeout pattern.
    u_long nonBlocking = 1;
    if (ioctlsocket(sock, FIONBIO, &nonBlocking) == SOCKET_ERROR) {
        closesocket(sock);
        return INVALID_SOCKET;
    }

    int connectResult = connect(sock, (sockaddr*)&addr, sizeof(addr));
    if (connectResult == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            Logf("republicmp  NETWORK: connect() failed, WSAGetLastError=%d", err);
            closesocket(sock);
            return INVALID_SOCKET;
        }

        // Polled in short steps so the cancel flag is checked; the result is read
        // from the same select() call that reported completion.
        const DWORD pollStepMs = 200;
        DWORD waited = 0;
        fd_set writeSet;
        fd_set exceptSet;
        for (;;) {
            if (cancelFlag && *cancelFlag) {
                Logf("republicmp  NETWORK: connect to %s:%u cancelled", hostIp, port);
                closesocket(sock);
                return INVALID_SOCKET;
            }

            FD_ZERO(&writeSet);
            FD_SET(sock, &writeSet);
            FD_ZERO(&exceptSet);
            FD_SET(sock, &exceptSet);

            DWORD stepMs = std::min(pollStepMs, connectTimeoutMs - waited);
            timeval stepTimeout;
            stepTimeout.tv_sec = stepMs / 1000;
            stepTimeout.tv_usec = (stepMs % 1000) * 1000;

            int selectResult = select(0, nullptr, &writeSet, &exceptSet, &stepTimeout);
            if (selectResult == SOCKET_ERROR) {
                Logf("republicmp  NETWORK: select() failed, WSAGetLastError=%d", WSAGetLastError());
                closesocket(sock);
                return INVALID_SOCKET;
            }
            if (selectResult > 0) {
                break; // writeSet/exceptSet now hold this call's real result
            }

            waited += stepMs;
            if (waited >= connectTimeoutMs) {
                Logf("republicmp  NETWORK: TIMEOUT - couldn't connect to %s:%u within %u ms", hostIp, port, connectTimeoutMs);
                closesocket(sock);
                return INVALID_SOCKET;
            }
        }

        if (FD_ISSET(sock, &exceptSet)) {
            // select() succeeded; the connect error itself is in SO_ERROR (e.g.
            // WSAECONNREFUSED when nothing is listening).
            int soError = 0;
            int soErrorLen = sizeof(soError);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&soError, &soErrorLen);
            Logf("republicmp  NETWORK: connect() failed (SO_ERROR=%d)", soError);
            closesocket(sock);
            return INVALID_SOCKET;
        }

        int soError = 0;
        int soErrorLen = sizeof(soError);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&soError, &soErrorLen) == SOCKET_ERROR || soError != 0) {
            Logf("republicmp  NETWORK: connect() failed (SO_ERROR=%d)", soError);
            closesocket(sock);
            return INVALID_SOCKET;
        }
    }

    u_long blocking = 0;
    if (ioctlsocket(sock, FIONBIO, &blocking) == SOCKET_ERROR ||
        !NetworkSetSocketTimeouts(sock, ioTimeoutMs)) {
        closesocket(sock);
        return INVALID_SOCKET;
    }

    Logf("republicmp  NETWORK: connected to %s:%u", hostIp, port);
    NetworkEnableKeepalive(sock);
    return sock;
}

// Overlapped operations leave socket configuration unchanged, allowing one
// sender and one receiver concurrently. Each exact call has one monotonic
// deadline; a peer cannot extend it indefinitely by trickling a few bytes.
static bool TcpExact(SOCKET sock, char* bytes, size_t size, bool sending)
{
    if (!size) return true;
    DWORD timeout = 0;
    int optionSize = sizeof(timeout);
    if (getsockopt(sock, SOL_SOCKET, sending ? SO_SNDTIMEO : SO_RCVTIMEO,
                   (char*)&timeout, &optionSize) == SOCKET_ERROR) return false;
    const ULONGLONG start = GetTickCount64();
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return false;
    size_t done = 0;
    bool ok = true;
    while (done < size) {
        const int count = (int)std::min(size - done, (size_t)INT_MAX);
        DWORD transferred = 0;
        {
            ULONGLONG elapsed = GetTickCount64() - start;
            if (timeout && elapsed >= timeout) { WSASetLastError(WSAETIMEDOUT); ok = false; break; }
            WSABUF buffer = { (ULONG)count, bytes + done };
            WSAOVERLAPPED operation = {};
            operation.hEvent = event;
            ResetEvent(event);
            DWORD flags = 0;
            int result = sending ? WSASend(sock, &buffer, 1, &transferred, 0, &operation, nullptr)
                                 : WSARecv(sock, &buffer, 1, &transferred, &flags, &operation, nullptr);
            if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) { ok = false; break; }
            if (result == SOCKET_ERROR) {
                elapsed = GetTickCount64() - start;
                DWORD remaining = !timeout ? INFINITE : elapsed < timeout ? (DWORD)(timeout - elapsed) : 0;
                DWORD waited = WaitForSingleObject(event, remaining);
                if (waited != WAIT_OBJECT_0) {
                    // The kernel still owns operation/buffer until cancellation
                    // completes. Drain it before either stack object goes away.
                    CancelIoEx((HANDLE)sock, &operation);
                    WSAGetOverlappedResult(sock, &operation, &transferred, TRUE, &flags);
                    WSASetLastError(waited == WAIT_TIMEOUT ? WSAETIMEDOUT : WSA_OPERATION_ABORTED);
                    ok = false;
                    break;
                }
                if (!WSAGetOverlappedResult(sock, &operation, &transferred, FALSE, &flags)) { ok = false; break; }
            }
            if (!transferred) { ok = false; break; }
        }
        done += transferred;
    }
    int error = WSAGetLastError();
    if (event) CloseHandle(event);
    if (!ok) Logf("republicmp  NETWORK: %s failed (%zu/%zu bytes, error=%d)", sending ? "send" : "receive", done, size, error);
    return ok;
}

bool NetworkSendExact(NetLink sock, const void* data, size_t size)
{
    if (SteamNet_IsLink(sock)) return SteamNet_Send(sock, data, size);
    return TcpExact(sock, (char*)data, size, true);
}

bool NetworkRecvExact(NetLink sock, void* data, size_t size)
{
    if (SteamNet_IsLink(sock)) return SteamNet_Recv(sock, data, size);
    return TcpExact(sock, (char*)data, size, false);
}

static constexpr unsigned long long kMaxTransferBytes = 2ull * 1024 * 1024 * 1024;

bool NetworkSendFile(NetLink sock, const char* filePath, NetworkProgressFn onProgress, void* userData)
{
    FILE* f = fopen(filePath, "rb");
    if (!f) {
        Logf("republicmp  NETWORK: couldn't open [%s] for reading", filePath);
        return false;
    }

    bool seekOk = _fseeki64(f, 0, SEEK_END) == 0;
    __int64 measured = seekOk ? _ftelli64(f) : -1;
    if (measured <= 0 || (unsigned long long)measured > kMaxTransferBytes || _fseeki64(f, 0, SEEK_SET) != 0) {
        Logf("republicmp  NETWORK: invalid file size or seek failure [%s]", filePath);
        fclose(f);
        return false;
    }
    unsigned long long fileSize = (unsigned long long)measured;

    if (!NetworkSendExact(sock, (const char*)&fileSize, sizeof(fileSize))) {
        fclose(f);
        return false;
    }

    Logf("republicmp  NETWORK: sending [%s], %llu bytes...", filePath, fileSize);

    char buffer[65536];
    unsigned long long sentTotal = 0;
    while (sentTotal < fileSize) {
        size_t toRead = (size_t)std::min((unsigned long long)sizeof(buffer), fileSize - sentTotal);
        size_t bytesRead = fread(buffer, 1, toRead, f);
        if (bytesRead == 0) {
            Logf("republicmp  NETWORK: fread() returned 0 early (sent %llu of %llu bytes)", sentTotal, fileSize);
            fclose(f);
            return false;
        }
        if (!NetworkSendExact(sock, buffer, bytesRead)) {
            fclose(f);
            return false;
        }
        sentTotal += bytesRead;
        if (onProgress) onProgress(sentTotal, fileSize, userData);
    }

    if (fclose(f) != 0) return false;
    Logf("republicmp  NETWORK: finished sending [%s], %llu bytes", filePath, sentTotal);
    return true;
}

bool NetworkReceiveFile(NetLink sock, const char* outFilePath, NetworkProgressFn onProgress, void* userData)
{
    unsigned long long fileSize = 0;
    if (!NetworkRecvExact(sock, (char*)&fileSize, sizeof(fileSize))) {
        return false;
    }

    if (!fileSize || fileSize > kMaxTransferBytes) {
        Logf("republicmp  NETWORK: refused invalid transfer size %llu (limit 2 GiB)", fileSize);
        return false;
    }
    // Keep the previous complete file intact on all errors. Exclusive creation
    // refuses a stale/active staging file instead of following or overwriting it.
    std::string partial = std::string(outFilePath) + ".part";
    HANDLE raw = CreateFileA(partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return false;
    int fd = _open_osfhandle((intptr_t)raw, _O_BINARY | _O_WRONLY);
    if (fd == -1) { CloseHandle(raw); DeleteFileA(partial.c_str()); return false; }
    FILE* f = _fdopen(fd, "wb");
    if (!f) {
        _close(fd);
        DeleteFileA(partial.c_str());
        return false;
    }

    Logf("republicmp  NETWORK: receiving [%s], expecting %llu bytes...", outFilePath, fileSize);

    char buffer[65536];
    unsigned long long receivedTotal = 0;
    while (receivedTotal < fileSize) {
        size_t toRead = (size_t)std::min((unsigned long long)sizeof(buffer), fileSize - receivedTotal);
        if (!NetworkRecvExact(sock, buffer, toRead)) {
            fclose(f);
            DeleteFileA(partial.c_str());
            return false;
        }
        if (fwrite(buffer, 1, toRead, f) != toRead) {
            Logf("republicmp  NETWORK: fwrite() didn't write everything (disk full?)");
            fclose(f);
            DeleteFileA(partial.c_str());
            return false;
        }
        receivedTotal += toRead;
        if (onProgress) onProgress(receivedTotal, fileSize, userData);
    }

    if (fclose(f) != 0) {
        DeleteFileA(partial.c_str());
        return false;
    }
    if (!MoveFileExA(partial.c_str(), outFilePath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(partial.c_str());
        return false;
    }
    Logf("republicmp  NETWORK: finished receiving [%s], %llu bytes", outFilePath, receivedTotal);
    return true;
}

bool NetworkPeerGone(NetLink sock)
{
    if (SteamNet_IsLink(sock)) return SteamNet_PeerGone(sock);
    if (sock == INVALID_SOCKET) return true;
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(sock, &readSet);
    timeval zero = { 0, 0 };
    int r = select(0, &readSet, nullptr, nullptr, &zero);
    if (r == SOCKET_ERROR) return true;
    if (r == 0) return false; // nothing pending - still up
    // Readable: data (peer alive), or EOF / reset / keepalive failure. The
    // socket is readable, so this peek does not block.
    char b;
    int n = recv(sock, &b, 1, MSG_PEEK);
    if (n > 0) return false;
    if (n == 0) Logf("republicmp  NETWORK: the peer closed the connection");
    else Logf("republicmp  NETWORK: connection lost, WSAGetLastError=%d", WSAGetLastError());
    return true;
}

bool NetworkWaitReadable(NetLink sock, DWORD waitMs, volatile bool* cancelFlag)
{
    if (SteamNet_IsLink(sock)) return SteamNet_WaitReadable(sock, waitMs, cancelFlag);
    if (sock == INVALID_SOCKET) return false;
    const DWORD pollStepMs = 200;
    for (DWORD waited = 0; waited < waitMs; waited += pollStepMs) {
        if (cancelFlag && *cancelFlag) return false;
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock, &readSet);
        timeval step = { 0, (long)pollStepMs * 1000 };
        int r = select(0, &readSet, nullptr, nullptr, &step);
        if (r == SOCKET_ERROR) return false;
        if (r > 0) return true;
    }
    return false;
}

void NetworkClose(NetLink sock)
{
    if (SteamNet_IsLink(sock)) return SteamNet_Close(sock);
    if (sock != INVALID_SOCKET) {
        SteamNet_StopListen(sock); // a listener's Steam side; nothing for a connection
        closesocket(sock);
    }
}

void NetworkShutdown(NetLink sock)
{
    if (SteamNet_IsLink(sock)) return SteamNet_Shutdown(sock);
    if (sock != INVALID_SOCKET) {
        shutdown(sock, SD_BOTH);
        // shutdown alone need not complete a pending receive on Windows.
        // All exact IO uses overlapped operations, including unlimited idle
        // receives, so cancellation wakes their owners before they close it.
        CancelIoEx((HANDLE)sock, nullptr);
    }
}

unsigned long long NetworkSteamId()
{
    return SteamNet_LocalId();
}

bool NetworkTakeJoinRequest(char* host, size_t hostSize)
{
    return SteamNet_TakeJoinRequest(host, hostSize);
}
