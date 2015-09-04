// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "txmempool.h"

#include "clientversion.h"
#include "consensus/consensus.h"
#include "consensus/validation.h"
#include "main.h"
#include "policy/fees.h"
#include "streams.h"
#include "util.h"
#include "utilmoneystr.h"
#include "version.h"

using namespace std;

CTxMemPoolEntry::CTxMemPoolEntry():
    nFee(0), nTxSize(0), nModSize(0), nUsageSize(0), nTime(0), dPriority(0.0), hadNoDependencies(false),
    nCountWithDescendants(0), nSizeWithDescendants(0), nFeesWithDescendants(0)
{
    nHeight = MEMPOOL_HEIGHT;
}

CTxMemPoolEntry::CTxMemPoolEntry(const CTransaction& _tx, const CAmount& _nFee,
                                 int64_t _nTime, double _dPriority,
                                 unsigned int _nHeight, bool poolHasNoInputsOf):
    tx(_tx), nFee(_nFee), nTime(_nTime), dPriority(_dPriority), nHeight(_nHeight),
    hadNoDependencies(poolHasNoInputsOf)
{
    nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
    nModSize = tx.CalculateModifiedSize(nTxSize);
    nUsageSize = RecursiveDynamicUsage(tx);

    nCountWithDescendants = 1;
    nSizeWithDescendants = nTxSize;
    nFeesWithDescendants = nFee;
}

CTxMemPoolEntry::CTxMemPoolEntry(const CTxMemPoolEntry& other)
{
    *this = other;
}

double
CTxMemPoolEntry::GetPriority(unsigned int currentHeight) const
{
    CAmount nValueIn = tx.GetValueOut()+nFee;
    double deltaPriority = ((double)(currentHeight-nHeight)*nValueIn)/nModSize;
    double dResult = dPriority + deltaPriority;
    return dResult;
}

// Update the given tx for any in-mempool descendants.
// Assumes that setMemPoolChildren is correct for the given tx and all
// descendants.
bool CTxMemPool::UpdateForDescendants(txiter updateIt, int maxDescendantsToVisit, cacheMap &cachedDescendants, const std::set<uint256> &setExclude)
{
    // Track the number of entries (outside setExclude) that we'd need to visit
    // (will bail out if it exceeds maxDescendantsToVisit)
    int nChildrenToVisit = 0; 

    setEntries stageEntries, setAllDescendants;
    stageEntries = GetMemPoolChildren(updateIt);

    while (!stageEntries.empty()) {
        setAllDescendants.insert(stageEntries.begin(), stageEntries.end());

        setEntries entriesToAdd;
        BOOST_FOREACH(const txiter cit, stageEntries) {
            if (cit->IsDirty()) {
                // Don't consider any more children if any descendant is dirty
                return false;
            }
            const setEntries &setChildren = GetMemPoolChildren(cit);
            BOOST_FOREACH(const txiter childEntry, setChildren) {
                std::map<txiter, setEntries>::iterator cacheIt = cachedDescendants.find(childEntry);
                if (cacheIt != cachedDescendants.end()) {
                    // We've already calculated this one, just add the entries for this set
                    // but don't traverse again.
                    BOOST_FOREACH(const txiter cacheEntry, cacheIt->second) {
                        // update visit count only for new child transactions
                        // (outside of setExclude and entriesToAdd)
                        if (setAllDescendants.insert(cacheEntry).second &&
                                !setExclude.count(cacheEntry->GetTx().GetHash()) &&
                                !entriesToAdd.count(cacheEntry)) {
                            nChildrenToVisit++;
                        }
                    }
                } else if (!setAllDescendants.count(childEntry)) {
                    // Try adding to entriesToAdd, and update our visit count
                    if (entriesToAdd.insert(childEntry).second && !setExclude.count(childEntry->GetTx().GetHash())) {
                        nChildrenToVisit++;
                    }
                }
                if (nChildrenToVisit > maxDescendantsToVisit) {
                    return false;
                }
            }
        }
        stageEntries = entriesToAdd;
    }
    // setAllDescendants now contains all in-mempool descendants of updateIt.
    // Update and add to cached descendant map
    int64_t modifySize = 0;
    CAmount modifyFee = 0;
    int64_t modifyCount = 0;
    BOOST_FOREACH(txiter cit, setAllDescendants) {
        if (!setExclude.count(cit->GetTx().GetHash())) {
            modifySize += cit->GetTxSize();
            modifyFee += cit->GetFee();
            modifyCount++;
            cachedDescendants[updateIt].insert(cit);
        }
    }
    mapTx.modify(updateIt, update_descendant_state(modifySize, modifyFee, modifyCount));
    return true;
}

