//---------------------------------------------------------- -*- Mode: C++ -*-
// $Id: //depot/SOURCE/OPENSOURCE/kfs/src/cc/libkfsIO/DiskConnection.h#5 $
//
// Created 2006/03/23
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

#ifndef _LIBIO_DISKCONNECTION_H
#define _LIBIO_DISKCONNECTION_H

#include <sys/types.h>
#include <aio.h>

#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>

// forward declaration
class DiskManager; 
class DiskConnection;

///
/// \typedef DiskConnectionPtr
/// DiskConnection is encapsulated in a smart pointer, so that when the
/// last reference is released, appropriate cleanup occurs.
///
typedef boost::shared_ptr<DiskConnection> DiskConnectionPtr;

#include "Chunk.h"
#include "KfsCallbackObj.h"
#include "IOBuffer.h"
#include "DiskEvent.h"

#include <deque>
using std::deque;

///
/// \file DiskConnection.h
/// \brief A disk connection is modeled after a network connection on
/// which I/O can be done.
///
/// A disk connection is owned by a KfsCallbackObj.  Whenever the
/// KfsCallbackObj needs to do disk I/O, it schedules the operation on
/// the disk connection.  The disk connection uses the disk manager
/// (@see DiskManager) to schedule the I/O.  The disk manager calls
/// the connection back when the operation completes.  The disk
/// connection in turn calls back the KfsCallbackObj with the result.
///

///
/// To allow pipelining of disk IO operations, particularly READ
/// requests---where a client can break-down the read requests into
/// multiple requests so that we can overlap disk/network
/// transfer---have a structure that tracks the status of individual
/// IO requests.  A DiskConnection keeps a queue of such outstanding
/// requests. 
///
struct DiskIORequest {
    
    DiskIORequest() : op(OP_NONE), offset(0), numBytes(0) { }
    DiskIORequest(DiskEventOp_t o, off_t f, size_t n) : 
        op(o), offset(f), numBytes(n) { }
    DiskEventOp_t op;  /// what is this request about
    off_t  offset;  /// offset from the chunk at which I/O should
                       /// be done
    size_t  numBytes; /// # of bytes in this request
    list<DiskEventPtr> diskEvents; /// disk events associated with
                                   /// this request.
    bool operator == (DiskIORequest &other) const {
        return ((offset == other.offset) && 
                (numBytes == other.numBytes));
    }
};

///
/// Disk Connection encapsulates an fd and some disk IO requests.  On
/// a given disk connection, you can do either a READ or a WRITE, but not
/// both.
///
class DiskConnection : 
    public boost::enable_shared_from_this<DiskConnection> {
public:
    // XXX: ChunkHandlePtr is for now.  Once we have a table of chunk
    // handles implemented in the chunk manager, this class will only
    // operate on an fd.
    DiskConnection(ChunkHandlePtr &handle, KfsCallbackObj *callbackObj);

    ~DiskConnection();

    /// Close the connection.  This will cause the events scheduled on
    /// this connection to be cancelled.
    void Close();

    ChunkHandlePtr &GetChunkHandle() { return mHandle; }

    /// Schedule a read on this connection at the specified offset for numBytes.
    /// @param[in] numBytes # of bytes that need to be read.
    /// @param[in] offset offset in the file at which to start reading data from.
    /// @retval # of bytes for which read was successfully scheduled;
    /// -1 if there was an error. 
    ssize_t Read(off_t offset, size_t numBytes);

    /// Completion handler for a read.
    int ReadDone(DiskEventPtr &doneEvent, int res);

    /// Schedule a write on this connection.  
    /// @param[in] numBytes # of bytes that need to be written
    /// @param[in] offset offset in the file at which to start writing data.
    /// @param[in] buf IOBuffer which contains data that should be written
    /// out to disk.
    /// @retval # of bytes for which write was successfully scheduled;
    /// -1 if there was an error. 
    ssize_t Write(off_t offset, size_t numBytes, IOBuffer *buf);

    /// Completion handler for a write.
    int WriteDone(DiskEventPtr &doneEvent, int res);

    /// Sync the previously written data to disk.
    int Sync();

    /// Completion handler for a sync.
    int SyncDone(DiskEventPtr &doneEvent, int res);

    /// Completion handler for a disk event.
    /// @param doneEvent Disk event that completed
    /// @param res Result of the event that completed
    ///
    int HandleDone(DiskEventPtr &doneEvent, int res) {
        if (doneEvent->op == OP_READ)
            return ReadDone(doneEvent, res);
        else if (doneEvent->op == OP_WRITE)
            return WriteDone(doneEvent, res);
        else
            return SyncDone(doneEvent, res);
    }

    // XXX: Need a way to build backpressure: if there are too many
    // I/O's outstanding, then throttle back...

private:
    /// Owning KfsCallbackObj.
    KfsCallbackObj	*mCallbackObj;
    /// XXX: This should become a file id eventually.
    ChunkHandlePtr	mHandle;

    /// Queue of disk IO requests that have been scheduled on this
    /// connection.  Whenever the I/O on the head of the queue is complete, the
    /// associated KfsCallbackObj is notified.
    deque<DiskIORequest>	mDiskIO;

};


#endif // _LIBIO_DISKCONNECTION_H
