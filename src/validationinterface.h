// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_VALIDATIONINTERFACE_H
#define BITCOIN_VALIDATIONINTERFACE_H

#include <primitives/transaction.h> // CTransaction(Ref)

#include <functional>
#include <memory>

class CBlock;
class CBlockIndex;
struct CBlockLocator;
class CBlockIndex;
class CConnman;
class CReserveScript;
class CValidationInterface;
class CValidationState;
class uint256;
class CScheduler;
class CTxMemPool;
enum class MemPoolRemovalReason;
class MempoolInterface;

// These functions dispatch to one or all registered wallets

/** Register a wallet to receive updates from core */
void RegisterValidationInterface(CValidationInterface* pwalletIn);
/** Unregister a wallet from core */
void UnregisterValidationInterface(CValidationInterface* pwalletIn);
/** Register a listener to receive updates from mempool */
void RegisterMempoolInterface(MempoolInterface* listener);
/** Unregister a listener from mempool */
void UnregisterMempoolInterface(MempoolInterface* listener);
/** Unregister all listeners from core and mempool */
void UnregisterAllValidationAndMempoolInterfaces();
/**
 * Pushes a function to callback onto the notification queue, guaranteeing any
 * callbacks generated prior to now are finished when the function is called.
 *
 * Be very careful blocking on func to be called if any locks are held -
 * validation interface clients may not be able to make progress as they often
 * wait for things like cs_main, so blocking until func is called with cs_main
 * will result in a deadlock (that DEBUG_LOCKORDER will miss).
 */
void CallFunctionInValidationInterfaceQueue(std::function<void ()> func);
/**
 * This is a synonym for the following, which asserts certain locks are not
 * held:
 *     std::promise<void> promise;
 *     CallFunctionInValidationInterfaceQueue([&promise] {
 *         promise.set_value();
 *     });
 *     promise.get_future().wait();
 */
void SyncWithValidationInterfaceQueue();

/**
 * An interface to get callbacks about transactions entering and leaving
 * mempool.
 *
 * Any class which extends both MempoolInterface and CValidationInterface will
 * see all callbacks across both well-ordered (see individual callback text for
 * details on the order guarantees).
 *
 * Callbacks called on a background thread have a separate order from those
 * called on the thread generating the callbacks.
 */
class MempoolInterface {
public:
    /**
     * Information about a newly-added-to-mempool transaction.
     */
    struct NewMempoolTransactionInfo {
        //! A shared pointer to the transaction which was added.
        CTransactionRef m_tx;
        //! The fee the added transaction paid
        CAmount m_fee;
        /**
         * The virtual transaction size.
         *
         * This is a policy field which considers the sigop cost of the
         * transaction as well as its weight, and reinterprets it as bytes.
         *
         * It is the primary metric by which the mining algorithm selects
         * transactions.
         */
        int64_t m_virtual_transaction_size;
        //! Whether this transaction should be considered for fee estimation
        bool m_valid_for_estimation; // TODO: Move this logic to CBlockPolicyEstimator
    };
protected:
    /**
     * Protected destructor so that instances can only be deleted by derived classes.
     * If that restriction is no longer desired, this should be made public and virtual.
     */
    ~MempoolInterface() = default;
    /**
     * Notifies listeners of a transaction having been added to mempool.
     *
     * Called on a background thread.
     */
    virtual void TransactionAddedToMempool(const NewMempoolTransactionInfo& info, const std::vector<CTransactionRef>& txn_replaced) {}
    /**
     * Notifies listeners of a transaction leaving mempool.
     *
     * This only fires for transactions which leave mempool because of expiry,
     * size limiting, or reorg (changes in lock times/coinbase maturity). This
     * does not include any transactions which are included in
     * MempoolUpdatedForBlockConnect or TransactionAddedToMempool(txn_replaced)
     *
     * reason == REORG is not ordered with BlockDisconnected/BlockDisconnected!
     *
     * Note that in some rare cases (eg mempool limiting) a
     * TransactionRemovedFromMempool event may fire with no corresponding
     * TransactionAddedToMempool event.
     * (TODO: remove this edge case)
     *
     * Called on a background thread.
     */
    virtual void TransactionRemovedFromMempool(const CTransactionRef &ptx, MemPoolRemovalReason reason) {}
    /**
     * Notifies listeners of mempool being updated for a block connection.
     *
     * Entries in tx_removed_in_block represent transactions which were in the
     * block and thus removed from the mempool. The tx_removed_in_block txn are
     * as they appear in the block, and may have different witnesses from the
     * version which was previously in the mempool.
     *
     * This callback fires prior to BlockConnected in CValidationInterface.
     *
     * Called on a background thread.
     */
    virtual void MempoolUpdatedForBlockConnect(const std::vector<CTransactionRef>& tx_removed_in_block, const std::vector<CTransactionRef>& tx_removed_conflicted) {}
    friend void ::RegisterMempoolInterface(MempoolInterface*);
    friend void ::UnregisterMempoolInterface(MempoolInterface*);
};

/**
 * An interface to get callbacks about block connection/disconnection.
 *
 * Any class which extends both MempoolInterface and CValidationInterface will
 * see all callbacks across both well-ordered (see individual callback text for
 * details on the order guarantees).
 *
 * Callbacks called on a background thread have a separate order from those
 * called on the thread generating the callbacks.
 */
