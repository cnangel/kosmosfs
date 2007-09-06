//---------------------------------------------------------- -*- Mode: C++ -*-
// $Id: //depot/SOURCE/OPENSOURCE/kfs/src/cc/libkfsIO/TcpSocket.cc#3 $
//
// Created 2006/03/10
// Author: Sriram Rao (Kosmix Corp.) 
//
// Copyright (C) 2006 Kosmix Corp.
//
// This file is part of Kosmos File System (KFS).
//
// KFS is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by
// the Free Software Foundation under version 3 of the License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see
// <http://www.gnu.org/licenses/>.
//
// 
//----------------------------------------------------------------------------

#include "TcpSocket.h"
#include "common/log.h"

#include "Globals.h"
using namespace libkfsio;

#include <cerrno>
#include <netdb.h>

using std::min;

TcpSocket::~TcpSocket()
{
    Close();
}

int TcpSocket::Listen(int port)
{
    struct sockaddr_in	ourAddr;
    int reuseAddr = 1;

    mSockFd = socket(PF_INET, SOCK_STREAM, 0);
    if (mSockFd == -1) {
        perror("Socket: ");
        return -1;
    }

    memset(&ourAddr, 0, sizeof(struct sockaddr_in));
    ourAddr.sin_family = AF_INET;
    ourAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    ourAddr.sin_port = htons(port);

    /* 
     * A piece of magic here: Before we bind the fd to a port, setup
     * the fd to reuse the address. If we move this line to after the
     * bind call, then things don't work out.  That is, we bind the fd
     * to a port; we panic; on restart, bind will fail with the address
     * in use (i.e., need to wait 2MSL for TCP's time-wait).  By tagging
     * the fd to reuse an address, everything is happy.
     */
    if (setsockopt(mSockFd, SOL_SOCKET, SO_REUSEADDR, 
                   (char *) &reuseAddr, sizeof(reuseAddr)) < 0) {
        perror("Setsockopt: ");
    }

    if (bind(mSockFd, (struct sockaddr *) &ourAddr, sizeof(ourAddr)) < 0) {
        perror("Bind: ");
        close(mSockFd);
        mSockFd = -1;
        return -1;
    }
    
    if (listen(mSockFd, 5) < 0) {
        perror("listen: ");
    }

    globals().ctrOpenNetFds.Update(1);

    return 0;
    
}

TcpSocket* TcpSocket::Accept()
{
    int fd;
    struct sockaddr_in	cliAddr;    
    TcpSocket *accSock;
    socklen_t cliAddrLen = sizeof(cliAddr);

    if ((fd = accept(mSockFd, (struct sockaddr *) &cliAddr, &cliAddrLen)) < 0) {
        perror("Accept: ");
        return NULL;
    }
    accSock = new TcpSocket(fd);

    accSock->SetupSocket();

    globals().ctrOpenNetFds.Update(1);

    return accSock;
}

int TcpSocket::Connect(const struct sockaddr_in *remoteAddr)
{
    int res;

    Close();

    mSockFd = socket(PF_INET, SOCK_STREAM, 0);
    if (mSockFd == -1) {
        return -1;
    }

    res = connect(mSockFd, (struct sockaddr *) remoteAddr, sizeof(struct sockaddr_in));
    if (res < 0) {
        perror("Connect: ");
        close(mSockFd);
        mSockFd = -1;
        return -1;
    }

    SetupSocket();

    globals().ctrOpenNetFds.Update(1);

    return 0;
}

int TcpSocket::Connect(const ServerLocation &location)
{
    struct hostent *hostInfo;
    struct sockaddr_in remoteAddr;
    int res;

    hostInfo = gethostbyname(location.hostname.c_str());
    if (hostInfo == NULL) {
        return -1;
    }

    Close();

    mSockFd = socket(PF_INET, SOCK_STREAM, 0);
    if (mSockFd == -1) {
        return -1;
    }

    memcpy(&remoteAddr.sin_addr.s_addr, hostInfo->h_addr, sizeof(struct in_addr));
    remoteAddr.sin_port = htons(location.port);
    remoteAddr.sin_family = AF_INET;

    res = connect(mSockFd, (struct sockaddr *) &remoteAddr, sizeof(struct sockaddr_in));

    if (res < 0) {
        perror("Connect: ");
        close(mSockFd);
        mSockFd = -1;
        return -1;
    }

    SetupSocket();

    globals().ctrOpenNetFds.Update(1);

    return 0;
}

void TcpSocket::SetupSocket()
{
    int bufSize = 65536;

    // get big send/recv buffers and setup the socket for non-blocking I/O
    if (setsockopt(mSockFd, SOL_SOCKET, SO_SNDBUF, (char *) &bufSize, sizeof(bufSize)) < 0) {
        perror("Setsockopt: ");
    }
    if (setsockopt(mSockFd, SOL_SOCKET, SO_RCVBUF, (char *) &bufSize, sizeof(bufSize)) < 0) {
        perror("Setsockopt: ");
    }
    fcntl(mSockFd, F_SETFL, O_NONBLOCK);

}