// vHashesToUpdate is the set of transaction hashes from a disconnected block
// which has been re-added to the mempool.
// for each entry, look for descendants that are outside hashesToUpdate, and
// add fee/size information for such descendants to the parent.
void CTxMemPool::UpdateTransactionsFromBlock(const std::vector<uint256> &vHashesToUpdate)
{
    // For each entry in vHashesToUpdate, store the set of in-mempool, but not
    // in-vHashesToUpdate transactions, so that we don't have to recalculate
    // descendants when we come across a previously seen entry.
    cacheMap mapMemPoolDescendantsToUpdate;

    // Use a set for lookups into vHashesToUpdate (these entries are already
    // accounted for in the state of their ancestors)
    std::set<uint256> setAlreadyIncluded(vHashesToUpdate.begin(), vHashesToUpdate.end());

    // Iterate in reverse, so that whenever we are looking at at a transaction
    // we are sure that all in-mempool descendants have already been processed.
    // This maximizes the benefit of the descendant cache and guarantees that
    // setMemPoolChildren will be updated, an assumption made in
    // UpdateForDescendants.
    BOOST_REVERSE_FOREACH(const uint256 &hash, vHashesToUpdate) {
        // we cache the in-mempool children to avoid duplicate updates
        setEntries setChildren;
        // calculate children from mapNextTx
        txiter it = mapTx.find(hash);
        if (it == mapTx.end()) {
            continue;
        }
        std::map<COutPoint, CInPoint>::iterator iter = mapNextTx.lower_bound(COutPoint(hash, 0));
        // First calculate the children, and update setMemPoolChildren to
        // include them, and update their setMemPoolParents to include this tx.
        for (; iter != mapNextTx.end() && iter->first.hash == hash; ++iter) {
            const uint256 &childHash = iter->second.ptx->GetHash();
            txiter childIter = mapTx.find(childHash);
            // We can skip updating entries we've encountered before or that
            // are in the block (which are already accounted for).
            if (setChildren.insert(childIter).second && !setAlreadyIncluded.count(childHash)) {
                UpdateChild(it, childIter, true);
                UpdateParent(childIter, it, true);
            }
        }
        if (!UpdateForDescendants(it, 100, mapMemPoolDescendantsToUpdate, setAlreadyIncluded)) {
            // Mark as dirty if we can't do the calculation.
            mapTx.modify(it, set_dirty());
        }
    }
}

bool CTxMemPool::CalculateMemPoolAncestors(const CTxMemPoolEntry &entry, setEntries &setAncestors, uint64_t limitAncestorCount, uint64_t limitAncestorSize, uint64_t limitDescendantCount, uint64_t limitDescendantSize, std::string &errString)
{
    setEntries parentHashes;
    const CTransaction &tx = entry.GetTx();

    // Get parents of this transaction that are in the mempool
    // entry may or may not already be in the mempool, so we iterate mapTx
    // to find parents, rather than try GetMemPoolParents(entry)
    // TODO: optimize this so that we only check limits and walk
    // tx.vin when called on entries not already in the mempool.
    for (unsigned int i = 0; i < tx.vin.size(); i++) {
        txiter piter = mapTx.find(tx.vin[i].prevout.hash);
        if (piter != mapTx.end()) {
            parentHashes.insert(piter);
            if (parentHashes.size() + 1 > limitAncestorCount) {
                errString = strprintf("too many unconfirmed parents [limit: %u]", limitAncestorCount);
                return false;
            }
        }
    }

    size_t totalSizeWithAncestors = entry.GetTxSize();

    while (!parentHashes.empty()) {
        setAncestors.insert(parentHashes.begin(), parentHashes.end());
        setEntries stageParentSet; 
        BOOST_FOREACH(const txiter &stageit, parentHashes) {
            assert(stageit != mapTx.end());

            totalSizeWithAncestors += stageit->GetTxSize();
            if (stageit->GetSizeWithDescendants() + entry.GetTxSize() > limitDescendantSize) {
                errString = strprintf("exceeds descendant size limit for tx %s [limit: %u]", stageit->GetTx().GetHash().ToString().substr(0,10), limitDescendantSize);
                return false;
            } else if (uint64_t(stageit->GetCountWithDescendants() + 1) > limitDescendantCount) {
                errString = strprintf("too many descendants for tx %s [limit: %u]", stageit->GetTx().GetHash().ToString().substr(0,10), limitDescendantCount);
                return false;
            } else if (totalSizeWithAncestors > limitAncestorSize) {
                errString = strprintf("exceeds ancestor size limit [limit: %u]", limitAncestorSize);
                return false;
            }

            const setEntries & setMemPoolParents = GetMemPoolParents(stageit);
            BOOST_FOREACH(const txiter &phash, setMemPoolParents) {
                // If this is a new ancestor, add it.
                if (setAncestors.count(phash) == 0) {
                    stageParentSet.insert(phash);
                }
                if (stageParentSet.size() + setAncestors.size() + 1 > limitAncestorCount) {
                    errString = strprintf("too many unconfirmed ancestors [limit: %u]", limitAncestorCount);
                    return false;
                }
            }    
        }
        parentHashes = stageParentSet;
    }

    return true;
}

void CTxMemPool::UpdateAncestorsOf(bool add, const uint256 &hash, setEntries &setAncestors)
{
    indexed_transaction_set::iterator it = mapTx.find(hash);
    setEntries parentHashes = GetMemPoolParents(it);
    BOOST_FOREACH(txiter phash, parentHashes) {
        // add or remove hash as a child of phash
        indexed_transaction_set::iterator pit = phash;
        assert (pit != mapTx.end());
        UpdateChild(pit, it, add);
    }
    int64_t updateCount = (add ? 1 : -1);
    int64_t updateSize = updateCount * it->GetTxSize();
    CAmount updateFee = updateCount * it->GetFee();
    BOOST_FOREACH(txiter ancestorHash, setAncestors) {
        indexed_transaction_set::iterator updateIt = ancestorHash;
        assert (updateIt != mapTx.end());
        mapTx.modify(updateIt, update_descendant_state(updateSize, updateFee, updateCount));
    }
}

