/*!
 * $Id: //depot/SOURCE/OPENSOURCE/kfs/src/cc/meta/request.cc#3 $
 *
 * \file request.cc
 * \brief process queue of outstanding metadata requests
 * \author Blake Lewis and Sriram Rao (Kosmix Corp.)
 *
 * Copyright (C) 2006 Kosmix Corp.
 *
 * This file is part of Kosmos File System (KFS).
 *
 * KFS is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation under version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <map>
#include "kfstree.h"
#include "queue.h"
#include "request.h"
#include "logger.h"
#include "checkpoint.h"
#include "util.h"
#include "LayoutManager.h"

#include "libkfsIO/Globals.h"
using namespace libkfsio;

using std::map;
using std::string;
using std::istringstream;

using namespace KFS;

MetaQueue <MetaRequest> requestList;

typedef void (*ReqHandler)(MetaRequest *r);
map <MetaOp, ReqHandler> handler;

typedef int (*ParseHandler)(Properties &, MetaRequest **);

static int parseHandlerLookup(Properties &prop, MetaRequest **r);
static int parseHandlerLookupPath(Properties &prop, MetaRequest **r);
static int parseHandlerCreate(Properties &prop, MetaRequest **r);
static int parseHandlerRemove(Properties &prop, MetaRequest **r);
static int parseHandlerRename(Properties &prop, MetaRequest **r);
static int parseHandlerMkdir(Properties &prop, MetaRequest **r);
static int parseHandlerRmdir(Properties &prop, MetaRequest **r);
static int parseHandlerReaddir(Properties &prop, MetaRequest **r);
static int parseHandlerGetalloc(Properties &prop, MetaRequest **r);
static int parseHandlerGetlayout(Properties &prop, MetaRequest **r);
static int parseHandlerAllocate(Properties &prop, MetaRequest **r);
static int parseHandlerTruncate(Properties &prop, MetaRequest **r);

static int parseHandlerLeaseAcquire(Properties &prop, MetaRequest **r);
static int parseHandlerLeaseRenew(Properties &prop, MetaRequest **r);

static int parseHandlerHello(Properties &prop, MetaRequest **r);

static int parseHandlerPing(Properties &prop, MetaRequest **r);
static int parseHandlerStats(Properties &prop, MetaRequest **r);

/// command -> parsehandler map
typedef map<string, ParseHandler> ParseHandlerMap;
typedef map<string, ParseHandler>::iterator ParseHandlerMapIter;

// handlers for parsing
ParseHandlerMap gParseHandlers;

// mapping for the counters
typedef map<MetaOp, Counter *> OpCounterMap;
typedef map<MetaOp, Counter *>::iterator OpCounterMapIter;
OpCounterMap gCounters;

static bool
file_exists(fid_t fid)
{
	return metatree.getFattr(fid) != NULL;
}

static bool
is_dir(fid_t fid)
{
	MetaFattr *fa = metatree.getFattr(fid);
	return fa != NULL && fa->type == KFS_DIR;
}

static void
AddCounter(const char *name, MetaOp opName)
{
	Counter *c = new Counter(name);
	globals().counterManager.AddCounter(c);
	gCounters[opName] = c;
}

void
KFS::RegisterCounters()
{
	static int calledOnce = 0;
	if (calledOnce)
		return;
	calledOnce = 1;

	AddCounter("Get alloc", META_GETALLOC);
	AddCounter("Get layout", META_GETLAYOUT);
	AddCounter("Lookup", META_LOOKUP);
	AddCounter("Lookup Path", META_LOOKUP_PATH);
	AddCounter("Allocate", META_ALLOCATE);
	AddCounter("Truncate", META_TRUNCATE);
	AddCounter("Create", META_CREATE);
	AddCounter("Remove", META_REMOVE);
	AddCounter("Rename", META_RENAME);
	AddCounter("Mkdir", META_MKDIR);
	AddCounter("Rmdir", META_RMDIR);
	AddCounter("Lease Acquire", META_LEASE_ACQUIRE);
	AddCounter("Lease Renew", META_LEASE_RENEW);
	AddCounter("Lease Cleanup", META_LEASE_CLEANUP);
	AddCounter("Chunkserver Hello ", META_HELLO);
	AddCounter("Chunkserver Bye ", META_BYE);
	AddCounter("Replication Checker ", META_CHUNK_REPLICATION_CHECK);
	AddCounter("Replication Done ", META_CHUNK_REPLICATE);
}

static void
UpdateCounter(MetaOp opName)
{
	Counter *c;
	OpCounterMapIter iter;

	iter = gCounters.find(opName);
	if (iter == gCounters.end())
		return;
	c = iter->second;
	c->Update(1);
}

/*
 * Submit a request to change the increment used for bumping up chunk version #.
 * @param[in] r  The request that depends on chunk-version-increment being written
 * out to disk as part of completing the request processing.
 */
void
KFS::ChangeIncarnationNumber(MetaRequest *r)
{
	++chunkVersionInc;
	MetaChangeChunkVersionInc *ccvi = new MetaChangeChunkVersionInc(chunkVersionInc, r);

	submit_request(ccvi);
}

/*
 * Boilerplate code for specific request types.  Cast to the
 * appropriate type, call the corresponding KFS tree routine,
 * then use the callback to return the results.
 */
static void
handle_lookup(MetaRequest *r)
{
	MetaLookup *req = static_cast <MetaLookup *>(r);
	MetaFattr *fa = metatree.lookup(req->dir, req->name);
	req->status = (fa == NULL) ? -ENOENT : 0;
	if (fa != NULL)
		req->result = *fa;
}

static void
handle_lookup_path(MetaRequest *r)
{
	MetaLookupPath *req = static_cast <MetaLookupPath *>(r);
	MetaFattr *fa = metatree.lookupPath(req->root, req->path);
	req->status = (fa == NULL) ? -ENOENT : 0;
	if (fa != NULL)
		req->result = *fa;
}

static void
handle_create(MetaRequest *r)
{
	MetaCreate *req = static_cast <MetaCreate *>(r);
	fid_t fid = 0;
	req->status = metatree.create(req->dir, req->name, &fid,
					req->numReplicas);
	req->fid = fid;
}

