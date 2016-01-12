/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/bgp_peer_close.h"


#include "bgp/bgp_log.h"
#include "bgp/bgp_peer_membership.h"
#include "bgp/bgp_peer_types.h"
#include "bgp/bgp_route.h"

#define PEER_CLOSE_MANAGER_LOG(msg) \
    BGP_LOG_PEER(Event, peer_, SandeshLevel::SYS_INFO, BGP_LOG_FLAG_ALL,       \
        BGP_PEER_DIR_NA, "PeerCloseManager: State " << GetStateName(state_) << \
        ", CloseAgain? " << (close_again_ ? "Yes" : "No") << ": " << msg);

#define SET_STATE(state)                                                  \
    do {                                                                  \
        PEER_CLOSE_MANAGER_LOG("Move to state " << GetStateName(state));  \
        state_ = state;                                                   \
    } while (false)

// Create an instance of PeerCloseManager with back reference to parent IPeer
PeerCloseManager::PeerCloseManager(IPeer *peer) :
        peer_(peer), stale_timer_(NULL), state_(NONE), close_again_(false) {
    SET_STATE(NONE);
    stats_.init++;
    if (peer->server())
        stale_timer_ = TimerManager::CreateTimer(*peer->server()->ioservice(),
                                                 "Graceful Restart StaleTimer");
}

PeerCloseManager::~PeerCloseManager() {
    TimerManager::DeleteTimer(stale_timer_);
}

const std::string PeerCloseManager::GetStateName(State state) const {
    switch (state) {
    case NONE:
        return "NONE";
    case GR_TIMER:
        return "GR_TIMER";
    case STALE:
        return "STALE";
    case SWEEP:
        return "SWEEP";
    case DELETE:
        return "DELETE";
    }
    assert(false);
    return "";
}

// Trigger closure of an IPeer
//
// Graceful                                    close_state_: NONE
// 1. RibIn Stale Marking and Ribout deletion  close_state_: STALE
// 2. StateMachine restart and GR timer start  close_state_: GR_TIMER
//
// Peer IsReady() in timer callback
// 3. RibIn Sweep and Ribout Generation        close_state_: SWEEP
// 4. UnregisterPeerComplete                   close_state_: NONE
//
// Peer not IsReady() in timer callback
// Goto step A
// Close() call during any state other than NONE: Cancel GR and goto step A
//
// NonGraceful                                 close_state_ = * (except DELETE)
// A. RibIn deletion and Ribout deletion       close_state_ = DELETE
// B. UnregisterPeerComplete => Peers delete/StateMachine restart
//                                             close_state_ = NONE
void PeerCloseManager::Close() {
    tbb::recursive_mutex::scoped_lock lock(mutex_);

    stats_.close++;

    // Ignore nested closures
    if (close_again_) {
        stats_.nested++;
        PEER_CLOSE_MANAGER_LOG("Nested close calls ignored");
        return;
    }

    switch (state_) {
    case NONE:
        break;

    case STALE:
    case GR_TIMER:
    case SWEEP:
    case DELETE:
        PEER_CLOSE_MANAGER_LOG("Nested close triggered");
        close_again_ = true;
        if (state_ != GR_TIMER)
            return;
        stale_timer_->Cancel();
        break;
    }
    ProcessClosure();
}

// Process RibIn staling related activities during peer closure
// Return true if at least ome time is started, false otherwise
void PeerCloseManager::StartRestartTimer(int time) {
    stale_timer_->Cancel();

    PEER_CLOSE_MANAGER_LOG("GR Timer started to fire after " << time <<
                           " seconds");
    stale_timer_->Start(time,
        boost::bind(&PeerCloseManager::RestartTimerCallback, this));
}

bool PeerCloseManager::RestartTimerCallback() {
    tbb::recursive_mutex::scoped_lock lock(mutex_);

    PEER_CLOSE_MANAGER_LOG("GR Timer callback started");
    if (state_ == GR_TIMER)
        ProcessClosure();
    return false;
}

// Route stale timer callback. If the peer has come back up, sweep routes for
// those address families that are still active. Delete the rest
void PeerCloseManager::ProcessClosure() {

    // If the peer is back up and this address family is still supported,
    // sweep old paths which may not have come back in the new session
    switch (state_) {
        case NONE:
            if (!peer_->peer_close()->IsCloseGraceful()) {
                SET_STATE(DELETE);
                stats_.deletes++;
            } else {
                SET_STATE(STALE);
                stats_.stale++;
            }
            break;
        case GR_TIMER:
            if (peer_->IsReady() && !close_again_) {
                SET_STATE(SWEEP);
                stats_.sweep++;
            } else {
                SET_STATE(DELETE);
                stats_.deletes++;
            }
            break;

        case STALE:
        case SWEEP:
        case DELETE:
            assert(false);
            return;
    }

    if (state_ == DELETE)
        peer_->peer_close()->CustomClose();
    peer_->server()->membership_mgr()->UnregisterPeer(peer_,
        boost::bind(&PeerCloseManager::GetCloseAction, this, _1, state_),
        boost::bind(&PeerCloseManager::UnregisterPeerComplete, this, _1, _2));
}