// TODO: pass a txiter instead?
void CTxMemPool::UpdateChildrenForRemoval(const uint256 &hash)
{
    txiter it = mapTx.find(hash);
    const setEntries &setMemPoolChildren = GetMemPoolChildren(it);
    BOOST_FOREACH(txiter updateIt, setMemPoolChildren) {
        assert(updateIt != mapTx.end());
        UpdateParent(updateIt, it, false);
    }
}

void CTxMemPool::UpdateForRemoveFromMempool(const std::set<uint256> &hashesToRemove)
{
    // For each entry, walk back all ancestors and decrement size associated with this
    // transaction
    uint64_t nNoLimit = std::numeric_limits<uint64_t>::max();
    BOOST_FOREACH(const uint256& removeHash, hashesToRemove) {
        setEntries setAncestors;
        const CTxMemPoolEntry &entry = *mapTx.find(removeHash);
        std::string dummy;
        CalculateMemPoolAncestors(entry, setAncestors, nNoLimit, nNoLimit, nNoLimit, nNoLimit, dummy);
        // Note that UpdateAncestorsOf severs the child links that point to
        // removeHash in the entries for the parents of removeHash.  This is
        // fine since we don't need to use the mempool children of any entries
        // to walk back over our ancestors (but we do need the mempool
        // parents!)
        UpdateAncestorsOf(false, removeHash, setAncestors);
    }
    // After updating all the ancestor sizes, we can now sever the link between each
    // transaction being removed and any mempool children (ie, update setMemPoolParents
    // for each direct child of a transaction being removed).
    BOOST_FOREACH(const uint256& removeHash, hashesToRemove) {
        UpdateChildrenForRemoval(removeHash);
    }
}

void CTxMemPoolEntry::SetDirty()
{
    nCountWithDescendants=0;
    nSizeWithDescendants=nTxSize;
    nFeesWithDescendants=nFee;
}

void CTxMemPoolEntry::UpdateState(int64_t modifySize, CAmount modifyFee, int64_t modifyCount)
{
    if (!IsDirty()) {
        nSizeWithDescendants += modifySize;
        nFeesWithDescendants += modifyFee;
        nCountWithDescendants += modifyCount;
    }
}

CTxMemPool::CTxMemPool(const CFeeRate& _minRelayFee) :
    nTransactionsUpdated(0)
{
    clear();

    // Sanity checks off by default for performance, because otherwise
    // accepting transactions becomes O(N^2) where N is the number
    // of transactions in the pool
    fSanityCheck = false;

    minerPolicyEstimator = new CBlockPolicyEstimator(_minRelayFee);
}

CTxMemPool::~CTxMemPool()
{
    delete minerPolicyEstimator;
}

void CTxMemPool::pruneSpent(const uint256 &hashTx, CCoins &coins)
{
    LOCK(cs);

    std::map<COutPoint, CInPoint>::iterator it = mapNextTx.lower_bound(COutPoint(hashTx, 0));

    // iterate over all COutPoints in mapNextTx whose hash equals the provided hashTx
    while (it != mapNextTx.end() && it->first.hash == hashTx) {
        coins.Spend(it->first.n); // and remove those outputs from coins
        it++;
    }
}

unsigned int CTxMemPool::GetTransactionsUpdated() const
{
    LOCK(cs);
    return nTransactionsUpdated;
}

void CTxMemPool::AddTransactionsUpdated(unsigned int n)
{
    LOCK(cs);
    nTransactionsUpdated += n;
}

bool CTxMemPool::addUnchecked(const uint256& hash, const CTxMemPoolEntry &entry, setEntries &setAncestors, bool fCurrentEstimate)
{
    // Add to memory pool without checking anything.
    // Used by main.cpp AcceptToMemoryPool(), which DOES do
    // all the appropriate checks.
    LOCK(cs);
    indexed_transaction_set::iterator newit = mapTx.insert(entry).first;
    mapLinks.insert(make_pair(newit, TxLinks()));

    // Update cachedInnerUsage to include contained transaction's usage.
    // (When we update the entry for in-mempool parents, memory usage will be
    // further updated.)
    cachedInnerUsage += entry.DynamicMemoryUsage();

    const CTransaction& tx = newit->GetTx();
    std::set<uint256> setParentTransactions;
    for (unsigned int i = 0; i < tx.vin.size(); i++) {
        mapNextTx[tx.vin[i].prevout] = CInPoint(&tx, i);
        setParentTransactions.insert(tx.vin[i].prevout.hash);
    }
    // Don't bother worrying about child transactions of this one.
    // Normal case of a new transaction arriving is that there can't be any
    // children, because such children would be orphans.
    // An exception to that is if a transaction enters that used to be in a block.
    // In that case, our disconnect block logic will call UpdateTransactionsFromBlock
    // to clean up the mess we're leaving here.

    // Update ancestors with information about this tx
    BOOST_FOREACH (const uint256 &phash, setParentTransactions) {
        txiter pit = mapTx.find(phash);
        if (pit != mapTx.end()) {
            UpdateParent(newit, pit, true);
        }
    }
    UpdateAncestorsOf(true, hash, setAncestors);

    nTransactionsUpdated++;
    totalTxSize += entry.GetTxSize();
    minerPolicyEstimator->processTransaction(entry, fCurrentEstimate);

    return true;
}