class CValidationInterface {
protected:
    /**
     * Protected destructor so that instances can only be deleted by derived classes.
     * If that restriction is no longer desired, this should be made public and virtual.
     */
    ~CValidationInterface() = default;
    /**
     * Notifies listeners of updated block chain tip
     *
     * Is called after a series of BlockConnected/BlockDisconnected events once
     * the chain has made forward progress and is now at the best-known-tip.
     *
     * If a block is found to be invalid, this event may trigger without
     * forward-progress, only to trigger again soon thereafter.
     * (TODO: remove this edge case) *
     *
     * Called on a background thread.
     */
    virtual void UpdatedBlockTip(const CBlockIndex *pindexNew, const CBlockIndex *pindexFork, bool fInitialDownload) {}
    /**
     * Notifies listeners of a block being connected.
     *
     * Called on a background thread.
     */
    virtual void BlockConnected(const std::shared_ptr<const CBlock> &block, const CBlockIndex *pindex) {}
    /**
     * Notifies listeners of a block being disconnected
     *
     * The ordering of BlockDisconnected and TransactionRemovedFromMempool
     * (for transactions removed due to memory constraints or lock time/
     * coinbase maturity chenges during the disconnection/reorg) is undefined,
     * and the TransactionRemovedFromMempool callbacks may occur *both* before
     * and after BlockDisconnected/BlockConnected calls!
     *
     * Called on a background thread.
     */
    virtual void BlockDisconnected(const std::shared_ptr<const CBlock> &block) {}
    /**
     * Notifies listeners of the new active block chain on-disk.
     *
     * Because flushing to disk happens in batches, this can happen
     * significantly after BlockConnected/UpdatedBlockTip calls (and always is
     * ordered after BlockConnected/UpdatedBlockTip).
     *
     * Called on a background thread.
     */
    virtual void SetBestChain(const CBlockLocator &locator) {}
    /**
     * Notifies listeners about an inventory item being seen on the network.
     *
     * Called on a background thread.
     */
    virtual void Inventory(const uint256 &hash) {}
    /** Tells listeners to broadcast their data. */
    virtual void ResendWalletTransactions(int64_t nBestBlockTime, CConnman* connman) {}
    /**
     * Notifies listeners of a block validation result.
     * If the provided CValidationState IsValid, the provided block
     * is guaranteed to be the current best block at the time the
     * callback was generated (not necessarily now)
     */
    virtual void BlockChecked(const CBlock&, const CValidationState&) {}
    /**
     * Notifies listeners that a block which builds directly on our current tip
     * has been received and connected to the headers tree, though not validated yet */
    virtual void NewPoWValidBlock(const CBlockIndex *pindex, const std::shared_ptr<const CBlock>& block) {};
    friend void ::RegisterValidationInterface(CValidationInterface*);
    friend void ::UnregisterValidationInterface(CValidationInterface*);
};

struct MainSignalsInstance;
class CMainSignals {
private:
    std::unique_ptr<MainSignalsInstance> m_internals;

    friend void ::RegisterValidationInterface(CValidationInterface*);
    friend void ::UnregisterValidationInterface(CValidationInterface*);
    friend void ::RegisterMempoolInterface(MempoolInterface*);
    friend void ::UnregisterMempoolInterface(MempoolInterface*);
    friend void ::UnregisterAllValidationAndMempoolInterfaces();
    friend void ::CallFunctionInValidationInterfaceQueue(std::function<void ()> func);

public:
    /** Register a CScheduler to give callbacks which should run in the background (may only be called once) */
    void RegisterBackgroundSignalScheduler(CScheduler& scheduler);
    /** Unregister a CScheduler to give callbacks which should run in the background - these callbacks will now be dropped! */
    void UnregisterBackgroundSignalScheduler();
    /** Call any remaining callbacks on the calling thread */
    void FlushBackgroundCallbacks();

    size_t CallbacksPending();

    void UpdatedBlockTip(const CBlockIndex *, const CBlockIndex *, bool fInitialDownload);
    void TransactionAddedToMempool(const MempoolInterface::NewMempoolTransactionInfo &, const std::shared_ptr<std::vector<CTransactionRef>>& txn_replaced);
    void MempoolUpdatedForBlockConnect(std::vector<CTransactionRef>&& tx_removed_in_block, std::vector<CTransactionRef>&& tx_removed_conflicted);
    void MempoolEntryRemoved(CTransactionRef tx, MemPoolRemovalReason reason);
    void BlockConnected(const std::shared_ptr<const CBlock> &, const CBlockIndex *pindex);
    void BlockDisconnected(const std::shared_ptr<const CBlock> &);
    void SetBestChain(const CBlockLocator &);
    void Inventory(const uint256 &);
    void Broadcast(int64_t nBestBlockTime, CConnman* connman);
    void BlockChecked(const CBlock&, const CValidationState&);
    void NewPoWValidBlock(const CBlockIndex *, const std::shared_ptr<const CBlock>&);
};

CMainSignals& GetMainSignals();

#endif // BITCOIN_VALIDATIONINTERFACE_H
