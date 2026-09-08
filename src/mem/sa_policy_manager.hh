/*
 * Copyright (c) 2023 The Regents of the University of California
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * Set-Associative DRAM Cache Policy Manager (SAPolManager).
 *
 * This is the N-way set-associative variant of PolicyManager. The only
 * functional difference from the direct-mapped PolicyManager is in the
 * tag store organization and the address decomposition:
 *
 *   [ TAG | SET INDEX (log2(numSets) bits) | BLOCK OFFSET (log2(blockSize) bits) ]
 *
 * where:
 *   numLines = dramCacheSize / blockSize
 *   numSets  = numLines / numWays
 *
 * The tag store holds (numSets * numWays) metadata entries, laid out
 * way-major within each set: the entry for (set, way) lives at index
 * (set * numWays + way). On a lookup we search all numWays entries of a
 * set for a tag match (hit). On a miss we select a victim way: an invalid
 * way first, otherwise the least-recently-used (LRU) way.
 *
 * numWays == 1 makes this behave exactly like the direct-mapped PolicyManager.
 */

#ifndef __SA_POLICY_MANAGER_HH__
#define __SA_POLICY_MANAGER_HH__

#include <cstdint>
#include <queue>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "base/callback.hh"
#include "base/compiler.hh"
#include "base/logging.hh"
#include "base/statistics.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "enums/Policy.hh"
#include "mem/mem_ctrl.hh"
#include "mem/mem_interface.hh"
#include "mem/packet.hh"
#include "mem/qport.hh"
#include "mem/request.hh"
#include "params/SAPolManager.hh"
#include "sim/clocked_object.hh"
#include "sim/cur_tick.hh"
#include "sim/eventq.hh"
#include "sim/system.hh"

namespace gem5
{

namespace memory
{
class SAPolManager : public AbstractMemory
{
  protected:

    class RespPortPolManager : public QueuedResponsePort
    {
      private:

        RespPacketQueue queue;
        SAPolManager& polMan;

      public:

        RespPortPolManager(const std::string& name, SAPolManager& _polMan)
            : QueuedResponsePort(name, queue),
              queue(_polMan, *this, true),
              polMan(_polMan)
        { }

      protected:

        Tick recvAtomic(PacketPtr pkt) override
                            {return polMan.recvAtomic(pkt);}

        Tick recvAtomicBackdoor(PacketPtr pkt, MemBackdoorPtr &backdoor) override
                            {return polMan.recvAtomicBackdoor(pkt, backdoor);}

        void recvFunctional(PacketPtr pkt) override
                            {polMan.recvFunctional(pkt);}

        bool recvTimingReq(PacketPtr pkt) override;
                            //{return polMan.recvTimingReq(pkt);}

        AddrRangeList getAddrRanges() const override
                            {return polMan.getAddrRanges();}

    };

    class ReqPortPolManager : public RequestPort
    {
      public:

        ReqPortPolManager(const std::string& name, SAPolManager& _polMan,
                          bool _isLoc)
            : RequestPort(name), polMan(_polMan), isLoc(_isLoc)
        { }

      protected:

        void recvReqRetry();

        bool recvTimingResp(PacketPtr pkt);

      private:

        SAPolManager& polMan;
        bool isLoc;

    };

    RespPortPolManager port;
    ReqPortPolManager locReqPort;
    ReqPortPolManager farReqPort;

    unsigned locBurstSize;
    unsigned farBurstSize;

    enums::Policy locMemPolicy;

    /**
     * The following are basic design parameters of the unified
     * DRAM cache controller, and are initialized based on parameter values.
     * The rowsPerBank is determined based on the capacity, number of
     * ranks and banks, the burst size, and the row buffer size.
     */

    unsigned long long dramCacheSize;
    unsigned blockSize;
    unsigned addrSize;
    unsigned orbMaxSize;
    unsigned orbSize;
    unsigned crbMaxSize;
    unsigned crbSize;
    bool alwaysHit;
    bool alwaysDirty;