// TODO: replace this hash with an iterator?
void CTxMemPool::removeUnchecked(const uint256& hash)
{
    indexed_transaction_set::iterator it = mapTx.find(hash);

    BOOST_FOREACH(const CTxIn& txin, it->GetTx().vin)
        mapNextTx.erase(txin.prevout);

    totalTxSize -= it->GetTxSize();
    cachedInnerUsage -= it->DynamicMemoryUsage();
    cachedInnerUsage -= memusage::DynamicUsage(mapLinks[it].parents) + memusage::DynamicUsage(mapLinks[it].children);
    mapLinks.erase(it);
    mapTx.erase(it);
    nTransactionsUpdated++;
    minerPolicyEstimator->removeTx(hash);
}

// Calculates descendants of hash that are not already in setDescendants, and adds to 
// setDescendants. Assumes hash is already a tx in the mempool and setMemPoolChildren
// is correct for tx and all descendants.
// Also assumes that if an entry is in setDescendants already, then all
// in-mempool descendants of it are already in setDescendants as well, so that we
// can save time by not iterating over those entries.
void CTxMemPool::CalculateDescendants(const uint256 &hash, std::set<uint256> &setDescendants)
{
    std::set<uint256> stage;
    if (setDescendants.count(hash) == 0) {
        stage.insert(hash);
    }
    // Traverse down the children of each hash, only adding children that are not
    // accounted for in setDescendants already (because those children have either
    // already been walked, or will be walked in this iteration).
    while (!stage.empty()) {
        setDescendants.insert(stage.begin(), stage.end());
        std::set<uint256> setNext;
        BOOST_FOREACH(const uint256 &stagehash, stage) {
            indexed_transaction_set::iterator it = mapTx.find(stagehash);
            const setEntries &setChildren = GetMemPoolChildren(it);
            BOOST_FOREACH(const txiter &childhash, setChildren) {
                if (!setDescendants.count(childhash->GetTx().GetHash())) {
                    setNext.insert(childhash->GetTx().GetHash());
                }
            }
        }
        stage = setNext;
    }
}

void CTxMemPool::remove(const CTransaction &origTx, std::list<CTransaction>& removed, bool fRecursive)
{
    // Remove transaction from memory pool
    {
        LOCK(cs);
        std::set<uint256> txToRemove;
        if (mapTx.count(origTx.GetHash())) {
            txToRemove.insert(origTx.GetHash());
        } else if (fRecursive) {
            // If recursively removing but origTx isn't in the mempool
            // be sure to remove any children that are in the pool. This can
            // happen during chain re-orgs if origTx isn't re-accepted into
            // the mempool for any reason.
            for (unsigned int i = 0; i < origTx.vout.size(); i++) {
                std::map<COutPoint, CInPoint>::iterator it = mapNextTx.find(COutPoint(origTx.GetHash(), i));
                if (it == mapNextTx.end())
                    continue;
                txToRemove.insert(it->second.ptx->GetHash());
            }
        }
        std::set<uint256> setAllRemoves;
        if (fRecursive) {
            BOOST_FOREACH(const uint256 &hash, txToRemove) {
                CalculateDescendants(hash, setAllRemoves);
            }
        } else {
            setAllRemoves = txToRemove;
        }
        BOOST_FOREACH(const uint256& hash, setAllRemoves) {
            removed.push_back(mapTx.find(hash)->GetTx());
        }
        RemoveStaged(setAllRemoves);
    }
}

void CTxMemPool::removeCoinbaseSpends(const CCoinsViewCache *pcoins, unsigned int nMemPoolHeight)
{
    // Remove transactions spending a coinbase which are now immature
    LOCK(cs);
    list<CTransaction> transactionsToRemove;
    for (indexed_transaction_set::const_iterator it = mapTx.begin(); it != mapTx.end(); it++) {
        const CTransaction& tx = it->GetTx();
        BOOST_FOREACH(const CTxIn& txin, tx.vin) {
            indexed_transaction_set::const_iterator it2 = mapTx.find(txin.prevout.hash);
            if (it2 != mapTx.end())
                continue;
            const CCoins *coins = pcoins->AccessCoins(txin.prevout.hash);
            if (fSanityCheck) assert(coins);
            if (!coins || (coins->IsCoinBase() && ((signed long)nMemPoolHeight) - coins->nHeight < COINBASE_MATURITY)) {
                transactionsToRemove.push_back(tx);
                break;
            }
        }
    }
    BOOST_FOREACH(const CTransaction& tx, transactionsToRemove) {
        list<CTransaction> removed;
        remove(tx, removed, true);
    }
}

void CTxMemPool::removeConflicts(const CTransaction &tx, std::list<CTransaction>& removed)
{
    // Remove transactions which depend on inputs of tx, recursively
    list<CTransaction> result;
    LOCK(cs);
    BOOST_FOREACH(const CTxIn &txin, tx.vin) {
        std::map<COutPoint, CInPoint>::iterator it = mapNextTx.find(txin.prevout);
        if (it != mapNextTx.end()) {
            const CTransaction &txConflict = *it->second.ptx;
            if (txConflict != tx)
            {
                remove(txConflict, removed, true);
            }
        }
    }
}

