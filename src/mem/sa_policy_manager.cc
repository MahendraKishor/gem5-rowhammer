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

#include "mem/sa_policy_manager.hh"

#include "base/trace.hh"
#include "debug/SAPolManager.hh"
#include "debug/Drain.hh"
#include "sim/sim_exit.hh"
#include "sim/stat_control.hh"
#include "sim/system.hh"

namespace gem5
{

namespace memory
{

SAPolManager::SAPolManager(const SAPolManagerParams &p):
    AbstractMemory(p),
    port(name() + ".port", *this),
    locReqPort(name() + ".loc_req_port", *this, true),
    farReqPort(name() + ".far_req_port", *this, false),
    locBurstSize(p.loc_burst_size),
    farBurstSize(p.far_burst_size),
    locMemPolicy(p.loc_mem_policy),
    dramCacheSize(p.dram_cache_size),
    blockSize(p.block_size),
    addrSize(p.addr_size),
    orbMaxSize(p.orb_max_size), orbSize(0),
    crbMaxSize(p.crb_max_size), crbSize(0),
    alwaysHit(p.always_hit), alwaysDirty(p.always_dirty),
    bypassDcache(p.bypass_dcache),
    numWays(p.num_ways), numSets(0),
    ndcAddrMapping(p.ndc_addr_mapping),
    pageBytes(p.page_size),
    intlvLowBits(p.intlv_low_bits),
    ndcLocAddrMapping(p.ndc_loc_addr_mapping),
    blockBits(0), indexBits(0), pageBits(0),
    tagLowBits(0), pfnIndexBits(0),
    frontendLatency(p.static_frontend_latency),
    backendLatency(p.static_backend_latency),
    tRP(p.tRP),
    tRCD_RD(p.tRCD_RD),
    tRL(p.tRL),
    numColdMisses(0),
    cacheWarmupRatio(p.cache_warmup_ratio),
    resetStatsWarmup(false),
    retryLLC(false), retryLLCFarMemWr(false),
    retryLocMemRead(false), retryFarMemRead(false),
    retryLocMemWrite(false), retryFarMemWrite(false),
    maxConf(0),
    locMemReadEvent([this]{ processLocMemReadEvent(); }, name()),
    locMemWriteEvent([this]{ processLocMemWriteEvent(); }, name()),
    farMemReadEvent([this]{ processFarMemReadEvent(); }, name()),
    farMemWriteEvent([this]{ processFarMemWriteEvent(); }, name()),
    polManStats(*this)
{
    panic_if(orbMaxSize<8, "ORB maximum size must be at least 8.\n");

    panic_if(numWays == 0, "num_ways must be at least 1.\n");

    panic_if(ndcLocAddrMapping && !ndcAddrMapping,
             "ndc_loc_addr_mapping requires ndc_addr_mapping.\n");

    unsigned long long numLines = dramCacheSize / blockSize;

    panic_if(numLines % numWays != 0,
             "Number of DRAM cache lines (%llu) must be divisible by "
             "num_ways (%u).\n", numLines, numWays);

    numSets = numLines / numWays;

    panic_if((numSets & (numSets - 1)) != 0,
             "Number of sets (%u) must be a power of two.\n", numSets);

    // Half the sets sit in each pseudo channel. That half has to stay a whole
    // multiple of the bank count so lifting the channel bit out cannot shift
    // the bank field; any power of two of at least 32 sets satisfies it.
    panic_if(ndcLocAddrMapping && numSets < 32,
             "ndc_loc_addr_mapping needs at least 32 sets, have %u.\n",
             numSets);

    tagMetadataStore.resize(numLines);

    blockBits = ceilLog2(blockSize);
    indexBits = ceilLog2(numSets);

    if (ndcAddrMapping) {
        panic_if(pageBytes == 0 || (pageBytes & (pageBytes - 1)) != 0,
                 "page_size (%u) must be a power of two.\n", pageBytes);

        pageBits = ceilLog2(pageBytes);

        panic_if(pageBits <= blockBits + intlvLowBits,
                 "The NDC mapping needs the page (%u B) to be larger than "
                 "block_size (%u B) times 2^intlv_low_bits (%u).\n",
                 pageBytes, blockSize, intlvLowBits);

        panic_if(indexBits < intlvLowBits,
                 "The NDC mapping needs at least intlv_low_bits (%u) index "
                 "bits, but the cache only has %u sets (%u index bits).\n",
                 intlvLowBits, numSets, indexBits);

        tagLowBits = pageBits - blockBits - intlvLowBits;
        pfnIndexBits = indexBits - intlvLowBits;

        panic_if(pageBits + pfnIndexBits > addrSize,
                 "The NDC mapping needs %u address bits (page %u + PFN index "
                 "%u) but addr_size is only %u.\n",
                 pageBits + pfnIndexBits, pageBits, pfnIndexBits, addrSize);

        // A page contributes 2^tagLowBits blocks to each of its sets, so
        // holding a whole page without a conflict miss needs exactly that
        // many ways (16 for a 4KiB page, 64B blocks and 2 interleaving bits).
        warn_if(ndcLocAddrMapping && numWays * blockSize != 1024,
                "NDC device mapping lays a set out as %u B, but the HBM row "
                "buffer is 1 KiB; a set will not be exactly one row.\n",
                numWays * blockSize);

        warn_if(numWays != (1u << tagLowBits),
                "NDC mapping maps %u blocks of a page onto each set but "
                "num_ways is %u; a page will not fit conflict-free.\n",
                1u << tagLowBits, numWays);
    }
}

Tick
SAPolManager::recvAtomic(PacketPtr pkt)
{
    if (!getAddrRange().contains(pkt->getAddr())) {
        panic("Can't handle address range for packet %s\n", pkt->print());
    }

    DPRINTF(SAPolManager, "recvAtomic: %s 0x%x\n",
                     pkt->cmdString(), pkt->getAddr());

    panic_if(pkt->cacheResponding(), "Should not see packets where cache "
             "is responding");

    // do the actual memory access and turn the packet into a response
    access(pkt);

    if (pkt->hasData()) {
        // this value is not supposed to be accurate, just enough to
        // keep things going, mimic a closed page
        // also this latency can't be 0
        // panic("Can't handle this process --> implement accessLatency() "
        //         "according to your interface. pkt: %s\n", pkt->print());
        return accessLatency();
    }

    return 0;
}

Tick
SAPolManager::recvAtomicBackdoor(PacketPtr pkt, MemBackdoorPtr &backdoor)
{
    Tick latency = recvAtomic(pkt);
    getBackdoor(backdoor);
    return latency;
}

void
SAPolManager::recvFunctional(PacketPtr pkt)
{
    bool found;

    if (getAddrRange().contains(pkt->getAddr())) {
        // rely on the abstract memory
        functionalAccess(pkt);
        found = true;
    } else {
        found = false;
    }

    panic_if(!found, "Can't handle address range for packet %s\n",
             pkt->print());
}

Tick
SAPolManager::accessLatency()
{
    // THIS IS FOR DRAM ONLY!
    return (tRP + tRCD_RD + tRL);
}

void
SAPolManager::init()
{
    if (!port.isConnected()) {
        fatal("Policy Manager %s is unconnected!\n", name());
    } else if (!locReqPort.isConnected()) {
        fatal("Policy Manager %s is unconnected!\n", name());
    } else if (!farReqPort.isConnected()) {
        fatal("Policy Manager %s is unconnected!\n", name());
    } else {
        port.sendRangeChange();
        //reqPort.recvRangeChange();
    }
}

bool
SAPolManager::recvTimingReq(PacketPtr pkt)
{
    if (bypassDcache) {
        return farReqPort.sendTimingReq(pkt);
    }
    // This is where we enter from the outside world
    DPRINTF(SAPolManager, "recvTimingReq: request %s addr 0x%x size %d\n",
            pkt->cmdString(), pkt->getAddr(), pkt->getSize());

    panic_if(pkt->cacheResponding(), "Should not see packets where cache "
             "is responding");

    panic_if(!(pkt->isRead() || pkt->isWrite()),
             "Should only see read and writes at memory controller\n");
    assert(pkt->getSize() != 0);

    // Calc avg gap between requests
    if (prevArrival != 0) {
        polManStats.totGap += curTick() - prevArrival;
    }
    prevArrival = curTick();

    // Find out how many memory packets a pkt translates to
    // If the burst size is equal or larger than the pkt size, then a pkt
    // translates to only one memory packet. Otherwise, a pkt translates to
    // multiple memory packets

    const Addr base_addr = pkt->getAddr();
    Addr addr = base_addr;
    uint32_t burst_size = locBurstSize;
    unsigned size = std::min((addr | (burst_size - 1)) + 1,
                    base_addr + pkt->getSize()) - addr;

    // check merging for writes
    if (pkt->isWrite()) {

        // polManStats.writePktSize[ceilLog2(size)]++;

        bool merged = isInWriteQueue.find((addr & ~(Addr(locBurstSize - 1)))) !=
            isInWriteQueue.end();

        if (merged) {

            polManStats.mergedWrBursts++;

            // farMemCtrl->accessInterface(pkt);

            // sendRespondToRequestor(pkt, frontendLatency);

            // return true;
        }
    }

    // check forwarding for reads
    bool foundInORB = false;
    bool foundInCRB = false;
    //bool foundInFarMemWrite = false;

    if (pkt->isRead()) {

        if (isInWriteQueue.find(pkt->getAddr()) != isInWriteQueue.end()) {

            if (!ORB.empty()) {
                for (const auto& e : ORB) {

                    // check if the read is subsumed in the write queue
                    // packet we are looking at
                    if (e.second->validEntry &&
                        e.second->owPkt->isWrite() &&
                        e.second->owPkt->getAddr() <= addr &&
                        ((addr + size) <=
                        (e.second->owPkt->getAddr() +
                        e.second->owPkt->getSize()))) {

                        foundInORB = true;

                        polManStats.servicedByWrQ++;

                        polManStats.bytesReadWrQ += burst_size;

                        break;
                    }
                }
            }

            if (!foundInORB && !CRB.empty()) {
                for (const auto& e : CRB) {

                    // check if the read is subsumed in the write queue
                    // packet we are looking at
                    if (e.second->isWrite() &&
                        e.second->getAddr() <= addr &&
                        ((addr + size) <=
                        (e.second->getAddr() + e.second->getSize()))) {

                        foundInCRB = true;

                        polManStats.servicedByWrQ++;

                        polManStats.bytesReadWrQ += burst_size;

                        break;
                    }
                }
            }

            if (!foundInORB && !foundInCRB && !pktFarMemWrite.empty()) {
                for (const auto& e : pktFarMemWrite) {
                    // check if the read is subsumed in the write queue
                    // packet we are looking at
                    if (e.second->getAddr() <= addr &&
                        ((addr + size) <=
                        (e.second->getAddr() +
                         e.second->getSize()))) {

                        // foundInFarMemWrite = true;

                        polManStats.servicedByWrQ++;

                        polManStats.bytesReadWrQ += burst_size;

                        break;
                    }
                }
            }
        }

        // if (foundInORB || foundInCRB || foundInFarMemWrite) {
        //     polManStats.readPktSize[ceilLog2(size)]++;

        //     farMemCtrl->accessInterface(pkt);

        //     sendRespondToRequestor(pkt, frontendLatency);

        //     return true;
        // }
    }

    // process conflicting requests.
    // conflicts are checked only based on Index of DRAM cache
    if (checkConflictInDramCache(pkt)) {

        polManStats.totNumConf++;

        if (CRB.size()>=crbMaxSize) {

            DPRINTF(SAPolManager, "CRBfull: %lld\n", pkt->getAddr());

            polManStats.totNumCRBFull++;

            retryLLC = true;

            if (pkt->isRead()) {
                polManStats.numRdRetry++;
            }
            else {
                polManStats.numWrRetry++;
            }
            return false;
        }

        CRB.push_back(std::make_pair(curTick(), pkt));

        if (pkt->isWrite()) {
            isInWriteQueue.insert(pkt->getAddr());
        }

        if (CRB.size() > maxConf) {
            maxConf = CRB.size();
            polManStats.maxNumConf = CRB.size();
        }
        return true;
    }
    // check if ORB or FMWB is full and set retry
    if (pktFarMemWrite.size() >= (orbMaxSize / 2)) {

        DPRINTF(SAPolManager, "FMWBfull: %lld\n", pkt->getAddr());

        retryLLCFarMemWr = true;

        if (pkt->isRead()) {
            polManStats.numRdRetry++;
        }
        else {
            polManStats.numWrRetry++;
        }
        return false;
    }

    if (ORB.size() >= orbMaxSize) {

        DPRINTF(SAPolManager, "ORBfull: addr %lld\n", pkt->getAddr());

        polManStats.totNumORBFull++;

        retryLLC = true;

        if (pkt->isRead()) {
            polManStats.numRdRetry++;
        }
        else {
            polManStats.numWrRetry++;
        }
        return false;
    }

    // if none of the above cases happens,
    // add ir to the ORB
    handleRequestorPkt(pkt);

    if (pkt->isWrite()) {
        isInWriteQueue.insert(pkt->getAddr());
    }

    // pktLocMemRead.push_back(pkt->getAddr());

    // polManStats.avgLocRdQLenEnq = pktLocMemRead.size();

    setNextState(ORB.at(pkt->getAddr()));

    handleNextState(ORB.at(pkt->getAddr()));

    DPRINTF(SAPolManager, "Policy manager accepted packet %lld\n", pkt->getAddr());

    return true;
}

void
SAPolManager::processLocMemReadEvent()
{
    // sanity check for the chosen packet
    auto orbEntry = ORB.at(pktLocMemRead.front());
    assert(orbEntry->validEntry);
    assert(orbEntry->state == locMemRead);
    assert(!orbEntry->issued);

    PacketPtr rdLocMemPkt = getPacket(returnLocAddr(orbEntry),
                                   blockSize,
                                   MemCmd::ReadReq);

    if (ndcLocAddrMapping) {
        rdLocMemPkt->senderState = new LocMemKey(pktLocMemRead.front());
    }

    if (locReqPort.sendTimingReq(rdLocMemPkt)) {
        DPRINTF(SAPolManager, "loc mem read is sent : %lld\n", rdLocMemPkt->getAddr());
        orbEntry->state = waitingLocMemReadResp;
        orbEntry->issued = true;
        orbEntry->locRdIssued = curTick();
        pktLocMemRead.pop_front();
        polManStats.sentLocRdPort++;
    } else {
        DPRINTF(SAPolManager, "loc mem read sending failed: %lld\n", rdLocMemPkt->getAddr());
        retryLocMemRead = true;
        freeLocKey(rdLocMemPkt);
        delete rdLocMemPkt;
        polManStats.failedLocRdPort++;
    }

    if (!pktLocMemRead.empty() && !locMemReadEvent.scheduled() && !retryLocMemRead) {
        schedule(locMemReadEvent, curTick()+1000);
    }
}

void
SAPolManager::processLocMemWriteEvent()
{
    // sanity check for the chosen packet
    auto orbEntry = ORB.at(pktLocMemWrite.front());
    assert(orbEntry->validEntry);
    assert(orbEntry->state == locMemWrite);
    assert(!orbEntry->issued);

    PacketPtr wrLocMemPkt = getPacket(returnLocAddr(orbEntry),
                                   blockSize,
                                   MemCmd::WriteReq);

    if (ndcLocAddrMapping) {
        wrLocMemPkt->senderState = new LocMemKey(pktLocMemWrite.front());
    }

    if (locReqPort.sendTimingReq(wrLocMemPkt)) {
        DPRINTF(SAPolManager, "loc mem write is sent : %lld\n", wrLocMemPkt->getAddr());
        orbEntry->state = waitingLocMemWriteResp;
        orbEntry->issued = true;
        orbEntry->locWrIssued = curTick();
        pktLocMemWrite.pop_front();
        polManStats.sentLocWrPort++;
    } else {
        DPRINTF(SAPolManager, "loc mem write sending failed: %lld\n", wrLocMemPkt->getAddr());
        retryLocMemWrite = true;
        freeLocKey(wrLocMemPkt);
        delete wrLocMemPkt;
        polManStats.failedLocWrPort++;
    }

    if (!pktLocMemWrite.empty() && !locMemWriteEvent.scheduled() && !retryLocMemWrite) {
        schedule(locMemWriteEvent, curTick()+1000);
    }
}

void
SAPolManager::processFarMemReadEvent()
{
    // sanity check for the chosen packet
    auto orbEntry = ORB.at(pktFarMemRead.front());
    assert(orbEntry->validEntry);
    assert(orbEntry->state == farMemRead);
    assert(!orbEntry->issued);

    PacketPtr rdFarMemPkt = getPacket(pktFarMemRead.front(),
                                      blockSize,
                                      MemCmd::ReadReq);

    if (farReqPort.sendTimingReq(rdFarMemPkt)) {
        DPRINTF(SAPolManager, "far mem read is sent : %lld\n", rdFarMemPkt->getAddr());
        orbEntry->state = waitingFarMemReadResp;
        orbEntry->issued = true;
        orbEntry->farRdIssued = curTick();
        pktFarMemRead.pop_front();
        polManStats.sentFarRdPort++;
    } else {
        DPRINTF(SAPolManager, "far mem read sending failed: %lld\n", rdFarMemPkt->getAddr());
        retryFarMemRead = true;
        delete rdFarMemPkt;
        polManStats.failedFarRdPort++;
    }

    if (!pktFarMemRead.empty() && !farMemReadEvent.scheduled() && !retryFarMemRead) {
        schedule(farMemReadEvent, curTick()+1000);
    }
}

void
SAPolManager::processFarMemWriteEvent()
{
    PacketPtr wrFarMemPkt = getPacket(pktFarMemWrite.front().second->getAddr(),
                                      blockSize,
                                      MemCmd::WriteReq);
    DPRINTF(SAPolManager, "FarMemWriteEvent: request %s addr %#x\n",
            wrFarMemPkt->cmdString(), wrFarMemPkt->getAddr());

    if (farReqPort.sendTimingReq(wrFarMemPkt)) {
        DPRINTF(SAPolManager, "far mem write is sent : %lld\n", wrFarMemPkt->getAddr());
        pktFarMemWrite.pop_front();
        polManStats.sentFarWrPort++;
    } else {
        DPRINTF(SAPolManager, "far mem write sending failed: %lld\n", wrFarMemPkt->getAddr());
        retryFarMemWrite = true;
        delete wrFarMemPkt;
        polManStats.failedFarWrPort++;
    }

    if (!pktFarMemWrite.empty() && !farMemWriteEvent.scheduled() && !retryFarMemWrite) {
        schedule(farMemWriteEvent, curTick()+1000);
    } else {
        if (drainState() == DrainState::Draining && pktFarMemWrite.empty() &&
            ORB.empty()) {
            DPRINTF(Drain, "SAPolManager done draining in farMemWrite\n");
            signalDrainDone();
        }
    }

    if (retryLLCFarMemWr && pktFarMemWrite.size()< (orbMaxSize / 2)) {

        DPRINTF(SAPolManager, "retryLLCFarMemWr sent\n");

        retryLLCFarMemWr = false;

        port.sendRetryReq();
    }
}

bool
SAPolManager::locMemRecvTimingResp(PacketPtr pkt)
{
    DPRINTF(SAPolManager, "locMemRecvTimingResp : %lld\n", pkt->getAddr());
    // Under the NDC device mapping the packet address is the (index, way)
    // device address, so the ORB key travels in the sender state instead.
    const Addr orbKey = locRespKey(pkt);
    auto orbEntry = ORB.at(orbKey);

    if (pkt->isRead()) {
        assert(orbEntry->state == waitingLocMemReadResp);

        if (orbEntry->handleDirtyLine &&
            (orbEntry->pol == enums::CascadeLakeNoPartWrs ||
            orbEntry->pol == enums::RambusHypo ||
            orbEntry->pol ==  enums::BearWriteOpt)
        ) {
            assert(!orbEntry->isHit);
            handleDirtyCacheLine(orbEntry);
        }
        orbEntry->locRdExit = curTick();
    }
    else {
        assert(pkt->isWrite());
        assert(orbEntry->state == waitingLocMemWriteResp);
        orbEntry->locWrExit = curTick();
    }

    // IMPORTANT:
    // orbEntry should not be used as the passed argument in setNextState and
    // handleNextState functions, reason: it's possible that orbEntry may be
    // deleted and updated, which will not be reflected here in the scope of
    // current lines since it's been read at line #508.
    setNextState(ORB.at(orbKey));

    handleNextState(ORB.at(orbKey));

    freeLocKey(pkt);

    delete pkt;

    return true;
}

bool
SAPolManager::farMemRecvTimingResp(PacketPtr pkt)
{
    if (bypassDcache) {
        port.schedTimingResp(pkt, curTick());
        return true;
    }

    DPRINTF(SAPolManager, "farMemRecvTimingResp : %lld , %s \n", pkt->getAddr(), pkt->cmdString());

    if (pkt->isRead()) {

        auto orbEntry = ORB.at(pkt->getAddr());

        DPRINTF(SAPolManager, "farMemRecvTimingResp : continuing to far read resp: %d\n",
        orbEntry->owPkt->isRead());

        assert(orbEntry->state == waitingFarMemReadResp);

        orbEntry->farRdExit = curTick();

        // IMPORTANT:
        // orbEntry should not be used as the passed argument in setNextState and
        // handleNextState functions, reason: it's possible that orbEntry may be
        // deleted and updated, which will not be reflected here in the scope of
        // current lines since it's been read at line #522.
        setNextState(ORB.at(pkt->getAddr()));

        // The next line is absolutely required since the orbEntry will
        // be deleted and renewed within setNextState()
        // orbEntry = ORB.at(pkt->getAddr());

        handleNextState(ORB.at(pkt->getAddr()));

        delete pkt;
    }
    else {
        assert(pkt->isWrite());
        delete pkt;
    }

    return true;
}

void
SAPolManager::locMemRecvReqRetry()
{
    // assert(retryLocMemRead || retryLocMemWrite);
    bool schedRd = false;
    bool schedWr = false;
    if (retryLocMemRead) {

        if (!locMemReadEvent.scheduled() && !pktLocMemRead.empty()) {
            schedule(locMemReadEvent, curTick());
        }
        retryLocMemRead = false;
        schedRd = true;
    }
    if (retryLocMemWrite) {
        if (!locMemWriteEvent.scheduled() && !pktLocMemWrite.empty()) {
            schedule(locMemWriteEvent, curTick());
        }
        retryLocMemWrite = false;
        schedWr = true;
    }
    if (!schedRd && !schedWr) {
            // panic("Wrong local mem retry event happend.\n");

            // TODO: there are cases where none of retryLocMemRead and retryLocMemWrite
            // are true, yet locMemRecvReqRetry() is called. I should fix this later.
            if (!locMemReadEvent.scheduled() && !pktLocMemRead.empty()) {
                schedule(locMemReadEvent, curTick());
            }
            if (!locMemWriteEvent.scheduled() && !pktLocMemWrite.empty()) {
                schedule(locMemWriteEvent, curTick());
            }
    }

    DPRINTF(SAPolManager, "locMemRecvReqRetry: %d , %d \n", schedRd, schedWr);
}

void
SAPolManager::farMemRecvReqRetry()
{
    if (bypassDcache) {
        port.sendRetryReq();
        return;
    }

    assert(retryFarMemRead || retryFarMemWrite);

    bool schedRd = false;
    bool schedWr = false;

    if (retryFarMemRead) {
        if (!farMemReadEvent.scheduled() && !pktFarMemRead.empty()) {
            schedule(farMemReadEvent, curTick());
        }
        retryFarMemRead = false;
        schedRd = true;
    }
    if (retryFarMemWrite) {
        if (!farMemWriteEvent.scheduled() && !pktFarMemWrite.empty()) {
            schedule(farMemWriteEvent, curTick());
        }
        retryFarMemWrite = false;
        schedWr = true;
    }
    // else {
    //     panic("Wrong far mem retry event happend.\n");
    // }

    DPRINTF(SAPolManager, "farMemRecvReqRetry: %d , %d \n", schedRd, schedWr);
}

void
SAPolManager::setNextState(reqBufferEntry* orbEntry)
{
    orbEntry->issued = false;
    enums::Policy pol = orbEntry->pol;
    reqState state = orbEntry->state;
    bool isRead = orbEntry->owPkt->isRead();
    bool isHit = orbEntry->isHit;
    bool isDirty = orbEntry->handleDirtyLine;

    // start --> read tag
    if (orbEntry->pol == enums::CascadeLakeNoPartWrs &&
        orbEntry->state == start) {
            orbEntry->state = locMemRead;
            orbEntry->locRdEntered = curTick();
            return;
    }

    // tag ready && read && hit --> DONE
    if (orbEntry->pol == enums::CascadeLakeNoPartWrs &&
        orbEntry->owPkt->isRead() &&
        orbEntry->state == waitingLocMemReadResp &&
        orbEntry->isHit) {
            // done
            // do nothing
            return;
    }

    // tag ready && write --> loc write
    if (orbEntry->pol == enums::CascadeLakeNoPartWrs &&
        orbEntry->owPkt->isWrite() &&
        orbEntry->state == waitingLocMemReadResp) {
            // write it to the DRAM cache
            orbEntry->state = locMemWrite;
            orbEntry->locWrEntered = curTick();
            return;
    }

    // loc read resp ready && read && miss --> far read
    if (orbEntry->pol == enums::CascadeLakeNoPartWrs &&
        orbEntry->owPkt->isRead() &&
        orbEntry->state == waitingLocMemReadResp &&
        !orbEntry->isHit) {

            orbEntry->state = farMemRead;
            orbEntry->farRdEntered = curTick();
            return;
    }

    // far read resp ready && read && miss --> loc write
    if (orbEntry->pol == enums::CascadeLakeNoPartWrs &&
        orbEntry->owPkt->isRead() &&
        orbEntry->state == waitingFarMemReadResp &&
        !orbEntry->isHit) {

            PacketPtr copyOwPkt = new Packet(orbEntry->owPkt,
                                             false,
                                             orbEntry->owPkt->isRead());

            accessAndRespond(orbEntry->owPkt,
                             frontendLatency + backendLatency + backendLatency);

            ORB.at(copyOwPkt->getAddr()) = new reqBufferEntry(
                                                orbEntry->validEntry,
                                                orbEntry->arrivalTick,
                                                orbEntry->tagDC,
                                                orbEntry->indexDC,
                                                orbEntry->wayDC,
                                                copyOwPkt,
                                                orbEntry->pol,
                                                orbEntry->state,
                                                orbEntry->issued,
                                                orbEntry->isHit,
                                                orbEntry->conflict,
                                                orbEntry->dirtyLineAddr,
                                                orbEntry->handleDirtyLine,
                                                orbEntry->locRdEntered,
                                                orbEntry->locRdIssued,
                                                orbEntry->locRdExit,
                                                orbEntry->locWrEntered,
                                                orbEntry->locWrIssued,
                                                orbEntry->locWrExit,
                                                orbEntry->farRdEntered,
                                                orbEntry->farRdIssued,
                                                orbEntry->farRdExit);
            delete orbEntry;

            orbEntry = ORB.at(copyOwPkt->getAddr());

            // if (orbEntry->handleDirtyLine) {
            //     handleDirtyCacheLine(orbEntry);
            // }
            orbEntry->state = locMemWrite;
            orbEntry->locWrEntered = curTick();
            return;
    }

    // loc write received
    if (orbEntry->pol == enums::CascadeLakeNoPartWrs &&
        // orbEntry->owPkt->isRead() &&
        // !orbEntry->isHit &&
        orbEntry->state == waitingLocMemWriteResp) {
            // done
            // do nothing
            return;
    }

    ////////////////////////////////////////////////////////////////////////
    /// RambusHypo

    // RD Hit Dirty & Clean, RD Miss Dirty, WR Miss Dirty
    // start --> read loc
    if (pol == enums::RambusHypo && state == start &&
        ((isRead && isHit) || (isRead && !isHit && isDirty) || (!isRead && !isHit && isDirty))
       ) {
            orbEntry->state = locMemRead;
            orbEntry->locRdEntered = curTick();
            return;
    }
    // RD Miss Clean
    // start --> read far
    if (pol == enums::RambusHypo && state == start &&
        (isRead && !isHit && !isDirty)
       ) {
            orbEntry->state = farMemRead;
            orbEntry->farRdEntered = curTick();
            return;
    }
    // WR Hit Dirty & Clean, WR Miss Clean
    // start --> write loc
    if (pol == enums::RambusHypo && state == start &&
        ((!isRead && isHit)|| (!isRead && !isHit && !isDirty))
       ) {
            orbEntry->state = locMemWrite;
            orbEntry->locWrEntered = curTick();
            return;
    }

    // RD Hit Dirty & Clean
    // start --> read loc --> done
    if (pol == enums::RambusHypo &&
        isRead && isHit &&
        state == waitingLocMemReadResp ) {
            // done
            // do nothing
            return;
    }

    // RD Miss Dirty:
    // start --> read loc --> read far
    if (pol == enums::RambusHypo &&
        isRead && !isHit && isDirty &&
        state == waitingLocMemReadResp ) {
            orbEntry->state = farMemRead;
            orbEntry->farRdEntered = curTick();
            return;
    }

    // WR Miss Dirty:
    // start --> read loc --> loc write
    if (pol == enums::RambusHypo &&
        !isRead && !isHit && isDirty &&
        state == waitingLocMemReadResp) {
            // write it to the DRAM cache
            orbEntry->state = locMemWrite;
            orbEntry->locWrEntered = curTick();
            return;
    }

    // RD Miss Clean & Dirty
    // start --> ... --> far read -> loc write
    if (pol == enums::RambusHypo &&
        (isRead && !isHit) &&
        state == waitingFarMemReadResp
       ) {
            PacketPtr copyOwPkt = new Packet(orbEntry->owPkt,
                                             false,
                                             orbEntry->owPkt->isRead());

            accessAndRespond(orbEntry->owPkt,
                             frontendLatency + backendLatency + backendLatency);

            ORB.at(copyOwPkt->getAddr()) = new reqBufferEntry(
                                                orbEntry->validEntry,
                                                orbEntry->arrivalTick,
                                                orbEntry->tagDC,
                                                orbEntry->indexDC,
                                                orbEntry->wayDC,
                                                copyOwPkt,
                                                orbEntry->pol,
                                                orbEntry->state,
                                                orbEntry->issued,
                                                orbEntry->isHit,
                                                orbEntry->conflict,
                                                orbEntry->dirtyLineAddr,
                                                orbEntry->handleDirtyLine,
                                                orbEntry->locRdEntered,
                                                orbEntry->locRdIssued,
                                                orbEntry->locRdExit,
                                                orbEntry->locWrEntered,
                                                orbEntry->locWrIssued,
                                                orbEntry->locWrExit,
                                                orbEntry->farRdEntered,
                                                orbEntry->farRdIssued,
                                                orbEntry->farRdExit);
            delete orbEntry;

            orbEntry = ORB.at(copyOwPkt->getAddr());
            orbEntry->state = locMemWrite;
            orbEntry->locWrEntered = curTick();
            return;
    }

    // loc write received
    if (pol == enums::RambusHypo &&
        state == waitingLocMemWriteResp) {
            assert (!(isRead && isHit));
            // done
            // do nothing
            return;
    }

    ////////////////////////////////////////////////////////////////////////
    // BEAR Write optimized
    if (orbEntry->pol == enums::BearWriteOpt &&
        orbEntry->state == start && !(orbEntry->owPkt->isWrite() && orbEntry->isHit)) {
            orbEntry->state = locMemRead;
            orbEntry->locRdEntered = curTick();
            DPRINTF(SAPolManager, "set: start -> locMemRead : %d\n", orbEntry->owPkt->getAddr());
            return;
    }

    if (orbEntry->pol == enums::BearWriteOpt &&
        orbEntry->state == start && orbEntry->owPkt->isWrite() && orbEntry->isHit) {
            orbEntry->state = locMemWrite;
            orbEntry->locRdEntered = curTick();
            DPRINTF(SAPolManager, "set: start -> locMemWrite : %d\n", orbEntry->owPkt->getAddr());
            return;
    }

    // tag ready && read && hit --> DONE
    if (orbEntry->pol == enums::BearWriteOpt &&
        orbEntry->owPkt->isRead() &&
        orbEntry->state == waitingLocMemReadResp &&
        orbEntry->isHit) {
            DPRINTF(SAPolManager, "set: waitingLocMemReadResp -> NONE : %d\n", orbEntry->owPkt->getAddr());

            // done
            // do nothing
            return;
    }

    // tag ready && write --> loc write
    if (orbEntry->pol == enums::BearWriteOpt &&
        orbEntry->owPkt->isWrite() &&
        orbEntry->state == waitingLocMemReadResp) {
            assert(!orbEntry->isHit);
            // write it to the DRAM cache
            orbEntry->state = locMemWrite;
            orbEntry->locWrEntered = curTick();
            DPRINTF(SAPolManager, "set: waitingLocMemReadResp -> locMemWrite : %d\n", orbEntry->owPkt->getAddr());
            return;
    }

    // loc read resp ready && read && miss --> far read
    if (orbEntry->pol == enums::BearWriteOpt &&
        orbEntry->owPkt->isRead() &&
        orbEntry->state == waitingLocMemReadResp &&
        !orbEntry->isHit) {

            orbEntry->state = farMemRead;
            orbEntry->farRdEntered = curTick();
            DPRINTF(SAPolManager, "set: waitingLocMemReadResp -> farMemRead : %d\n", orbEntry->owPkt->getAddr());
            return;
    }

    // far read resp ready && read && miss --> loc write
    if (orbEntry->pol == enums::BearWriteOpt &&
        orbEntry->owPkt->isRead() &&
        orbEntry->state == waitingFarMemReadResp &&
        !orbEntry->isHit) {

            PacketPtr copyOwPkt = new Packet(orbEntry->owPkt,
                                             false,
                                             orbEntry->owPkt->isRead());

            accessAndRespond(orbEntry->owPkt,
                             frontendLatency + backendLatency + backendLatency);

            ORB.at(copyOwPkt->getAddr()) = new reqBufferEntry(
                                                orbEntry->validEntry,
                                                orbEntry->arrivalTick,
                                                orbEntry->tagDC,
                                                orbEntry->indexDC,
                                                orbEntry->wayDC,
                                                copyOwPkt,
                                                orbEntry->pol,
                                                orbEntry->state,
                                                orbEntry->issued,
                                                orbEntry->isHit,
                                                orbEntry->conflict,
                                                orbEntry->dirtyLineAddr,
                                                orbEntry->handleDirtyLine,
                                                orbEntry->locRdEntered,
                                                orbEntry->locRdIssued,
                                                orbEntry->locRdExit,
                                                orbEntry->locWrEntered,
                                                orbEntry->locWrIssued,
                                                orbEntry->locWrExit,
                                                orbEntry->farRdEntered,
                                                orbEntry->farRdIssued,
                                                orbEntry->farRdExit);
            delete orbEntry;

            orbEntry = ORB.at(copyOwPkt->getAddr());

            // if (orbEntry->handleDirtyLine) {
            //     handleDirtyCacheLine(orbEntry);
            // }
            orbEntry->state = locMemWrite;
            orbEntry->locWrEntered = curTick();
            DPRINTF(SAPolManager, "set: waitingFarMemReadResp -> locMemWrite : %d\n", orbEntry->owPkt->getAddr());
            return;
    }

    // loc write received
    if (orbEntry->pol == enums::BearWriteOpt &&
        // orbEntry->owPkt->isRead() &&
        // !orbEntry->isHit &&
        orbEntry->state == waitingLocMemWriteResp) {
            DPRINTF(SAPolManager, "set: waitingLocMemWriteResp -> NONE : %d\n", orbEntry->owPkt->getAddr());

            // done
            // do nothing
            return;
    }
}

void
SAPolManager::handleNextState(reqBufferEntry* orbEntry)
{
    ////////////////////////////////////////////////////////////////////////
    // CascadeLakeNoPartWrs

    if (orbEntry->pol == enums::CascadeLakeNoPartWrs &&
        orbEntry->state == locMemRead) {

        // assert(!pktLocMemRead.empty());

        pktLocMemRead.push_back(orbEntry->owPkt->getAddr());

        polManStats.avgLocRdQLenEnq = pktLocMemRead.size();

        if (!locMemReadEvent.scheduled()) {
            schedule(locMemReadEvent, curTick());
        }
        return;
    }

    if (orbEntry->pol == enums::CascadeLakeNoPartWrs &&
        orbEntry->owPkt->isRead() &&
        orbEntry->state == waitingLocMemReadResp &&
        orbEntry->isHit) {
            // DONE
            // send the respond to the requestor

            PacketPtr copyOwPkt = new Packet(orbEntry->owPkt,
                                             false,
                                             orbEntry->owPkt->isRead());

            accessAndRespond(orbEntry->owPkt,
                             frontendLatency + backendLatency);

            ORB.at(copyOwPkt->getAddr()) = new reqBufferEntry(
                                                orbEntry->validEntry,
                                                orbEntry->arrivalTick,
                                                orbEntry->tagDC,
                                                orbEntry->indexDC,
                                                orbEntry->wayDC,
                                                copyOwPkt,
                                                orbEntry->pol,
                                                orbEntry->state,
                                                orbEntry->issued,
                                                orbEntry->isHit,
                                                orbEntry->conflict,
                                                orbEntry->dirtyLineAddr,
                                                orbEntry->handleDirtyLine,
                                                orbEntry->locRdEntered,
                                                orbEntry->locRdIssued,
                                                orbEntry->locRdExit,
                                                orbEntry->locWrEntered,
                                                orbEntry->locWrIssued,
                                                orbEntry->locWrExit,
                                                orbEntry->farRdEntered,
                                                orbEntry->farRdIssued,
                                                orbEntry->farRdExit);
            delete orbEntry;

            orbEntry = ORB.at(copyOwPkt->getAddr());

            // clear ORB
            resumeConflictingReq(orbEntry);

            return;
    }

    if (orbEntry->pol == enums::CascadeLakeNoPartWrs &&
        orbEntry->owPkt->isRead() &&
        orbEntry->state == farMemRead) {

            assert(!orbEntry->isHit);

            // do a read from far mem
            pktFarMemRead.push_back(orbEntry->owPkt->getAddr());

            polManStats.avgFarRdQLenEnq = pktFarMemRead.size();

            if (!farMemReadEvent.scheduled()) {
                schedule(farMemReadEvent, curTick());
            }
            return;

    }

    if (orbEntry->pol == enums::CascadeLakeNoPartWrs &&
        orbEntry->state == locMemWrite) {

            if (orbEntry->owPkt->isRead()) {
                assert(!orbEntry->isHit);
            }

            // do a read from far mem
            pktLocMemWrite.push_back(orbEntry->owPkt->getAddr());

            polManStats.avgLocWrQLenEnq = pktLocMemWrite.size();


            if (!locMemWriteEvent.scheduled()) {
                schedule(locMemWriteEvent, curTick());
            }
            return;

    }

    if (orbEntry->pol == enums::CascadeLakeNoPartWrs &&
        // orbEntry->owPkt->isRead() &&
        // !orbEntry->isHit &&
        orbEntry->state == waitingLocMemWriteResp) {
            // DONE
            // clear ORB
            resumeConflictingReq(orbEntry);

            return;
    }

    ////////////////////////////////////////////////////////////////////////
    // Rambus Hypo
    if (orbEntry->pol == enums::RambusHypo &&
        orbEntry->state == locMemRead) {

        pktLocMemRead.push_back(orbEntry->owPkt->getAddr());

        polManStats.avgLocRdQLenEnq = pktLocMemRead.size();

        if (!locMemReadEvent.scheduled()) {
            schedule(locMemReadEvent, curTick());
        }
        return;
    }

    if (orbEntry->pol == enums::RambusHypo &&
        orbEntry->owPkt->isRead() &&
        orbEntry->state == waitingLocMemReadResp &&
        orbEntry->isHit) {
            // DONE
            // send the respond to the requestor

            PacketPtr copyOwPkt = new Packet(orbEntry->owPkt,
                                             false,
                                             orbEntry->owPkt->isRead());

            accessAndRespond(orbEntry->owPkt,
                             frontendLatency + backendLatency);

            ORB.at(copyOwPkt->getAddr()) = new reqBufferEntry(
                                                orbEntry->validEntry,
                                                orbEntry->arrivalTick,
                                                orbEntry->tagDC,
                                                orbEntry->indexDC,
                                                orbEntry->wayDC,
                                                copyOwPkt,
                                                orbEntry->pol,
                                                orbEntry->state,
                                                orbEntry->issued,
                                                orbEntry->isHit,
                                                orbEntry->conflict,
                                                orbEntry->dirtyLineAddr,
                                                orbEntry->handleDirtyLine,
                                                orbEntry->locRdEntered,
                                                orbEntry->locRdIssued,
                                                orbEntry->locRdExit,
                                                orbEntry->locWrEntered,
                                                orbEntry->locWrIssued,
                                                orbEntry->locWrExit,
                                                orbEntry->farRdEntered,
                                                orbEntry->farRdIssued,
                                                orbEntry->farRdExit);
            delete orbEntry;

            orbEntry = ORB.at(copyOwPkt->getAddr());

            // clear ORB
            resumeConflictingReq(orbEntry);

            return;
    }

    if (orbEntry->pol == enums::RambusHypo &&
        orbEntry->state == farMemRead) {

            assert(orbEntry->owPkt->isRead() && !orbEntry->isHit);

            // do a read from far mem
            pktFarMemRead.push_back(orbEntry->owPkt->getAddr());

            polManStats.avgFarRdQLenEnq = pktFarMemRead.size();

            if (!farMemReadEvent.scheduled()) {
                schedule(farMemReadEvent, curTick());
            }
            return;

    }

    if (orbEntry->pol == enums::RambusHypo &&
        orbEntry->state == locMemWrite) {

            if (orbEntry->owPkt->isRead()) {
                assert(!orbEntry->isHit);
            }

            // do a read from far mem
            pktLocMemWrite.push_back(orbEntry->owPkt->getAddr());

            polManStats.avgLocWrQLenEnq = pktLocMemWrite.size();


            if (!locMemWriteEvent.scheduled()) {
                schedule(locMemWriteEvent, curTick());
            }
            return;

    }

    if (orbEntry->pol == enums::RambusHypo &&
        orbEntry->state == waitingLocMemWriteResp) {
            // DONE
            // clear ORB
            resumeConflictingReq(orbEntry);

            return;
    }

    ////////////////////////////////////////////////////////////////////////
    // BEAR Write Optmized
    if (orbEntry->pol == enums::BearWriteOpt &&
        orbEntry->state == locMemRead) {

        pktLocMemRead.push_back(orbEntry->owPkt->getAddr());

        polManStats.avgLocRdQLenEnq = pktLocMemRead.size();

        if (!locMemReadEvent.scheduled()) {
            schedule(locMemReadEvent, curTick());
        }
        return;
    }

    if (orbEntry->pol == enums::BearWriteOpt &&
        orbEntry->owPkt->isRead() &&
        orbEntry->state == waitingLocMemReadResp &&
        orbEntry->isHit) {
            // DONE
            // send the respond to the requestor

            PacketPtr copyOwPkt = new Packet(orbEntry->owPkt,
                                             false,
                                             orbEntry->owPkt->isRead());

            accessAndRespond(orbEntry->owPkt,
                             frontendLatency + backendLatency);

            ORB.at(copyOwPkt->getAddr()) = new reqBufferEntry(
                                                orbEntry->validEntry,
                                                orbEntry->arrivalTick,
                                                orbEntry->tagDC,
                                                orbEntry->indexDC,
                                                orbEntry->wayDC,
                                                copyOwPkt,
                                                orbEntry->pol,
                                                orbEntry->state,
                                                orbEntry->issued,
                                                orbEntry->isHit,
                                                orbEntry->conflict,
                                                orbEntry->dirtyLineAddr,
                                                orbEntry->handleDirtyLine,
                                                orbEntry->locRdEntered,
                                                orbEntry->locRdIssued,
                                                orbEntry->locRdExit,
                                                orbEntry->locWrEntered,
                                                orbEntry->locWrIssued,
                                                orbEntry->locWrExit,
                                                orbEntry->farRdEntered,
                                                orbEntry->farRdIssued,
                                                orbEntry->farRdExit);
            delete orbEntry;

            orbEntry = ORB.at(copyOwPkt->getAddr());

            // clear ORB
            resumeConflictingReq(orbEntry);

            return;
    }

    if (orbEntry->pol == enums::BearWriteOpt &&
        orbEntry->owPkt->isRead() &&
        orbEntry->state == farMemRead) {

            assert(!orbEntry->isHit);

            // do a read from far mem
            pktFarMemRead.push_back(orbEntry->owPkt->getAddr());

            polManStats.avgFarRdQLenEnq = pktFarMemRead.size();

            if (!farMemReadEvent.scheduled()) {
                schedule(farMemReadEvent, curTick());
            }
            return;

    }

    if (orbEntry->pol == enums::BearWriteOpt &&
        orbEntry->state == locMemWrite) {

            if (orbEntry->owPkt->isRead()) {
                assert(!orbEntry->isHit);
            }

            // do a read from far mem
            pktLocMemWrite.push_back(orbEntry->owPkt->getAddr());

            polManStats.avgLocWrQLenEnq = pktLocMemWrite.size();


            if (!locMemWriteEvent.scheduled()) {
                schedule(locMemWriteEvent, curTick());
            }
            return;

    }

    if (orbEntry->pol == enums::BearWriteOpt &&
        // orbEntry->owPkt->isRead() &&
        // !orbEntry->isHit &&
        orbEntry->state == waitingLocMemWriteResp) {
            // DONE
            // clear ORB
            resumeConflictingReq(orbEntry);

            return;
    }
}

void
SAPolManager::handleRequestorPkt(PacketPtr pkt)
{
    reqBufferEntry* orbEntry = new reqBufferEntry(
                                true, curTick(),
                                returnTagDC(pkt->getAddr(), pkt->getSize()),
                                returnIndexDC(pkt->getAddr(), pkt->getSize()),
                                -1,                  // wayDC: resolved by checkHitOrMiss
                                pkt,
                                locMemPolicy, start,
                                false, false, false,
                                -1, false,
                                MaxTick, MaxTick, MaxTick,
                                MaxTick, MaxTick, MaxTick,
                                MaxTick, MaxTick, MaxTick
                            );

    ORB.emplace(pkt->getAddr(), orbEntry);

    polManStats.avgORBLen = ORB.size();
    polManStats.avgLocRdQLenStrt = countLocRdInORB();
    polManStats.avgFarRdQLenStrt = countFarRdInORB();
    polManStats.avgLocWrQLenStrt = countLocWrInORB();
    polManStats.avgFarWrQLenStrt = countFarWr();

    Addr addr = pkt->getAddr();
    unsigned burst_size = locBurstSize;
    unsigned size = std::min((addr | (burst_size - 1)) + 1,
                              addr + pkt->getSize()) - addr;

    if(pkt->isRead()) {
        polManStats.bytesReadSys += size;
        polManStats.readPktSize[ceilLog2(size)]++;
        polManStats.readReqs++;
    } else {
        polManStats.bytesWrittenSys += size;
        polManStats.writePktSize[ceilLog2(size)]++;
        polManStats.writeReqs++;
    }

    if (pkt->isWrite()) {

        PacketPtr copyOwPkt = new Packet(orbEntry->owPkt,
                                             false,
                                             orbEntry->owPkt->isRead());

        accessAndRespond(orbEntry->owPkt,
                         frontendLatency + backendLatency);

        ORB.at(copyOwPkt->getAddr()) = new reqBufferEntry(
                                            orbEntry->validEntry,
                                            orbEntry->arrivalTick,
                                            orbEntry->tagDC,
                                            orbEntry->indexDC,
                                            orbEntry->wayDC,
                                            copyOwPkt,
                                            orbEntry->pol,
                                            orbEntry->state,
                                            orbEntry->issued,
                                            orbEntry->isHit,
                                            orbEntry->conflict,
                                            orbEntry->dirtyLineAddr,
                                            orbEntry->handleDirtyLine,
                                            orbEntry->locRdEntered,
                                            orbEntry->locRdIssued,
                                            orbEntry->locRdExit,
                                            orbEntry->locWrEntered,
                                            orbEntry->locWrIssued,
                                            orbEntry->locWrExit,
                                            orbEntry->farRdEntered,
                                            orbEntry->farRdIssued,
                                            orbEntry->farRdExit);
        delete orbEntry;

        orbEntry = ORB.at(copyOwPkt->getAddr());
    }

    checkHitOrMiss(orbEntry);

    Addr entryIdx = orbEntry->indexDC * numWays + orbEntry->wayDC;
    auto &line = tagMetadataStore.at(entryIdx);

    if (!orbEntry->isHit && line.validLine && line.dirtyLine) {
        orbEntry->dirtyLineAddr = line.farMemAddr;
        orbEntry->handleDirtyLine = true;
    }

    line.tagDC = orbEntry->tagDC;
    line.indexDC = orbEntry->indexDC;
    line.validLine = true;

    if (orbEntry->owPkt->isRead()) {
        if (!orbEntry->isHit) {
            line.dirtyLine = false;
        }
    } else {
        line.dirtyLine = true;
    }

    line.farMemAddr = orbEntry->owPkt->getAddr();
    line.lastAccess = curTick();
}

bool
SAPolManager::ndcConflict(Addr addr, const reqBufferEntry* ignore)
{
    Addr set = returnIndexDC(addr, blockSize);
    Addr tag = returnTagDC(addr, blockSize);

    unsigned outstandingInSet = 0;

    for (auto e = ORB.begin(); e != ORB.end(); ++e) {
        auto entry = e->second;

        if (entry == ignore || !entry->validEntry ||
            entry->indexDC != set) {
            continue;
        }

        // The same line is already in flight, so it has to be serialized.
        if (entry->tagDC == tag) {
            return true;
        }

        outstandingInSet++;
    }

    // Every way of the set already has an outstanding request, so there is no
    // way left to victimize that is not itself waiting on a fill.
    return outstandingInSet >= numWays;
}

bool
SAPolManager::checkConflictInDramCache(PacketPtr pkt)
{
    unsigned indexDC = returnIndexDC(pkt->getAddr(), pkt->getSize());

    // Under the NDC mapping a shared set index is not by itself a conflict:
    // the ways of the set hold independent lines, and a page deliberately maps
    // all of its blocks into the same few sets.
    if (ndcAddrMapping && !ndcConflict(pkt->getAddr())) {
        return false;
    }

    for (auto e = ORB.begin(); e != ORB.end(); ++e) {
        if (indexDC == e->second->indexDC && e->second->validEntry) {

            e->second->conflict = true;

            return true;
        }
    }
    return false;
}

void
SAPolManager::checkHitOrMiss(reqBufferEntry* orbEntry)
{
    // access the tagMetadataStore data structure to
    // check if it's hit or miss

    Addr set = orbEntry->indexDC;
    int hitWay = findMatchingWay(set, orbEntry->tagDC);

    bool currValid;
    bool currDirty;

    if (hitWay != -1) {
        orbEntry->isHit = true;
        orbEntry->wayDC = hitWay;
        auto &line = tagMetadataStore.at(set * numWays + hitWay);
        currValid = line.validLine;
        currDirty = line.dirtyLine;
    } else {
        orbEntry->isHit = false;
        orbEntry->wayDC = findVictimWay(set);
        auto &victim = tagMetadataStore.at(set * numWays + orbEntry->wayDC);
        currValid = victim.validLine;
        currDirty = victim.dirtyLine;
    }

    if (orbEntry->isHit) {

        polManStats.numTotHits++;

        if (orbEntry->owPkt->isRead()) {
            polManStats.numRdHit++;
            if (currDirty) {
                polManStats.numRdHitDirty++;
            } else {
                polManStats.numRdHitClean++;
            }
        } else {
            polManStats.numWrHit++;
            if (currDirty) {
                polManStats.numWrHitDirty++;
            } else {
                polManStats.numWrHitClean++;
            }
        }

    } else {

        polManStats.numTotMisses++;

        if (currValid) {
            polManStats.numHotMisses++;
        } else {
            polManStats.numColdMisses++;
            numColdMisses++;
        }

        if (orbEntry->owPkt->isRead()) {
            if (currDirty && currValid) {
                polManStats.numRdMissDirty++;
            } else {
                polManStats.numRdMissClean++;
            }
        } else {
            if (currDirty && currValid) {
                polManStats.numWrMissDirty++;
            } else {
                polManStats.numWrMissClean++;
            }

        }
    }

    // if ( numColdMisses >= (unsigned)(cacheWarmupRatio * dramCacheSize/blockSize) && !resetStatsWarmup ) {
    //    std::cout << curTick() << " --------------------------1\n";
    //    exitSimLoopNow("cacheIsWarmedup");
    //    std::cout << curTick() << " --------------------------2\n";
    //    resetStatsWarmup = true;
    // }
}

void
SAPolManager::accessAndRespond(PacketPtr pkt, Tick static_latency)
{
    DPRINTF(SAPolManager, "Responding to Address %d \n", pkt->getAddr());

    bool needsResponse = pkt->needsResponse();
    // do the actual memory access which also turns the packet into a
    // response
    panic_if(!getAddrRange().contains(pkt->getAddr()),
             "Can't handle address range for packet %s\n", pkt->print());
    access(pkt);

    // turn packet around to go back to requestor if response expected
    if (needsResponse) {
        // access already turned the packet into a response
        assert(pkt->isResponse());
        // response_time consumes the static latency and is charged also
        // with headerDelay that takes into account the delay provided by
        // the xbar and also the payloadDelay that takes into account the
        // number of data beats.
        Tick response_time = curTick() + static_latency + pkt->headerDelay +
                             pkt->payloadDelay;
        // Here we reset the timing of the packet before sending it out.
        pkt->headerDelay = pkt->payloadDelay = 0;

        // queue the packet in the response queue to be sent out after
        // the static latency has passed
        port.schedTimingResp(pkt, response_time);
    } else {
        // @todo the packet is going to be deleted, and the MemPacket
        // is still having a pointer to it
        pendingDelete.reset(pkt);
    }

    DPRINTF(SAPolManager, "Done\n");

    return;
}

PacketPtr
SAPolManager::getPacket(Addr addr, unsigned size, const MemCmd& cmd,
                   Request::FlagsType flags)
{
    // Create new request
    RequestPtr req = std::make_shared<Request>(addr, size, flags,
                                               0);
    // Dummy PC to have PC-based prefetchers latch on; get entropy into higher
    // bits
    req->setPC(((Addr)0) << 2);

    // Embed it in a packet
    PacketPtr pkt = new Packet(req, cmd);

    uint8_t* pkt_data = new uint8_t[req->getSize()];

    pkt->dataDynamic(pkt_data);

    if (cmd.isWrite()) {
        std::fill_n(pkt_data, req->getSize(), (uint8_t)0);
    }

    return pkt;
}

void
SAPolManager::sendRespondToRequestor(PacketPtr pkt, Tick static_latency)
{
    PacketPtr copyOwPkt = new Packet(pkt,
                                     false,
                                     pkt->isRead());
    copyOwPkt->makeResponse();

    Tick response_time = curTick() + static_latency + copyOwPkt->headerDelay + copyOwPkt->payloadDelay;
    // Here we reset the timing of the packet before sending it out.
    copyOwPkt->headerDelay = copyOwPkt->payloadDelay = 0;

    // queue the packet in the response queue to be sent out after
    // the static latency has passed
    port.schedTimingResp(copyOwPkt, response_time);

}

bool
SAPolManager::resumeConflictingReq(reqBufferEntry* orbEntry)
{
    bool conflictFound = false;

    if (orbEntry->owPkt->isWrite()) {
        isInWriteQueue.erase(orbEntry->owPkt->getAddr());
    }

    logStatsPolMan(orbEntry);

    for (auto e = CRB.begin(); e != CRB.end(); ++e) {

        auto entry = *e;

        if (returnIndexDC(entry.second->getAddr(), entry.second->getSize())
            == orbEntry->indexDC) {

                // The waiting request is resumed without re-running the
                // admission test, so under the NDC mapping only wake it if it
                // really becomes admissible once this entry has retired.
                // Scanning the whole set (rather than just the matching line)
                // keeps requests that were blocked by set occupancy from
                // starving.
                if (ndcAddrMapping &&
                    ndcConflict(entry.second->getAddr(), orbEntry)) {
                    continue;
                }

                conflictFound = true;

                Addr confAddr = entry.second->getAddr();

                ORB.erase(orbEntry->owPkt->getAddr());

                delete orbEntry->owPkt;

                delete orbEntry;

                handleRequestorPkt(entry.second);

                ORB.at(confAddr)->arrivalTick = entry.first;

                CRB.erase(e);

                checkConflictInCRB(ORB.at(confAddr));

                // pktLocMemRead.push_back(confAddr);

                // polManStats.avgLocRdQLenEnq = pktLocMemRead.size();

                setNextState(ORB.at(confAddr));

                handleNextState(ORB.at(confAddr));

                break;
        }

    }

    if (!conflictFound) {

        ORB.erase(orbEntry->owPkt->getAddr());

        delete orbEntry->owPkt;

        delete orbEntry;

        if (retryLLC) {
            DPRINTF(SAPolManager, "retryLLC: sent\n");
            retryLLC = false;
            port.sendRetryReq();
        } else {
            if (drainState() == DrainState::Draining && ORB.empty() &&
                pktFarMemWrite.empty()) {
                DPRINTF(SAPolManager, "SAPolManager done draining\n");
                signalDrainDone();
            }
        }
    }

    return conflictFound;
}

void
SAPolManager::checkConflictInCRB(reqBufferEntry* orbEntry)
{
    for (auto e = CRB.begin(); e != CRB.end(); ++e) {

        auto entry = *e;

        if (returnIndexDC(entry.second->getAddr(),entry.second->getSize())
            == orbEntry->indexDC) {
                orbEntry->conflict = true;
                break;
        }
    }
}

unsigned
SAPolManager::countLocRdInORB()
{
    unsigned count =0;
    for (auto i : ORB) {
        if (i.second->state == locMemRead) {
            count++;
        }
    }
    return count;
}

unsigned
SAPolManager::countFarRdInORB()
{
    unsigned count =0;
    for (auto i : ORB) {
        if (i.second->state == farMemRead) {
            count++;
        }
    }
    return count;
}

unsigned
SAPolManager::countLocWrInORB()
{
    unsigned count =0;
    for (auto i : ORB) {
        if (i.second->state == locMemWrite) {
            count++;
        }
    }
    return count;
}

unsigned
SAPolManager::countFarWr()
{
    return pktFarMemWrite.size();
}

AddrRangeList
SAPolManager::getAddrRanges()
{
    return farReqPort.getAddrRanges();
}

Addr
SAPolManager::returnIndexDC(Addr request_addr, unsigned size)
{
    int index_bits = ceilLog2(numSets);
    // Use the DRAM-cache line size (blockSize), not the request size, so a
    // larger DRAM-cache block (e.g. 512B) groups multiple LLC-sized (64B)
    // requests into one cache line.
    int block_bits = ceilLog2(blockSize);
    (void)size;

    if (ndcAddrMapping) {
        // NDC: the interleaving bits just above the block offset stay in the
        // index, the rest of the index comes from the PFN. Every block of a
        // page therefore shares a set, which is one DRAM row.
        Addr intlv = intlvLowBits == 0 ? 0 :
            bits(request_addr, blockBits + intlvLowBits - 1, blockBits);

        if (pfnIndexBits == 0) {
            return intlv;
        }

        Addr pfn = bits(request_addr, pageBits + pfnIndexBits - 1, pageBits);

        return (pfn << intlvLowBits) | intlv;
    }

    return bits(request_addr, block_bits + index_bits-1, block_bits);
}

Addr
SAPolManager::returnTagDC(Addr request_addr, unsigned size)
{
    int index_bits = ceilLog2(numSets);
    // Use the DRAM-cache line size (blockSize), not the request size, so a
    // larger DRAM-cache block (e.g. 512B) groups multiple LLC-sized (64B)
    // requests into one cache line.
    int block_bits = ceilLog2(blockSize);
    (void)size;

    if (ndcAddrMapping) {
        // NDC: the intra-page bits the conventional mapping would use as a
        // column address become the low tag bits, and everything above the
        // PFN index bits becomes the high tag bits. These are the bits the
        // in-subarray CAM compares against the ways of the set.
        Addr tag_low = tagLowBits == 0 ? 0 :
            bits(request_addr, pageBits - 1, blockBits + intlvLowBits);

        if (pageBits + pfnIndexBits >= addrSize) {
            return tag_low;
        }

        Addr tag_high =
            bits(request_addr, addrSize - 1, pageBits + pfnIndexBits);

        return (tag_high << tagLowBits) | tag_low;
    }

    return bits(request_addr, addrSize-1, (index_bits+block_bits));
}

Addr
SAPolManager::returnLocAddr(const reqBufferEntry* orbEntry)
{
    if (!ndcLocAddrMapping) {
        return orbEntry->owPkt->getAddr();
    }

    // NDC: the set index becomes the device address of a row and the way becomes
    // the column within it. Laying a set's numWays lines out contiguously makes
    // it exactly one row buffer, so the DRAM interface decodes bank -> rank -> row
    // from the index and the ways of a set are row-buffer hits.
    assert(orbEntry->wayDC >= 0);

    // Index bit 0 is PA[6], which selects the channel and must not do anything
    // else. Keep it out of the bits the DRAM interface decodes -- otherwise it
    // would also land in the bank field and each pseudo channel could only
    // reach half of its banks -- by lifting it above the whole array. The
    // interface is then left with the bank group from PA[7] and the remaining
    // bank and row bits from the PFN part of the index.
    const Addr chan = (Addr)orbEntry->indexDC & 1;
    const Addr set = (Addr)orbEntry->indexDC >> 1;

    return (((set * numWays) + orbEntry->wayDC) * blockSize) +
           (chan * (dramCacheSize / 2));
}

Addr
SAPolManager::locRespKey(PacketPtr pkt)
{
    auto* key = dynamic_cast<LocMemKey*>(pkt->senderState);

    return key ? key->orbKey : pkt->getAddr();
}

void
SAPolManager::freeLocKey(PacketPtr pkt)
{
    auto* key = dynamic_cast<LocMemKey*>(pkt->senderState);

    if (key) {
        pkt->senderState = nullptr;
        delete key;
    }
}

int
SAPolManager::findMatchingWay(Addr set, Addr tag)
{
    Addr base = set * numWays;
    for (unsigned w = 0; w < numWays; w++) {
        auto &entry = tagMetadataStore.at(base + w);
        if (entry.validLine && entry.tagDC == tag) {
            return (int)w;
        }
    }
    return -1;
}

int
SAPolManager::findVictimWay(Addr set)
{
    Addr base = set * numWays;

    for (unsigned w = 0; w < numWays; w++) {
        if (!tagMetadataStore.at(base + w).validLine) {
            return (int)w;
        }
    }

    unsigned victim = 0;
    Tick oldest = tagMetadataStore.at(base).lastAccess;
    for (unsigned w = 1; w < numWays; w++) {
        if (tagMetadataStore.at(base + w).lastAccess < oldest) {
            oldest = tagMetadataStore.at(base + w).lastAccess;
            victim = w;
        }
    }
    return (int)victim;
}

void
SAPolManager::handleDirtyCacheLine(reqBufferEntry* orbEntry)
{
    assert(orbEntry->dirtyLineAddr != -1);

    // create a new request packet
    PacketPtr wbPkt = getPacket(orbEntry->dirtyLineAddr,
                                orbEntry->owPkt->getSize(),
                                MemCmd::WriteReq);

    pktFarMemWrite.push_back(std::make_pair(curTick(), wbPkt));

    polManStats.avgFarWrQLenEnq = pktFarMemWrite.size();

    if (!farMemWriteEvent.scheduled()) {
            schedule(farMemWriteEvent, curTick());
    }

    polManStats.numWrBacks++;
}

void
SAPolManager::logStatsPolMan(reqBufferEntry* orbEntry)
{
    polManStats.totPktsServiceTime += ((curTick() - orbEntry->arrivalTick)/1000);
    polManStats.totPktsORBTime += ((curTick() - orbEntry->locRdEntered)/1000);
    polManStats.totTimeFarRdtoSend += ((orbEntry->farRdIssued - orbEntry->farRdEntered)/1000);
    polManStats.totTimeFarRdtoRecv += ((orbEntry->farRdExit - orbEntry->farRdIssued)/1000);
    polManStats.totTimeInLocRead += ((orbEntry->locRdExit - orbEntry->locRdEntered)/1000);
    polManStats.totTimeInLocWrite += ((orbEntry->locWrExit - orbEntry->locWrEntered)/1000);
    polManStats.totTimeInFarRead += ((orbEntry->farRdExit - orbEntry->farRdEntered)/1000);

}


void
SAPolManager::ReqPortPolManager::recvReqRetry()
{
    if (isLoc)
        polMan.locMemRecvReqRetry();
    else
        polMan.farMemRecvReqRetry();
}

bool
SAPolManager::ReqPortPolManager::recvTimingResp(PacketPtr pkt)
{
    if (isLoc)
        return polMan.locMemRecvTimingResp(pkt);
    else
        return polMan.farMemRecvTimingResp(pkt);
}

SAPolManager::SAPolManagerStats::SAPolManagerStats(SAPolManager &_polMan)
    : statistics::Group(&_polMan),
    polMan(_polMan),

/////
    ADD_STAT(readReqs, statistics::units::Count::get(),
             "Number of read requests accepted"),
    ADD_STAT(writeReqs, statistics::units::Count::get(),
             "Number of write requests accepted"),

