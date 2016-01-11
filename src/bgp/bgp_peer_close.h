/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef SRC_BGP_BGP_PEER_CLOSE_H_
#define SRC_BGP_BGP_PEER_CLOSE_H_

#include <tbb/recursive_mutex.h>

#include "base/timer.h"
#include "base/util.h"
#include "base/queue_task.h"
#include "db/db_table_walker.h"
#include "bgp/ipeer.h"

class IPeerRib;
class BgpRoute;
class BgpTable;

// PeerCloseManager
//
// Manager close process of an IPeer (And hence should support both BgpPeers
// and XmppPeers)
//
// Among other things, RibIns and RibOuts of peers must be closed/deleted
// completely before a peer can be completely closed/deleted. This class
// provides this capability.
//
// RibIn and RibOut close are handled by invoking Unregister request with
// PeerRibMembershipManager class.
//
// Once RibIns and RibOuts are processed, notification callback function is
// invoked to signal the completion of close process
//
class PeerCloseManager {
public:
    enum State { NONE, STALE, GR_TIMER, SWEEP, DELETE };

    static const int kDefaultGracefulRestartTime = 60;  // Seconds

    // thread: bgp::StateMachine
    explicit PeerCloseManager(IPeer *peer);
    virtual ~PeerCloseManager();

    IPeer *peer() { return peer_; }

    void Close();
    bool RestartTimerCallback();
    void UnregisterPeerComplete(IPeer *ipeer, BgpTable *table);
    int GetCloseAction(IPeerRib *peer_rib, State state);
    void ProcessRibIn(DBTablePartBase *root, BgpRoute *rt, BgpTable *table,
                      int action_mask);
    bool IsCloseInProgress();
    void StartRestartTimer(int time);

private:
    friend class PeerCloseManagerTest;

    void ProcessClosure();

    IPeer *peer_;
    Timer *stale_timer_;
    State state_;
    bool close_again_;
    tbb::recursive_mutex mutex_;
};

#endif  // SRC_BGP_BGP_PEER_CLOSE_H_