/**
 * Called when a block is connected. Removes from mempool and updates the miner fee estimator.
 */
void CTxMemPool::removeForBlock(const std::vector<CTransaction>& vtx, unsigned int nBlockHeight,
                                std::list<CTransaction>& conflicts, bool fCurrentEstimate)
{
    LOCK(cs);
    std::vector<CTxMemPoolEntry> entries;
    BOOST_FOREACH(const CTransaction& tx, vtx)
    {
        uint256 hash = tx.GetHash();

        indexed_transaction_set::iterator i = mapTx.find(hash);
        if (i != mapTx.end())
            entries.push_back(*i);
    }
    BOOST_FOREACH(const CTransaction& tx, vtx)
    {
        std::list<CTransaction> dummy;
        remove(tx, dummy, false);
        removeConflicts(tx, conflicts);
        ClearPrioritisation(tx.GetHash());
    }
    // After the txs in the new block have been removed from the mempool, update policy estimates
    minerPolicyEstimator->processBlock(nBlockHeight, entries, fCurrentEstimate);
}

void CTxMemPool::clear()
{
    LOCK(cs);
    mapLinks.clear();
    mapTx.clear();
    mapNextTx.clear();
    totalTxSize = 0;
    cachedInnerUsage = 0;
    bypassedSize = 0;
    ++nTransactionsUpdated;
}

void CTxMemPool::check(const CCoinsViewCache *pcoins) const
{
    if (!fSanityCheck)
        return;

    LogPrint("mempool", "Checking mempool with %u transactions and %u inputs\n", (unsigned int)mapTx.size(), (unsigned int)mapNextTx.size());

    uint64_t checkTotal = 0;
    uint64_t innerUsage = 0;

    CCoinsViewCache mempoolDuplicate(const_cast<CCoinsViewCache*>(pcoins));

    LOCK(cs);
    list<const CTxMemPoolEntry*> waitingOnDependants;
    for (indexed_transaction_set::const_iterator it = mapTx.begin(); it != mapTx.end(); it++) {
        unsigned int i = 0;
        checkTotal += it->GetTxSize();
        innerUsage += it->DynamicMemoryUsage(); 
        const CTransaction& tx = it->GetTx();
        const TxLinks &links = mapLinks.find(it)->second;
        innerUsage += memusage::DynamicUsage(links.parents) + memusage::DynamicUsage(links.children);
        bool fDependsWait = false;
        setEntries setParentCheck;
        BOOST_FOREACH(const CTxIn &txin, tx.vin) {
            // Check that every mempool transaction's inputs refer to available coins, or other mempool tx's.
            indexed_transaction_set::const_iterator it2 = mapTx.find(txin.prevout.hash);
            if (it2 != mapTx.end()) {
                const CTransaction& tx2 = it2->GetTx();
                assert(tx2.vout.size() > txin.prevout.n && !tx2.vout[txin.prevout.n].IsNull());
                fDependsWait = true;
                setParentCheck.insert(it2);
            } else {
                const CCoins* coins = pcoins->AccessCoins(txin.prevout.hash);
                assert(coins && coins->IsAvailable(txin.prevout.n));
            }
            // Check whether its inputs are marked in mapNextTx.
            std::map<COutPoint, CInPoint>::const_iterator it3 = mapNextTx.find(txin.prevout);
            assert(it3 != mapNextTx.end());
            assert(it3->second.ptx == &tx);
            assert(it3->second.n == i);
            i++;
        }
        assert(setParentCheck == GetMemPoolParents(it));
        // Check children against mapNextTx
        CTxMemPool::setEntries setChildrenCheck;
        std::map<COutPoint, CInPoint>::const_iterator iter = mapNextTx.lower_bound(COutPoint(it->GetTx().GetHash(), 0));
        int64_t childSizes = 0;
        CAmount childFees = 0;
        for (; iter != mapNextTx.end() && iter->first.hash == it->GetTx().GetHash(); ++iter) {
            txiter childit = mapTx.find(iter->second.ptx->GetHash());
            if (setChildrenCheck.insert(childit).second) {
                childSizes += childit->GetTxSize();
                childFees += childit->GetFee();
            }
        }
        assert(setChildrenCheck == GetMemPoolChildren(it));
        // Also check to make sure size/fees is greater than sum with immediate children.
        // just a sanity check, not definitive that this calc is correct...
        // also check that the size is less than the size of the entire mempool.
        if (!it->IsDirty()) {
            assert(it->GetSizeWithDescendants() >= childSizes + int64_t(it->GetTxSize()));
            assert(it->GetFeesWithDescendants() >= childFees + it->GetFee());
        } else {
            assert(it->GetSizeWithDescendants() == int64_t(it->GetTxSize()));
            assert(it->GetFeesWithDescendants() == it->GetFee());
        }
        assert(it->GetFeesWithDescendants() >= 0);

        if (fDependsWait)
            waitingOnDependants.push_back(&(*it));
        else {
            CValidationState state;
            assert(CheckInputs(tx, state, mempoolDuplicate, false, 0, false, NULL));
            UpdateCoins(tx, state, mempoolDuplicate, 1000000);
        }
    }
    unsigned int stepsSinceLastRemove = 0;
    while (!waitingOnDependants.empty()) {
        const CTxMemPoolEntry* entry = waitingOnDependants.front();
        waitingOnDependants.pop_front();
        CValidationState state;
        if (!mempoolDuplicate.HaveInputs(entry->GetTx())) {
            waitingOnDependants.push_back(entry);
            stepsSinceLastRemove++;
            assert(stepsSinceLastRemove < waitingOnDependants.size());
        } else {
            assert(CheckInputs(entry->GetTx(), state, mempoolDuplicate, false, 0, false, NULL));
            UpdateCoins(entry->GetTx(), state, mempoolDuplicate, 1000000);
            stepsSinceLastRemove = 0;
        }
    }
    for (std::map<COutPoint, CInPoint>::const_iterator it = mapNextTx.begin(); it != mapNextTx.end(); it++) {
        uint256 hash = it->second.ptx->GetHash();
        indexed_transaction_set::const_iterator it2 = mapTx.find(hash);
        const CTransaction& tx = it2->GetTx();
        assert(it2 != mapTx.end());
        assert(&tx == it->second.ptx);
        assert(tx.vin.size() > it->second.n);
        assert(it->first == it->second.ptx->vin[it->second.n].prevout);
    }

    assert(totalTxSize == checkTotal);
    assert(innerUsage == cachedInnerUsage);
}