static void
handle_mkdir(MetaRequest *r)
{
	MetaMkdir *req = static_cast <MetaMkdir *>(r);
	fid_t fid = 0;
	req->status = metatree.mkdir(req->dir, req->name, &fid);
	req->fid = fid;
}

/*!
 * \brief Remove a file in a directory.  Also, remove the chunks
 * associated with the file.  For removing chunks, we send off
 * RPCs to the appropriate chunkservers.
 */

static void
handle_remove(MetaRequest *r)
{
	MetaRemove *req = static_cast <MetaRemove *>(r);
	req->status = metatree.remove(req->dir, req->name);
}

static void
handle_rmdir(MetaRequest *r)
{
	MetaRmdir *req = static_cast <MetaRmdir *>(r);
	req->status = metatree.rmdir(req->dir, req->name);
}

static void
handle_readdir(MetaRequest *r)
{
	MetaReaddir *req = static_cast <MetaReaddir *>(r);
	if (!file_exists(req->dir))
		req->status = -ENOENT;
	else if (!is_dir(req->dir))
		req->status = -ENOTDIR;
	else
		req->status = metatree.readdir(req->dir, req->v);
}

class EnumerateLocations {
	vector <ServerLocation> &v;
public:
	EnumerateLocations(vector <ServerLocation> &result): v(result) { }
	void operator () (ChunkServerPtr c)
	{
		ServerLocation l = c->GetServerLocation();
		v.push_back(l);
	}
};

/*!
 * \brief Get the allocation information for a specific chunk in a file.
 */
static void
handle_getalloc(MetaRequest *r)
{
	MetaGetalloc *req = static_cast <MetaGetalloc *>(r);
	MetaChunkInfo *chunkInfo;
	vector<ChunkServerPtr> c;

	if (!file_exists(req->fid)) {
		COSMIX_LOG_DEBUG("handle_getalloc: no such file");
		req->status = -ENOENT;
		return;
	}

	req->status = metatree.getalloc(req->fid, req->offset, &chunkInfo);
	if (req->status != 0) {
		COSMIX_LOG_DEBUG(
			"handle_getalloc(%lld, %lld) = %d: kfsop failed",
			req->fid, req->offset, req->status);
		return;
	}

	req->chunkId = chunkInfo->chunkId;
	req->chunkVersion = chunkInfo->chunkVersion;
	if (gLayoutManager.GetChunkToServerMapping(req->chunkId, c) != 0) {
		COSMIX_LOG_DEBUG("handle_getalloc: no chunkservers");
		req->status = -ENOENT;
		return;
	}
	for_each(c.begin(), c.end(), EnumerateLocations(req->locations));
	req->status = 0;
}

/*!
 * \brief Get the allocation information for a file.  Determine
 * how many chunks there and where they are located.
 */
static void
handle_getlayout(MetaRequest *r)
{
	MetaGetlayout *req = static_cast <MetaGetlayout *>(r);
	vector<MetaChunkInfo*> chunkInfo;
	vector<ChunkServerPtr> c;

	if (!file_exists(req->fid)) {
		req->status = -ENOENT;
		return;
	}

	req->status = metatree.getalloc(req->fid, chunkInfo);
	if (req->status != 0)
		return;

	for (vector<ChunkLayoutInfo>::size_type i = 0; i < chunkInfo.size(); ++i) {
		ChunkLayoutInfo l;

		l.offset = chunkInfo[i]->offset;
		l.chunkId = chunkInfo[i]->chunkId;
		l.chunkVersion = chunkInfo[i]->chunkVersion;
		if (gLayoutManager.GetChunkToServerMapping(l.chunkId, c) != 0) {
			req->status = -EHOSTUNREACH;
			return;
		}
		for_each(c.begin(), c.end(), EnumerateLocations(l.locations));
		req->v.push_back(l);
	}
	req->status = 0;
}

class ChunkVersionChanger {
	fid_t fid;
	chunkId_t chunkId;
	seq_t chunkVers;
public:
	ChunkVersionChanger(fid_t f, chunkId_t c, seq_t v) :
		fid(f), chunkId(c), chunkVers(v) { }
	void operator() (ChunkServerPtr p) {
		p->NotifyChunkVersChange(fid, chunkId, chunkVers);
	}
};

/*!
 * \brief handle an allocation request for a chunk in a file.
 * \param[in] r		write allocation request
 *
 * Write allocation proceeds as follows:
 *  1. The client has sent a write allocation request which has been
 * parsed and turned into an RPC request (which is handled here).
 *  2. We first get a unique chunk identifier (after validating the
 * fileid).
 *  3. We send the request to the layout manager to pick a location
 * for the chunk.
 *  4. The layout manager picks a location and sends an RPC to the
 * corresponding chunk server to create the chunk.
 *  5. When the RPC is going on, processing for this request is
 * suspended.
 *  6. When the RPC reply is received, this request gets re-activated
 * and we come back to this function.
 *  7. Assuming that the chunk server returned a success,  we update
 * the metatree to link the chunkId with the fileid (from this
 * request).
 *  8. Processing for this request is now complete; it is logged and
 * a reply is sent back to the client.
 *
 * Versioning/Leases introduces a few wrinkles to the above steps:
 * In step #2, the metatree could return -EEXIST if an allocation
 * has been done for the <fid, offset>.  In such a case, we need to
 * check with the layout manager to see if a new lease is required.
 * If a new lease is required, the layout manager bumps up the version
 * # for the chunk and notifies the chunkservers.  The message has to
 * be suspended until the chunkservers ack.  After the message is
 * restarted, we need to update the metatree to reflect the newer
 * version # before notifying the client.
 *
 * On the other hand, if a new lease isn't required, then the layout
 * manager tells us where the data has been placed; the process for
 * the request is therefore complete.
 */