    bool bypassDcache;

    /**
     * Set-associativity parameters.
     * numWays : associativity (N ways per set).
     * numSets : number of sets = (dramCacheSize / blockSize) / numWays.
     */
    unsigned numWays;
    unsigned numSets;

    /**
     * Native DRAM Cache address mapping.
     *
     * ndcAddrMapping : select the NDC decomposition instead of the
     *                  conventional low-order-bits set index.
     * pageBytes      : OS page size, marks the PFN boundary.
     * intlvLowBits   : low bits above the block offset that stay in the
     *                  index because they select channel / bank group (PA[7:6]).
     *
     * The remaining widths are derived once in the constructor:
     *
     *   blockBits    = log2(blockSize)                        PA[5:0]
     *   intlvLowBits                                          PA[7:6] -> index
     *   tagLowBits   = pageBits - blockBits - intlvLowBits    PA[11:8] -> tag
     *   pfnIndexBits = indexBits - intlvLowBits               PFN -> index
     *   pageBits     = log2(pageBytes)
     *   indexBits    = log2(numSets)
     *
     * Every address bit lands in exactly one of {offset, index, tag}, so the
     * decomposition stays lossless and two distinct lines can never share an
     * (index, tag) pair.
     */
    bool ndcAddrMapping;
    unsigned pageBytes;
    unsigned intlvLowBits;

    /**
     * Address the cache device the way NDC does: a set is one DRAM row, its
     * numWays lines are the columns of that row. The local address is then
     * synthesized from (index, way) instead of being the requestor's address,
     * so accesses to different ways of a set become row-buffer hits.
     */
    bool ndcLocAddrMapping;

    unsigned blockBits;
    unsigned indexBits;
    unsigned pageBits;
    unsigned tagLowBits;
    unsigned pfnIndexBits;

    /**
     * Pipeline latency of the controller frontend. The frontend
     * contribution is added to writes (that complete when they are in
     * the write buffer) and reads that are serviced the write buffer.
     */
    const Tick frontendLatency;

    /**
     * Pipeline latency of the backend and PHY. Along with the
     * frontend contribution, this latency is added to reads serviced
     * by the memory.
     */
    const Tick backendLatency;

    Tick tRP;
    Tick tRCD_RD;
    Tick tRL;

    unsigned numColdMisses;
    float cacheWarmupRatio;
    bool resetStatsWarmup;

    Tick prevArrival;

    std::unordered_set<Addr> isInWriteQueue;

    /**
     * Carries the ORB key on local-memory packets. Once the local address is
     * synthesized from (index, way) it no longer equals the address the ORB
     * is keyed by, so the key has to travel with the packet and come back on
     * the response.
     */
    class LocMemKey : public Packet::SenderState
    {
      public:
        const Addr orbKey;
        explicit LocMemKey(Addr key) : orbKey(key) {}
    };

    struct tagMetaStoreEntry
    {
      // DRAM cache related metadata
      Addr tagDC;
      Addr indexDC;
      // constant to indicate that the cache line is valid
      bool validLine = false;
      // constant to indicate that the cache line is dirty
      bool dirtyLine = false;
      Addr farMemAddr;
      // Tick of the most recent access, used for LRU victim selection.
      Tick lastAccess = 0;
    };

    /** A storage to keep the tag and metadata for the
     * DRAM Cache entries.
     */
    std::vector<tagMetaStoreEntry> tagMetadataStore;

    /** Different states a packet can transition from one
     * to the other while it's process in the DRAM Cache
     * Controller.
     */
    enum reqState
    {
      start,
      locMemRead,  waitingLocMemReadResp,
      locMemWrite, waitingLocMemWriteResp,
      farMemRead,  waitingFarMemReadResp,
      farMemWrite, waitingFarMemWriteResp
    };