void CTxMemPool::queryHashes(vector<uint256>& vtxid)
{
    vtxid.clear();

    LOCK(cs);
    vtxid.reserve(mapTx.size());
    for (indexed_transaction_set::iterator mi = mapTx.begin(); mi != mapTx.end(); ++mi)
        vtxid.push_back(mi->GetTx().GetHash());
}

bool CTxMemPool::lookup(uint256 hash, CTransaction& result) const
{
    LOCK(cs);
    indexed_transaction_set::const_iterator i = mapTx.find(hash);
    if (i == mapTx.end()) return false;
    result = i->GetTx();
    return true;
}

CFeeRate CTxMemPool::estimateFee(int nBlocks) const
{
    LOCK(cs);
    return minerPolicyEstimator->estimateFee(nBlocks);
}
double CTxMemPool::estimatePriority(int nBlocks) const
{
    LOCK(cs);
    return minerPolicyEstimator->estimatePriority(nBlocks);
}

bool
CTxMemPool::WriteFeeEstimates(CAutoFile& fileout) const
{
    try {
        LOCK(cs);
        fileout << 109900; // version required to read: 0.10.99 or later
        fileout << CLIENT_VERSION; // version that wrote the file
        minerPolicyEstimator->Write(fileout);
    }
    catch (const std::exception&) {
        LogPrintf("CTxMemPool::WriteFeeEstimates(): unable to write policy estimator data (non-fatal)\n");
        return false;
    }
    return true;
}

bool
CTxMemPool::ReadFeeEstimates(CAutoFile& filein)
{
    try {
        int nVersionRequired, nVersionThatWrote;
        filein >> nVersionRequired >> nVersionThatWrote;
        if (nVersionRequired > CLIENT_VERSION)
            return error("CTxMemPool::ReadFeeEstimates(): up-version (%d) fee estimate file", nVersionRequired);

        LOCK(cs);
        minerPolicyEstimator->Read(filein);
    }
    catch (const std::exception&) {
        LogPrintf("CTxMemPool::ReadFeeEstimates(): unable to read policy estimator data (non-fatal)\n");
        return false;
    }
    return true;
}

void CTxMemPool::PrioritiseTransaction(const uint256 hash, const string strHash, double dPriorityDelta, const CAmount& nFeeDelta)
{
    {
        LOCK(cs);
        std::pair<double, CAmount> &deltas = mapDeltas[hash];
        deltas.first += dPriorityDelta;
        deltas.second += nFeeDelta;
    }
    LogPrintf("PrioritiseTransaction: %s priority += %f, fee += %d\n", strHash, dPriorityDelta, FormatMoney(nFeeDelta));
}

void CTxMemPool::ApplyDeltas(const uint256 hash, double &dPriorityDelta, CAmount &nFeeDelta)
{
    LOCK(cs);
    std::map<uint256, std::pair<double, CAmount> >::iterator pos = mapDeltas.find(hash);
    if (pos == mapDeltas.end())
        return;
    const std::pair<double, CAmount> &deltas = pos->second;
    dPriorityDelta += deltas.first;
    nFeeDelta += deltas.second;
}

void CTxMemPool::ClearPrioritisation(const uint256 hash)
{
    LOCK(cs);
    mapDeltas.erase(hash);
}

bool CTxMemPool::HasNoInputsOf(const CTransaction &tx) const
{
    LOCK(cs);
    for (unsigned int i = 0; i < tx.vin.size(); i++)
        if (exists(tx.vin[i].prevout.hash))
            return false;
    return true;
}

CCoinsViewMemPool::CCoinsViewMemPool(CCoinsView *baseIn, CTxMemPool &mempoolIn) : CCoinsViewBacked(baseIn), mempool(mempoolIn) { }

