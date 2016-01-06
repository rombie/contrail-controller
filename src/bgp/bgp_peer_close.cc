/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/bgp_peer_close.h"


#include "bgp/bgp_log.h"
#include "bgp/bgp_peer_membership.h"
#include "bgp/bgp_route.h"

// IPeer CloseProcess
//
// Graceful                                    close_state_ = NONE
// 1. RibIn Stale Marking and Ribout deletion  close_state_ = STALE
// 2. StateMachine restart and GR timer start  close_state_ = GR_TIMER
//
// Peer IsReady() in timer callback
// 3. RibIn Sweep and Ribout Generation        close_state_ = SWEEP
// 4. UnregisterPeerComplete                   close_state_ = NONE
//
// Peer not IsReady() in timer callback
// Goto step A
//
// Close() call during any state other than NONE: Cancel GR and goto step A
//
//
//
// NonGraceful                                 close_state_ = * (except DELETE)
// A. RibIn deletion and Ribout deletion       close_state_ = DELETE
// B. UnregisterPeerComplete => Peers deletion/StateMachine restart close_state_ = NONE

// Create an instance of PeerCloseManager with back reference to parent IPeer
PeerCloseManager::PeerCloseManager(IPeer *peer) :
        peer_(peer),
        stale_timer_(NULL),
        state_(NONE),
        close_again_(false) {
    if (peer->server()) {
        stale_timer_ = TimerManager::CreateTimer(*peer->server()->ioservice(),
                                                 "Graceful Restart StaleTimer");
    }
}

PeerCloseManager::~PeerCloseManager() {
    TimerManager::DeleteTimer(stale_timer_);
}

// Process RibIn staling related activities during peer closure
// Return true if at least ome time is started, false otherwise
void PeerCloseManager::StartStaleTimer() {
    stale_timer_->Start(PeerCloseManager::kDefaultGracefulRestartTime * 1000,
        boost::bind(&PeerCloseManager::StaleTimerCallback, this));
    state_ = GR_TIMER;
}

void PeerCloseManager::FireStaleTimerNow() {
    if (state_ != GR_TIMER)
        return;

    // Cancel the old one and start a new one to trigger asap
    stale_timer_->Cancel();
    stale_timer_->Start(0,
            boost::bind(&PeerCloseManager::StaleTimerCallback, this));
}

// Route stale timer callback. If the peer has come back up, sweep routes for
// those address families that are still active. Delete the rest
bool PeerCloseManager::StaleTimerCallback() {
    // Protect this method from possible parallel new close request
    tbb::recursive_mutex::scoped_lock lock(mutex_);

    if (state_ != GR_TIMER)
        return false;
    stale_timer_->Cancel();

    // If the peer is back up and this address family is still supported,
    // sweep old paths which may not have come back in the new session
    state_ = peer_->IsReady() ? SWEEP : DELETE;

    peer_->server()->membership_mgr()->UnregisterPeer(peer_,
        boost::bind(&PeerCloseManager::GetActionAtStart, this, _1),
        boost::bind(&PeerCloseManager::UnregisterPeerComplete, this, _1, _2));
    return false;
}

bool PeerCloseManager::IsCloseInProgress() {
    tbb::recursive_mutex::scoped_lock lock(mutex_);
    return state_ != NONE;
}

//
// Concurrency: Runs in the context of the BGP peer rib membership task.
//
// Close process for this peer in terms of walking RibIns and RibOuts are
// complete. Do the final cleanups necessary and notify interested party
//
void PeerCloseManager::UnregisterPeerComplete(IPeer *ipeer, BgpTable *table) {
    tbb::recursive_mutex::scoped_lock lock(mutex_);

    assert(state_ != NONE);
    IPeerClose *peer_close = peer_->peer_close();

    if (state_ == DELETE) {
        peer_close->DeleteComplete();
        return;
    }

    if (!close_again_ && state_ == SWEEP) {
        state_ = NONE;
        return;
    }

    if (close_again_) {
        state_ = DELETE;

        // Start process to delete this peer's RibIns and RibOuts. Peer can be
        // deleted only after these (asynchronous) activities are complete
        peer_->server()->membership_mgr()->UnregisterPeer(peer_,
            boost::bind(&PeerCloseManager::GetActionAtStart, this, _1),
            boost::bind(&PeerCloseManager::UnregisterPeerComplete, this,
                        _1, _2));
        return;
    }

    BGP_LOG_PEER(Event, peer_, SandeshLevel::SYS_INFO, BGP_LOG_FLAG_ALL,
                 BGP_PEER_DIR_NA, "Close procedure completed");

    // If any stale timer has to be launched, then to wait for some time hoping
    // for the peer (and the paths) to come back up.
    if (state_ == STALE) {
        peer_close->CloseComplete();
        StartStaleTimer();
    }
}