    ADD_STAT(servicedByWrQ, statistics::units::Count::get(),
             "Number of controller read bursts serviced by the write queue"),
    ADD_STAT(mergedWrBursts, statistics::units::Count::get(),
             "Number of controller write bursts merged with an existing one"),

    ADD_STAT(numRdRetry, statistics::units::Count::get(),
             "Number of times read queue was full causing retry"),
    ADD_STAT(numWrRetry, statistics::units::Count::get(),
             "Number of times write queue was full causing retry"),

    ADD_STAT(readPktSize, statistics::units::Count::get(),
             "Read request sizes (log2)"),
    ADD_STAT(writePktSize, statistics::units::Count::get(),
             "Write request sizes (log2)"),

    ADD_STAT(bytesReadWrQ, statistics::units::Byte::get(),
             "Total number of bytes read from write queue"),
    ADD_STAT(bytesReadSys, statistics::units::Byte::get(),
             "Total read bytes from the system interface side"),
    ADD_STAT(bytesWrittenSys, statistics::units::Byte::get(),
             "Total written bytes from the system interface side"),

    ADD_STAT(avgRdBWSys, statistics::units::Rate<
                statistics::units::Byte, statistics::units::Second>::get(),
             "Average system read bandwidth in Byte/s"),
    ADD_STAT(avgWrBWSys, statistics::units::Rate<
                statistics::units::Byte, statistics::units::Second>::get(),
             "Average system write bandwidth in Byte/s"),