static void
handle_allocate(MetaRequest *r)
{
	MetaAllocate *req = static_cast<MetaAllocate *>(r);

	if (!req->layoutDone) {
		COSMIX_LOG_DEBUG("Starting layout for req:%lld", req->opSeqno);
		// force an allocation
		req->chunkId = 0;
		// start at step #2 above.
		req->status = metatree.allocateChunkId(
				req->fid, req->offset, &req->chunkId,
				&req->chunkVersion, &req->numReplicas);
		if ((req->status != 0) && (req->status != -EEXIST)) {
			// we have a problem
			return;
		}
		if (req->status == -EEXIST) {
			bool isNewLease = false;
			// Get a (new) lease if possible
			req->status = gLayoutManager.GetChunkWriteLease(req, isNewLease);
			if (req->status != 0) {
				// couln't get the lease...bail
				return;
			}
			if (!isNewLease) {
				COSMIX_LOG_DEBUG("Got valid lease for req:%lld",
						req->opSeqno);
				// we got a valid lease.  so, return
				return;
			}
			// new lease and chunkservers have been notified
			// so, wait for them to ack

		} else if (gLayoutManager.AllocateChunk(req) != 0) {
			// we have a problem
			req->status = -ENOSPC;
			return;
		}
		// we have queued an RPC to the chunkserver.  so, hold
		// off processing (step #5)
		req->suspended = true;
		return;
	}
	COSMIX_LOG_DEBUG("Layout is done for req:%lld", req->opSeqno);

	if (req->status != 0) {
		// we have a problem: it is possible that the server
		// went down.  ask the client to retry....
		req->status = -KFS::EALLOCFAILED;

		metatree.getChunkVersion(req->fid, req->chunkId,
					&req->chunkVersion);
		if (req->chunkVersion > 0) {
			// reset version #'s at the chunkservers
			for_each(req->servers.begin(), req->servers.end(),
				ChunkVersionChanger(req->fid, req->chunkId,
						req->chunkVersion));
		} else {
			// this is the first time the chunk was allocated.
			// since the allocation failed, remove existence of this chunk
			// on the metaserver.
			gLayoutManager.RemoveChunkToServerMapping(req->chunkId);
		}
		req->suspended = true;
		ChangeIncarnationNumber(req);
		return;
	}
	// layout is complete (step #6)
	req->suspended = false;

	// update the tree (step #7) and since we turned off the
	// suspend flag, the request will be logged and go on its
	// merry way.
	req->status = metatree.assignChunkId(req->fid, req->offset,
					req->chunkId, req->chunkVersion);
	if (req->status != 0)
		COSMIX_LOG_DEBUG("Assign chunk id failed...");
}

static void
handle_truncate(MetaRequest *r)
{
	MetaTruncate *req = static_cast <MetaTruncate *>(r);
	chunkOff_t allocOffset = 0;

	req->status = metatree.truncate(req->fid, req->offset, &allocOffset);
	if (req->status > 0) {
		// an allocation is needed
		MetaAllocate *alloc = new MetaAllocate(req->opSeqno, req->fid,
							allocOffset);

		COSMIX_LOG_DEBUG("Suspending truncation due to alloc at offset: %lld",
				allocOffset);

		// tie things together
		alloc->req = r;
		req->suspended = true;
		handle_allocate(alloc);
	}
}

static void
handle_rename(MetaRequest *r)
{
	MetaRename *req = static_cast <MetaRename *>(r);
	req->status = metatree.rename(req->dir, req->oldname, req->newname,
					req->overwrite);
}

static void
handle_checkpoint(MetaRequest *r)
{
	r->status = 0;
}

static void
handle_hello(MetaRequest *r)
{
	MetaHello *req = static_cast <MetaHello *>(r);

	gLayoutManager.AddNewServer(req);
	req->status = 0;
}

static void
handle_bye(MetaRequest *r)
{
	MetaBye *req = static_cast <MetaBye *>(r);

	gLayoutManager.ServerDown(req->server.get());
	req->status = 0;
}

static void
handle_lease_acquire(MetaRequest *r)
{
	MetaLeaseAcquire *req = static_cast <MetaLeaseAcquire *>(r);

	req->status = gLayoutManager.GetChunkReadLease(req);
}

static void
handle_lease_renew(MetaRequest *r)
{
	MetaLeaseRenew *req = static_cast <MetaLeaseRenew *>(r);

	req->status = gLayoutManager.LeaseRenew(req);
}

static void
handle_lease_cleanup(MetaRequest *r)
{
	MetaLeaseCleanup *req = static_cast <MetaLeaseCleanup *>(r);

	gLayoutManager.LeaseCleanup();
	// some leases are gone.  so, cleanup dumpster
	metatree.cleanupDumpster();
	req->status = 0;
}

static void
handle_chunk_replication_check(MetaRequest *r)
{
	MetaChunkReplicationCheck *req = static_cast <MetaChunkReplicationCheck *>(r);

	gLayoutManager.ChunkReplicationChecker();
	req->status = 0;
}

static void
handle_chunk_replication_done(MetaRequest *r)
{
	MetaChunkReplicate *req = static_cast <MetaChunkReplicate *>(r);

	gLayoutManager.ChunkReplicationDone(req);
}

static void
handle_change_chunkVersionInc(MetaRequest *r)
{
	r->status = 0;
}

static void
handle_ping(MetaRequest *r)
{
	MetaPing *req = static_cast <MetaPing *>(r);

	req->status = 0;

	gLayoutManager.Ping(req->servers);

}

static void
handle_stats(MetaRequest *r)
{
	MetaStats *req = static_cast <MetaStats *>(r);
	ostringstream os;

	req->status = 0;

	globals().counterManager.Show(os);
	req->stats = os.str();

}



/*
 * Map request types to the functions that handle them.
 */