    /**
     * A class for the entries of the
     * outstanding request buffer (ORB).
     */
    class reqBufferEntry
    {
      public:

        bool validEntry;
        Tick arrivalTick;

        // DRAM cache related metadata
        Addr tagDC;
        Addr indexDC;
        int wayDC;

        // pointer to the outside world (ow) packet received from llc
        const PacketPtr owPkt;

        enums::Policy pol;
        reqState state;

        bool issued;
        bool isHit;
        bool conflict;

        Addr dirtyLineAddr;
        bool handleDirtyLine;

        // recording the tick when the req transitions into a new stats.
        // The subtract between each two consecutive states entrance ticks,
        // is the number of ticks the req spent in the proceeded state.
        // The subtract between entrance and issuance ticks for each state,
        // is the number of ticks for waiting time in that state.
        Tick locRdEntered;
        Tick locRdIssued;
        Tick locRdExit;
        Tick locWrEntered;
        Tick locWrIssued;
        Tick locWrExit;
        Tick farRdEntered;
        Tick farRdIssued;
        Tick farRdExit;

        reqBufferEntry(
          bool _validEntry, Tick _arrivalTick,
          Addr _tagDC, Addr _indexDC, int _wayDC,
          PacketPtr _owPkt,
          enums::Policy _pol, reqState _state,
          bool _issued, bool _isHit, bool _conflict,
          Addr _dirtyLineAddr, bool _handleDirtyLine,
          Tick _locRdEntered, Tick _locRdIssued, Tick _locRdExit,
          Tick _locWrEntered, Tick _locWrIssued, Tick _locWrExit,
          Tick _farRdEntered, Tick _farRdIssued, Tick _farRdExit)
        :
        validEntry(_validEntry), arrivalTick(_arrivalTick),
        tagDC(_tagDC), indexDC(_indexDC), wayDC(_wayDC),
        owPkt( _owPkt),
        pol(_pol), state(_state),
        issued(_issued), isHit(_isHit), conflict(_conflict),
        dirtyLineAddr(_dirtyLineAddr), handleDirtyLine(_handleDirtyLine),
        locRdEntered(_locRdEntered), locRdIssued(_locRdIssued), locRdExit(_locRdExit),
        locWrEntered(_locWrEntered), locWrIssued(_locWrIssued), locWrExit(_locWrExit),
        farRdEntered(_farRdEntered), farRdIssued(_farRdIssued), farRdExit(_farRdExit)
        { }
    };

    /**
     * This is the outstanding request buffer (ORB) data
     * structure, the main DS within the DRAM Cache
     * Controller. The key is the address, for each key
     * the map returns a reqBufferEntry which maintains
     * the entire info related to that address while it's
     * been processed in the DRAM Cache controller.
     */
    std::map<Addr,reqBufferEntry*> ORB;

    typedef std::pair<Tick, PacketPtr> timeReqPair;
    /**
     * This is the second important data structure
     * within the DRAM cache controller which holds
     * received packets that had conflict with some
     * other address(s) in the DRAM Cache that they
     * are still under process in the controller.
     * Once thoes addresses are finished processing,
     * Conflicting Requets Buffre (CRB) is consulted
     * to see if any packet can be moved into the
     * outstanding request buffer and start being
     * processed in the DRAM cache controller.
     */
    std::vector<timeReqPair> CRB;

    /**
     * This is a unified retry flag for both reads and writes.
     * It helps remember if we have to retry a request when available.
     */
    bool retryLLC;
    bool retryLLCFarMemWr;
    bool retryLocMemRead;
    bool retryFarMemRead;
    bool retryLocMemWrite;
    bool retryFarMemWrite;

    /**
     * Keep ownership of packets that complete without a timing response
     * (e.g., writebacks), matching the standard controller pattern.
     */
    std::unique_ptr<Packet> pendingDelete;

