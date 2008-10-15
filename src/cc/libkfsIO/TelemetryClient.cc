//---------------------------------------------------------- -*- Mode: C++ -*-
// $Id$
//
// Created 2008/09/14
// Author: Sriram Rao
//
// Copyright 2008 Quantcast Corporation.  All rights reserved.
//
// This file is part of Kosmos File System (KFS).
//
// Licensed under the Apache License, Version 2.0
// (the "License"); you may not use this file except in compliance with
// the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the License.
//
// \brief Telemetry client for KFS.  This sends out a packet to the
// server whenever an operation is slow; it also receives packets from
// the server that identifies the current set of slow nodes.
// 
//----------------------------------------------------------------------------

#include "TelemetryClient.h"
#include "telemetry/packet.h"

#include <cerrno>
#include <unistd.h>
#include <netdb.h>
#include <poll.h>

using std::vector;
using std::string;
using namespace KFS;

void TelemetryClient::Init(const struct ip_mreq &imreq, 
                           int multicastPort,
                           const std::string &srvIp, int srvPort)
{
    int status;
    struct sockaddr_in saddr;
    int one = 1;

    mServerIp = srvIp;
    mServerPort = srvPort;
    mLastNotification = time(0);

    memset(&saddr, 0, sizeof(struct sockaddr_in));

    // open a UDP socket
    mSock = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (mSock < 0 ) {
        perror("Error creating socket");
        mSock = -1;
        return;
    }

    if (setsockopt(mSock, SOL_SOCKET, SO_REUSEADDR, 
                   (char *) &one, sizeof(one)) < 0) {
        perror("Setsockopt: ");
    }

    fcntl(mSock, F_SETFL, O_NONBLOCK);

    saddr.sin_family = PF_INET;
    saddr.sin_port = htons(multicastPort);
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    status = bind(mSock, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in));

    if ( status < 0 ) {
        perror("Error binding socket to interface");
        mSock = -1;
        return;
    }

    // JOIN multicast group on default interface
    status = setsockopt(mSock, IPPROTO_IP, IP_ADD_MEMBERSHIP, 
                        (const void *)&imreq, sizeof(struct ip_mreq));

    if (status < 0) {
        perror("Unable to join multicast group for receving telemetry data");
    }

    char hostname[256];
    if (gethostname(hostname, 256)) {
        perror("gethostname: ");
    }

    // convert to IP address
    struct hostent *hent = gethostbyname(hostname);

    memcpy(&mAddr, hent->h_addr, hent->h_length);
}

TelemetryClient::~TelemetryClient()
{
    if (mSock < 0)
        return;
    close(mSock);
}

void
TelemetryClient::publish(double timetaken, string opname)
{
    publish(mAddr, timetaken, opname);
}

void
TelemetryClient::publish(struct in_addr &target, double timetaken, string opname)
{
    if (mSock < 0)
        return;

    TelemetryClntPacket_t tpkt(mAddr, target, timetaken, opname.c_str());

    socklen_t socklen = sizeof(struct sockaddr_in);
    struct sockaddr_in saddr;

    memset(&saddr, 0, sizeof(struct sockaddr_in));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(mServerPort);
    saddr.sin_addr.s_addr = inet_addr(mServerIp.c_str());

    sendto(mSock, &tpkt, sizeof(TelemetryClntPacket_t), 0,
           (struct sockaddr *)&saddr, socklen);
}

int
TelemetryClient::getNotification(vector<struct in_addr> &slowNodes)
{
    if (mSock < 0)
        return 0;

    int status;
    time_t now = time(0);
    TelemetryServerPacket_t tpkt;
    struct sockaddr_in saddr;
    socklen_t socklen;

    struct pollfd pollfds;
    pollfds.fd = mSock;
    pollfds.events = POLLIN;
    pollfds.revents = 0;
    
    status = poll(&pollfds, 1, 0);

    // if we haven't heard anything for over 2 mins, clear state
    if (now - mLastNotification > 120) {
        slowNodes.clear();
    }
         
    if (pollfds.revents & POLLIN) {
        bool gotPacket = false;
        while (1) {
            // pull all the packets
            status = recvfrom(mSock, &tpkt, sizeof(TelemetryServerPacket_t), 0,
                              (struct sockaddr *) &saddr, (socklen_t *) &socklen);
            if (status < 0) {
                return gotPacket ? 0 : -EAGAIN;
            }
            gotPacket = true;
            slowNodes.clear();
            mLastNotification = now;
            for (int i = 0; i < tpkt.numNodes; i++)
                slowNodes.push_back(tpkt.slowNodes[i]);
        }
    }
    return 0;
}