static void
setup_handlers()
{
	handler[META_LOOKUP] = handle_lookup;
	handler[META_LOOKUP_PATH] = handle_lookup_path;
	handler[META_CREATE] = handle_create;
	handler[META_MKDIR] = handle_mkdir;
	handler[META_REMOVE] = handle_remove;
	handler[META_RMDIR] = handle_rmdir;
	handler[META_READDIR] = handle_readdir;
	handler[META_GETALLOC] = handle_getalloc;
	handler[META_GETLAYOUT] = handle_getlayout;
	handler[META_ALLOCATE] = handle_allocate;
	handler[META_TRUNCATE] = handle_truncate;
	handler[META_RENAME] = handle_rename;
	handler[META_CHECKPOINT] = handle_checkpoint;
	handler[META_CHUNK_REPLICATE] = handle_chunk_replication_done;
	handler[META_CHUNK_REPLICATION_CHECK] = handle_chunk_replication_check;
	// Chunk server -> Meta server op
	handler[META_HELLO] = handle_hello;
	handler[META_BYE] = handle_bye;

	// Lease related ops
	handler[META_LEASE_ACQUIRE] = handle_lease_acquire;
	handler[META_LEASE_RENEW] = handle_lease_renew;
	handler[META_LEASE_CLEANUP] = handle_lease_cleanup;
	handler[META_CHANGE_CHUNKVERSIONINC] = handle_change_chunkVersionInc;

	// Monitoring RPCs
	handler[META_PING] = handle_ping;
	handler[META_STATS] = handle_stats;

	gParseHandlers["LOOKUP"] = parseHandlerLookup;
	gParseHandlers["LOOKUP_PATH"] = parseHandlerLookupPath;
	gParseHandlers["CREATE"] = parseHandlerCreate;
	gParseHandlers["MKDIR"] = parseHandlerMkdir;
	gParseHandlers["REMOVE"] = parseHandlerRemove;
	gParseHandlers["RMDIR"] = parseHandlerRmdir;
	gParseHandlers["READDIR"] = parseHandlerReaddir;
	gParseHandlers["GETALLOC"] = parseHandlerGetalloc;
	gParseHandlers["GETLAYOUT"] = parseHandlerGetlayout;
	gParseHandlers["ALLOCATE"] = parseHandlerAllocate;
	gParseHandlers["TRUNCATE"] = parseHandlerTruncate;
	gParseHandlers["RENAME"] = parseHandlerRename;

	// Lease related ops
	gParseHandlers["LEASE_ACQUIRE"] = parseHandlerLeaseAcquire;
	gParseHandlers["LEASE_RENEW"] = parseHandlerLeaseRenew;

	// Meta server <-> Chunk server ops
	gParseHandlers["HELLO"] = parseHandlerHello;

	gParseHandlers["PING"] = parseHandlerPing;
	gParseHandlers["STATS"] = parseHandlerStats;
}

/*!
 * \brief request queue initialization
 */
void
KFS::initialize_request_handlers()
{
	setup_handlers();
}

/*!
 * \brief remove successive requests for the queue and carry them out.
 */
void
KFS::process_request()
{
	MetaRequest *r = requestList.dequeue();
	map <MetaOp, ReqHandler>::iterator h = handler.find(r->op);
	if (h == handler.end())
		r->status = -ENOSYS;
	else
		((*h).second)(r);

	if (!r->suspended) {
		UpdateCounter(r->op);
		oplog.add_pending(r);
	}
}

/*!
 * \brief add a new request to the queue
 * \param[in] r the request
 */
void
KFS::submit_request(MetaRequest *r)
{
	requestList.enqueue(r);
}

/*!
 * \brief print out the leaf nodes for debugging
 */
void
KFS::printleaves()
{
	metatree.printleaves();
}

/*!
 * \brief log lookup request (nop)
 */
int
MetaLookup::log(ofstream &file) const
{
	return 0;
}

/*!
 * \brief log lookup path request (nop)
 */
int
MetaLookupPath::log(ofstream &file) const
{
	return 0;
}

/*!
 * \brief log a file create
 */
int
MetaCreate::log(ofstream &file) const
{
	file << "create/dir/" << dir << "/name/" << name <<
		"/id/" << fid << "/numReplicas/" << (int) numReplicas << '\n';
	return file.fail() ? -EIO : 0;
}

/*!
 * \brief log a directory create
 */
int
MetaMkdir::log(ofstream &file) const
{
	file << "mkdir/dir/" << dir << "/name/" << name <<
		"/id/" << fid << '\n';
	return file.fail() ? -EIO : 0;
}

/*!
 * \brief log a file deletion
 */
int
MetaRemove::log(ofstream &file) const
{
	file << "remove/dir/" << dir << "/name/" << name << '\n';
	return file.fail() ? -EIO : 0;
}

/*!
 * \brief log a directory deletion
 */
int
MetaRmdir::log(ofstream &file) const
{
	file << "rmdir/dir/" << dir << "/name/" << name << '\n';
	return file.fail() ? -EIO : 0;
}

/*!
 * \brief log directory read (nop)
 */
int
MetaReaddir::log(ofstream &file) const
{
	return 0;
}

/*!
 * \brief log getalloc (nop)
 */
int
MetaGetalloc::log(ofstream &file) const
{
	return 0;
}

/*!
 * \brief log getlayout (nop)
 */
int
MetaGetlayout::log(ofstream &file) const
{
	return 0;
}

/*!
 * \brief log a chunk allocation
 */
int
MetaAllocate::log(ofstream &file) const
{
	file << "allocate/file/" << fid << "/offset/" << offset
	     << "/chunkId/" << chunkId
	     << "/chunkVersion/" << chunkVersion << '\n';
	return file.fail() ? -EIO : 0;
}

/*!
 * \brief log a file truncation
 */
int
MetaTruncate::log(ofstream &file) const
{
	file << "truncate/file/" << fid << "/offset/" << offset << '\n';
	return file.fail() ? -EIO : 0;
}

/*!
 * \brief log a rename
 */
int
MetaRename::log(ofstream &file) const
{
	file << "rename/dir/" << dir << "/old/" <<
		oldname << "/new/" << newname << '\n';
	return file.fail() ? -EIO : 0;
}

/*!
 * \brief Log a chunk-version-increment change to disk.
*/
int
MetaChangeChunkVersionInc::log(ofstream &file) const
{
	file << "chunkVersionInc/" << cvi << '\n';
	return file.fail() ? -EIO : 0;
}

/*!
 * \brief close log and begin checkpoint generation
 */
int
MetaCheckpoint::log(ofstream &file) const
{
	return oplog.finishLog();
}

/*!
 * \brief for a chunkserver hello, there is nothing to log
 */
int
MetaHello::log(ofstream &file) const
{
	return 0;
}

/*!
 * \brief for a chunkserver's death, there is nothing to log
 */
int
MetaBye::log(ofstream &file) const
{
	return 0;
}