    /**
     * A queue for evicted dirty lines of DRAM cache,
     * to be written back to the backing memory.
     * These packets are not maintained in the ORB.
     */
    std::deque <timeReqPair> pktFarMemWrite;

    // Maintenance Queues
    std::deque <Addr> pktLocMemRead;
    std::deque <Addr> pktLocMemWrite;
    std::deque <Addr> pktFarMemRead;

    // Maintenance variables
    unsigned maxConf;

    AddrRangeList getAddrRanges();

    // events
    void processLocMemReadEvent();
    EventFunctionWrapper locMemReadEvent;

    void processLocMemWriteEvent();
    EventFunctionWrapper locMemWriteEvent;

    void processFarMemReadEvent();
    EventFunctionWrapper farMemReadEvent;

    void processFarMemWriteEvent();
    EventFunctionWrapper farMemWriteEvent;

    // management functions
    void setNextState(reqBufferEntry* orbEntry);
    void handleNextState(reqBufferEntry* orbEntry);
    void sendRespondToRequestor(PacketPtr pkt, Tick static_latency);
    void printQSizes() {}
    void handleRequestorPkt(PacketPtr pkt);
    void checkHitOrMiss(reqBufferEntry* orbEntry);
    void handleDirtyCacheLine(reqBufferEntry* orbEntry);
    bool checkConflictInDramCache(PacketPtr pkt);
    void checkConflictInCRB(reqBufferEntry* orbEntry);
    bool resumeConflictingReq(reqBufferEntry* orbEntry);
    void logStatsPolMan(reqBufferEntry* orbEntry);
    void accessAndRespond(PacketPtr pkt, Tick static_latency);
    PacketPtr getPacket(Addr addr, unsigned size, const MemCmd& cmd, Request::FlagsType flags = 0);
    Tick accessLatency();

    unsigned countLocRdInORB();
    unsigned countFarRdInORB();
    unsigned countLocWrInORB();
    unsigned countFarWr();

    Addr returnIndexDC(Addr pkt_addr, unsigned size);
    Addr returnTagDC(Addr pkt_addr, unsigned size);

    // Device address of a line: index -> row (via bank/rank/row), way ->
    // column within that row. Falls back to the requestor's address when the
    // NDC device mapping is disabled.
    Addr returnLocAddr(const reqBufferEntry* orbEntry);
    // ORB key of a local-memory response, from its LocMemKey sender state.
    Addr locRespKey(PacketPtr pkt);
    // Drop the sender state attached by returnLocAddr, if any.
    void freeLocKey(PacketPtr pkt);

    // Set-associative helpers.
    // Search every way of a set for a matching, valid tag.
    // Returns the matching way, or -1 on a miss.
    int findMatchingWay(Addr set, Addr tag);
    // Pick a victim way within a set: first invalid way, otherwise LRU.
    int findVictimWay(Addr set);

    /**
     * Admission test used when the NDC address mapping is enabled.
     *
     * The conventional test treats any in-flight request with a matching set
     * index as a conflict. That is the right rule for a direct-mapped cache,
     * but under the NDC mapping every block of a page shares a set, so it
     * would serialize a whole page and destroy memory-level parallelism.
     *
     * A set-associative cache only has to block when
     *   (a) the very same line is already being processed, or
     *   (b) the set already has numWays outstanding requests, in which case
     *       there is no way left to allocate a victim in without evicting a
     *       fill that is still in flight.
     *
     * `ignore` excludes an entry that is about to be retired.
     */
    bool ndcConflict(Addr addr, const reqBufferEntry* ignore = nullptr);

    // port management
    void locMemRecvReqRetry();
    void farMemRecvReqRetry();

    //void locMemRetryReq() {}
    //void farMemRetryReq() {}

    bool locMemRecvTimingResp(PacketPtr pkt);
    bool farMemRecvTimingResp(PacketPtr pkt);
    struct SAPolManagerStats : public statistics::Group
    {
      SAPolManagerStats(SAPolManager &polMan);