//
// Get the type of RibIn close action at start (Not during graceful restart
// timer callback, where in we walk the Rib again to sweep the routes)
//
int PeerCloseManager::GetActionAtStart(IPeerRib *peer_rib) {
    int action = MembershipRequest::INVALID;

    if (state_ == DELETE && peer_rib->IsRibOutRegistered())
        action |= static_cast<int>(MembershipRequest::RIBOUT_DELETE);

    if (!peer_rib->IsRibInRegistered())
        return action;

    // If graceful restart timer is already running, then this is a second
    // close before previous restart has completed. Abort graceful restart
    // and delete the routes instead
    if (state_ == DELETE) {
        action |= static_cast<int>(MembershipRequest::RIBIN_DELETE);
    } else if (state_ == STALE) {
        action |= static_cast<int>(MembershipRequest::RIBIN_STALE);
    } else if (state_ == STALE) {
        action |= static_cast<int>(MembershipRequest::RIBIN_SWEEP);
    }
    return (action);
}

void PeerCloseManager::Close() {
    tbb::recursive_mutex::scoped_lock lock(mutex_);

    // Ignore nested closures
    if (close_again_)
        return;

    if (state_ != NONE) {
        close_again_ = true;
        BGP_LOG_PEER(Event, peer_, SandeshLevel::SYS_INFO, BGP_LOG_FLAG_ALL,
                     BGP_PEER_DIR_NA, "Close procedure already in progress");
        FireStaleTimerNow();
        return;
    }

    IPeerClose *peer_close = peer_->peer_close();
    state_ = peer_close->IsCloseGraceful() ? STALE : DELETE;

    BGP_LOG_PEER(Event, peer_, SandeshLevel::SYS_INFO, BGP_LOG_FLAG_ALL,
                 BGP_PEER_DIR_NA, "Close procedure initiated");
    peer_close->CustomClose();

    // Start process to delete this peer's RibIns and RibOuts. Peer can be
    // deleted only after these (asynchronous) activities are complete.
    peer_->server()->membership_mgr()->UnregisterPeer(peer_,
        boost::bind(&PeerCloseManager::GetActionAtStart, this, _1),
        boost::bind(&PeerCloseManager::UnregisterPeerComplete, this, _1, _2));
}

// For graceful-restart, we take mark-and-sweep approach instead of directly
// deleting the paths. In the first walk, local-preference is lowered so that
// the paths are least preferred and they are marked stale. After some time, if
// the peer session does not come back up, we delete all the paths and the peer
// itself. If the session did come back up, we flush only those paths that were
// not learned again in the new session.

//
// Concurrency: Runs in the context of the DB Walker task launched by peer rib
// membership manager
//
// DBWalker callback routine for each of the RibIn prefix.
//
void PeerCloseManager::ProcessRibIn(DBTablePartBase *root, BgpRoute *rt,
                                    BgpTable *table, int action_mask) {
    DBRequest::DBOperation oper;
    BgpAttrPtr attrs;
    MembershipRequest::Action  action;

    // Look for the flags that we care about
    action = static_cast<MembershipRequest::Action>(action_mask &
                (MembershipRequest::RIBIN_STALE |
                 MembershipRequest::RIBIN_SWEEP |
                 MembershipRequest::RIBIN_DELETE));

    if (action == MembershipRequest::INVALID) return;

    // Process all paths sourced from this peer_. Multiple paths could exist
    // in ecmp cases.
    for (Route::PathList::iterator it = rt->GetPathList().begin(), next = it;
         it != rt->GetPathList().end(); it = next) {
        next++;

        // Skip secondary paths.
        if (dynamic_cast<BgpSecondaryPath *>(it.operator->())) continue;
        BgpPath *path = static_cast<BgpPath *>(it.operator->());
        if (path->GetPeer() != peer_) continue;

        switch (action) {
            case MembershipRequest::RIBIN_SWEEP:

                // Stale paths must be deleted
                if (!path->IsStale()) {
                    return;
                }
                oper = DBRequest::DB_ENTRY_DELETE;
                attrs = NULL;
                break;

            case MembershipRequest::RIBIN_DELETE:

                // This path must be deleted. Hence attr is not required
                oper = DBRequest::DB_ENTRY_DELETE;
                attrs = NULL;
                break;

            case MembershipRequest::RIBIN_STALE:

                // This path must be marked for staling. Update the local
                // preference and update the route accordingly
                oper = DBRequest::DB_ENTRY_ADD_CHANGE;

                // Update attrs with maximum local preference so that this path
                // is least preferred
                // TODO(ananth): Check for the right local-pref value to use
                attrs = peer_->server()->attr_db()->\
                        ReplaceLocalPreferenceAndLocate(path->GetAttr(), 1);
                path->SetStale();
                break;

            default:
                return;
        }

        // Feed the route modify/delete request to the table input process
        table->InputCommon(root, rt, path, peer_, NULL, oper, attrs,
            path->GetPathId(), path->GetFlags(), path->GetLabel());
    }

    return;
}
