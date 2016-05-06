/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef __VNSW_OPERDB_INIT__
#define __VNSW_OPERDB_INIT__

#include <base/util.h>
#include <cmn/agent_cmn.h>
#include <cmn/agent.h>

class DBEntryBase;
class Agent;
class DB;
class GlobalVrouter;
class PathPreferenceModule;
class IFMapDependencyManager;
class MulticastHandler;
class InstanceManager;
class NexthopManager;
class AgentSandeshManager;
class AgentProfile;
class VRouter;
class BgpAsAService;

class OperDB {
public:
    OperDB(Agent *agent);
    virtual ~OperDB();

    void CreateDBTables(DB *);
    void RegisterDBClients();
    void Init();
    void CreateDefaultVrf();
    void DeleteRoutes();
    void Shutdown();

    Agent *agent() const { return agent_; }
    MulticastHandler *multicast() const { return multicast_.get(); }
    GlobalVrouter *global_vrouter() const { return global_vrouter_.get(); }
    PathPreferenceModule *route_preference_module() const {
        return route_preference_module_.get();
    }
    IFMapDependencyManager *dependency_manager() {
        return dependency_manager_.get();
    }
    InstanceManager *instance_manager() {
        return instance_manager_.get();
    }
    DomainConfig *domain_config_table() {
        return domain_config_.get();
    }
    NexthopManager *nexthop_manager() {
      return nexthop_manager_.get();
    }
    AgentSandeshManager *agent_sandesh_manager() {
        return agent_sandesh_manager_.get();
    }
    VRouter *vrouter() const { return vrouter_.get(); }
    BgpAsAService *bgp_as_a_service() const { return bgp_as_a_service_.get(); }

    AgentProfile *agent_profile() const { return profile_.get(); }
private:
    OperDB();

    Agent *agent_;
    std::unique_ptr<MulticastHandler> multicast_;
    std::unique_ptr<GlobalVrouter> global_vrouter_;
    std::unique_ptr<PathPreferenceModule> route_preference_module_;
    std::unique_ptr<IFMapDependencyManager> dependency_manager_;
    std::unique_ptr<InstanceManager> instance_manager_;
    std::unique_ptr<DomainConfig> domain_config_;
    std::unique_ptr<NexthopManager> nexthop_manager_;
    std::unique_ptr<AgentSandeshManager> agent_sandesh_manager_;
    std::unique_ptr<AgentProfile> profile_;
    std::unique_ptr<VRouter> vrouter_;
    std::unique_ptr<BgpAsAService> bgp_as_a_service_;
    DISALLOW_COPY_AND_ASSIGN(OperDB);
};
#endif