      void regStats() override;

      const SAPolManager &polMan;

      // All statistics that the model needs to capture
      statistics::Scalar readReqs;
      statistics::Scalar writeReqs;

      statistics::Scalar servicedByWrQ;
      statistics::Scalar mergedWrBursts;

      statistics::Scalar numRdRetry;
      statistics::Scalar numWrRetry;

      statistics::Vector readPktSize;
      statistics::Vector writePktSize;

      statistics::Scalar bytesReadWrQ;
      statistics::Scalar bytesReadSys;
      statistics::Scalar bytesWrittenSys;

      // Average bandwidth
      statistics::Formula avgRdBWSys;
      statistics::Formula avgWrBWSys;

      statistics::Scalar totGap;
      statistics::Formula avgGap;

      // DRAM Cache Specific Stats
      statistics::Average avgORBLen;
      statistics::Average avgLocRdQLenStrt;
      statistics::Average avgLocWrQLenStrt;
      statistics::Average avgFarRdQLenStrt;
      statistics::Average avgFarWrQLenStrt;

      statistics::Average avgLocRdQLenEnq;
      statistics::Average avgLocWrQLenEnq;
      statistics::Average avgFarRdQLenEnq;
      statistics::Average avgFarWrQLenEnq;

      statistics::Scalar numWrBacks;
      statistics::Scalar totNumConf;
      statistics::Scalar totNumORBFull;
      statistics::Scalar totNumCRBFull;

      statistics::Scalar maxNumConf;

      statistics::Scalar sentLocRdPort;
      statistics::Scalar sentLocWrPort;
      statistics::Scalar failedLocRdPort;
      statistics::Scalar failedLocWrPort;
      statistics::Scalar recvdRdPort;
      statistics::Scalar sentFarRdPort;
      statistics::Scalar sentFarWrPort;
      statistics::Scalar failedFarRdPort;
      statistics::Scalar failedFarWrPort;

      statistics::Scalar totPktsServiceTime;
      statistics::Scalar totPktsORBTime;
      statistics::Scalar totTimeFarRdtoSend;
      statistics::Scalar totTimeFarRdtoRecv;
      statistics::Scalar totTimeFarWrtoSend;
      statistics::Scalar totTimeInLocRead;
      statistics::Scalar totTimeInLocWrite;
      statistics::Scalar totTimeInFarRead;

      statistics::Scalar numTotHits;
      statistics::Scalar numTotMisses;
      statistics::Scalar numColdMisses;
      statistics::Scalar numHotMisses;
      statistics::Scalar numRdMissClean;
      statistics::Scalar numRdMissDirty;
      statistics::Scalar numRdHit;
      statistics::Scalar numWrMissClean;
      statistics::Scalar numWrMissDirty;
      statistics::Scalar numWrHit;
      statistics::Scalar numRdHitDirty;
      statistics::Scalar numRdHitClean;
      statistics::Scalar numWrHitDirty;
      statistics::Scalar numWrHitClean;

    };

    SAPolManagerStats polManStats;

  public:

    SAPolManager(const SAPolManagerParams &p);

    void init();

    Port &getPort(const std::string &if_name,
                  PortID idx=InvalidPortID);

    // For preparing for checkpoints
    DrainState drain() override;

    // Serializes the tag state so that we don't have to warm up each time.
    void serialize(CheckpointOut &cp) const override;
    void unserialize(CheckpointIn &cp) override;

    protected:

      Tick recvAtomic(PacketPtr pkt);
      Tick recvAtomicBackdoor(PacketPtr pkt, MemBackdoorPtr &backdoor);
      void recvFunctional(PacketPtr pkt);
      bool recvTimingReq(PacketPtr pkt);
};

} // namespace memory
} // namespace gem5

#endif //__SA_POLICY_MANAGER_HH__
