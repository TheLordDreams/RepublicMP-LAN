// SPDX-License-Identifier: GPL-3.0-only
// Actual TCP implementation, with inert Steam/QA dependencies. No game/account.
#include "../transport/network.cpp"
#include <thread>
#include <vector>
#include <fstream>
#include <filesystem>
#include <cassert>
#include <iostream>

void SteamNet_Bind(const TsmHost*) {}
void SteamLobby_Bind(const TsmHost*) {}
bool SteamNet_Init() { return false; }
bool SteamNet_IsLink(NetLink) { return false; }
bool SteamNet_ParseId(const char*, unsigned long long*) { return false; }
void SteamNet_Listen(NetLink,unsigned short) {}
void SteamNet_StopListen(NetLink) {}
NetLink SteamNet_Accept(NetLink,DWORD) { return NET_NO_LINK; }
NetLink SteamNet_Connect(unsigned long long,unsigned short,DWORD,DWORD,volatile bool*) { return NET_NO_LINK; }
void SteamNet_SetTimeouts(NetLink,DWORD) {}
void SteamNet_PeerAddress(NetLink,char*,size_t) {}
bool SteamNet_Send(NetLink,const void*,size_t) { return false; }
bool SteamNet_Recv(NetLink,void*,size_t) { return false; }
bool SteamNet_PeerGone(NetLink) { return true; }
bool SteamNet_WaitReadable(NetLink,DWORD,volatile bool*) { return false; }
void SteamNet_Shutdown(NetLink) {}
void SteamNet_Close(NetLink) {}
unsigned long long SteamNet_LocalId() { return 0; }
bool SteamNet_TakeJoinRequest(char*,size_t) { return false; }
unsigned short QaAutomation_ConnectPort(const char*,unsigned short port) { return port; }
static void quiet(const char*, ...) {}