    ADD_STAT(totGap, statistics::units::Tick::get(),
             "Total gap between requests"),
    ADD_STAT(avgGap, statistics::units::Rate<
                statistics::units::Tick, statistics::units::Count>::get(),
             "Average gap between requests"),

    ADD_STAT(avgORBLen, statistics::units::Rate<
                statistics::units::Count, statistics::units::Tick>::get(),
             "Average ORB length"),
    ADD_STAT(avgLocRdQLenStrt, statistics::units::Rate<
                statistics::units::Count, statistics::units::Tick>::get(),
             "Average local read queue length"),
    ADD_STAT(avgLocWrQLenStrt, statistics::units::Rate<
                statistics::units::Count, statistics::units::Tick>::get(),
             "Average local write queue length"),
    ADD_STAT(avgFarRdQLenStrt, statistics::units::Rate<
                statistics::units::Count, statistics::units::Tick>::get(),
             "Average far read queue length"),
    ADD_STAT(avgFarWrQLenStrt, statistics::units::Rate<
                statistics::units::Count, statistics::units::Tick>::get(),
             "Average far write queue length"),

    ADD_STAT(avgLocRdQLenEnq, statistics::units::Rate<
                statistics::units::Count, statistics::units::Tick>::get(),
             "Average local read queue length when enqueuing"),
    ADD_STAT(avgLocWrQLenEnq, statistics::units::Rate<
                statistics::units::Count, statistics::units::Tick>::get(),
             "Average local write queue length when enqueuing"),
    ADD_STAT(avgFarRdQLenEnq, statistics::units::Rate<
                statistics::units::Count, statistics::units::Tick>::get(),
             "Average far read queue length when enqueuing"),
    ADD_STAT(avgFarWrQLenEnq, statistics::units::Rate<
                statistics::units::Count, statistics::units::Tick>::get(),
             "Average far write queue length when enqueuing"),