bool CCoinsViewMemPool::GetCoins(const uint256 &txid, CCoins &coins) const {
    // If an entry in the mempool exists, always return that one, as it's guaranteed to never
    // conflict with the underlying cache, and it cannot have pruned entries (as it contains full)
    // transactions. First checking the underlying cache risks returning a pruned entry instead.
    CTransaction tx;
    if (mempool.lookup(txid, tx)) {
        coins = CCoins(tx, MEMPOOL_HEIGHT);
        return true;
    }
    return (base->GetCoins(txid, coins) && !coins.IsPruned());
}

bool CCoinsViewMemPool::HaveCoins(const uint256 &txid) const {
    return mempool.exists(txid) || base->HaveCoins(txid);
}

size_t CTxMemPool::DynamicMemoryUsage() const {
    LOCK(cs);
    // Estimate the overhead of mapTx to be 9 pointers + an allocation, as no exact formula for boost::multi_index_contained is implemented.
    return memusage::MallocUsage(sizeof(CTxMemPoolEntry) + 9 * sizeof(void*)) * mapTx.size() + memusage::DynamicUsage(mapNextTx) + memusage::DynamicUsage(mapDeltas) + memusage::DynamicUsage(mapLinks) + cachedInnerUsage;
}

size_t CTxMemPool::GuessDynamicMemoryUsage(const CTxMemPoolEntry& entry) const {
    setEntries s;
    return memusage::MallocUsage(sizeof(CTxMemPoolEntry) + 9 * sizeof(void*)) + entry.DynamicMemoryUsage() + (memusage::IncrementalDynamicUsage(mapNextTx) + memusage::IncrementalDynamicUsage(s)) * entry.GetTx().vin.size() + memusage::IncrementalDynamicUsage(mapLinks);
}

bool CTxMemPool::StageTrimToSize(size_t sizelimit, const CTxMemPoolEntry& toadd, CAmount nFeesReserved,
                                 std::set<uint256>& stage, CAmount& nFeesRemoved) {
    std::set<uint256> protect;
    BOOST_FOREACH(const CTxIn& in, toadd.GetTx().vin) {
        protect.insert(in.prevout.hash);
    }

    size_t incUsage = GuessDynamicMemoryUsage(toadd);
    size_t expsize = DynamicMemoryUsage() + incUsage; // Track the expected resulting memory usage of the mempool.
    if (expsize <= sizelimit) {
        return true;
    }
    size_t sizeToTrim = std::min(expsize - sizelimit, incUsage);
    return TrimMempool(sizeToTrim, protect, nFeesReserved, toadd.GetTxSize(), toadd.GetFee(), true, 10, stage, nFeesRemoved);
}

void CTxMemPool::SurplusTrim(int multiplier, CFeeRate minRelayRate, size_t usageToTrim) {
    CFeeRate excessRate(multiplier * minRelayRate.GetFeePerK());
    std::set<uint256> noprotect;
    CAmount nFeesRemoved = 0;
    std::set<uint256> stageTrimDelete;
    size_t sizeToTrim = usageToTrim / 4;  //Conservatively assume we have transactions at least 1/4 the size of the mempool space they've taken
    if (TrimMempool(usageToTrim, noprotect, 0, sizeToTrim, excessRate.GetFee(sizeToTrim), false, 100, stageTrimDelete, nFeesRemoved)) {
        size_t oldUsage = DynamicMemoryUsage();
        size_t txsToDelete = stageTrimDelete.size();
        RemoveStaged(stageTrimDelete);
        size_t curUsage = DynamicMemoryUsage();
        LogPrint("mempool", "Removing %u transactions (%ld total usage) using periodic trim from reserve size\n", txsToDelete, oldUsage - curUsage);
    }
}