int TcpSocket::GetRemoteName(struct sockaddr *remoteAddr, socklen_t *remoteLen)
{
    if (getpeername(mSockFd, remoteAddr, remoteLen) < 0) {
        perror("getpeername: ");
        return -1;
    }
    return 0;
}

int TcpSocket::Send(const char *buf, int bufLen)
{
    int nwrote;

    nwrote = send(mSockFd, buf, bufLen, 0);
    if (nwrote > 0) {
        globals().ctrNetBytesWritten.Update(nwrote);
    }
    return nwrote;
}

int TcpSocket::Recv(char *buf, int bufLen)
{
    int nread;

    nread = recv(mSockFd, buf, bufLen, 0);
    if (nread > 0) {
        globals().ctrNetBytesRead.Update(nread);
    }

    return nread;
}

int TcpSocket::Peek(char *buf, int bufLen)
{
    int nread;

    nread = recv(mSockFd, buf, bufLen, MSG_PEEK);
    return nread;
}

bool TcpSocket::IsGood()
{
    if (mSockFd < 0)
        return false;

    char c;
    
    // the socket could've been closed by the system because the peer
    // died.  so, tell if the socket is good, peek to see if any data
    // can be read; read returns 0 if the socket has been
    // closed. otherwise, will get -1 with errno=EAGAIN.
    
    if (Peek(&c, 1) == 0)
        return false;

    return true;
}


void TcpSocket::Close()
{
    if (mSockFd < 0) {
        return;
    }
    close(mSockFd);
    mSockFd = -1;
    globals().ctrOpenNetFds.Update(-1);
}

int TcpSocket::DoSynchSend(const char *buf, int bufLen)
{
    int numSent = 0;
    int res;

    while (numSent < bufLen) {
        res = Send(buf + numSent, bufLen - numSent);
        if (res == 0)
            return 0;
        if ((res < 0) && (errno == EAGAIN))
            continue;
        if (res < 0)
            break;
        numSent += res;
    }
    if (numSent > 0) {
        globals().ctrNetBytesWritten.Update(numSent);
    }
    return numSent;
}

// 
// Receive data within a certain amount of time.  If the server is too slow in responding, bail
//
int TcpSocket::DoSynchRecv(char *buf, int bufLen, struct timeval &timeout)
{
    int numRecd = 0;
    int res, nfds;
    fd_set fds;

    while (numRecd < bufLen) {
        FD_ZERO(&fds);
        FD_SET(mSockFd, &fds);
        nfds = select(mSockFd + 1, &fds, NULL, NULL, &timeout);
        if ((nfds == 0) && 
            ((timeout.tv_sec == 0) && (timeout.tv_usec == 0))) {
            COSMIX_LOG_DEBUG("Timeout in synch recv");
            return numRecd > 0 ? numRecd : -ETIMEDOUT;
        }

        res = Recv(buf + numRecd, bufLen - numRecd);
        if (res == 0)
            return 0;
        if ((res < 0) && (errno == EAGAIN))
            continue;
        if (res < 0)
            break;
        numRecd += res;
    }
    if (numRecd > 0) {
        globals().ctrNetBytesRead.Update(numRecd);
    }

    return numRecd;
}


// 
// Receive data within a certain amount of time and discard them.  If
// the server is too slow in responding, bail
//
int TcpSocket::DoSynchDiscard(int nbytes, struct timeval &timeout)
{
    int numRecd = 0, ntodo, res;
    const int bufSize = 4096;
    char buf[bufSize];

    while (numRecd < nbytes) {
        ntodo = min(nbytes - numRecd, bufSize);
        res = DoSynchRecv(buf, ntodo, timeout);
        if (res == -ETIMEDOUT)
            return numRecd;
        assert(numRecd >= 0);
        if (numRecd < 0)
            break;
        numRecd += res;
    }
    return numRecd;
}

// 
// Peek data within a certain amount of time.  If the server is too slow in responding, bail
//
int TcpSocket::DoSynchPeek(char *buf, int bufLen, struct timeval &timeout)
{
    int numRecd = 0;
    int res, nfds;
    fd_set fds;

    while (1) {
        FD_ZERO(&fds);
        FD_SET(mSockFd, &fds);
        nfds = select(mSockFd + 1, &fds, NULL, NULL, &timeout);
        if ((nfds == 0) && 
            ((timeout.tv_sec == 0) && (timeout.tv_usec == 0))) {
            return -ETIMEDOUT;
        }

        res = Peek(buf + numRecd, bufLen - numRecd);
        if (res == 0)
            return 0;
        if ((res < 0) && (errno == EAGAIN))
            continue;
        if (res < 0)
            break;
        numRecd += res;
        if (numRecd > 0)
            break;
    }
    return numRecd;
}