struct Pair {
    SOCKET a, b;
    Pair(bool backpressure = false) {
        SOCKET listener = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        assert(listener != INVALID_SOCKET);
        if (backpressure) { int small=1024; assert(setsockopt(listener,SOL_SOCKET,SO_RCVBUF,(char*)&small,sizeof(small))==0); }
        sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        assert(bind(listener,(sockaddr*)&addr,sizeof(addr))==0);
        assert(listen(listener,1)==0);
        int len=sizeof(addr); assert(getsockname(listener,(sockaddr*)&addr,&len)==0);
        a=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        if (backpressure) { int zero=0; assert(setsockopt(a,SOL_SOCKET,SO_SNDBUF,(char*)&zero,sizeof(zero))==0); }
        assert(connect(a,(sockaddr*)&addr,sizeof(addr))==0);
        b=accept(listener,nullptr,nullptr); assert(b!=INVALID_SOCKET);
        closesocket(listener);
        NetworkSetTimeouts(a,2000); NetworkSetTimeouts(b,2000);
    }
    ~Pair(){ NetworkShutdown(a); NetworkShutdown(b); closesocket(a); closesocket(b); }
};
static void writeFile(const char* name,const std::string& text) { std::ofstream(name,std::ios::binary)<<text; }
static std::string readFile(const char* name) { std::ifstream f(name,std::ios::binary); return {std::istreambuf_iterator<char>(f),{}}; }
static int passed=0;
static void pass(const char* name) { ++passed; std::cout<<"PASS "<<name<<std::endl; }
int main() {
    // A regression to an unbounded wait must fail the suite, never hang it.
    std::thread([] { Sleep(30000); std::cerr<<"FAIL: test watchdog"<<std::endl; ExitProcess(99); }).detach();
    TsmHost host{}; host.structSize=sizeof(host); host.log=quiet; Network_Bind(&host);
    assert(NetworkInit());
    {
        Pair p; std::vector<char> sent(200000),got(sent.size());
        for(size_t i=0;i<sent.size();++i) sent[i]=(char)(i%251);
        std::thread sender([&]{for(size_t off=0;off<sent.size();){size_t n=std::min(size_t(173),sent.size()-off);assert(NetworkSendExact(p.a,sent.data()+off,n));off+=n;}});
        assert(NetworkRecvExact(p.b,got.data(),got.size())); sender.join(); assert(got==sent); pass("fragmented exact receive");
    }
    {
        Pair p; NetworkSetTimeouts(p.b,250); char c; auto start=GetTickCount64();
        assert(!NetworkRecvExact(p.b,&c,1)); auto elapsed=GetTickCount64()-start;
        assert(elapsed>=200 && elapsed<2000); pass("silent hello deadline");
    }
    {
        Pair p; NetworkSetTimeouts(p.b,250); char data[16];
        std::thread trickle([&]{for(int i=0;i<8;++i){Sleep(65); if(send(p.a,"x",1,0)!=1) break;}});
        auto start=GetTickCount64(); assert(!NetworkRecvExact(p.b,data,sizeof(data)));
        assert(GetTickCount64()-start<650); NetworkShutdown(p.b); trickle.join();
        DWORD timeout=0;int n=sizeof(timeout);assert(getsockopt(p.b,SOL_SOCKET,SO_RCVTIMEO,(char*)&timeout,&n)==0);assert(timeout==250);
        pass("partial trickle cannot extend deadline; option unchanged");
    }
    {
        Pair p(true); NetworkSetSendTimeout(p.a,250);
        std::vector<char> data(32*1024*1024,'x'); auto start=GetTickCount64();
        assert(!NetworkSendExact(p.a,data.data(),data.size())); assert(GetTickCount64()-start<2000);
        pass("peer stops reading: send deadline");
    }
    for(DWORD timeout : {0ul,5000ul}) {
        Pair p; NetworkSetTimeouts(p.b,timeout); bool result=true;
        std::thread receiver([&]{char c; result=NetworkRecvExact(p.b,&c,1);});
        Sleep(100); auto start=GetTickCount64();NetworkShutdown(p.b);receiver.join();
        assert(!result && GetTickCount64()-start<1500); pass(timeout?"cancel overlapped receive":"cancel blocking receive");
    }
    const char* output="received-test.bin";
    {
        Pair p; std::string content(180000,'z'); writeFile("send-test.bin",content);writeFile(output,"old");
        std::thread sender([&]{assert(NetworkSendFile(p.a,"send-test.bin"));});
        assert(NetworkReceiveFile(p.b,output)); sender.join();assert(readFile(output)==content);
        assert(!std::filesystem::exists("received-test.bin.part")); pass("complete file publishes atomically");
    }
    {
        Pair p; writeFile(output,"previous complete file");
        unsigned long long size=1000;assert(NetworkSendExact(p.a,&size,sizeof(size)));
        assert(NetworkSendExact(p.a,"tiny",4));NetworkShutdown(p.a);
        assert(!NetworkReceiveFile(p.b,output));assert(readFile(output)=="previous complete file");
        assert(!std::filesystem::exists("received-test.bin.part"));pass("interrupted file preserves previous version");
    }
    for(unsigned long long size : {0ull,2ull*1024*1024*1024+1}) {
        Pair p;writeFile(output,"untouched");assert(NetworkSendExact(p.a,&size,sizeof(size)));
        assert(!NetworkReceiveFile(p.b,output));assert(readFile(output)=="untouched");pass("invalid transfer size rejected");
    }
    {
        Pair p;writeFile("received-test.bin.part","existing partial");unsigned long long size=4;
        assert(NetworkSendExact(p.a,&size,sizeof(size)));assert(!NetworkReceiveFile(p.b,output));
        assert(readFile("received-test.bin.part")=="existing partial");std::filesystem::remove("received-test.bin.part");
        pass("existing partial never overwritten");
    }
    {
        Pair p; DWORD before=0,after=0;GetProcessHandleCount(GetCurrentProcess(),&before);
        for(int i=0;i<200;++i) {char c;assert(NetworkSendExact(p.a,"x",1));assert(NetworkRecvExact(p.b,&c,1));}
        GetProcessHandleCount(GetCurrentProcess(),&after);assert(after<=before+1);pass("deadline events do not leak handles");
    }
    WSACleanup(); std::cout<<"ALL OK: "<<passed<<" TCP tests"<<std::endl;
}