bool CTxMemPool::TrimMempool(size_t sizeToTrim, std::set<uint256> &protect, CAmount nFeesReserved, size_t sizeToUse, CAmount feeToUse,
                             bool mustTrimAllSize, int iterextra, std::set<uint256>& stage, CAmount &nFeesRemoved) {
    size_t usageRemoved = 0;
    indexed_transaction_set::nth_index<1>::type::reverse_iterator it = mapTx.get<1>().rbegin();
    int fails = 0; // Number of mempool transactions iterated over that were not included in the stage.
    int iterperfail = 10;
    int itertotal = 0;
    int failmax = 10; //Try no more than 10 starting transactions
    seed_insecure_rand();
    // Iterate from lowest feerate to highest feerate in the mempool:
    while (usageRemoved < sizeToTrim && it != mapTx.get<1>().rend()) {
        if (insecure_rand()%10) {
            // Only try 1/10 of the transactions so we don't get stuck on the same long chains
            it++;
            continue;
        }
        const uint256& hash = it->GetTx().GetHash();
        if (stage.count(hash)) {
            // If the transaction is already staged for deletion, we know its descendants are already processed, so skip it.
            it++;
            continue;
        }
        if ((double)it->GetFee() * sizeToUse > (double)feeToUse * it->GetTxSize()) {
            // If the transaction's feerate is worse than what we're looking for, nothing else we will iterate over
            // could improve the staged set. If we don't have an acceptable solution by now, bail out.
            break;
        }
        std::deque<uint256> todo; // List of hashes that we still need to process (descendants of 'hash').
        std::set<uint256> now; // Set of hashes that will need to be added to stage if 'hash' is included.
        CAmount nowfee = 0; // Sum of the fees in 'now'.
        size_t nowsize = 0; // Sum of the tx sizes in 'now'.
        size_t nowusage = 0; // Sum of the memory usages of transactions in 'now'.
        todo.push_back(it->GetTx().GetHash()); // Add 'hash' to the todo list, to initiate processing its children.
        bool good = true; // Whether including 'hash' (and all its descendants) is a good idea.
        // Iterate breadth-first over all descendants of transaction with hash 'hash'.
        while (!todo.empty()) {
            uint256 hashnow = todo.front();
            if (protect.count(hashnow)) {
                // If this transaction is in the protected set, we're done with 'hash'.
                good = false;
                break;
            }
            itertotal++; // We only count transactions we actually had to go find in the mempool.
            if (itertotal > iterextra + iterperfail*(fails+1)) {
                good = false;
                break;
            }
            const CTxMemPoolEntry* origTx = &*mapTx.find(hashnow);
            nowfee += origTx->GetFee();
            if (nFeesReserved + nFeesRemoved + nowfee > feeToUse) {
                // If this pushes up to the total fees deleted too high, we're done with 'hash'.
                good = false;
                break;
            }
            todo.pop_front();
            // Add 'hashnow' to the 'now' set, and update its statistics.
            now.insert(hashnow);
            nowusage += GuessDynamicMemoryUsage(*origTx);
            nowsize += origTx->GetTxSize();
            // Find dependencies of 'hashnow' and them to todo.
            std::map<COutPoint, CInPoint>::iterator iter = mapNextTx.lower_bound(COutPoint(hashnow, 0));
            while (iter != mapNextTx.end() && iter->first.hash == hashnow) {
                const uint256& nexthash = iter->second.ptx->GetHash();
                if (!(stage.count(nexthash) || now.count(nexthash))) {
                    todo.push_back(nexthash);
                }
                iter++;
            }
        }
        if (good && (double)nowfee * sizeToUse > (double)feeToUse * nowsize) {
            // The new transaction's feerate is below that of the set we're removing.
            good = false;
        }
        if (good) {
            stage.insert(now.begin(), now.end());
            nFeesRemoved += nowfee;
            usageRemoved += nowusage;
        } else {
            fails++;
            if (fails > failmax) {
                // Bail out after traversing failmax transactions that are not acceptable.
                break;
            }
        }
        it++;
    }
    //We've added all we can.  Is it enough?
    if (mustTrimAllSize && usageRemoved < sizeToTrim)
        return false;
    if (stage.empty() && sizeToTrim > 0)
        return false;
    return true;
}

void CTxMemPool::RemoveStaged(std::set<uint256>& stage) {
    UpdateForRemoveFromMempool(stage);
    BOOST_FOREACH(const uint256& hash, stage) {
        removeUnchecked(hash);
    }
}

int CTxMemPool::Expire(int64_t time) {
    LOCK(cs);
    indexed_transaction_set::nth_index<2>::type::iterator it = mapTx.get<2>().begin();
    std::set<uint256> toremove;
    while (it != mapTx.get<2>().end() && it->GetTime() < time) {
        toremove.insert(it->GetTx().GetHash());
        it++;
    }
    std::set<uint256> stage;
    BOOST_FOREACH(const uint256 &hash, toremove) {
        CalculateDescendants(hash, stage);
    }
    RemoveStaged(stage);
    return stage.size();
}

bool CTxMemPool::addUnchecked(const uint256&hash, const CTxMemPoolEntry &entry, bool fCurrentEstimate)
{
    setEntries setAncestors;
    uint64_t nNoLimit = std::numeric_limits<uint64_t>::max();
    std::string dummy;
    CalculateMemPoolAncestors(entry, setAncestors, nNoLimit, nNoLimit, nNoLimit, nNoLimit, dummy);
    return addUnchecked(hash, entry, setAncestors, fCurrentEstimate);
}

void CTxMemPool::UpdateChild(txiter entry, txiter child, bool add)
{
    setEntries s;
    if (add && mapLinks[entry].children.insert(child).second) {
        cachedInnerUsage += memusage::IncrementalDynamicUsage(s);
    } else if (!add && mapLinks[entry].children.erase(child)) {
        cachedInnerUsage -= memusage::IncrementalDynamicUsage(s);
    }
}

void CTxMemPool::UpdateParent(txiter entry, txiter parent, bool add)
{
    setEntries s;
    if (add && mapLinks[entry].parents.insert(parent).second) {
        cachedInnerUsage += memusage::IncrementalDynamicUsage(s);
    } else if (!add && mapLinks[entry].parents.erase(parent)) {
        cachedInnerUsage -= memusage::IncrementalDynamicUsage(s);
    }
}

const CTxMemPool::setEntries & CTxMemPool::GetMemPoolParents(txiter entry) const
{
    assert (entry != mapTx.end());
    return mapLinks.find(entry)->second.parents;
}

const CTxMemPool::setEntries & CTxMemPool::GetMemPoolChildren(txiter entry) const
{
    assert (entry != mapTx.end());
    return mapLinks.find(entry)->second.children;
}
