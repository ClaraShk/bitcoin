#include "simulation.h"
#include "ccl/cclglobals.h"

#include "chainparams.h"
#include "init.h"
#include "main.h"
#include "consensus/validation.h"
#include "primitives/transaction.h"
#include "primitives/block.h"
#include "util.h"

#include <string>
#include <boost/interprocess/sync/file_lock.hpp>

using namespace boost;
using namespace std;

Simulation::Simulation(date sdate, date edate, string datadir, bool loadMempool)
 : logdir(datadir),
   begindate(sdate), enddate(edate),
   loadMempoolAtStartup(loadMempool)
{
    LoadFiles(begindate);
    if (blkfile->IsNull()) {
        LogPrintf("Simulation: can't open block file, continuing without\n");
    }
    if (txfile->IsNull()) {
        LogPrintf("Simulation: can't open tx file, continuing without\n");
    }
    if (headersfile->IsNull()) {
        LogPrintf("Simulation: can't open headers file, continuing without\n");
    }
    if (cmpctblockfile->IsNull()) {
        LogPrintf("Simulation: can't open cmpctblock file, continuing without\n");
    }
    if (blocktxnfile->IsNull()) {
        LogPrintf("Simulation: can't open blocktxn file, continuing without\n");
    }

    // Actually, this should error if the right date can't be found...
    if (loadMempoolAtStartup) {
        InitAutoFile(mempoolfile, "mempool.", begindate);
        if (mempoolfile->IsNull()) {
            LogPrintf("Simulation: can't open mempool file, continuing without\n");
        }
    }
}

void Simulation::LoadFiles(date d)
{
    InitAutoFile(txfile, "tx.", d);
    InitAutoFile(blkfile, "block.", d);
    InitAutoFile(headersfile, "headers.", d);
    InitAutoFile(cmpctblockfile, "cmpctblock.", d);
    InitAutoFile(blocktxnfile, "blocktxn.", d);
}

void Simulation::InitAutoFile(unique_ptr<CAutoFile> &which, std::string fileprefix, date d)
{
    for (date s=d; s<= enddate; s += days(1)) {
        string filename = fileprefix + boost::gregorian::to_iso_string(s);
        boost::filesystem::path fullpath = logdir / filename;
        which.reset(new CAutoFile(fopen(fullpath.string().c_str(), "rb"),
                    SER_DISK, CLIENT_VERSION));
        if (!which->IsNull()) {
            LogPrintf("Simulation: InitAutoFile opened %s\n", fullpath.string().c_str());
            break;
        }
    }
}


void Simulation::operator()()
{
    LogPrintf("Simulation starting\n");

    date curdate = begindate;
    if (loadMempoolAtStartup) {
        // Start up with beginning mempool
        //cclGlobals->InitMemPool(*mempoolfile);
        LogPrintf("Simulation: not loading mempool! Doesn't really work...\n");
    } else {
        LogPrintf("Simulation: not loading mempool\n");
    }
    while (curdate <= enddate) {
        bool txEOF = false;
        bool blkEOF = false;
        bool hdrEOF = false;
        bool cbEOF = false;
        bool btEOF = false;

        BlockEvent blockEvent;
        TxEvent txEvent;
        HeadersEvent headersEvent;
        CompactBlockEvent cmpctblockEvent;
        BlockTransactionsEvent blocktxnEvent;

        while (!txEOF || !blkEOF || !hdrEOF || !cbEOF || !btEOF) {
            if (!txEOF && !txEvent.valid) {
                txEOF = !ReadEvent(*txfile, &txEvent);
            }
            if (!blkEOF && !blockEvent.valid) {
                blkEOF = !ReadEvent(*blkfile, &blockEvent);
            }
            if (!hdrEOF && !headersEvent.valid) {
                hdrEOF = !ReadEvent(*headersfile, &headersEvent);
            }
            if (!cbEOF && !cmpctblockEvent.valid) {
                cbEOF = !ReadEvent(*cmpctblockfile, &cmpctblockEvent);
            }
            if (!btEOF && !blocktxnEvent.valid) {
                btEOF = !ReadEvent(*blocktxnfile, &blocktxnEvent);
            }

            vector<CCLEvent *> validEvents;
            if (txEvent.valid) validEvents.push_back(&txEvent);
            if (blockEvent.valid) validEvents.push_back(&blockEvent);
            if (headersEvent.valid) validEvents.push_back(&headersEvent);
            if (cmpctblockEvent.valid) validEvents.push_back(&cmpctblockEvent);
            if (blocktxnEvent.valid) validEvents.push_back(&blocktxnEvent);
            if (validEvents.empty()) break;

            CCLEvent *nextEvent = validEvents[0];
            for (size_t i=1; i<validEvents.size(); ++i) {
                if (*validEvents[i] < *nextEvent) nextEvent = validEvents[i];
            }
            timeInMicros = nextEvent->timeMicros;
            SetMockTime(nextEvent->timeMicros / 1000000);

            if (nextEvent == &txEvent) {
                ProcessTransaction(txEvent.obj);
                txEvent.reset();
            } else if (nextEvent == &blockEvent) {
                CValidationState state;
                ProcessNewBlock(state, Params(), NULL, &blockEvent.obj, true, NULL);
                blockEvent.reset();
            } else if (nextEvent == &headersEvent) {
                CValidationState state;
                for (size_t i=0; i<headersEvent.obj.size(); ++i) {
                    // The 3rd argument to AcceptBlockHeader is only
                    // used for catching misbehaving nodes.  This could
                    // cause a sim-live discrepancy where
                    if (!AcceptBlockHeader(headersEvent.obj[i], state, NULL)) {
                        int nDoS;
                        if (state.IsInvalid(nDoS)) break;
                    }
                }
                headersEvent.reset();
            } else if (nextEvent == &cmpctblockEvent) {
                // TODO: add a compactblock handler!
                cmpctblockEvent.reset();
            } else if (nextEvent == &blocktxnEvent) {
                // TODO: add a blocktxn handler
                blocktxnEvent.reset();
            }
        }
        curdate += days(1);
        LoadFiles(curdate);
    }
    LogPrintf("Simulation exiting, mempool size: %lu\n", cclGlobals->mempool->size());
    StartShutdown();
}