/*!
 * \brief for a chunkserver allocate, there is nothing to log
 */
int
MetaChunkAllocate::log(ofstream &file) const
{
	return 0;
}

/*!
 * \brief log a chunk delete; (nop)
 */
int
MetaChunkDelete::log(ofstream &file) const
{
	return 0;
}

/*!
 * \brief log a chunk truncation; (nop)
 */
int
MetaChunkTruncate::log(ofstream &file) const
{
	return 0;
}

/*!
 * \brief log a heartbeat to a chunk server; (nop)
 */
int
MetaChunkHeartbeat::log(ofstream &file) const
{
	return 0;
}

/*!
 * \brief log a stale notify to a chunk server; (nop)
 */
int
MetaChunkStaleNotify::log(ofstream &file) const
{
	return 0;
}

/*!
 * \brief When notifying a chunkserver of a version # change, there is
 * nothing to log.
 */
int
MetaChunkVersChange::log(ofstream &file) const
{
	return 0;
}

/*!
 * \brief When asking a chunkserver to replicate a chunk, there is
 * nothing to log.
 */
int
MetaChunkReplicate::log(ofstream &file) const
{
	return 0;
}

/*!
 * \brief for a ping, there is nothing to log
 */
int
MetaPing::log(ofstream &file) const
{
	return 0;
}

/*!
 * \brief for a stats request, there is nothing to log
 */
int
MetaStats::log(ofstream &file) const
{
	return 0;
}

/*!
 * \brief for a lease acquire request, there is nothing to log
 */
int
MetaLeaseAcquire::log(ofstream &file) const
{
	return 0;
}

/*!
 * \brief for a lease renew request, there is nothing to log
 */
int
MetaLeaseRenew::log(ofstream &file) const
{
	return 0;
}

/*!
 * \brief for a lease cleanup request, there is nothing to log
 */
int
MetaLeaseCleanup::log(ofstream &file) const
{
	return 0;
}

/*!
 * \brief This is an internally generated op.  There is
 * nothing to log.
 */
int
MetaChunkReplicationCheck::log(ofstream &file) const
{
	return 0;
}

/*!
 * \brief parse a command sent by a client
 *
 * Commands are of the form:
 * <COMMAND NAME> \r\n
 * {header: value \r\n}+\r\n
 *
 * The general model in parsing the client command:
 * 1. Each command has its own parser
 * 2. Extract out the command name and find the parser for that
 * command
 * 3. Dump the header/value pairs into a properties object, so that we
 * can extract the header/value fields in any order.
 * 4. Finally, call the parser for the command sent by the client.
 *
 * @param[in] cmdBuf: buffer containing the request sent by the client
 * @param[in] cmdLen: length of cmdBuf
 * @param[out] res: A piece of memory allocated by calling new that
 * contains the data for the request.  It is the caller's
 * responsibility to delete the memory returned in res.
 * @retval 0 on success;  -1 if there is an error
 */
int
KFS::ParseCommand(char *cmdBuf, int cmdLen, MetaRequest **res)
{
	const char *delims = " \r\n";
	// header/value pairs are separated by a :
	const char separator = ':';
	string cmdStr;
	string::size_type cmdEnd;
	Properties prop;
	istringstream ist(cmdBuf);
	ParseHandlerMapIter entry;
	ParseHandler handler;

	// get the first line and find the command name
	ist >> cmdStr;
	// trim the command
	cmdEnd = cmdStr.find_first_of(delims);
	if (cmdEnd != cmdStr.npos) {
		cmdStr.erase(cmdEnd);
	}

	// find the parse handler and parse the thing
	entry = gParseHandlers.find(cmdStr);
	if (entry == gParseHandlers.end())
		return -1;
	handler = entry->second;

	prop.loadProperties(ist, separator, false);

	return (*handler)(prop, res);
}

/*!
 * \brief Various parse handlers.  All of them follow the same model:
 * @param[in] prop: A properties table filled with values sent by the client
 * @param[out] r: If parse is successful, returns a dynamically
 * allocated meta request object. It is the callers responsibility to get rid
 * of this pointer.
 * @retval 0 if parse is successful; -1 otherwise.
 *
 * XXX: Need to make MetaRequest a smart pointer
 */

static int
parseHandlerLookup(Properties &prop, MetaRequest **r)
{
	fid_t dir;
	const char *name;
	seq_t seq;

	seq = prop.getValue("Cseq", (seq_t) -1);
	dir = prop.getValue("Parent File-handle", (fid_t) -1);
	if (dir < 0)
		return -1;
	name = prop.getValue("Filename", (const char*) NULL);
	if (name == NULL)
		return -1;
	*r = new MetaLookup(seq, dir, name);
	return 0;
}

static int
parseHandlerLookupPath(Properties &prop, MetaRequest **r)
{
	fid_t root;
	const char *path;
	seq_t seq;

	seq = prop.getValue("Cseq", (seq_t) -1);
	root = prop.getValue("Root File-handle", (fid_t) -1);
	if (root < 0)
		return -1;
	path = prop.getValue("Pathname", (const char *) NULL);
	if (path == NULL)
		return -1;
	*r = new MetaLookupPath(seq, root, path);
	return 0;
}

static int
parseHandlerCreate(Properties &prop, MetaRequest **r)
{
	fid_t dir;
	const char *name;
	seq_t seq;
	int16_t numReplicas;

	seq = prop.getValue("Cseq", (seq_t) -1);
	dir = prop.getValue("Parent File-handle", (fid_t) -1);
	if (dir < 0)
		return -1;
	name = prop.getValue("Filename", (const char *) NULL);
	if (name == NULL)
		return -1;
	numReplicas = prop.getValue("Num-replicas", 1);
	if (numReplicas == 0)
		return -1;
	// cap replication at 3
	if (numReplicas > NUM_REPLICAS_PER_FILE)
		numReplicas = NUM_REPLICAS_PER_FILE;

	*r = new MetaCreate(seq, dir, name, numReplicas);
	return 0;
}

