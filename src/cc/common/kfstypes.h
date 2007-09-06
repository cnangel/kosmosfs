//---------------------------------------------------------- -*- Mode: C++ -*-
// $Id: //depot/SOURCE/OPENSOURCE/kfs/src/cc/common/kfstypes.h#3 $
//
// \brief Common declarations for KFS (meta/chunk/client-lib)
//
// Created 2006/10/20
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
//----------------------------------------------------------------------------

#ifndef COMMON_KFSTYPES_H
#define COMMON_KFSTYPES_H

extern "C" {
#include <stdlib.h>
}
#include <cerrno>
#include <cassert>

namespace KFS {

typedef long long seq_t;        //!< request sequence no. for logging
typedef long long seqid_t;      //!< sequence number id's for file/chunks
typedef seqid_t fid_t;          //!< file ID
typedef seqid_t chunkId_t;      //!< chunk ID
typedef long long chunkOff_t;   //!< chunk offset
const fid_t ROOTFID = 2;        //!< special fid for "/

//!< Declarations as used in the Chunkserver/client-library
typedef int64_t kfsFileId_t;
typedef int64_t kfsChunkId_t;
typedef int64_t kfsSeq_t;

const size_t CHUNKSIZE = 1u << 26; //!< (64MB)
const int MAX_RPC_HEADER_LEN = 1024; //!< Max length of header in RPC req/response
const unsigned short int NUM_REPLICAS_PER_FILE = 3; //!< default degree of replication

//!< Default lease interval of 1 min
const int LEASE_INTERVAL_SECS = 60;

//!< Error codes for KFS specific errors
// version # being presented by client doesn't match what the server has
const int EBADVERS = 1000;

// lease has expired
const int ELEASEEXPIRED = 1001;

// checksum for data on a server is bad; client should read from elsewhere
const int EBADCKSUM = 1002;

// data lives on chunkservers that are all non-reachable
const int EDATAUNAVAIL = 1003;

// an error to indicate a server is busy and can't take on new work
const int ESERVERBUSY = 1004;

// an error occurring during allocation; the client will see this error
// code and retry. 
const int EALLOCFAILED = 1005;
}

#endif // COMMON_KFSTYPES_H