    ADD_STAT(numWrBacks,
            "Total number of write backs from DRAM cache to main memory"),
    ADD_STAT(totNumConf,
            "Total number of packets conflicted on DRAM cache"),
    ADD_STAT(totNumORBFull,
            "Total number of packets ORB full"),
    ADD_STAT(totNumCRBFull,
            "Total number of packets conflicted yet couldn't "
            "enter confBuffer"),

    ADD_STAT(maxNumConf,
        "Maximum number of packets conflicted on DRAM cache"),

    ADD_STAT(sentLocRdPort,
             "stat"),
    ADD_STAT(sentLocWrPort,
             "stat"),
    ADD_STAT(failedLocRdPort,
             "stat"),
    ADD_STAT(failedLocWrPort,
             "stat"),
    ADD_STAT(recvdRdPort,
             "stat"),
    ADD_STAT(sentFarRdPort,
             "stat"),
    ADD_STAT(sentFarWrPort,
             "stat"),
    ADD_STAT(failedFarRdPort,
             "stat"),
    ADD_STAT(failedFarWrPort,
             "stat"),

    ADD_STAT(totPktsServiceTime,
            "stat"),
    ADD_STAT(totPktsORBTime,
            "stat"),
    ADD_STAT(totTimeFarRdtoSend,
            "stat"),
    ADD_STAT(totTimeFarRdtoRecv,
            "stat"),
    ADD_STAT(totTimeFarWrtoSend,
            "stat"),
    ADD_STAT(totTimeInLocRead,
            "stat"),
    ADD_STAT(totTimeInLocWrite,
            "stat"),
    ADD_STAT(totTimeInFarRead,
            "stat"),