static int
parseHandlerRemove(Properties &prop, MetaRequest **r)
{
	fid_t dir;
	const char *name;
	seq_t seq;

	seq = prop.getValue("Cseq", (seq_t) -1);
	dir = prop.getValue("Parent File-handle", (fid_t) -1);
	if (dir < 0)
		return -1;
	name = prop.getValue("Filename", (const char *) NULL);
	if (name == NULL)
		return -1;
	*r = new MetaRemove(seq, dir, name);
	return 0;
}

static int
parseHandlerMkdir(Properties &prop, MetaRequest **r)
{
	fid_t dir;
	const char *name;
	seq_t seq;

	seq = prop.getValue("Cseq", (seq_t) -1);
	dir = prop.getValue("Parent File-handle", (fid_t) -1);
	if (dir < 0)
		return -1;
	name = prop.getValue("Directory", (const char *) NULL);
	if (name == NULL)
		return -1;
	*r = new MetaMkdir(seq, dir, name);
	return 0;
}

static int
parseHandlerRmdir(Properties &prop, MetaRequest **r)
{
	fid_t dir;
	const char *name;
	seq_t seq;

	seq = prop.getValue("Cseq", (seq_t) -1);
	dir = prop.getValue("Parent File-handle", (fid_t) -1);
	if (dir < 0)
		return -1;
	name = prop.getValue("Directory", (const char *) NULL);
	if (name == NULL)
		return -1;
	*r = new MetaRmdir(seq, dir, name);
	return 0;
}

static int
parseHandlerReaddir(Properties &prop, MetaRequest **r)
{
	fid_t dir;
	seq_t seq;

	seq = prop.getValue("Cseq", (seq_t) -1);
	dir = prop.getValue("Directory File-handle", (fid_t) -1);
	if (dir < 0)
		return -1;
	*r = new MetaReaddir(seq, dir);
	return 0;
}

static int
parseHandlerGetalloc(Properties &prop, MetaRequest **r)
{
	fid_t fid;
	seq_t seq;
	chunkOff_t offset;

	seq = prop.getValue("Cseq", (seq_t) -1);
	fid = prop.getValue("File-handle", (fid_t) -1);
	offset = prop.getValue("Chunk-offset", (chunkOff_t) -1);
	if ((fid < 0) || (offset < 0))
		return -1;
	*r = new MetaGetalloc(seq, fid, offset);
	return 0;
}

static int
parseHandlerGetlayout(Properties &prop, MetaRequest **r)
{
	fid_t fid;
	seq_t seq;

	seq = prop.getValue("Cseq", (seq_t) -1);
	fid = prop.getValue("File-handle", (fid_t) -1);
	if (fid < 0)
		return -1;
	*r = new MetaGetlayout(seq, fid);
	return 0;
}

static int
parseHandlerAllocate(Properties &prop, MetaRequest **r)
{
	fid_t fid;
	seq_t seq;
	chunkOff_t offset;

	seq = prop.getValue("Cseq", (seq_t) -1);
	fid = prop.getValue("File-handle", (fid_t) -1);
	offset = prop.getValue("Chunk-offset", (chunkOff_t) -1);
	if ((fid < 0) || (offset < 0))
		return -1;
	*r = new MetaAllocate(seq, fid, offset);
	return 0;
}

static int
parseHandlerTruncate(Properties &prop, MetaRequest **r)
{
	fid_t fid;
	seq_t seq;
	chunkOff_t offset;

	seq = prop.getValue("Cseq", (seq_t) -1);
	fid = prop.getValue("File-handle", (fid_t) -1);
	offset = prop.getValue("Offset", (chunkOff_t) -1);
	if ((fid < 0) || (offset < 0))
		return -1;
	*r = new MetaTruncate(seq, fid, offset);
	return 0;
}

static int
parseHandlerRename(Properties &prop, MetaRequest **r)
{
	fid_t fid;
	seq_t seq;
	const char *oldname;
	const char *newpath;
	bool overwrite;

	seq = prop.getValue("Cseq", (seq_t) -1);
	fid = prop.getValue("Parent File-handle", (fid_t) -1);
	oldname = prop.getValue("Old-name", (const char *) NULL);
	newpath = prop.getValue("New-path", (const char *) NULL);
	overwrite = (prop.getValue("Overwrite", 0)) == 1;
	if ((fid < 0) || (oldname == NULL) || (newpath == NULL))
		return -1;

	*r = new MetaRename(seq, fid, oldname, newpath, overwrite);
	return 0;
}

/*!
 * \brief Parse out the headers from a HELLO message.  The message
 * body contains the id's of the chunks hosted on the server.
 */
static int
parseHandlerHello(Properties &prop, MetaRequest **r)
{
	seq_t seq = prop.getValue("Cseq", (seq_t) -1);
	MetaHello *hello;

	hello = new MetaHello(seq);
	hello->location.hostname = prop.getValue("Chunk-server-name", "");
	hello->location.port = prop.getValue("Chunk-server-port", -1);
	if (!hello->location.IsValid()) {
		delete hello;
		return -1;
	}
	hello->totalSpace = prop.getValue("Total-space", (long long) 0);
	hello->usedSpace = prop.getValue("Used-space", (long long) 0);
	// # of chunks hosted on this server
	hello->numChunks = prop.getValue("Num-chunks", 0);
	// The chunk names follow in the body.  This field tracks
	// the length of the message body
	hello->contentLength = prop.getValue("Content-length", 0);

	*r = hello;
	return 0;
}

/*!
 * \brief Parse out the headers from a LEASE_ACQUIRE message.
 */
int
parseHandlerLeaseAcquire(Properties &prop, MetaRequest **r)
{
	seq_t seq = prop.getValue("Cseq", (seq_t) -1);
	chunkId_t chunkId = prop.getValue("Chunk-handle", (chunkId_t) -1);

	*r = new MetaLeaseAcquire(seq, chunkId);
	return 0;
}

/*!
 * \brief Parse out the headers from a LEASE_RENEW message.
 */