bool PeerCloseManager::IsCloseInProgress() {
    tbb::recursive_mutex::scoped_lock lock(mutex_);
    return state_ != NONE;
}

// Concurrency: Runs in the context of the BGP peer rib membership task.
//
// Close process for this peer in terms of walking RibIns and RibOuts are
// complete. Do the final cleanups necessary and notify interested party
void PeerCloseManager::UnregisterPeerComplete(IPeer *ipeer, BgpTable *table) {
    tbb::recursive_mutex::scoped_lock lock(mutex_);

    assert(state_ == STALE || state_ == SWEEP || state_ == DELETE);

    if (state_ == DELETE) {
        peer_->peer_close()->Delete();
        SET_STATE(NONE);
        stats_.init++;
        close_again_ = false;
        return;
    }

    if (close_again_) {
        SET_STATE(DELETE);
        stats_.deletes++;
        peer_->peer_close()->CustomClose();
        peer_->server()->membership_mgr()->UnregisterPeer(peer_,
            boost::bind(&PeerCloseManager::GetCloseAction, this, _1, state_),
            boost::bind(&PeerCloseManager::UnregisterPeerComplete, this,
                        _1, _2));
        return;
    }

    if (state_ == SWEEP) {
        SET_STATE(NONE);
        stats_.init++;
        return;
    }

    PEER_CLOSE_MANAGER_LOG("Close procedure completed");

    // If any stale timer has to be launched, then to wait for some time hoping
    // for the peer (and the paths) to come back up.
    peer_->peer_close()->CloseComplete();
    StartRestartTimer(PeerCloseManager::kDefaultGracefulRestartTime * 1000);
    SET_STATE(GR_TIMER);
    stats_.gr_timer++;
}

// Get the type of RibIn close action at start (Not during graceful restart
// timer callback, where in we walk the Rib again to sweep the routes)
int PeerCloseManager::GetCloseAction(IPeerRib *peer_rib, State state) {
    int action = MembershipRequest::INVALID;

    if ((state == STALE || state == DELETE) && peer_rib->IsRibOutRegistered())
        action |= static_cast<int>(MembershipRequest::RIBOUT_DELETE);

    if (!peer_rib->IsRibInRegistered())
        return action;

    // If graceful restart timer is already running, then this is a second
    // close before previous restart has completed. Abort graceful restart
    // and delete the routes instead
    switch (state_) {
    case NONE:
        break;
    case STALE:
        action |= static_cast<int>(MembershipRequest::RIBIN_STALE);
        break;
    case GR_TIMER:
        break;
    case SWEEP:
        action |= static_cast<int>(MembershipRequest::RIBIN_SWEEP);
        break;
    case DELETE:
        action |= static_cast<int>(MembershipRequest::RIBIN_DELETE);
        break;
    }
    return (action);
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

    if (action == MembershipRequest::INVALID)
        return;

    // Process all paths sourced from this peer_. Multiple paths could exist
    // in ecmp cases.
    for (Route::PathList::iterator it = rt->GetPathList().begin(), next = it;
         it != rt->GetPathList().end(); it = next) {
        next++;

        // Skip secondary paths.
        if (dynamic_cast<BgpSecondaryPath *>(it.operator->()))
            continue;
        BgpPath *path = static_cast<BgpPath *>(it.operator->());
        if (path->GetPeer() != peer_)
            continue;

        switch (action) {
            case MembershipRequest::RIBIN_SWEEP:

                // Stale paths must be deleted
                if (!path->IsStale())
                    return;
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
                           path->GetPathId(), path->GetFlags(),
                           path->GetLabel());
    }

    return;
}

void PeerCloseManager::FillCloseInfo(BgpNeighborResp *resp) {
    tbb::recursive_mutex::scoped_lock lock(mutex_);

    PeerCloseInfo peer_close_info;
    peer_close_info.state = GetStateName(state_);
    peer_close_info.close_again = close_again_;
    peer_close_info.init = stats_.init;
    peer_close_info.close = stats_.close;
    peer_close_info.nested = stats_.nested;
    peer_close_info.deletes = stats_.deletes;
    peer_close_info.stale = stats_.stale;
    peer_close_info.sweep = stats_.sweep;
    peer_close_info.gr_timer = stats_.gr_timer;

    resp->set_peer_close_info(peer_close_info);
}