    ADD_STAT(numTotHits,
            "stat"),
    ADD_STAT(numTotMisses,
            "stat"),
    ADD_STAT(numColdMisses,
            "stat"),
    ADD_STAT(numHotMisses,
            "stat"),
    ADD_STAT(numRdMissClean,
            "stat"),
    ADD_STAT(numRdMissDirty,
            "stat"),
    ADD_STAT(numRdHit,
            "stat"),
    ADD_STAT(numWrMissClean,
            "stat"),
    ADD_STAT(numWrMissDirty,
            "stat"),
    ADD_STAT(numWrHit,
            "stat"),
    ADD_STAT(numRdHitDirty,
            "stat"),
    ADD_STAT(numRdHitClean,
            "stat"),
    ADD_STAT(numWrHitDirty,
            "stat"),
    ADD_STAT(numWrHitClean,
            "stat")

{
}

void
SAPolManager::SAPolManagerStats::regStats()
{
    using namespace statistics;

    avgORBLen.precision(4);
    avgLocRdQLenStrt.precision(2);
    avgLocWrQLenStrt.precision(2);
    avgFarRdQLenStrt.precision(2);
    avgFarWrQLenStrt.precision(2);

    avgLocRdQLenEnq.precision(2);
    avgLocWrQLenEnq.precision(2);
    avgFarRdQLenEnq.precision(2);
    avgFarWrQLenEnq.precision(2);

    readPktSize.init(ceilLog2(polMan.blockSize) + 1);
    writePktSize.init(ceilLog2(polMan.blockSize) + 1);

    avgRdBWSys.precision(8);
    avgWrBWSys.precision(8);
    avgGap.precision(2);

    // Formula stats
    avgRdBWSys = (bytesReadSys) / simSeconds;
    avgWrBWSys = (bytesWrittenSys) / simSeconds;

    avgGap = totGap / (readReqs + writeReqs);

}

Port &
SAPolManager::getPort(const std::string &if_name, PortID idx)
{
    panic_if(idx != InvalidPortID, "This object doesn't support vector ports");

    // This is the name from the Python SimObject declaration (SimpleMemobj.py)
    if (if_name == "port") {
        return port;
    } else if (if_name == "loc_req_port") {
        return locReqPort;
    } else if (if_name == "far_req_port") {
        return farReqPort;
    } else {
        // pass it along to our super class
        panic("PORT NAME ERROR !!!!\n");
    }
}

DrainState
SAPolManager::drain()
{
    if (!ORB.empty() || !pktFarMemWrite.empty()) {
        DPRINTF(Drain, "DRAM cache is not drained! Have %d in ORB and %d in "
                "writeback queue.\n", ORB.size(), pktFarMemWrite.size());
        return DrainState::Draining;
    } else {
        return DrainState::Drained;
    }
}

void
SAPolManager::serialize(CheckpointOut &cp) const
{
    ScopedCheckpointSection sec(cp, "tagMetadataStore");
    paramOut(cp, "numEntries", tagMetadataStore.size());

    int count = 0;
    for (auto const &entry : tagMetadataStore) {
        ScopedCheckpointSection sec_entry(cp,csprintf("Entry%d", count++));
        if (entry.validLine) {
            paramOut(cp, "validLine", entry.validLine);
            paramOut(cp, "tagDC", entry.tagDC);
            paramOut(cp, "indexDC", entry.indexDC);
            paramOut(cp, "dirtyLine", entry.dirtyLine);
            paramOut(cp, "farMemAddr", entry.farMemAddr);
            paramOut(cp, "lastAccess", entry.lastAccess);
        } else {
            paramOut(cp, "validLine", entry.validLine);
        }
    }
}

void
SAPolManager::unserialize(CheckpointIn &cp)
{
    ScopedCheckpointSection sec(cp, "tagMetadataStore");
    int num_entries = 0;
    paramIn(cp, "numEntries", num_entries);
    warn_if(num_entries > tagMetadataStore.size(), "Unserializing larger tag "
            "store into a smaller tag store. Stopping when index doesn't fit");
    warn_if(num_entries < tagMetadataStore.size(), "Unserializing smaller "
            "tag store into a larger tag store. Not fully warmed up.");

    for (int i = 0; i < num_entries; i++) {
        ScopedCheckpointSection sec_entry(cp,csprintf("Entry%d", i));

        bool valid = false;
        paramIn(cp, "validLine", valid);
        if (valid && (unsigned)i < tagMetadataStore.size()) {
            Addr tag = 0;
            Addr index = 0;
            bool dirty = false;
            Addr far_addr = 0;
            Tick last_access = 0;
            paramIn(cp, "tagDC", tag);
            paramIn(cp, "indexDC", index);
            paramIn(cp, "dirtyLine", dirty);
            paramIn(cp, "farMemAddr", far_addr);
            paramIn(cp, "lastAccess", last_access);
            tagMetadataStore[i].tagDC = tag;
            tagMetadataStore[i].indexDC = index;
            tagMetadataStore[i].validLine = valid;
            tagMetadataStore[i].dirtyLine = dirty;
            tagMetadataStore[i].farMemAddr = far_addr;
            tagMetadataStore[i].lastAccess = last_access;
        }
    }
}



bool
SAPolManager::RespPortPolManager::recvTimingReq(PacketPtr pkt)
{
    return polMan.recvTimingReq(pkt);
}

} // namespace memory
} // namespace gem5
