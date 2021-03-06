#ifndef __LOCNET_TEST_IMPLEMENTATIONS_H__
#define __LOCNET_TEST_IMPLEMENTATIONS_H__

#include "locnet.hpp"



namespace LocNet
{


class DummyNodeConnectionFactory: public INodeProxyFactory
{
public:
    
    std::shared_ptr<INodeMethods> ConnectTo(const NetworkEndpoint &endpoint) override;
};


class DummyChangeListenerFactory: public IChangeListenerFactory
{
public:
    
    std::shared_ptr<IChangeListener> Create(std::shared_ptr<ILocalServiceMethods> localService) override;
};



class ChangeCounter : public IChangeListener
{
    SessionId _sessionId;
    
public:
    
    size_t addedCount   = 0;
    size_t updatedCount = 0;
    size_t removedCount = 0;
    
    ChangeCounter(const SessionId &sessionId);
    
    const SessionId& sessionId() const override;
    
    void OnRegistered() override;
    void AddedNode  (const NodeDbEntry &node) override;
    void UpdatedNode(const NodeDbEntry &node) override;
    void RemovedNode(const NodeDbEntry &node) override;
};



struct NodeRegistry : public INodeProxyFactory
{
    typedef std::unordered_map< Address, std::shared_ptr<Node> >   NodeContainer;
    
protected:
    
    NodeContainer _nodes;
    
public:
    
    const NodeContainer& nodes() const;
    void Register(std::shared_ptr<Node> node);
    
    std::shared_ptr<INodeMethods> ConnectTo(const NetworkEndpoint &endpoint) override;
};



class TestClock
{
    std::chrono::system_clock::time_point _now;
    
public:
    
    TestClock();
    
    std::chrono::system_clock::time_point now() const;
    void elapse(std::chrono::duration<int64_t> period);
};


struct InMemDbEntry : public NodeDbEntry
{
    InMemDbEntry(const InMemDbEntry& other);
    InMemDbEntry(const NodeDbEntry& other, std::chrono::system_clock::time_point expiresAt);
    
    std::chrono::system_clock::time_point _expiresAt = std::chrono::system_clock::now();
};


// NOTE NOT PERSISTENT, suited for development/testing only
class InMemorySpatialDatabase : public ISpatialDatabase
{
    static std::random_device _randomDevice;
    
    NodeInfo _myNodeInfo;
    std::unordered_map<NodeId,InMemDbEntry> _nodes;
    std::shared_ptr<TestClock> _testClock;
    std::chrono::duration<int64_t> _entryExpirationPeriod;
    
    std::vector<NodeDbEntry> GetNodes(NodeRelationType relationType) const;
    
    ThreadSafeChangeListenerRegistry _listenerRegistry;
    
public:
    
    InMemorySpatialDatabase( const NodeInfo &myNodeInfo, std::shared_ptr<TestClock> testClock,
        std::chrono::duration<int64_t> entryExpirationPeriod );
    
    Distance GetDistanceKm(const GpsLocation &one, const GpsLocation &other) const override;

    std::shared_ptr<NodeDbEntry> Load(const NodeId &nodeId) const override;
    void Store (const NodeDbEntry &node, bool expires = true) override;
    void Update(const NodeDbEntry &node, bool expires = true) override;
    void Remove(const NodeId &nodeId) override;
    void ExpireOldNodes() override;
    
    IChangeListenerRegistry& changeListenerRegistry() override;

    NodeDbEntry ThisNode() const override;
    std::vector<NodeDbEntry> GetNodes(NodeContactRoleType roleType) override;
    
    size_t GetNodeCount() const override;
    size_t GetNodeCount(NodeRelationType filter) const override;
    std::vector<NodeDbEntry> GetNeighbourNodesByDistance() const override;
    std::vector<NodeDbEntry> GetRandomNodes(
        size_t maxNodeCount, Neighbours filter) const override;
    
    std::vector<NodeDbEntry> GetClosestNodesByDistance(const GpsLocation &position,
        Distance radiusKm, size_t maxNodeCount, Neighbours filter) const override;
};



struct TestConfig : public Config
{
    // While testing command line args are forwarded to the tester framework, only argv0 is used to load up resources
    static std::string ExecPath;
    static std::chrono::duration<uint32_t> DbExpirationPeriod;
    //static std::pair<size_t, const char**> TestArgs();
    
    NodeInfo        _nodeInfo;
    TcpPort         _localPort = 0;
    std::string     _logPath;
    std::string     _dbPath;
    size_t          _neighbourhoodTargetSize = 5;
    std::vector<NetworkEndpoint> _seedNodes;
        
    
    TestConfig(const NodeInfo &nodeInfo);   // For test servers
    TestConfig();                           // For test clients where only network options matter
    
    const NodeInfo& myNodeInfo() const override;
    TcpPort localServicePort() const override;
    
    const std::string& logPath() const override;
    const std::string& dbPath() const override;
    
    bool isTestMode() const override;
    const std::vector<NetworkEndpoint>& seedNodes() const override;
    size_t neighbourhoodTargetSize() const override;
    
    std::chrono::duration<uint32_t> requestExpirationPeriod() const override;
    std::chrono::duration<uint32_t> dbMaintenancePeriod() const override;
    std::chrono::duration<uint32_t> dbExpirationPeriod() const override;
    std::chrono::duration<uint32_t> discoveryPeriod() const override;
};


} // namespace LocNet

#endif // __LOCNET_TEST_IMPLEMENTATIONS_H__