int
parseHandlerLeaseRenew(Properties &prop, MetaRequest **r)
{
	seq_t seq = prop.getValue("Cseq", (seq_t) -1);
	chunkId_t chunkId = prop.getValue("Chunk-handle", (chunkId_t) -1);
	int64_t leaseId = prop.getValue("Lease-id", (int64_t) -1);
	string leaseTypeStr = prop.getValue("Lease-type", "READ_LEASE");
	LeaseType leaseType;

	if (leaseTypeStr == "WRITE_LEASE")
		leaseType = WRITE_LEASE;
	else
		leaseType = READ_LEASE;

	*r = new MetaLeaseRenew(seq, leaseType, chunkId, leaseId);
	return 0;
}

/*!
 * \brief Parse out the headers from a PING message.
 */
int
parseHandlerPing(Properties &prop, MetaRequest **r)
{
	seq_t seq = prop.getValue("Cseq", (seq_t) -1);

	*r = new MetaPing(seq);
	return 0;
}

/*!
 * \brief Parse out the headers from a STATS message.
 */
int
parseHandlerStats(Properties &prop, MetaRequest **r)
{
	seq_t seq = prop.getValue("Cseq", (seq_t) -1);

	*r = new MetaStats(seq);
	return 0;
}

/*!
 * \brief Generate response (a string) for various requests that
 * describes the result of the request execution.  The generated
 * response string is based on the KFS protocol.  All follow the same
 * model:
 * @param[out] os: A string stream that contains the response.
 */
void
MetaLookup::response(ostringstream &os)
{
	static string fname[] = { "empty", "file", "dir" };

	os << "OK\r\n";
	os << "Cseq: " << opSeqno << "\r\n";
	os << "Status: " << status << "\r\n";
	if (status < 0) {
		os << "\r\n";
		return;
	}
	os << "File-handle: " << toString(result.id()) << "\r\n";
	os << "Type: " << fname[result.type] << "\r\n";
	os << "Chunk-count: " << toString(result.chunkcount) << "\r\n";
	sendtime(os, "M-Time:", result.mtime, "\r\n");
	sendtime(os, "C-Time:", result.ctime, "\r\n");
	sendtime(os, "CR-Time:", result.crtime, "\r\n\r\n");
}

void
MetaLookupPath::response(ostringstream &os)
{
	static string fname[] = { "empty", "file", "dir" };

	os << "OK\r\n";
	os << "Cseq: " << opSeqno << "\r\n";
	os << "Status: " << status << "\r\n";
	if (status < 0) {
		os << "\r\n";
		return;
	}
	os << "File-handle: " << toString(result.id()) << "\r\n";
	os << "Type: " << fname[result.type] << "\r\n";
	os << "Chunk-count: " << toString(result.chunkcount) << "\r\n";
	sendtime(os, "M-Time:", result.mtime, "\r\n");
	sendtime(os, "C-Time:", result.ctime, "\r\n");
	sendtime(os, "CR-Time:", result.crtime, "\r\n\r\n");
}

void
MetaCreate::response(ostringstream &os)
{
	os << "OK\r\n";
	os << "Cseq: " << opSeqno << "\r\n";
	os << "Status: " << status << "\r\n";
	if (status < 0) {
		os << "\r\n";
		return;
	}
	os << "File-handle: " << toString(fid) << "\r\n\r\n";
}

void
MetaRemove::response(ostringstream &os)
{
	os << "OK\r\n";
	os << "Cseq: " << opSeqno << "\r\n";
	os << "Status: " << status << "\r\n\r\n";
}

void
MetaMkdir::response(ostringstream &os)
{
	os << "OK\r\n";
	os << "Cseq: " << opSeqno << "\r\n";
	os << "Status: " << status << "\r\n";
	if (status < 0) {
		os << "\r\n";
		return;
	}
	os << "File-handle: " << toString(fid) << "\r\n\r\n";
}

void
MetaRmdir::response(ostringstream &os)
{
	os << "OK\r\n";
	os << "Cseq: " << opSeqno << "\r\n";
	os << "Status: " << status << "\r\n\r\n";
}

void
MetaReaddir::response(ostringstream &os)
{
	vector<MetaDentry *>::iterator iter;
	MetaDentry *d;
	string res;
	int numEntries = 0;

	os << "OK\r\n";
	os << "Cseq: " << opSeqno << "\r\n";
	os << "Status: " << status << "\r\n";
	if (status < 0) {
		os << "\r\n";
		return;
	}
	// Send over the names---1 name per line so it is easy to
	// extract it out
	// XXX This should include the file id also, and probably
	// the other NFS READDIR elements, namely a cookie and
	// eof indicator to support reading less than a whole
	// directory at a time.
	for (iter = v.begin(); iter != v.end(); ++iter) {
		d = *iter;
		// "/" doesn't have "/" as an entry in it.
		if ((dir == ROOTFID) && (d->getName() == "/"))
			continue;

		res = res + d->getName() + "\n";
		++numEntries;
	}
	os << "Num-Entries: " << numEntries << "\r\n";
	os << "Content-length: " << res.length() << "\r\n\r\n";
	if (res.length() > 0)
		os << res;
}

void
MetaRename::response(ostringstream &os)
{
	os << "OK\r\n";
	os << "Cseq: " << opSeqno << "\r\n";
	os << "Status: " << status << "\r\n\r\n";
}

class ListServerLocations {
	ostringstream &os;
public:
	ListServerLocations(ostringstream &out): os(out) { }
	void operator () (const ServerLocation &s)
	{
		os << " " <<  s.ToString();
	}
};


void
MetaGetalloc::response(ostringstream &os)
{
	os << "OK\r\n";
	os << "Cseq: " << opSeqno << "\r\n";
	os << "Status: " << status << "\r\n";
	if (status < 0) {
		os << "\r\n";
		return;
	}
	os << "Chunk-handle: " << chunkId << "\r\n";
	os << "Chunk-version: " << chunkVersion << "\r\n";
	os << "Num-replicas: " << locations.size() << "\r\n";

	assert(locations.size() > 0);

	os << "Replicas:";
	for_each(locations.begin(), locations.end(), ListServerLocations(os));
	os << "\r\n\r\n";
}

