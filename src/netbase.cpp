// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2013-2015 The Anoncoin Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Many builder specific things set in the config file, don't forget to include it this way in your source files.
#ifdef HAVE_CONFIG_H
#include "config/anoncoin-config.h"
#endif

#ifdef HAVE_GETADDRINFO_A
#include <netdb.h>
#endif

#include "netbase.h"

#include "addrman.h"        // For looking up local b32.i2p addresses as base64 i2p destinations
#include "hash.h"
#include "sync.h"
#include "ui_interface.h"
#include "uint256.h"
#include "util.h"

#ifndef WIN32
#if HAVE_INET_PTON
#include <arpa/inet.h>
#endif
#include <fcntl.h>
#endif

#include <boost/algorithm/string/case_conv.hpp> // for to_lower()
#include <boost/algorithm/string/predicate.hpp> // for startswith() and endswith()
#include <boost/foreach.hpp>

#if !defined(HAVE_MSG_NOSIGNAL) && !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

using namespace std;

// Settings
static proxyType proxyInfo[NET_MAX];
static CService nameProxy;
static CCriticalSection cs_proxyInfos;
// ToDo: Further analysis from debuging i2p connections may reveal that this setting is too low.
//       Connected peertable entries show ping times >13000ms  Changing this number's default
//       to 4 times what Bitcoin had it set too... Or possibly add a new variable for I2p specifically.
// int nConnectTimeout = 5000;
int nConnectTimeout = 20000;
bool fNameLookup = false;

static const unsigned char pchIPv4[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };

enum Network ParseNetwork(std::string net) {
    boost::to_lower(net);
    if (net == "ipv4") return NET_IPV4;
    if (net == "ipv6") return NET_IPV6;
    if (net == "tor" || net == "onion")  return NET_TOR;
#ifdef ENABLE_I2PSAM
    if (net == "i2p") return NET_NATIVE_I2P;
#endif
    return NET_UNROUTABLE;
}

std::string GetNetworkName(enum Network net) {
    switch(net)
    {
    case NET_IPV4: return "ipv4";
    case NET_IPV6: return "ipv6";
    case NET_TOR : return "tor";
#ifdef ENABLE_I2PSAM
    case NET_NATIVE_I2P: return "i2p";
#endif

    default: return "???";
    }
}

void SplitHostPort(std::string in, int &portOut, std::string &hostOut) {
    size_t colon = in.find_last_of(':');
    // if a : is found, and it either follows a [...], or no other : is in the string, treat it as port separator
    bool fHaveColon = colon != in.npos;
    bool fBracketed = fHaveColon && (in[0]=='[' && in[colon-1]==']'); // if there is a colon, and in[0]=='[', colon is not 0, so in[colon-1] is safe
    bool fMultiColon = fHaveColon && (in.find_last_of(':',colon-1) != in.npos);
    if (fHaveColon && (colon==0 || fBracketed || !fMultiColon)) {
        char *endp = NULL;
        int n = strtol(in.c_str() + colon + 1, &endp, 10);
        if (endp && *endp == 0 && n >= 0) {
            in = in.substr(0, colon);
            if (n > 0 && n < 0x10000)
                portOut = n;
        }
    }
    if (in.size()>0 && in[0] == '[' && in[in.size()-1] == ']')
        hostOut = in.substr(1, in.size()-2);
    else
        hostOut = in;
}

bool static LookupIntern(const char *pszName, std::vector<CNetAddr>& vIP, unsigned int nMaxSolutions, bool fAllowLookup)
{
    vIP.clear();

    {
        CNetAddr addr;
        std::string strName( pszName );
        if (addr.SetSpecial(strName)) {
            vIP.push_back(addr);
            return true;
        }
#ifdef ENABLE_I2PSAM
        // The problem is:  if SetSpecial returns false, we don't know why it failed or what happen
        else if( isStringI2pDestination( strName ) ) {
            // LogPrintf( "...." );  so SetSpecial now has extensive logging support of errors, need more put it here
            return false;   // we're done here, a dns seed node could not be found or any other b32.i2p address failed to be found
        }
#endif
    }

#ifdef HAVE_GETADDRINFO_A
    struct in_addr ipv4_addr;
#ifdef HAVE_INET_PTON
    if (inet_pton(AF_INET, pszName, &ipv4_addr) > 0) {
        vIP.push_back(CNetAddr(ipv4_addr));
        return true;
    }

    struct in6_addr ipv6_addr;
    if (inet_pton(AF_INET6, pszName, &ipv6_addr) > 0) {
        vIP.push_back(CNetAddr(ipv6_addr));
        return true;
    }
#else
    ipv4_addr.s_addr = inet_addr(pszName);
    if (ipv4_addr.s_addr != INADDR_NONE) {
        vIP.push_back(CNetAddr(ipv4_addr));
        return true;
    }
#endif
#endif

    struct addrinfo aiHint;
    memset(&aiHint, 0, sizeof(struct addrinfo));
    aiHint.ai_socktype = SOCK_STREAM;
    aiHint.ai_protocol = IPPROTO_TCP;
    aiHint.ai_family = AF_UNSPEC;
#ifdef WIN32
    aiHint.ai_flags = fAllowLookup ? 0 : AI_NUMERICHOST;
#else
    aiHint.ai_flags = fAllowLookup ? AI_ADDRCONFIG : AI_NUMERICHOST;
#endif

    struct addrinfo *aiRes = NULL;
#ifdef HAVE_GETADDRINFO_A
    struct gaicb gcb, *query = &gcb;
    memset(query, 0, sizeof(struct gaicb));
    gcb.ar_name = pszName;
    gcb.ar_request = &aiHint;
    int nErr = getaddrinfo_a(GAI_NOWAIT, &query, 1, NULL);
    if (nErr)
        return false;

    do {
        // Should set the timeout limit to a resonable value to avoid
        // generating unnecessary checking call during the polling loop,
        // while it can still response to stop request quick enough.
        // 2 seconds looks fine in our situation.
        struct timespec ts = { 2, 0 };
        gai_suspend(&query, 1, &ts);
        boost::this_thread::interruption_point();

        nErr = gai_error(query);
        if (0 == nErr)
            aiRes = query->ar_result;
    } while (nErr == EAI_INPROGRESS);
#else
    int nErr = getaddrinfo(pszName, NULL, &aiHint, &aiRes);
#endif
    if (nErr)
        return false;

    struct addrinfo *aiTrav = aiRes;
    while (aiTrav != NULL && (nMaxSolutions == 0 || vIP.size() < nMaxSolutions))
    {
        if (aiTrav->ai_family == AF_INET)
        {
            assert(aiTrav->ai_addrlen >= sizeof(sockaddr_in));
            vIP.push_back(CNetAddr(((struct sockaddr_in*)(aiTrav->ai_addr))->sin_addr));
        }

        if (aiTrav->ai_family == AF_INET6)
        {
            assert(aiTrav->ai_addrlen >= sizeof(sockaddr_in6));
            vIP.push_back(CNetAddr(((struct sockaddr_in6*)(aiTrav->ai_addr))->sin6_addr));
        }

        aiTrav = aiTrav->ai_next;
    }

    freeaddrinfo(aiRes);

    return (vIP.size() > 0);
}

