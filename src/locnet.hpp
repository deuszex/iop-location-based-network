#ifndef __LOCNET_BUSINESS_LOGIC_H__
#define __LOCNET_BUSINESS_LOGIC_H__

#include <random>
#include <unordered_map>

#include "spatialdb.hpp"



namespace LocNet
{



// Local interface for services running on the same hardware
class ILocalServiceMethods
{
public:
    
    virtual ~ILocalServiceMethods() {}
    
    virtual void RegisterService(const ServiceInfo &serviceInfo) = 0;
    virtual void DeregisterService(ServiceType serviceType) = 0;
    virtual std::vector<NodeInfo> GetNeighbourNodesByDistance() const = 0;
    
    // NOTE methods used through this interface, but not exported to remote nodes
    virtual void AddListener(std::shared_ptr<IChangeListener> listener) = 0;
    virtual void RemoveListener(const SessionId &listenerId) = 0;
};



// Interface provided for other nodes of the same network (running on remote machines)
class INodeMethods
{
public:
    
    virtual ~INodeMethods() {}
    
    virtual size_t GetNodeCount() const = 0;
    virtual std::vector<NodeInfo> GetRandomNodes(
        size_t maxNodeCount, Neighbours filter) const = 0;
    
    virtual std::vector<NodeInfo> GetClosestNodesByDistance(const GpsLocation &location,
        Distance radiusKm, size_t maxNodeCount, Neighbours filter) const = 0;
    
    virtual std::shared_ptr<NodeInfo> AcceptColleague(const NodeInfo &node) = 0;
    virtual std::shared_ptr<NodeInfo> RenewColleague (const NodeInfo &node) = 0;
    virtual std::shared_ptr<NodeInfo> AcceptNeighbour(const NodeInfo &node) = 0;
    virtual std::shared_ptr<NodeInfo> RenewNeighbour (const NodeInfo &node) = 0;
};


// Interface provided to serve higher level services and clients
class IClientMethods
{
public:
    
    virtual ~IClientMethods() {}

    virtual const std::unordered_map<ServiceType,ServiceInfo,EnumHasher>& GetServices() const = 0;
    
    virtual std::vector<NodeInfo> GetNeighbourNodesByDistance() const = 0;
    virtual std::vector<NodeInfo> GetClosestNodesByDistance(const GpsLocation &location,
        Distance radiusKm, size_t maxNodeCount, Neighbours filter) const = 0;
};



// Factory interface to create node listener objects.
// Needed to properly separate the network/messaging layer and context from this code
// that can be used to send notifications but otherwise completely independent.
class IChangeListenerFactory
{
public:
    
    virtual ~IChangeListenerFactory() {}
    
    virtual std::shared_ptr<IChangeListener> Create(
        std::shared_ptr<ILocalServiceMethods> localService) = 0;
};


// Factory interface to return callable node methods for potentially remote nodes,
// hiding away the exact way and complexity of communication.
class INodeConnectionFactory
{
public:
    
    virtual ~INodeConnectionFactory() {}
    
    virtual std::shared_ptr<INodeMethods> ConnectTo(const NetworkEndpoint &endpoint) = 0;
};



// Implementation of all provided interfaces in a single class
class Node : public ILocalServiceMethods, public IClientMethods, public INodeMethods
{
    static std::random_device _randomDevice;
    
    NodeInfo _myNodeInfo;
    std::shared_ptr<ISpatialDatabase>       _spatialDb;
    std::shared_ptr<INodeConnectionFactory> _connectionFactory;
    
    // TODO consider if this should be also persistant, thus included in SpatialDatabase.
    //      Pro: service list survives restart/crash, services don't have to detect restart and register again.
    //      Con: if services are also restarted, will give false results.
    // TODO If no need to be persistent, this is still not threadsafe, though problems are highly unlikely.
    //      Should also use something like spatialdb.hpp:ThreadSafeChangeListenerRegistry here.
    std::unordered_map<ServiceType, ServiceInfo, EnumHasher> _services;
    
    std::vector<NetworkEndpoint> _seedNodes;
    
    
    std::shared_ptr<INodeMethods> SafeConnectTo(const NetworkEndpoint &endpoint);
    bool SafeStoreNode(const NodeDbEntry &entry,
        std::shared_ptr<INodeMethods> nodeConnection = std::shared_ptr<INodeMethods>(),
        bool isSeedNode = false);
    
    bool InitializeWorld();
    bool InitializeNeighbourhood();
    
    Distance GetBubbleSize(const GpsLocation &location) const;
    bool BubbleOverlaps(const GpsLocation &newNodeLocation,
                        const std::string &nodeIdToIgnore = "") const;
    
public:
    
    Node( const NodeInfo &myNodeInfo,
          std::shared_ptr<ISpatialDatabase> spatialDb,
          std::shared_ptr<INodeConnectionFactory> connectionFactory,
          const std::vector<NetworkEndpoint> &seedNodes);

    void EnsureMapFilled();
    
    void DetectedExternalAddress(const Address &address);
    
    void ExpireOldNodes();
    void RenewNodeRelations();
    void DiscoverUnknownAreas();
    
    // Interface provided to serve higher level services and clients
    const std::unordered_map<ServiceType,ServiceInfo,EnumHasher>& GetServices() const override;
    // + GetClosestNodes() which is the same as for network instances on remote machines
    
    // Local interface for services running on the same hardware
    void RegisterService(const ServiceInfo &serviceInfo) override;
    void DeregisterService(ServiceType serviceType) override;
    
    void AddListener(std::shared_ptr<IChangeListener> listener) override;
    void RemoveListener(const SessionId &sessionId) override;
    
    // Interface provided for the same network instances running on remote machines
    size_t GetNodeCount() const override;
    std::vector<NodeInfo> GetNeighbourNodesByDistance() const override;
    
    std::vector<NodeInfo> GetRandomNodes(
        size_t maxNodeCount, Neighbours filter) const override;
    
    std::vector<NodeInfo> GetClosestNodesByDistance(const GpsLocation &location,
        Distance radiusKm, size_t maxNodeCount, Neighbours filter) const override;    
        
    std::shared_ptr<NodeInfo> AcceptColleague(const NodeInfo &node) override;
    std::shared_ptr<NodeInfo> RenewColleague (const NodeInfo &node) override;
    std::shared_ptr<NodeInfo> AcceptNeighbour(const NodeInfo &node) override;
    std::shared_ptr<NodeInfo> RenewNeighbour (const NodeInfo &node) override;
};


} // namespace LocNet


#endif // __LOCNET_BUSINESS_LOGIC_H__