void
MetaGetlayout::response(ostringstream &os)
{
	vector<ChunkLayoutInfo>::iterator iter;
	ChunkLayoutInfo l;
	string res;

	os << "OK\r\n";
	os << "Cseq: " << opSeqno << "\r\n";
	os << "Status: " << status << "\r\n";
	if (status < 0) {
		os << "\r\n";
		return;
	}
	os << "Num-chunks: " << v.size() << "\r\n";
	// Send over the layout info
	for (iter = v.begin(); iter != v.end(); ++iter) {
		l = *iter;
		res = res + l.toString();
	}
	os << "Content-length: " << res.length() << "\r\n\r\n";

	if (res.length() > 0)
		os << res;
}

class PrintChunkServerLocations {
	ostringstream &os;
public:
	PrintChunkServerLocations(ostringstream &out): os(out) { }
	void operator () (ChunkServerPtr &s)
	{
		os << " " <<  s->ServerID();
	}
};

void
MetaAllocate::response(ostringstream &os)
{
	os << "OK\r\n";
	os << "Cseq: " << opSeqno << "\r\n";
	os << "Status: " << status << "\r\n";
	if (status < 0) {
		os << "\r\n";
		return;
	}
	os << "Chunk-handle: " << chunkId << "\r\n";
	os << "Chunk-version: " << chunkVersion << "\r\n";

	os << "Master: " << master->ServerID() << "\r\n";
	os << "Num-replicas: " << servers.size() << "\r\n";

	assert(servers.size() > 0);
	os << "Replicas:";
	for_each(servers.begin(), servers.end(), PrintChunkServerLocations(os));
	os << "\r\n\r\n";
}

void
MetaLeaseAcquire::response(ostringstream &os)
{
	os << "OK\r\n";
	os << "Cseq: " << opSeqno << "\r\n";
	os << "Status: " << status << "\r\n";
	if (status >= 0) {
		os << "Lease-id: " << leaseId << "\r\n";
	}
	os << "\r\n";
}

void
MetaLeaseRenew::response(ostringstream &os)
{
	os << "OK\r\n";
	os << "Cseq: " << opSeqno << "\r\n";
	os << "Status: " << status << "\r\n\r\n";
}

void
MetaTruncate::response(ostringstream &os)
{
	os << "OK\r\n";
	os << "Cseq: " << opSeqno << "\r\n";
	os << "Status: " << status << "\r\n\r\n";
}

void
MetaPing::response(ostringstream &os)
{
	os << "OK\r\n";
	os << "Cseq: " << opSeqno << "\r\n";
	os << "Status: " << status << "\r\n";
	os << "Servers: " << servers << "\r\n\r\n";
}

void
MetaStats::response(ostringstream &os)
{
	os << "OK\r\n";
	os << "Cseq: " << opSeqno << "\r\n";
	os << "Status: " << status << "\r\n";
	os << stats << "\r\n";
}

/*!
 * \brief Generate request (a string) that should be sent to the chunk
 * server.  The generated request string is based on the KFS
 * protocol.  All follow the same model:
 * @param[out] os: A string stream that contains the response.
 */
void
MetaChunkAllocate::request(ostringstream &os)
{
	MetaAllocate *allocOp = static_cast<MetaAllocate *>(req);
	assert(allocOp != NULL);

	os << "ALLOCATE \r\n";
	os << "Cseq: " << opSeqno << "\r\n";
	os << "Version: KFS/1.0\r\n";
	os << "File-handle: " << allocOp->fid << "\r\n";
	os << "Chunk-handle: " << allocOp->chunkId << "\r\n";
	os << "Chunk-version: " << allocOp->chunkVersion << "\r\n";
	if (leaseId >= 0) {
		os << "Lease-id: " << leaseId << "\r\n";
	}

	os << "Num-servers: " << allocOp->servers.size() << "\r\n";
	assert(allocOp->servers.size() > 0);

	os << "Servers:";
	for_each(allocOp->servers.begin(), allocOp->servers.end(),
			PrintChunkServerLocations(os));
	os << "\r\n\r\n";
}

void
MetaChunkDelete::request(ostringstream &os)
{
	os << "DELETE \r\n";
	os << "Cseq: " << opSeqno << "\r\n";
	os << "Version: KFS/1.0\r\n";
	os << "Chunk-handle: " << chunkId << "\r\n\r\n";
}

void
MetaChunkTruncate::request(ostringstream &os)
{
	os << "TRUNCATE \r\n";
	os << "Cseq: " << opSeqno << "\r\n";
	os << "Version: KFS/1.0\r\n";
	os << "Chunk-handle: " << chunkId << "\r\n";
	os << "Chunk-size: " << chunkSize << "\r\n\r\n";
}

void
MetaChunkHeartbeat::request(ostringstream &os)
{
	os << "HEARTBEAT \r\n";
	os << "Cseq: " << opSeqno << "\r\n";
	os << "Version: KFS/1.0\r\n\r\n";
}

void
MetaChunkStaleNotify::request(ostringstream &os)
{
	string s;
	vector<chunkId_t>::size_type i;

	os << "STALE_CHUNKS \r\n";
	os << "Cseq: " << opSeqno << "\r\n";
	os << "Version: KFS/1.0\r\n";
	os << "Num-chunks: " << staleChunkIds.size() << "\r\n";
	for (i = 0; i < staleChunkIds.size(); ++i) {
		s += toString(staleChunkIds[i]);
		s += " ";
	}
	os << "Content-length: " << s.length() << "\r\n\r\n";
	os << s;
}

void
MetaChunkVersChange::request(ostringstream &os)
{
	os << "CHUNK_VERS_CHANGE \r\n";
	os << "Cseq: " << opSeqno << "\r\n";
	os << "Version: KFS/1.0\r\n";
	os << "File-handle: " << fid << "\r\n";
	os << "Chunk-handle: " << chunkId << "\r\n";
	os << "Chunk-version: " << chunkVersion << "\r\n\r\n";
}

void
MetaChunkReplicate::request(ostringstream &os)
{
	os << "REPLICATE \r\n";
	os << "Cseq: " << opSeqno << "\r\n";
	os << "Version: KFS/1.0\r\n";
	os << "File-handle: " << fid << "\r\n";
	os << "Chunk-handle: " << chunkId << "\r\n";
	os << "Chunk-version: " << chunkVersion << "\r\n";
	os << "Chunk-location: " << srcLocation.ToString() << "\r\n\r\n";
}