bool LookupHost(const char *pszName, std::vector<CNetAddr>& vIP, unsigned int nMaxSolutions, bool fAllowLookup)
{
    std::string strHost(pszName);
    if (strHost.empty())
        return false;
    if (boost::algorithm::starts_with(strHost, "[") && boost::algorithm::ends_with(strHost, "]"))
    {
        strHost = strHost.substr(1, strHost.size() - 2);
    }

    return LookupIntern(strHost.c_str(), vIP, nMaxSolutions, fAllowLookup);
}

bool LookupHostNumeric(const char *pszName, std::vector<CNetAddr>& vIP, unsigned int nMaxSolutions)
{
    return LookupHost(pszName, vIP, nMaxSolutions, false);
}

bool Lookup(const char *pszName, std::vector<CService>& vAddr, int portDefault, bool fAllowLookup, unsigned int nMaxSolutions)
{
    if (pszName[0] == 0)
        return false;
    int port = portDefault;
    std::string hostname = "";
    SplitHostPort(std::string(pszName), port, hostname);            // SplitHostPort also removes leading and trailing [] brackets

    std::vector<CNetAddr> vIP;
    bool fRet = LookupIntern(hostname.c_str(), vIP, nMaxSolutions, fAllowLookup);
    if (!fRet)
        return false;
    vAddr.resize(vIP.size());
    for (unsigned int i = 0; i < vIP.size(); i++)
        vAddr[i] = CService(vIP[i], port);
    return true;
}

bool Lookup(const char *pszName, CService& addr, int portDefault, bool fAllowLookup)
{
    std::vector<CService> vService;
    bool fRet = Lookup(pszName, vService, portDefault, fAllowLookup, 1);
    if (!fRet)
        return false;
    addr = vService[0];
    return true;
}

bool LookupNumeric(const char *pszName, CService& addr, int portDefault)
{
    return Lookup(pszName, addr, portDefault, false);
}

bool static Socks5(string strDest, int port, SOCKET& hSocket)
{
    LogPrintf("SOCKS5 connecting %s\n", strDest);
    if (strDest.size() > 255)
    {
        closesocket(hSocket);
        return error("Hostname too long");
    }
    char pszSocks5Init[] = "\5\1\0";
    ssize_t nSize = sizeof(pszSocks5Init) - 1;

    ssize_t ret = send(hSocket, pszSocks5Init, nSize, MSG_NOSIGNAL);
    if (ret != nSize)
    {
        closesocket(hSocket);
        return error("Error sending to proxy");
    }
    char pchRet1[2];
    if (recv(hSocket, pchRet1, 2, 0) != 2)
    {
        closesocket(hSocket);
        return error("Error reading proxy response");
    }
    if (pchRet1[0] != 0x05 || pchRet1[1] != 0x00)
    {
        closesocket(hSocket);
        return error("Proxy failed to initialize");
    }
    string strSocks5("\5\1");
    strSocks5 += '\000'; strSocks5 += '\003';
    strSocks5 += static_cast<char>(std::min((int)strDest.size(), 255));
    strSocks5 += strDest;
    strSocks5 += static_cast<char>((port >> 8) & 0xFF);
    strSocks5 += static_cast<char>((port >> 0) & 0xFF);
    ret = send(hSocket, strSocks5.c_str(), strSocks5.size(), MSG_NOSIGNAL);
    if (ret != (ssize_t)strSocks5.size())
    {
        closesocket(hSocket);
        return error("Error sending to proxy");
    }
    char pchRet2[4];
    if (recv(hSocket, pchRet2, 4, 0) != 4)
    {
        closesocket(hSocket);
        return error("Error reading proxy response");
    }
    if (pchRet2[0] != 0x05)
    {
        closesocket(hSocket);
        return error("Proxy failed to accept request");
    }
    if (pchRet2[1] != 0x00)
    {
        closesocket(hSocket);
        switch (pchRet2[1])
        {
            case 0x01: return error("Proxy error: general failure");
            case 0x02: return error("Proxy error: connection not allowed");
            case 0x03: return error("Proxy error: network unreachable");
            case 0x04: return error("Proxy error: host unreachable");
            case 0x05: return error("Proxy error: connection refused");
            case 0x06: return error("Proxy error: TTL expired");
            case 0x07: return error("Proxy error: protocol error");
            case 0x08: return error("Proxy error: address type not supported");
            default:   return error("Proxy error: unknown");
        }
    }
    if (pchRet2[2] != 0x00)
    {
        closesocket(hSocket);
        return error("Error: malformed proxy response");
    }
    char pchRet3[256];
    switch (pchRet2[3])
    {
        case 0x01: ret = recv(hSocket, pchRet3, 4, 0) != 4; break;
        case 0x04: ret = recv(hSocket, pchRet3, 16, 0) != 16; break;
        case 0x03:
        {
            ret = recv(hSocket, pchRet3, 1, 0) != 1;
            if (ret) {
                closesocket(hSocket);
                return error("Error reading from proxy");
            }
            int nRecv = pchRet3[0];
            ret = recv(hSocket, pchRet3, nRecv, 0) != nRecv;
            break;
        }
        default: closesocket(hSocket); return error("Error: malformed proxy response");
    }
    if (ret)
    {
        closesocket(hSocket);
        return error("Error reading from proxy");
    }
    if (recv(hSocket, pchRet3, 2, 0) != 2)
    {
        closesocket(hSocket);
        return error("Error reading from proxy");
    }
    LogPrintf("SOCKS5 connected %s\n", strDest);
    return true;
}

