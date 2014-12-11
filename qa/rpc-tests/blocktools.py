# blocktools.py - utilities for manipulating blocks and transactions
#
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#

from mininode import *

# Create a block (with regtest difficulty)
def create_block(hashprev, coinbase, nTime=None):
    block = CBlock()
    if nTime is None:
        import time
        block.nTime = int(time.time())
    else:
        block.nTime = nTime
    block.hashPrevBlock = hashprev
    block.nBits = 0x207fffff # Will break after a difficulty adjustment...
    block.vtx.append(coinbase)
    block.hashMerkleRoot = block.calc_merkle_root()
    block.calc_sha256()
    return block

counter=0
# Create an anyone-can-spend coinbase transaction, assuming no miner fees
def create_coinbase():
    global counter
    coinbase = CTransaction()
    coinbase.vin.append(CTxIn(COutPoint(0, 0xffffffff), "%2s" % counter, 0xffffffff))
    counter += 1
    coinbaseoutput = CTxOut()
    coinbaseoutput.nValue = 50*100000000
    coinbaseoutput.scriptPubKey = ""
    coinbase.vout = [ coinbaseoutput ]
    coinbase.calc_sha256()
    return coinbase

# Create a transaction with an anyone-can-spend output
def create_transaction(prevtx, sig, value):
    tx = CTransaction()
    for i in range(len(prevtx.vout)):
        tx.vin.append(CTxIn(COutPoint(prevtx.sha256, i), sig, 0xffffffff))
    tx.vout.append(CTxOut(value, ""))
    tx.calc_sha256()
    return tx

