#pragma once

#include "common/types.h"
#include <set>
#include <string>

namespace minidb {

/*
 * Transaction: represents a single database transaction.
 * Tracks its ID, state, and the set of locks it holds.
 */
class Transaction {
public:
    explicit Transaction(txn_id_t txn_id)
        : txn_id_(txn_id), state_(TxnState::ACTIVE) {}

    txn_id_t  GetTxnId()  const { return txn_id_; }
    TxnState  GetState()  const { return state_; }
    void      SetState(TxnState s) { state_ = s; }

    // Lock tracking (for Strict 2PL — released only on commit/abort)
    void AddLock(const std::string& resource, LockMode mode) {
        held_locks_.insert(resource);
    }

    void ClearLocks() { held_locks_.clear(); }

    const std::set<std::string>& GetHeldLocks() const { return held_locks_; }

private:
    txn_id_t               txn_id_;
    TxnState               state_;
    std::set<std::string>  held_locks_;  // resource names (table names)
};

} // namespace minidb
