/*
 * Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/bgp_mvpn.h"

#include <boost/foreach.hpp>

#include "base/task_annotations.h"
#include "bgp/ermvpn/ermvpn_route.h"
#include "bgp/ermvpn/ermvpn_table.h"
#include "bgp/bgp_multicast.h"
#include "bgp/bgp_server.h"
#include "bgp/extended-community/vrf_route_import.h"
#include "bgp/mvpn/mvpn_table.h"
#include "bgp/routing-instance/path_resolver.h"
#include "bgp/routing-instance/routing_instance.h"
#include "bgp/rtarget/rtarget_address.h"

using std::make_pair;
using std::ostringstream;
using std::string;

MvpnState::MvpnState(const SG &sg) :
        sg_(sg), global_ermvpn_tree_rt_(NULL), refcount_(0) {
}

MvpnState::~MvpnState() {
    assert(!global_ermvpn_tree_rt_);
    assert(spmsi_routes_received_.empty());
    assert(cjoin_routes_received_.empty());
}

class MvpnProjectManager::DeleteActor : public LifetimeActor {
public:
    explicit DeleteActor(MvpnProjectManager *manager)
        : LifetimeActor(manager->table_->routing_instance()->server()->
                lifetime_manager()), manager_(manager) {
    }
    virtual ~DeleteActor() {
    }

    virtual bool MayDelete() const {
        return true;
    }

    virtual void Shutdown() {
    }

    virtual void Destroy() {
        manager_->table_->DestroyMvpnProjectManager();
    }

private:
    MvpnProjectManager *manager_;
};

MvpnProjectManager::MvpnProjectManager(ErmVpnTable *table)
        : table_(table),
          listener_id_(DBTable::kInvalidId),
          table_delete_ref_(this, table->deleter()) {
    deleter_.reset(new DeleteActor(this));
    Initialize();
}

MvpnProjectManager::~MvpnProjectManager() {
}

void MvpnProjectManager::Initialize() {
    assert(!table_->IsMaster());
    AllocPartitions();

    listener_id_ = table_->Register(
        boost::bind(&MvpnProjectManager::RouteListener, this, _1, _2),
        "MvpnProjectManager");
}

void MvpnProjectManager::Terminate() {
    table_->Unregister(listener_id_);
    FreePartitions();
}

void MvpnProjectManager::AllocPartitions() {
    for (int part_id = 0; part_id < table_->PartitionCount(); part_id++)
        partitions_.push_back(new MvpnProjectManagerPartition(this, part_id));
}

void MvpnProjectManager::FreePartitions() {
    for (size_t part_id = 0; part_id < partitions_.size(); part_id++) {
        delete partitions_[part_id];
    }
    partitions_.clear();
}

MvpnProjectManagerPartition *MvpnProjectManager::GetPartition(int part_id) {
    return partitions_[part_id];
}

const MvpnProjectManagerPartition *MvpnProjectManager::GetPartition(
        int part_id) const {
    return partitions_[part_id];
}

void MvpnProjectManager::ManagedDelete() {
    deleter_->Delete();
}

MvpnProjectManagerPartition::MvpnProjectManagerPartition(
        MvpnProjectManager *manager, int part_id)
    : manager_(manager), part_id_(part_id) {
}

MvpnProjectManagerPartition::~MvpnProjectManagerPartition() {
}

MvpnState *MvpnProjectManagerPartition::CreateState(const SG &sg) {
    MvpnState *state = new MvpnState(sg);
    assert(states_.insert(make_pair(sg, state)).second);
    return state;
}

MvpnState *MvpnProjectManagerPartition::LocateState(const SG &sg) {
    MvpnState *mvpn_state = GetState(sg);
    return mvpn_state ?: CreateState(sg);
}

const MvpnState *MvpnProjectManagerPartition::GetState(const SG &sg) const {
    StateMap::const_iterator iter = states_.find(sg);
    return iter != states_.end() ?  iter->second : NULL;
}

MvpnState *MvpnProjectManagerPartition::GetState(const SG &sg) {
    StateMap::iterator iter = states_.find(sg);
    return iter != states_.end() ?  iter->second : NULL;
}

void MvpnProjectManagerPartition::DeleteState(MvpnState *mvpn_state) {
    assert(mvpn_state->refcount_);
    if (--mvpn_state->refcount_)
        return;
    states_.erase(mvpn_state->sg());
    delete mvpn_state;
}

MvpnNeighbor::MvpnNeighbor() : asn_(0), vrf_id_(0), external_(false) {
}

MvpnNeighbor::MvpnNeighbor(const IpAddress &address, uint32_t asn,
        uint16_t vrf_id, bool external) :
    address_(address), asn_(asn), vrf_id_(vrf_id), external_(external) {
    ostringstream os;
    os << address << ":" << asn << ":" << vrf_id << ":" << external;
    name_ = os.str();
}

string MvpnNeighbor::ToString() const {
    return name_;
}

const IpAddress &MvpnNeighbor::address() const {
    return address_;
}

uint16_t MvpnNeighbor::vrf_id() const {
    return vrf_id_;
}

uint32_t MvpnNeighbor::asn() const {
    return asn_;
}

bool MvpnNeighbor::operator==(const MvpnNeighbor &rhs) const {
    return address_ == rhs.address_ && asn_ == rhs.asn_ &&
           vrf_id_ == rhs.vrf_id_ && external_ == rhs.external_;
}

bool MvpnManager::FindNeighbor(const IpAddress &address, MvpnNeighbor *nbr)
        const {
    tbb::reader_writer_lock::scoped_lock_read lock(neighbors_mutex_);

    NeighborsMap::const_iterator iter = neighbors_.find(address);
    if (iter != neighbors_.end()) {
        *nbr = iter->second;
        return true;
    }
    return false;
}

MvpnState::SG::SG(const Ip4Address &source, const Ip4Address &group) :
    source(IpAddress(source)), group(IpAddress(group)) {
}

MvpnState::SG::SG(const ErmVpnRoute *route) :
        source(route->GetPrefix().source()),
        group(route->GetPrefix().group()) {
}

MvpnState::SG::SG(const MvpnRoute *route) :
        source(route->GetPrefix().source()),
        group(route->GetPrefix().group()) {
}

MvpnState::SG::SG(const IpAddress &source, const IpAddress &group) :
    source(source), group(group) {
}

bool MvpnState::SG::operator<(const SG &other)  const {
    return (source < other.source) ?  true : (group < other.source);
}

const MvpnState::SG &MvpnState::sg() const {
    return sg_;
}

ErmVpnRoute *MvpnState::global_ermvpn_tree_rt() {
    return global_ermvpn_tree_rt_;
}

const ErmVpnRoute *MvpnState::global_ermvpn_tree_rt() const {
    return global_ermvpn_tree_rt_;
}

MvpnRoute *MvpnState::spmsi_rt() {
    return spmsi_rt_;
}

const MvpnRoute *MvpnState::spmsi_rt() const {
    return spmsi_rt_;
}

const MvpnState::RoutesSet &MvpnState::spmsi_routes() const {
    return spmsi_routes_received_;
}

MvpnState::RoutesSet &MvpnState::spmsi_routes() {
    return spmsi_routes_received_;
}

const MvpnState::RoutesSet &MvpnState::cjoin_routes_received() const {
    return cjoin_routes_received_;
}

MvpnState::RoutesSet *MvpnState::cjoin_routes_received() {
    return &cjoin_routes_received_;
}

void MvpnState::set_global_ermvpn_tree_rt(ErmVpnRoute *global_ermvpn_tree_rt) {
    global_ermvpn_tree_rt_ = global_ermvpn_tree_rt;
}

void MvpnState::set_spmsi_rt(MvpnRoute *spmsi_rt) {
    spmsi_rt_ = spmsi_rt;
}

MvpnDBState::MvpnDBState() : state(NULL), route(NULL) {
    if (state)
        state->refcount_++;
}

MvpnDBState::MvpnDBState(MvpnState *state) : state(state) , route(NULL) {
    if (state)
        state->refcount_++;
}

MvpnDBState::MvpnDBState(MvpnRoute *route) : state(NULL) , route(route) {
    if (state)
        state->refcount_++;
}

MvpnDBState::MvpnDBState(MvpnState *state, MvpnRoute *route) :
        state(state) , route(route) {
    if (state)
        state->refcount_++;
}

class MvpnManager::DeleteActor : public LifetimeActor {
public:
    explicit DeleteActor(MvpnManager *manager)
        : LifetimeActor(manager->table_->routing_instance()->server()->
                lifetime_manager()), manager_(manager) {
    }
    virtual ~DeleteActor() {
    }

    virtual bool MayDelete() const {
        return true;
    }

    virtual void Shutdown() {
    }

    virtual void Destroy() {
        manager_->table_->DestroyManager();
    }

private:
    MvpnManager *manager_;
};

MvpnManager::MvpnManager(MvpnTable *table)
        : table_(table),
          listener_id_(DBTable::kInvalidId),
          table_delete_ref_(this, table->deleter()) {
    deleter_.reset(new DeleteActor(this));
    Initialize();
}

MvpnManager::~MvpnManager() {
}

MvpnTable *MvpnManager::table() {
    return table_;
}

const MvpnTable *MvpnManager::table() const {
    return table_;
}

int MvpnManager::listener_id() const {
    return listener_id_;
}

PathResolver *MvpnManager::path_resolver() {
    return table_->path_resolver();
}

PathResolver *MvpnManager::path_resolver() const {
    return table_->path_resolver();
}

void MvpnManager::Terminate() {
    // TODO(Ananth) FindPath and delete the two auto AD routes.
    MvpnRoute *type1_route = table_->FindType1ADRoute();
    if  (type1_route)
        type1_route->Delete();

    MvpnRoute *type2_route = table_->FindType2ADRoute();
    if  (type2_route)
        type2_route->Delete();

    // Delete Type2 Inter AS Auto-Discovery Route.
    table_->FindType2ADRoute()->Delete();
    table_->Unregister(listener_id_);
    FreePartitions();
}

LifetimeActor *MvpnManager::deleter() {
    return deleter_.get();
}

const LifetimeActor *MvpnManager::deleter() const {
    return deleter_.get();
}

void MvpnManager::ManagedDelete() {
    deleter_->Delete();
}

bool MvpnManager::deleted() const {
    return deleter_->IsDeleted();
}

void MvpnManager::AllocPartitions() {
    for (int part_id = 0; part_id < table_->PartitionCount(); part_id++)
        partitions_.push_back(new MvpnManagerPartition(this, part_id));
}

void MvpnManager::FreePartitions() {
    for (size_t part_id = 0; part_id < partitions_.size(); part_id++) {
        delete partitions_[part_id];
    }
    partitions_.clear();
}

MvpnManagerPartition *MvpnManager::GetPartition(int part_id) {
    return partitions_[part_id];
}

const MvpnManagerPartition *MvpnManager::GetPartition(int part_id) const {
    return GetPartition(part_id);
}

void MvpnManager::NotifyAllRoutes() {
    table_->NotifyAllEntries();
}

MvpnTable *MvpnManagerPartition::table() {
    return manager_->table();
}

const MvpnTable *MvpnManagerPartition::table() const {
    return manager_->table();
}

int MvpnManagerPartition::listener_id() const {
    return manager_->listener_id();
}

MvpnManagerPartition::MvpnManagerPartition(MvpnManager *manager, int part_id)
    : manager_(manager), part_id_(part_id) {
}

MvpnManagerPartition::~MvpnManagerPartition() {
}

MvpnProjectManagerPartition *
MvpnManagerPartition::GetProjectManagerPartition() {
    MvpnProjectManager *project_manager = manager_->GetProjectManager();
    return project_manager ? project_manager->GetPartition(part_id_) : NULL;
}

const MvpnProjectManagerPartition *
MvpnManagerPartition::GetProjectManagerPartition() const {
    MvpnProjectManager *project_manager = manager_->GetProjectManager();
    return project_manager ? project_manager->GetPartition(part_id_) : NULL;
}

MvpnProjectManager *MvpnManager::GetProjectManager() {
    return table_->GetProjectManager();
}

const MvpnProjectManager *MvpnManager::GetProjectManager() const {
    return table_->GetProjectManager();
}

int MvpnProjectManager::listener_id() const {
    return listener_id_;
}

int MvpnProjectManagerPartition::listener_id() const {
    return manager_->listener_id();
}

MvpnState *MvpnManagerPartition::LocateState(MvpnRoute *rt) {
    MvpnProjectManagerPartition *project_manager_partition =
        GetProjectManagerPartition();
    if (!project_manager_partition)
        return NULL;
    MvpnState::SG sg = MvpnState::SG(rt->GetPrefix().sourceIpAddress(),
                                     rt->GetPrefix().groupIpAddress());
    return project_manager_partition->GetState(sg);
}

const MvpnState *MvpnManagerPartition::GetState(MvpnRoute *rt) const {
    const MvpnProjectManagerPartition *project_manager_partition =
        GetProjectManagerPartition();
    if (!project_manager_partition)
        return NULL;
    MvpnState::SG sg = MvpnState::SG(rt->GetPrefix().sourceIpAddress(),
                                     rt->GetPrefix().groupIpAddress());
    return project_manager_partition->GetState(sg);
}

MvpnState *MvpnManagerPartition::GetState(MvpnRoute *rt) {
    return const_cast<MvpnState *>(
        static_cast<const MvpnManagerPartition *>(this)->GetState(rt));
}

const MvpnState *MvpnManagerPartition::GetState(ErmVpnRoute *rt) const {
    const MvpnProjectManagerPartition *project_manager_partition =
        GetProjectManagerPartition();
    if (!project_manager_partition)
        return NULL;
    MvpnState::SG sg = MvpnState::SG(rt->GetPrefix().source(),
                                     rt->GetPrefix().group());
    return project_manager_partition->GetState(sg);
}

MvpnState *MvpnManagerPartition::GetState(ErmVpnRoute *rt) {
    return const_cast<MvpnState *>(
        static_cast<const MvpnManagerPartition *>(this)->GetState(rt));
}

void MvpnManagerPartition::DeleteState(MvpnState *state) {
    MvpnProjectManagerPartition *project_manager_partition =
        GetProjectManagerPartition();
    if (!project_manager_partition)
        return;
    project_manager_partition->DeleteState(state);
}

ErmVpnTable *MvpnProjectManager::table() {
    return table_;
}

const ErmVpnTable *MvpnProjectManager::table() const {
    return table_;
}

ErmVpnTable *MvpnProjectManagerPartition::table() {
    return manager_->table();
}

const ErmVpnTable *MvpnProjectManagerPartition::table() const {
    return manager_->table();
}

////////////////////////////////////////////////////////////////////////////////

// Initialize MvpnManager by allcating one MvpnManagerPartition for each DB
// partition, and register a route listener for the MvpnTable.
void MvpnManager::Initialize() {
    assert(!table_->IsMaster());
    AllocPartitions();

    listener_id_ = table_->Register(
        boost::bind(&MvpnManager::RouteListener, this, _1, _2),
        "MvpnManager");

    // Originate Type1 Intra AS Auto-Discovery Route.
    table_->LocateType1ADRoute();

    // Originate Type2 Inter AS Auto-Discovery Route.
    table_->LocateType2ADRoute();
}

// MvpnTable route listener callback function.
//
// Process changes (create/update/delete) to all different types of MvpnRoute.
void MvpnManager::RouteListener(DBTablePartBase *tpart, DBEntryBase *db_entry) {
    CHECK_CONCURRENCY("db::DBTable");

    MvpnRoute *route = dynamic_cast<MvpnRoute *>(db_entry);
    assert(route);

    MvpnManagerPartition *partition = partitions_[tpart->index()];

    // Process Type1 Intra-AS and Type2 Inter-AS routes.
    if (route->GetPrefix().type() == MvpnPrefix::IntraASPMSIADRoute ||
            route->GetPrefix().type() == MvpnPrefix::InterASPMSIADRoute) {
        UpdateNeighbor(route);
        return;
    }

    // Process Type3 S-PMSI route.
    if (route->GetPrefix().type() == MvpnPrefix::SPMSIADRoute) {
        partition->ProcessType3SPMSIRoute(route);
        return;
    }

    // Process Type7 C-Join route.
    if (route->GetPrefix().type() == MvpnPrefix::SourceTreeJoinRoute) {
        if (partition->ProcessType7SourceTreeJoinRoute(route)) {
            route->NotifyOrDelete();
        }
        return;
    }

    // Process Type4 LeafAD route.
    if (route->GetPrefix().type() == MvpnPrefix::LeafADRoute) {
        partition->ProcessType4LeafADRoute(route);
        return;
    }
}

// Update MVPN neighbor list with create/delete/update of auto-discovery routes.
//
// Protect access to neighbors_ map with a mutex as the same be 'read' off other
// DB tasks in parallel. (Type-1 and Type-2 do not carrry any <S,G> information.
void MvpnManager::UpdateNeighbor(MvpnRoute *route) {
    // TODO(Ananth) Shouldn't this be best-path's nexthop address ?
    IpAddress address = route->GetPrefix().originatorIpAddress();
    RouteDistinguisher rd = route->GetPrefix().route_distinguisher();

    // Check if an entry is already present.
    MvpnNeighbor old_neighbor;
    bool found = FindNeighbor(address, &old_neighbor);

    if (!route->IsValid()) {
        if (found) {
            tbb::reader_writer_lock::scoped_lock lock(neighbors_mutex_);
            neighbors_.erase(address);
            NotifyAllRoutes();
        }
        return;
    }

    MvpnNeighbor neighbor(address, route->GetPrefix().asn(), rd.GetVrfId(),
        route->GetPrefix().type() == MvpnPrefix::InterASPMSIADRoute);

    // Ignore if there is no change.
    if (found && old_neighbor == neighbor)
        return;

    tbb::reader_writer_lock::scoped_lock lock(neighbors_mutex_);
    if (found)
        neighbors_.erase(address);
    assert(neighbors_.insert(make_pair(address, neighbor)).second);

    // TODO(Ananth) Only need to re-evaluate all type-7 join routes.
    NotifyAllRoutes();
}

bool MvpnProjectManagerPartition::IsUsableGlobalTreeRootRoute(
        ErmVpnRoute *ermvpn_route) const {
    if (!ermvpn_route || !ermvpn_route->IsUsable())
        return false;
    if (!table()->tree_manager())
        return false;
    ErmVpnRoute *global_rt = table()->tree_manager()->GetPartition(part_id_)->
        GetGlobalTreeRootRoute(ermvpn_route);
    return global_rt == ermvpn_route;
}

// ErmVpnTable route listener callback function.
//
// Process changes (create/update/delete) to GlobalErmVpnRoute in vrf.ermvpn.0
void MvpnProjectManager::RouteListener(DBTablePartBase *tpart,
        DBEntryBase *db_entry) {
    CHECK_CONCURRENCY("db::DBTable");
    MvpnProjectManagerPartition *partition = GetPartition(tpart->index());
    partition->RouteListener(db_entry);
}

void MvpnProjectManagerPartition::RouteListener(DBEntryBase *db_entry) {
    ErmVpnRoute *ermvpn_route = dynamic_cast<ErmVpnRoute *>(db_entry);
    assert(ermvpn_route);

    // We only care about global tree routes for mvpn stitching.
    if (ermvpn_route->GetPrefix().type() != ErmVpnPrefix::GlobalTreeRoute)
        return;

    MvpnDBState *mvpn_dbstate = dynamic_cast<MvpnDBState *>(
        ermvpn_route->GetState(table(), listener_id()));

    if (!IsUsableGlobalTreeRootRoute(ermvpn_route)) {
        if (!mvpn_dbstate)
            return;
        MvpnState *mvpn_state = mvpn_dbstate->state;
        mvpn_state->set_global_ermvpn_tree_rt(NULL);

        // Notify all originated t-4 routes for PMSI re-computation.
        BOOST_FOREACH(MvpnRoute *route, mvpn_state->spmsi_routes()) {
            route->Notify();
        }
        ermvpn_route->ClearState(table(), listener_id());
        return;
    }

    MvpnState *mvpn_state;
    if (!mvpn_dbstate) {
        MvpnState::SG sg(ermvpn_route);
        mvpn_state = LocateState(sg);
        mvpn_dbstate = new MvpnDBState(mvpn_state);
        ermvpn_route->SetState(table(), listener_id(), mvpn_dbstate);
    } else {
        mvpn_state = mvpn_dbstate->state;
    }

    // Note down current usable ermvpn route for stitching to mvpn.
    mvpn_dbstate->state->set_global_ermvpn_tree_rt(ermvpn_route);

    // Notify all originated t-4 routes for PMSI re-computation.
    BOOST_FOREACH(MvpnRoute *route, mvpn_state->spmsi_routes()) {
        route->Notify();
    }
}

RouteDistinguisher MvpnManager::GetSourceRouteDistinguisher(
    const BgpPath *path) const {
    MvpnNeighbor neighbor;
    if (!FindResolvedNeighbor(path, &neighbor))
        return RouteDistinguisher();

    // Form source id based on the neighbor address and neighbor vrf id.
    return RouteDistinguisher(neighbor.address().to_v4().to_ulong(),
                              neighbor.vrf_id());
}

bool MvpnManager::FindResolvedNeighbor(const BgpPath *path,
        MvpnNeighbor *neighbor) const {
    const BgpAttr *attr = path->GetAttr();
    if (!attr)
        return false;

    if (!attr->ext_community())
        return false;

    bool vrf_route_import_found = false;
    ExtCommunity::ExtCommunityValue rt_import;

    // Use rt-import from the resolved path as export route-target.
    BOOST_FOREACH(const ExtCommunity::ExtCommunityValue &value,
                  attr->ext_community()->communities()) {
        if (ExtCommunity::is_vrf_route_import(value)) {
            vrf_route_import_found = true;
            rt_import = value;
            break;
        }
    }

    if (!vrf_route_import_found)
        return false;

    VrfRouteImport vrf_import(rt_import);

    // Find if the resolved path points to an active Mvpn neighbor based on the
    // IP address encoded inside the vrf import route target extended community.
    if (!FindNeighbor(vrf_import.GetIPv4Address(), neighbor))
        return false;
    return true;
}

bool MvpnManagerPartition::ProcessType7SourceTreeJoinRoute(MvpnRoute *join_rt) {
    MvpnDBState *mvpn_dbstate = dynamic_cast<MvpnDBState *>(
        join_rt->GetState(table(), listener_id()));

    if (!mvpn_dbstate && !join_rt->IsUsable())
        return false;

    // TODO(Ananth) Check if there is active sender route present before
    // originating SPMSI route towards the receivers.
    if (!join_rt->IsUsable()) {
        MvpnState *state = GetState(join_rt);
        if (state)
            state->cjoin_routes_received()->erase(join_rt);

        // DB State is maintained for any S-PMSI route originated as a Sender
        // for the received type7 join routes. If such a route was originated
        // before, delete the same as there is no more receiver interested to
        // receive multicast traffic for this C-<S,G>.
        if (mvpn_dbstate) {
            // Delete any S-PMSI route originated earlier as there is no
            // interested receivers for this route (S,G).
            if (mvpn_dbstate->route) {
                BgpPath *path = mvpn_dbstate->route->FindPath(
                    BgpPath::Local, 0);
                if (path)
                    mvpn_dbstate->route->DeletePath(path);
                mvpn_dbstate->route->NotifyOrDelete();
                mvpn_dbstate->route = NULL;
                if (state)
                    state->spmsi_rt_ = NULL;
            }
            join_rt->ClearState(table(), listener_id());
            return true;
        }
        return false;
    }

    MvpnState *state = LocateState(join_rt);
    const BgpPath *path = join_rt->BestPath();
    assert(!path);

    if (!mvpn_dbstate) {
        mvpn_dbstate = new MvpnDBState(state);
        join_rt->SetState(table(), listener_id(), mvpn_dbstate);
    }

    // If the path is a secondary path, it implies the sender side. This would
    // be for both the cases, where in routes are received over bgp (in this
    // case primary path will be bgp.mvpn.0) and in case of local replication
    // (in this case, primary path will be another vrf.mvpn.0). In either of
    // these cases, S-PMSI path needs to be originated if not already done so.
    if (dynamic_cast<const BgpSecondaryPath *>(path)) {
        // Originate/Update S-PMSI route towards the receivers.
        MvpnRoute *spmsi_rt = mvpn_dbstate->route;
        if (!spmsi_rt) {
            spmsi_rt = table()->LocateType3SPMSIRoute(join_rt);
            mvpn_dbstate->route = spmsi_rt;
            state->set_spmsi_rt(spmsi_rt);
        } else {
            BgpPath *old_path = spmsi_rt->FindPath(BgpPath::Local, 0);

            // Path already exists!
            if (old_path)
                return false;
        }
        BgpPath *path = new BgpPath(NULL, 0, BgpPath::Local,
                                    join_rt->BestPath()->GetAttr(), 0, 0, 0);
        spmsi_rt->InsertPath(path);
        return true;
    }

    // This is case in the receiver side, where in Usable join routes have been
    // added/modified in a vrf.mvpn.0 table.
    state->cjoin_routes_received()->insert(join_rt);
    return true;
}

// Process changes to Type3 S-PMSI routes by originating or deleting Type 4
// Lead AD paths as appropriate.
void MvpnManagerPartition::ProcessType3SPMSIRoute(MvpnRoute *spmsi_rt) {
    // Retrieve any state associcated with this S-PMSI route.
    MvpnDBState *mvpn_dbstate = dynamic_cast<MvpnDBState *>(
        spmsi_rt->GetState(table(), listener_id()));

    MvpnRoute *leaf_ad_route = NULL;
    if (!spmsi_rt->IsUsable()) {
        if (!mvpn_dbstate)
            return;
        MvpnState *mvpn_state = GetState(spmsi_rt);
        assert(mvpn_dbstate->state == mvpn_state);

        // Check if a type4 LeadAD path was already originated before for this
        // S-PMSI path. If so, delete it as the S-PMSI path is no nonger usable.
        leaf_ad_route = mvpn_dbstate->route;
        if (leaf_ad_route) {
            BgpPath *path = leaf_ad_route->FindPath(BgpPath::Local, 0);
            if (path)
                leaf_ad_route->DeletePath(path);
            mvpn_dbstate->route = NULL;
        }

        assert(mvpn_state->spmsi_routes().erase(spmsi_rt));
        spmsi_rt->ClearState(table(), listener_id());
        DeleteState(mvpn_state);
        if (leaf_ad_route)
            leaf_ad_route->NotifyOrDelete();
        return;
    }

    // A valid S-PMSI path has been imported to a table. Originate a new
    // LeafAD path, if GlobalErmVpnTreeRoute is available to stitch.
    // TODO(Ananth) If LeafInfoRequired bit is not set in the S-PMSI route,
    // then we do not need to originate a leaf ad route for this s-pmsi rt.
    MvpnState *mvpn_state = LocateState(spmsi_rt);
    if (!mvpn_state)
        return;
    if (!mvpn_dbstate) {
        mvpn_dbstate = new MvpnDBState(mvpn_state);
        spmsi_rt->SetState(table(), listener_id(), mvpn_dbstate);
        assert(mvpn_state->spmsi_routes().insert(spmsi_rt).second);
    } else {
        leaf_ad_route = mvpn_dbstate->route;
    }

    ErmVpnRoute *global_rt = mvpn_state->global_ermvpn_tree_rt();
    if (!global_rt || !global_rt->IsUsable()) {
        // There is no ermvpn route available to stitch at this time. Remove any
        // originated Type4 LeafAD route. DB State shall remain on the route as
        // SPMSI route itself is still a usable route.
        if (leaf_ad_route) {
            BgpPath *path = leaf_ad_route->FindPath(BgpPath::Local, 0);
            if (path)
                leaf_ad_route->DeletePath(path);
            mvpn_dbstate->route = NULL;
            leaf_ad_route->NotifyOrDelete();
        }
        return;
    }

    if (!leaf_ad_route) {
        leaf_ad_route = table()->LocateType4LeafADRoute(spmsi_rt);
        mvpn_dbstate->route = leaf_ad_route;
    }
    BgpPath *old_path = leaf_ad_route->FindPath(BgpPath::Local, 0);

    // For LeafAD routes, rtarget is always <sender-router-id>:0.
    BgpAttrPtr attrp = BgpAttrPtr(spmsi_rt->BestPath()->GetAttr());
    ExtCommunity::ExtCommunityList rtarget;
    rtarget.push_back(RouteTarget(spmsi_rt->GetPrefix().originator(), 0).
                                  GetExtCommunity());
    ExtCommunityPtr ext_community = table()->server()->extcomm_db()->
            ReplaceRTargetAndLocate(attrp->ext_community(), rtarget);

    uint32_t label;
    Ip4Address address;
    uint32_t tunnel_types_list;
    // TODO(Ananth) Add TunnelType extended community also.
    assert(global_rt->GetLeafAdTunnelInfo(&label, &address,
                                          &tunnel_types_list));

    // Retrieve PMSI tunnel attribute from the GlobalErmVpnTreeRoute.
    PmsiTunnelSpec *pmsi_spec = new PmsiTunnelSpec();
    pmsi_spec->tunnel_flags = 0;
    pmsi_spec->tunnel_type = PmsiTunnelSpec::IngressReplication;
    pmsi_spec->SetLabel(label);
    pmsi_spec->SetIdentifier(address);

    // Replicate the LeafAD path with appropriate PMSI tunnel info as part of
    // the path attributes. Community should be route-target with root ingress
    // PE router-id + 0 (Page 254).
    BgpAttrPtr new_attr =
        table()->server()->attr_db()->ReplacePmsiTunnelAndLocate(
            spmsi_rt->BestPath()->GetAttr(), pmsi_spec);

    if (old_path) {
        // Ignore if there is no change in the path attributes of the already
        // originated lead ad path.
        if (old_path->GetAttr() == new_attr.get())
            return;
        leaf_ad_route->DeletePath(old_path);
    }

    BgpPath *path = new BgpPath(NULL, 0, BgpPath::Local, attrp, 0, 0, 0);
    leaf_ad_route->InsertPath(path);
    leaf_ad_route->Notify();

    // TODO(Ananth) Notify forest node local route.
}

void MvpnManagerPartition::ProcessType4LeafADRoute(MvpnRoute *leaf_ad) {
    const BgpPath *src_path = leaf_ad->BestPath();
    if (!dynamic_cast<const BgpSecondaryPath *>(src_path))
        return;

    // LeafAD route has been imported into a table. Retrieve PMSI information
    // from the path attribute and update the ingress sender (agent).
}