bool static ConnectSocketDirectly(const CService &addrConnect, SOCKET& hSocketRet, int nTimeout)
{
    hSocketRet = INVALID_SOCKET;

    struct sockaddr_storage sockaddr;
    socklen_t len = sizeof(sockaddr);
    if (!addrConnect.GetSockAddr((struct sockaddr*)&sockaddr, &len)) {
        LogPrintf("Cannot connect to %s: unsupported network\n", addrConnect.ToString());
        return false;
    }

    SOCKET hSocket = socket(((struct sockaddr*)&sockaddr)->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (hSocket == INVALID_SOCKET)
        return false;
#ifdef SO_NOSIGPIPE
    int set = 1;
    setsockopt(hSocket, SOL_SOCKET, SO_NOSIGPIPE, (void*)&set, sizeof(int));
#endif

#ifdef WIN32
    u_long fNonblock = 1;
    if (ioctlsocket(hSocket, FIONBIO, &fNonblock) == SOCKET_ERROR)
#else
    int fFlags = fcntl(hSocket, F_GETFL, 0);
    if (fcntl(hSocket, F_SETFL, fFlags | O_NONBLOCK) == -1)
#endif
    {
        closesocket(hSocket);
        return false;
    }

    if (connect(hSocket, (struct sockaddr*)&sockaddr, len) == SOCKET_ERROR)
    {
        // WSAEINVAL is here because some legacy version of winsock uses it
        if (WSAGetLastError() == WSAEINPROGRESS || WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINVAL)
        {
            struct timeval timeout;
            timeout.tv_sec  = nTimeout / 1000;
            timeout.tv_usec = (nTimeout % 1000) * 1000;

            fd_set fdset;
            FD_ZERO(&fdset);
            FD_SET(hSocket, &fdset);
            int nRet = select(hSocket + 1, NULL, &fdset, NULL, &timeout);
            if (nRet == 0)
            {
                LogPrint("net", "connection to %s timeout\n", addrConnect.ToString());
                closesocket(hSocket);
                return false;
            }
            if (nRet == SOCKET_ERROR)
            {
                LogPrintf("select() for %s failed: %s\n", addrConnect.ToString(), NetworkErrorString(WSAGetLastError()));
                closesocket(hSocket);
                return false;
            }
            socklen_t nRetSize = sizeof(nRet);
#ifdef WIN32
            if (getsockopt(hSocket, SOL_SOCKET, SO_ERROR, (char*)(&nRet), &nRetSize) == SOCKET_ERROR)
#else
            if (getsockopt(hSocket, SOL_SOCKET, SO_ERROR, &nRet, &nRetSize) == SOCKET_ERROR)
#endif
            {
                LogPrintf("getsockopt() for %s failed: %s\n", addrConnect.ToString(), NetworkErrorString(WSAGetLastError()));
                closesocket(hSocket);
                return false;
            }
            if (nRet != 0)
            {
                LogPrintf("connect() to %s failed after select(): %s\n", addrConnect.ToString(), NetworkErrorString(nRet));
                closesocket(hSocket);
                return false;
            }
        }
#ifdef WIN32
        else if (WSAGetLastError() != WSAEISCONN)
#else
        else
#endif
        {
            LogPrintf("connect() to %s failed: %s\n", addrConnect.ToString(), NetworkErrorString(WSAGetLastError()));
            closesocket(hSocket);
            return false;
        }
    }

    // this isn't even strictly necessary
    // CNode::ConnectNode immediately turns the socket back to non-blocking
    // but we'll turn it back to blocking just in case
#ifdef WIN32
    fNonblock = 0;
    if (ioctlsocket(hSocket, FIONBIO, &fNonblock) == SOCKET_ERROR)
#else
    fFlags = fcntl(hSocket, F_GETFL, 0);
    if (fcntl(hSocket, F_SETFL, fFlags & ~O_NONBLOCK) == SOCKET_ERROR)
#endif
    {
        closesocket(hSocket);
        return false;
    }

    hSocketRet = hSocket;
    return true;
}

bool SetProxy(enum Network net, CService addrProxy) {
    assert(net >= 0 && net < NET_MAX);
    if (!addrProxy.IsValid())
        return false;
    LOCK(cs_proxyInfos);
    proxyInfo[net] = addrProxy;
    return true;
}

bool GetProxy(enum Network net, proxyType &proxyInfoOut) {
    assert(net >= 0 && net < NET_MAX);
    LOCK(cs_proxyInfos);
    if (!proxyInfo[net].IsValid())
        return false;
    proxyInfoOut = proxyInfo[net];
    return true;
}

bool SetNameProxy(CService addrProxy) {
    if (!addrProxy.IsValid())
        return false;
    LOCK(cs_proxyInfos);
    nameProxy = addrProxy;
    return true;
}

bool GetNameProxy(CService &nameProxyOut) {
    LOCK(cs_proxyInfos);
    if(!nameProxy.IsValid())
        return false;
    nameProxyOut = nameProxy;
    return true;
}

bool HaveNameProxy() {
    LOCK(cs_proxyInfos);
    return nameProxy.IsValid();
}

bool IsProxy(const CNetAddr &addr) {
    LOCK(cs_proxyInfos);
    for (int i = 0; i < NET_MAX; i++) {
        if (addr == (CNetAddr)proxyInfo[i])
            return true;
    }
    return false;
}

#ifdef ENABLE_I2PSAM
// All this really does is for i2p connect & accept is set the socket to nonblocking, code could more than
// likely be removed and use the regular ip version in places where the code overlapps anyway.
bool SetI2pSocketOptions(SOCKET& hSocket)
{
    if (hSocket == INVALID_SOCKET)
        return false;
#ifdef SO_NOSIGPIPE
    int set = 1;
    setsockopt(hSocket, SOL_SOCKET, SO_NOSIGPIPE, (void*)&set, sizeof(int));
#endif

#ifdef WIN32
    u_long fNonblock = 1;
    if (ioctlsocket(hSocket, FIONBIO, &fNonblock) == SOCKET_ERROR)
#else
    int fFlags = fcntl(hSocket, F_GETFL, 0);
    if (fcntl(hSocket, F_SETFL, fFlags | O_NONBLOCK) == -1)
#endif
    {
        closesocket(hSocket);
        hSocket = INVALID_SOCKET;
        return false;
    }
    return true;
}
#endif // ENABLE_I2PSAM

bool ConnectSocket(const CService &addrDest, SOCKET& hSocketRet, int nTimeout)
{
    proxyType proxy;

#ifdef ENABLE_I2PSAM
    if( addrDest.IsI2P() ) {
        assert( addrDest.IsNativeI2P() );
        SOCKET streamSocket = I2PSession::Instance().connect(addrDest.GetI2pDestination(), false/*, streamSocket*/);
        if( SetI2pSocketOptions(streamSocket) ) {
            hSocketRet = streamSocket;
            return true;
        }
        return false;
    }
#endif // ENABLE_I2PSAM

    // no proxy needed (none set for target network)
    if (!GetProxy(addrDest.GetNetwork(), proxy))
        return ConnectSocketDirectly(addrDest, hSocketRet, nTimeout);

    SOCKET hSocket = INVALID_SOCKET;

    // first connect to proxy server
    if (!ConnectSocketDirectly(proxy, hSocket, nTimeout))
        return false;
    // do socks negotiation
    if (!Socks5(addrDest.ToStringIP(), addrDest.GetPort(), hSocket))
        return false;

    hSocketRet = hSocket;
    return true;
}

bool ConnectSocketByName(CService &addr, SOCKET& hSocketRet, const char *pszDest, int portDefault, int nTimeout)
{
    string strDest;
    int port = portDefault;
    SplitHostPort(string(pszDest), port, strDest);          // Also strips off any leading and trailing [ ] characters

    SOCKET hSocket = INVALID_SOCKET;

    CService nameProxy;
    GetNameProxy(nameProxy);

    CService addrResolved(CNetAddr(strDest, fNameLookup && !HaveNameProxy()), port);
    if (addrResolved.IsValid()) {
        addr = addrResolved;
        return ConnectSocket(addr, hSocketRet, nTimeout);
    }

    addr = CService("0.0.0.0:0");

    if (!HaveNameProxy())
        return false;
    // first connect to name proxy server
    if (!ConnectSocketDirectly(nameProxy, hSocket, nTimeout))
        return false;
    // do socks negotiation
    if (!Socks5(strDest, (unsigned short)port, hSocket))
        return false;

    hSocketRet = hSocket;
    return true;
}

void CNetAddr::Init()
{
    memset(ip, 0, sizeof(ip));
#ifdef ENABLE_I2PSAM
    memset(i2pDest, 0, NATIVE_I2P_DESTINATION_SIZE);
#endif
}

void CNetAddr::SetIP(const CNetAddr& ipIn)
{
    memcpy(ip, ipIn.ip, sizeof(ip));
#ifdef ENABLE_I2PSAM
    memcpy(i2pDest, ipIn.i2pDest, NATIVE_I2P_DESTINATION_SIZE);
#endif
}

static const unsigned char pchOnionCat[] = {0xFD,0x87,0xD8,0x7E,0xEB,0x43};

/** \brief
    Implementing support for i2p addresses by using a similar idea as tor(aka onion addrs) so based on the ideas found here:
    https://www.cypherpunk.at/onioncat_trac/wiki/GarliCat
    we're going to use the beginning bytes of the ip address to indicate i2p destination is the payload here.
 *
 */
static const unsigned char pchGarlicCat[] = { 0xFD,0x60, 0xDB,0x4D, 0xDD,0xB5 };        // A /48 ip6 prefix for I2P destinations...

/**
 * Returns TRUE if the address name can be looked up and resolved
 SetSpecial is the workhorse routine which handles the I2P b32.i2p and full base64 destination addresses.  If dns is true, base32 address lookups are
 allowed and done as well.  New protocol 70009+ layer definition introduces the concept of a pchGarlicCat address held in the ip storage area, as the
 new primary way to know this address is FOR the i2p network, and not some other destination.  That is built here as well.
 */
bool CNetAddr::SetSpecial(const std::string &strName)
{
#ifdef ENABLE_I2PSAM
    // Any address that ends in b32.i2p should be valid here, as the router itself is used to lookup the base64 destination, it returns without an address
    // if the string can not be found, if it's a base64 address, we can use it as is, to setup a new correctly formated CNetAddr object
    if( isStringI2pDestination( strName ) )                                      // Perhaps we've been given a I2P address, they come in 2 different ways
    {                                                                       // We're given a possible valid .b32.i2p address or a native I2P destination
        string addr;
        if( isValidI2pB32( strName ) ) {
            // 1st try our new local address book for the lookup....
            // NOTE: Adding this line of code was extremely expensive for the developer, it broke the build system, which could no longer link
            // and create the Anoncoin-cli executable.  It requires that you link in the addrman.cpp/h module, as its now included here in netbase,
            // This had  never been done until now.  You will be required to upgrade your code to support the chainparamsbase.cpp/h module concept,
            // and remove all references to chainparams.cpp/h in your build script, if your going to try and just add this one line of code.
            //  Upgrading your source base to v10 technology was the solution, but very time consuming, expensive and stressful.  It may appear to
            // be just one line of code, but it is not nearly as simple as that when your starting from and working with a v9 source code base.
            // ...GR
            addr = addrman.GetI2pBase64Destination( strName );
            if( IsI2PEnabled() && fNameLookup ) {                           // Check for dns set, we should at least log the error, if not.
                int64_t iNow = GetTime();
                if( !addr.size() )                                          // If we couldn't find it, much more to do..
                    addr = I2PSession::Instance().namingLookup(strName);    // Expensive, but lets try, this could take a very long while...
                else
                    LogPrintf( "That Base64 i2p destination you needed, was found locally to match: %s\n", strName );
                // If the address returned is a non-zero length string, the lookup was successful
                if( !isValidI2pAddress( addr ) ) {                          // Not sure why, but that shouldn't happen, could be a 'new' destination type we can't yet handle
                    LogPrintf( "WARNING - After spending %llds looking, neither AddrMan or the I2P Router was able to find that address for you: %s\n", GetTime() - iNow, strName );
                    return false;                                           // Not some thing we can use
                } // else  // Otherwise the AddrMan or I2P router was able to find an I2P destination for this address, and it's now stored in 'addr' as a base64 string
                    // LogPrintf( "AddrMan or I2P Router lookup found [%s] address as destination\n[%s]\n", strName, addr );
            } else {                                                        // Log should tell the user they have DNS turned off, so this can't work
                LogPrintf( "WARNING - Unable to lookup [%s], No i2p router or dns=0 must be set\n", strName );
                return false;
            }
        } else                                                              // It was a native I2P address to begin with
            addr = strName;                                                 // Prep for memcpy()
        // If we make it here 'addr' has i2p destination address as a base 64 string...
        // Now we can build the output array of bytes as we need for protocol 70009+ by using the concept of a IP6 string we call pchGarlicCat
        memcpy(ip, pchGarlicCat, sizeof(pchGarlicCat));
        memcpy(i2pDest, addr.c_str(), NATIVE_I2P_DESTINATION_SIZE);         // So now copy it to our CNetAddr obect variable
        return true;                                                        // Special handling taken care of
    }
#endif // ENABLE_I2PSAM

    if (strName.size()>6 && strName.substr(strName.size() - 6, 6) == ".onion") {
        std::vector<unsigned char> vchAddr = DecodeBase32(strName.substr(0, strName.size() - 6).c_str());
        if (vchAddr.size() != 16-sizeof(pchOnionCat))
            return false;
        memcpy(ip, pchOnionCat, sizeof(pchOnionCat));
        for (unsigned int i=0; i<16-sizeof(pchOnionCat); i++)
            ip[i + sizeof(pchOnionCat)] = vchAddr[i];
        return true;
    }
    return false;
}

CNetAddr::CNetAddr()
{
    Init();
}

CNetAddr::CNetAddr(const struct in_addr& ipv4Addr)
{
    memcpy(ip,    pchIPv4, 12);
    memcpy(ip+12, &ipv4Addr, 4);
#ifdef ENABLE_I2PSAM
    memset(i2pDest, 0, NATIVE_I2P_DESTINATION_SIZE);
#endif
}

CNetAddr::CNetAddr(const struct in6_addr& ipv6Addr)
{
    memcpy(ip, &ipv6Addr, 16);
#ifdef ENABLE_I2PSAM
    memset(i2pDest, 0, NATIVE_I2P_DESTINATION_SIZE);
#endif
}

CNetAddr::CNetAddr(const char *pszIp, bool fAllowLookup)
{
    Init();
    std::vector<CNetAddr> vIP;
    if (LookupHost(pszIp, vIP, 1, fAllowLookup))
        *this = vIP[0];
}

CNetAddr::CNetAddr(const std::string &strIp, bool fAllowLookup)
{
    Init();
#ifdef XXXXXXXENABLE_I2PSAM
    // Don't think this is needed anymore, LookupHost->LookupIntern->SetSpecial anyway
    // isValidI2pAddress call is needed for fixed seed i2p addresses, see: chainparam.cpp
    if( isStringI2pDestination( strIp ) )
        SetSpecial( strIp );
    else
#endif
    {
        std::vector<CNetAddr> vIP;
        if (LookupHost(strIp.c_str(), vIP, 1, fAllowLookup))
            *this = vIP[0];
    }
}

unsigned int CNetAddr::GetByte(int n) const
{
    return ip[15-n];
}

bool CNetAddr::IsIPv4() const
{
    return (memcmp(ip, pchIPv4, sizeof(pchIPv4)) == 0);
}

bool CNetAddr::IsIPv6() const
{
#ifdef ENABLE_I2PSAM
    return (!IsIPv4() && !IsTor() && !IsI2P());
#else   // Original Code
    return (!IsIPv4() && !IsTor());
#endif
}

bool CNetAddr::IsRFC1918() const
{
    return IsIPv4() && (
        GetByte(3) == 10 ||
        (GetByte(3) == 192 && GetByte(2) == 168) ||
        (GetByte(3) == 172 && (GetByte(2) >= 16 && GetByte(2) <= 31)));
}

bool CNetAddr::IsRFC3927() const
{
    return IsIPv4() && (GetByte(3) == 169 && GetByte(2) == 254);
}

bool CNetAddr::IsRFC3849() const
{
    return GetByte(15) == 0x20 && GetByte(14) == 0x01 && GetByte(13) == 0x0D && GetByte(12) == 0xB8;
}

bool CNetAddr::IsRFC3964() const
{
    return (GetByte(15) == 0x20 && GetByte(14) == 0x02);
}

bool CNetAddr::IsRFC6052() const
{
    static const unsigned char pchRFC6052[] = {0,0x64,0xFF,0x9B,0,0,0,0,0,0,0,0};
    return (memcmp(ip, pchRFC6052, sizeof(pchRFC6052)) == 0);
}

bool CNetAddr::IsRFC4380() const
{
    return (GetByte(15) == 0x20 && GetByte(14) == 0x01 && GetByte(13) == 0 && GetByte(12) == 0);
}

bool CNetAddr::IsRFC4862() const
{
    static const unsigned char pchRFC4862[] = {0xFE,0x80,0,0,0,0,0,0};
    return (memcmp(ip, pchRFC4862, sizeof(pchRFC4862)) == 0);
}

bool CNetAddr::IsRFC4193() const
{
    return ((GetByte(15) & 0xFE) == 0xFC);
}

bool CNetAddr::IsRFC6145() const
{
    static const unsigned char pchRFC6145[] = {0,0,0,0,0,0,0,0,0xFF,0xFF,0,0};
    return (memcmp(ip, pchRFC6145, sizeof(pchRFC6145)) == 0);
}

bool CNetAddr::IsRFC4843() const
{
    return (GetByte(15) == 0x20 && GetByte(14) == 0x01 && GetByte(13) == 0x00 && (GetByte(12) & 0xF0) == 0x10);
}

bool CNetAddr::IsTor() const
{
    return (memcmp(ip, pchOnionCat, sizeof(pchOnionCat)) == 0);
}

#ifdef ENABLE_I2PSAM
bool CNetAddr::IsI2P() const
{
    return (memcmp(ip, pchGarlicCat, sizeof(pchGarlicCat)) == 0);
}

bool CNetAddr::IsNativeI2P() const
{
    static const unsigned char pchAAAA[] = {'A','A','A','A'};
    // For unsigned char [] it's quicker here to just do a memory comparison .verses. conversion to a string.
    // In order for this to work however, it's important that the memory has been cleared when this object
    // was created.
    // ToDo: More work could be done here to confirm it will never mistakenly see a valid native i2p address.
    return (memcmp(i2pDest + NATIVE_I2P_DESTINATION_SIZE - sizeof(pchAAAA), pchAAAA, sizeof(pchAAAA)) == 0);
}

std::string CNetAddr::GetI2pDestination() const
{
    return i2pDest[0] != 0 ? std::string(i2pDest, i2pDest + NATIVE_I2P_DESTINATION_SIZE) : std::string();
}

/** \brief Sets the i2pDest field to the callers given string
    The ip field is not touched, if the parameter given is zero length, otherwise it is set to the GarlicCat
 *
 * \param sDestination const std::string
 * \return bool true if the address is now set to a valid i2p destination, otherwise false
 *
 */
bool CNetAddr::SetI2pDestination( const std::string& sDestination )
{
    size_t iSize = sDestination.size();
    if( iSize ) {
        Init();
        memcpy(ip, pchGarlicCat, sizeof(pchGarlicCat));
    } else          // First & always if we're given some non-zero value, Make sure the whole field is zeroed out
        memset(i2pDest, 0, NATIVE_I2P_DESTINATION_SIZE);

    // Copy what the caller wants put there, up to the max size
    // Its not going to be valid, if the size is wrong, but do it anyway
    if( iSize ) memcpy( i2pDest, sDestination.c_str(), iSize < NATIVE_I2P_DESTINATION_SIZE ? iSize : NATIVE_I2P_DESTINATION_SIZE );
    return (iSize == NATIVE_I2P_DESTINATION_SIZE) && IsNativeI2P();
}

// Convert this netaddress objects native i2p address into a b32.i2p address
std::string CNetAddr::ToB32String() const
{
    return B32AddressFromDestination( GetI2pDestination() );
}
#endif // ENABLE_I2PSAM

bool CNetAddr::IsLocal() const
{
#ifdef ENABLE_I2PSAM
    // This address is local if it is the same as the public key of the session we have open,
    // ToDo: Compare the destination address with the session mydestination, this works for
    // now (maybe), but should be done better with getting the info from the i2psam module
    if( IsI2P() ) {
        string sMyDest = GetArg("-i2p.mydestination.publickey", "");
        return sMyDest == GetI2pDestination();
    }
#endif

    // IPv4 loopback
   if (IsIPv4() && (GetByte(3) == 127 || GetByte(3) == 0))
       return true;

   // IPv6 loopback (::1/128)
   static const unsigned char pchLocal[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
   if (memcmp(ip, pchLocal, 16) == 0)
       return true;

   return false;
}

bool CNetAddr::IsMulticast() const
{
    return    (IsIPv4() && (GetByte(3) & 0xF0) == 0xE0)
           || (GetByte(15) == 0xFF);
}

bool CNetAddr::IsValid() const
{
#ifdef ENABLE_I2PSAM
    if( IsI2P() )
        return IsNativeI2P();
#endif
    // Cleanup 3-byte shifted addresses caused by garbage in size field
    // of addr messages from versions before 0.2.9 checksum.
    // Two consecutive addr messages look like this:
    // header20 vectorlen3 addr26 addr26 addr26 header20 vectorlen3 addr26 addr26 addr26...
    // so if the first length field is garbled, it reads the second batch
    // of addr misaligned by 3 bytes.
    if (memcmp(ip, pchIPv4+3, sizeof(pchIPv4)-3) == 0)
        return false;

    // unspecified IPv6 address (::/128)
    unsigned char ipNone[16] = {};
    if (memcmp(ip, ipNone, 16) == 0)
        return false;

    // documentation IPv6 address
    if (IsRFC3849())
        return false;

    if (IsIPv4())
    {
        // INADDR_NONE
        uint32_t ipNone = INADDR_NONE;
        if (memcmp(ip+12, &ipNone, 4) == 0)
            return false;

        // 0
        ipNone = 0;
        if (memcmp(ip+12, &ipNone, 4) == 0)
            return false;
    }

    return true;
}

bool CNetAddr::IsRoutable() const
{
    // By allowing RFC1918 addresses you can have routes setup for connections on your local ipv4 network, this is BAD if done
    // improperly, and is actually complicated to solve if you want to allow local private networks to share p2p data, but
    // not have it shared externally.  However our newest builds detect this, reject sharing those private addresses outside
    // yet allow p2p exchange to work well over the private network with separate destinations on each side.
    bool fDetermined = IsValid() && !(IsRFC3927() || IsRFC4862() || ( IsRFC4193() && !(IsTor() || IsI2P()) ) || IsRFC4843() || IsLocal());
    // bool fDetermined = IsValid() && !(IsRFC1918() || IsRFC3927() || IsRFC4862() || ( IsRFC4193() && !(IsTor() || IsI2P()) ) || IsRFC4843() || IsLocal());
    // LogPrintf( "Is this address %s routable? %s  It appears it is Valid=%s, Local=%s, I2P=%s\n", ToString(), fDetermined ? "YES" : "NO", IsValid() ? "1" : "0", IsLocal() ? "1" : "0", IsI2P() ? "1" : "0");
    // LogPrintf( "RFC1918=%s RFC3927=%s RFC4862=%s RFC4193=%s RFC4843=%s\n", IsRFC1918() ? "1" : "0", IsRFC3927() ? "1" : "0", IsRFC4862() ? "1" : "0", IsRFC4193() ? "1" : "0", IsRFC4843() ? "1" : "0" );
    return fDetermined;
}

enum Network CNetAddr::GetNetwork() const
{
    if (!IsRoutable())
        return NET_UNROUTABLE;

    if (IsIPv4())
        return NET_IPV4;

    if (IsTor())
        return NET_TOR;

#ifdef ENABLE_I2PSAM
    if (IsI2P()) return NET_NATIVE_I2P;
#endif

    return NET_IPV6;
}

std::string CNetAddr::ToStringIP() const
{
#ifdef ENABLE_I2PSAM
    if( IsI2P() )
        return IsNativeI2P() ? ToB32String() : "???.b32.i2p";
#endif
    if (IsTor())
        return EncodeBase32(&ip[6], 10) + ".onion";
    CService serv(*this, 0);
    struct sockaddr_storage sockaddr;
    socklen_t socklen = sizeof(sockaddr);
    if (serv.GetSockAddr((struct sockaddr*)&sockaddr, &socklen)) {
        char name[1025] = "";
        if (!getnameinfo((const struct sockaddr*)&sockaddr, socklen, name, sizeof(name), NULL, 0, NI_NUMERICHOST))
            return std::string(name);
    }
    if (IsIPv4())
        return strprintf("%u.%u.%u.%u", GetByte(3), GetByte(2), GetByte(1), GetByte(0));
    else
        return strprintf("%x:%x:%x:%x:%x:%x:%x:%x",
                         GetByte(15) << 8 | GetByte(14), GetByte(13) << 8 | GetByte(12),
                         GetByte(11) << 8 | GetByte(10), GetByte(9) << 8 | GetByte(8),
                         GetByte(7) << 8 | GetByte(6), GetByte(5) << 8 | GetByte(4),
                         GetByte(3) << 8 | GetByte(2), GetByte(1) << 8 | GetByte(0));
}

std::string CNetAddr::ToString() const
{
    return ToStringIP();
}

bool operator==(const CNetAddr& a, const CNetAddr& b)
{
#ifdef ENABLE_I2PSAM
    return (memcmp(a.ip, b.ip, 16) == 0 && memcmp(a.i2pDest, b.i2pDest, NATIVE_I2P_DESTINATION_SIZE) == 0);
#else                                               // Use the original code
    return (memcmp(a.ip, b.ip, 16) == 0);
#endif
}

bool operator!=(const CNetAddr& a, const CNetAddr& b)
{
#ifdef ENABLE_I2PSAM
    return (memcmp(a.ip, b.ip, 16) != 0 || memcmp(a.i2pDest, b.i2pDest, NATIVE_I2P_DESTINATION_SIZE) != 0);
#else                                               // Use the original code
    return (memcmp(a.ip, b.ip, 16) != 0);
#endif
}

bool operator<(const CNetAddr& a, const CNetAddr& b)
{
#ifdef ENABLE_I2PSAM
    return (memcmp(a.ip, b.ip, 16) < 0 || (memcmp(a.ip, b.ip, 16) == 0 && memcmp(a.i2pDest, b.i2pDest, NATIVE_I2P_DESTINATION_SIZE) < 0));
#else                                               // Use the original code
    return (memcmp(a.ip, b.ip, 16) < 0);
#endif
}

bool CNetAddr::GetInAddr(struct in_addr* pipv4Addr) const
{
    if (!IsIPv4())
        return false;
    memcpy(pipv4Addr, ip+12, 4);
    return true;
}

bool CNetAddr::GetIn6Addr(struct in6_addr* pipv6Addr) const
{
#ifdef ENABLE_I2PSAM
    if (IsNativeI2P()) return false;
#endif
    memcpy(pipv6Addr, ip, 16);
    return true;
}

// get canonical identifier of an address' group
// no two connections will be attempted to addresses with the same group
std::vector<unsigned char> CNetAddr::GetGroup() const
{
    std::vector<unsigned char> vchRet;
    int nClass = NET_IPV6;
    int nStartByte = 0;
    int nBits = 16;

#ifdef ENABLE_I2PSAM
    if( IsI2P() ) {
        vchRet.resize(NATIVE_I2P_DESTINATION_SIZE + 1);
        vchRet[0] = NET_NATIVE_I2P;
        memcpy(&vchRet[1], i2pDest, NATIVE_I2P_DESTINATION_SIZE);
        return vchRet;
    }
#endif // ENABLE_I2PSAM

    // all local addresses belong to the same group
    if (IsLocal())
    {
        nClass = 255;
        nBits = 0;
    }

    // all unroutable addresses belong to the same group
    if (!IsRoutable())
    {
        nClass = NET_UNROUTABLE;
        nBits = 0;
    }
    // for IPv4 addresses, '1' + the 16 higher-order bits of the IP
    // includes mapped IPv4, SIIT translated IPv4, and the well-known prefix
    else if (IsIPv4() || IsRFC6145() || IsRFC6052())
    {
        nClass = NET_IPV4;
        nStartByte = 12;
    }
    // for 6to4 tunnelled addresses, use the encapsulated IPv4 address
    else if (IsRFC3964())
    {
        nClass = NET_IPV4;
        nStartByte = 2;
    }
    // for Teredo-tunnelled IPv6 addresses, use the encapsulated IPv4 address
    else if (IsRFC4380())
    {
        vchRet.push_back(NET_IPV4);
        vchRet.push_back(GetByte(3) ^ 0xFF);
        vchRet.push_back(GetByte(2) ^ 0xFF);
        return vchRet;
    }
    else if (IsTor())
    {
        nClass = NET_TOR;
        nStartByte = 6;
        nBits = 4;
    }
    // for he.net, use /36 groups
    // ToDo: Figure out why this different between the Anoncoin v0.8.5.5 and v0.8.5.6 versions.
    // the line from v0.8.5.5 is now commented out.  Bitcoin v10 has the same value as Anoncoin v0.8.5.6, so using that.
    // Which is correct?
    else if (GetByte(15) == 0x20 && GetByte(14) == 0x01 && GetByte(13) == 0x04 && GetByte(12) == 0x70)
    // else if (GetByte(15) == 0x20 && GetByte(14) == 0x11 && GetByte(13) == 0x04 && GetByte(12) == 0x70)
        nBits = 36;
    // for the rest of the IPv6 network, use /32 groups
    else
        nBits = 32;

    vchRet.push_back(nClass);
    while (nBits >= 8)
    {
        vchRet.push_back(GetByte(15 - nStartByte));
        nStartByte++;
        nBits -= 8;
    }
    if (nBits > 0)
        vchRet.push_back(GetByte(15 - nStartByte) | ((1 << nBits) - 1));

    return vchRet;
}

uint64_t CNetAddr::GetHash() const
{
#ifdef ENABLE_I2PSAM
    uint256 hash = IsI2P() ? Hash(i2pDest, i2pDest + NATIVE_I2P_DESTINATION_SIZE) : Hash(&ip[0], &ip[16]);
#else                                               // Use the original code
    uint256 hash = Hash(&ip[0], &ip[16]);
#endif
    uint64_t nRet;
    memcpy(&nRet, &hash, sizeof(nRet));
    return nRet;
}

void CNetAddr::print() const
{
    LogPrintf("CNetAddr(%s)\n", ToString());
}

// private extensions to enum Network, only returned by GetExtNetwork,
// and only used in GetReachabilityFrom
static const int NET_UNKNOWN = NET_MAX + 0;
static const int NET_TEREDO  = NET_MAX + 1;
int static GetExtNetwork(const CNetAddr *addr)
{
    if (addr == NULL)
        return NET_UNKNOWN;
    if (addr->IsRFC4380())
        return NET_TEREDO;
    return addr->GetNetwork();
}

/** Calculates a metric for how reachable (*this) is from a given partner */
int CNetAddr::GetReachabilityFrom(const CNetAddr *paddrPartner) const
{
    enum Reachability {
        REACH_UNREACHABLE,
        REACH_DEFAULT,
        REACH_TEREDO,
        REACH_IPV6_WEAK,
        REACH_IPV4,
        REACH_IPV6_STRONG,
        REACH_PRIVATE
    };

    if (!IsRoutable())
        return REACH_UNREACHABLE;

    int ourNet = GetExtNetwork(this);
    int theirNet = GetExtNetwork(paddrPartner);
    bool fTunnel = IsRFC3964() || IsRFC6052() || IsRFC6145();

    switch(theirNet) {
    case NET_IPV4:
        switch(ourNet) {
        default:       return REACH_DEFAULT;
        case NET_IPV4: return REACH_IPV4;
        }
    case NET_IPV6:
        switch(ourNet) {
        default:         return REACH_DEFAULT;
        case NET_TEREDO: return REACH_TEREDO;
        case NET_IPV4:   return REACH_IPV4;
        case NET_IPV6:   return fTunnel ? REACH_IPV6_WEAK : REACH_IPV6_STRONG; // only prefer giving our IPv6 address if it's not tunnelled
        }
#ifdef ENABLE_I2PSAM
    case NET_NATIVE_I2P:
        switch(ourNet) {
        default:             return REACH_UNREACHABLE;
        case NET_NATIVE_I2P: return REACH_PRIVATE;
        }
#endif
    case NET_TOR:
        switch(ourNet) {
        default:         return REACH_DEFAULT;
        case NET_IPV4:   return REACH_IPV4; // Tor users can connect to IPv4 as well
        case NET_TOR:    return REACH_PRIVATE;
        }
    case NET_TEREDO:
        switch(ourNet) {
        default:          return REACH_DEFAULT;
        case NET_TEREDO:  return REACH_TEREDO;
        case NET_IPV6:    return REACH_IPV6_WEAK;
        case NET_IPV4:    return REACH_IPV4;
        }
    case NET_UNKNOWN:
    case NET_UNROUTABLE:
    default:
        switch(ourNet) {
        default:          return REACH_DEFAULT;
        case NET_TEREDO:  return REACH_TEREDO;
        case NET_IPV6:    return REACH_IPV6_WEAK;
        case NET_IPV4:    return REACH_IPV4;
        case NET_TOR:     return REACH_PRIVATE; // either from Tor, or don't care about our address
#ifdef ENABLE_I2PSAM
        case NET_NATIVE_I2P: return REACH_PRIVATE;  // Same for i2p
#endif
        }
    }
}

void CService::Init()
{
    port = 0;                   // This initialization fact becomes important to the programmer for object comparisons and other details, as we do not use the port for services on i2p
}

CService::CService()
{
    Init();
}

CService::CService(const CNetAddr& cip, unsigned short portIn) : CNetAddr(cip), port(portIn)
{
}

CService::CService(const struct in_addr& ipv4Addr, unsigned short portIn) : CNetAddr(ipv4Addr), port(portIn)
{
}

CService::CService(const struct in6_addr& ipv6Addr, unsigned short portIn) : CNetAddr(ipv6Addr), port(portIn)
{
}

CService::CService(const struct sockaddr_in& addr) : CNetAddr(addr.sin_addr), port(ntohs(addr.sin_port))
{
    assert(addr.sin_family == AF_INET);
}

CService::CService(const struct sockaddr_in6 &addr) : CNetAddr(addr.sin6_addr), port(ntohs(addr.sin6_port))
{
   assert(addr.sin6_family == AF_INET6);
}

bool CService::SetSockAddr(const struct sockaddr *paddr)
{
    switch (paddr->sa_family) {
    case AF_INET:
        *this = CService(*(const struct sockaddr_in*)paddr);
        return true;
    case AF_INET6:
        *this = CService(*(const struct sockaddr_in6*)paddr);
        return true;
    default:
        return false;
    }
}

CService::CService(const char *pszIpPort, bool fAllowLookup)
{
    Init();
    CService ip;
    if (Lookup(pszIpPort, ip, 0, fAllowLookup))
        *this = ip;
}

CService::CService(const char *pszIpPort, int portDefault, bool fAllowLookup)
{
    Init();
    CService ip;
    if (Lookup(pszIpPort, ip, portDefault, fAllowLookup))
        *this = ip;
}

CService::CService(const std::string &strIpPort, bool fAllowLookup)
{
    Init();
    CService ip;
    if (Lookup(strIpPort.c_str(), ip, 0, fAllowLookup))
        *this = ip;
}

CService::CService(const std::string &strIpPort, int portDefault, bool fAllowLookup)
{
    Init();
    CService ip;
    if (Lookup(strIpPort.c_str(), ip, portDefault, fAllowLookup))
        *this = ip;
}

unsigned short CService::GetPort() const
{
    return port;
}

bool operator==(const CService& a, const CService& b)
{
#ifdef ENABLE_I2PSAM
    if( a.IsNativeI2P() ) {             // True if the 1st service is I2P
        if( b.IsNativeI2P() ) {         // and if the 2nd is also, compare the i2p addresses
            return a.GetI2pDestination() == b.GetI2pDestination();
        } else                          // One is i2p the other is not
            return false;               // So return that they are not the same
    } else if( b.IsNativeI2P() )        // One is i2p the other is not
        return false;                   // So return that they are not the same
    else                                // Neither are I2P addresses, so use the original code
#endif
    return (CNetAddr)a == (CNetAddr)b && a.port == b.port;
}

bool operator!=(const CService& a, const CService& b)
{
#ifdef ENABLE_I2PSAM
    if( a.IsNativeI2P() ) {             // True if the 1st service is I2P
        if( b.IsNativeI2P() ) {         // and if the 2nd is also, compare the i2p addresses
            return a.GetI2pDestination() != b.GetI2pDestination();
        } else                          // One is i2p the other is not
            return true;                // So return that they are not the same
    } else if( b.IsNativeI2P() )        // One is i2p the other is not
        return true;                    // So return that they are not the same
    else                                // Neither are I2P addresses, so use the original code
#endif
    return (CNetAddr)a != (CNetAddr)b || a.port != b.port;      // Use the original code
}

bool operator<(const CService& a, const CService& b)
{
#ifdef ENABLE_I2PSAM
    if( a.IsNativeI2P() ) {             // True if the 1st service is I2P
        if( b.IsNativeI2P() ) {         // and if the 2nd is also, compare the i2p addresses
            return a.GetI2pDestination() < b.GetI2pDestination();
        } else                          // One is i2p the other is not
            return false;               // So return that they are not the same, I2P addresses are considered greater than IP addrs
    } else if( b.IsNativeI2P() )        // One is i2p the other is not
        return true;                    // So return that they are not the same, Addr 'a' is not I2P, but Addr 'b' is an I2P address
    else                                // Neither are I2P addresses, so use the original code
#endif
    return (CNetAddr)a < (CNetAddr)b || ((CNetAddr)a == (CNetAddr)b && a.port < b.port);
}


bool CService::GetSockAddr(struct sockaddr* paddr, socklen_t *addrlen) const
{
    if (IsIPv4()) {
        if (*addrlen < (socklen_t)sizeof(struct sockaddr_in))
            return false;
        *addrlen = sizeof(struct sockaddr_in);
        struct sockaddr_in *paddrin = (struct sockaddr_in*)paddr;
        memset(paddrin, 0, *addrlen);
        if (!GetInAddr(&paddrin->sin_addr))
            return false;
        paddrin->sin_family = AF_INET;
        paddrin->sin_port = htons(port);
        return true;
    }
    if (IsIPv6()) {
        if (*addrlen < (socklen_t)sizeof(struct sockaddr_in6))
            return false;
        *addrlen = sizeof(struct sockaddr_in6);
        struct sockaddr_in6 *paddrin6 = (struct sockaddr_in6*)paddr;
        memset(paddrin6, 0, *addrlen);
        if (!GetIn6Addr(&paddrin6->sin6_addr))
            return false;
        paddrin6->sin6_family = AF_INET6;
        paddrin6->sin6_port = htons(port);
        return true;
    }
    return false;
}

std::vector<unsigned char> CService::GetKey() const
{
     std::vector<unsigned char> vKey;

#ifdef ENABLE_I2PSAM
    if (IsNativeI2P())
    {
        assert( IsI2P() );
        vKey.resize(NATIVE_I2P_DESTINATION_SIZE);
        memcpy(&vKey[0], i2pDest, NATIVE_I2P_DESTINATION_SIZE);
        return vKey;
    }
#endif
     vKey.resize(18);
     memcpy(&vKey[0], ip, 16);
     vKey[16] = port / 0x100;
     vKey[17] = port & 0x0FF;
     return vKey;
}

std::string CService::ToStringPort() const
{
    return strprintf("%u", port);
}

std::string CService::ToStringIPPort() const
{
#ifdef ENABLE_I2PSAM
    if( IsI2P() ) return ToStringIP();                // Drop the port for i2p addresses
    std:string PortStr = ToStringPort();
    return ( IsIPv4() || IsTor() ) ? ToStringIP() + ":" + PortStr : "[" + ToStringIP() + "]:" + PortStr;
#else
    std:string PortStr = ToStringPort();
    return ( IsIPv4() || IsTor() ) ? ToStringIP() + ":" + PortStr : "[" + ToStringIP() + "]:" + PortStr;
#endif
}

std::string CService::ToString() const
{
    return ToStringIPPort();
}

void CService::print() const
{
    LogPrintf("CService(%s)\n", ToString());
}

void CService::SetPort(unsigned short portIn)
{
    port = portIn;
}

#ifdef WIN32
std::string NetworkErrorString(int err)
{
    char buf[256];
    buf[0] = 0;
    if(FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK,
            NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            buf, sizeof(buf), NULL))
    {
        return strprintf("%s (%d)", buf, err);
    }
    else
    {
        return strprintf("Unknown error (%d)", err);
    }
}
#else
std::string NetworkErrorString(int err)
{
    char buf[256];
    const char *s = buf;
    buf[0] = 0;
    /* Too bad there are two incompatible implementations of the
     * thread-safe strerror. */
#ifdef STRERROR_R_CHAR_P /* GNU variant can return a pointer outside the passed buffer */
    s = strerror_r(err, buf, sizeof(buf));
#else /* POSIX variant always returns message in buffer */
    (void) strerror_r(err, buf, sizeof(buf));
#endif
    return strprintf("%s (%d)", s, err);
}
#endif

void AddTimeData(const CNetAddr& ip, int64_t nTime)
{
    int64_t nOffsetSample = nTime - GetTime();

    LOCK(cs_nTimeOffset);
    // Ignore duplicates
    static set<CNetAddr> setKnown;
    if (!setKnown.insert(ip).second)
        return;

    // Add data
    static CMedianFilter<int64_t> vTimeOffsets(200,0);
    vTimeOffsets.input(nOffsetSample);
    LogPrintf("Added time data, samples %d, offset %+d (%+d minutes)\n", vTimeOffsets.size(), nOffsetSample, nOffsetSample/60);
    if (vTimeOffsets.size() >= 5 && vTimeOffsets.size() % 2 == 1)
    {
        int64_t nMedian = vTimeOffsets.median();
        std::vector<int64_t> vSorted = vTimeOffsets.sorted();
        // Only let other nodes change our time by so much
        if (abs64(nMedian) < 35 * 60)
        {
            nTimeOffset = nMedian;
        }
        else
        {
            nTimeOffset = 0;

            static bool fDone;
            if (!fDone)
            {
                // If nobody has a time different than ours but within 5 minutes of ours, give a warning
                bool fMatch = false;
                BOOST_FOREACH(int64_t nOffset, vSorted)
                    if (nOffset != 0 && abs64(nOffset) < 5 * 60)
                        fMatch = true;

                if (!fMatch)
                {
                    fDone = true;
                    string strMessage = _("Warning: Please check that your computer's date and time are correct! If your clock is wrong Anoncoin will not work properly.");
                    strMiscWarning = strMessage;
                    LogPrintf("*** %s\n", strMessage);
                    uiInterface.ThreadSafeMessageBox(strMessage, "", CClientUIInterface::MSG_WARNING);
                }
            }
        }
        if (fDebug) {
            BOOST_FOREACH(int64_t n, vSorted)
                LogPrintf("%+d  ", n);
            LogPrintf("|  ");
        }
        LogPrintf("nTimeOffset = %+d  (%+d minutes)\n", nTimeOffset, nTimeOffset/60);
    }
}
