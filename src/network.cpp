#include <chrono>
#include <functional>

#include <easylogging++.h>

#include "network.hpp"

using namespace std;
using namespace asio::ip;



namespace LocNet
{


static const size_t ThreadPoolSize = 1;
static const size_t MaxMessageSize = 1024 * 1024;
static const chrono::duration<uint32_t> NormalStreamExpirationPeriod    = chrono::seconds(10);
//static const chrono::duration<uint32_t> KeepAliveStreamExpirationPeriod = chrono::hours(168);

static const size_t MessageHeaderSize = 5;
static const size_t MessageSizeOffset = 1;


Address NetworkInterface::AddressFromIpv4Bytes(const std::string &bytes)
{
    address_v4::bytes_type v4AddrBytes;
    copy( &bytes.front(), &bytes.front() + v4AddrBytes.size(), v4AddrBytes.data() );
    address_v4 ipv4(v4AddrBytes);
    return ipv4.to_string();
}

Address NetworkInterface::AddressFromIpv6Bytes(const std::string &bytes)
{
    address_v6::bytes_type v6AddrBytes;
    copy( &bytes.front(), &bytes.front() + v6AddrBytes.size(), v6AddrBytes.data() );
    address_v6 ipv6(v6AddrBytes);
    return ipv6.to_string();
}

bool NetworkInterface::isIpv4() const
{
    try { return address::from_string(_address).is_v4(); }
    catch (...) { return false; }
}

bool NetworkInterface::isIpv6() const
{
    try { return address::from_string(_address).is_v6(); }
    catch (...) { return false; }
}

bool NetworkInterface::isLoopback() const
{
    try { return address::from_string(_address).is_loopback(); }
    catch (...) { return false; }
}

string NetworkInterface::Ipv4Bytes() const
{
    string result;
    auto bytes = address::from_string(_address).to_v4().to_bytes();
    for (uint8_t byte : bytes)
        { result.push_back(byte); }
    return result;
}

string NetworkInterface::Ipv6Bytes() const
{
    string result;
    auto bytes = address::from_string(_address).to_v6().to_bytes();
    for (uint8_t byte : bytes)
        { result.push_back(byte); }
    return result;
}



TcpServer::TcpServer(TcpPort portNumber) : _ioService(),
    _acceptor( _ioService, tcp::endpoint( tcp::v4(), portNumber ) ),
    _threadPool(), _shutdownRequested(false)
{
    // Switch the acceptor to listening state
    LOG(DEBUG) << "Start accepting connections";
    _acceptor.listen();
    
    shared_ptr<tcp::socket> socket( new tcp::socket(_ioService) );
    _acceptor.async_accept( *socket,
        [this, socket] (const asio::error_code &ec) { AsyncAcceptHandler(socket, ec); } );
    
    // Start the specified number of job processor threads
    for (size_t idx = 0; idx < ThreadPoolSize; ++idx)
    {
        _threadPool.push_back( thread( [this]
        {
            while (! _shutdownRequested)
                { _ioService.run(); }
        } ) );
    }
}


TcpServer::~TcpServer()
{
    Shutdown();
    
    _ioService.stop();
    for (auto &thr : _threadPool)
        { thr.join(); }
}


void TcpServer::Shutdown()
    { _shutdownRequested = true; }



ProtoBufDispatchingTcpServer::ProtoBufDispatchingTcpServer( TcpPort portNumber,
        shared_ptr<IProtoBufRequestDispatcherFactory> dispatcherFactory ) :
    TcpServer(portNumber), _dispatcherFactory(dispatcherFactory) {}



void ProtoBufDispatchingTcpServer::AsyncAcceptHandler(
    std::shared_ptr<asio::ip::tcp::socket> socket, const asio::error_code &ec)
{
    if (ec)
    {
        LOG(ERROR) << "Failed to accept connection: " << ec;
        return;
    }
    LOG(DEBUG) << "Connection accepted from "
        << socket->remote_endpoint().address().to_string() << ":" << socket->remote_endpoint().port() << " to "
        << socket->local_endpoint().address().to_string()  << ":" << socket->local_endpoint().port();
    
    // Keep accepting connections on the socket
    shared_ptr<tcp::socket> nextSocket( new tcp::socket(_ioService) );
    _acceptor.async_accept( *nextSocket,
        [this, nextSocket] (const asio::error_code &ec) { AsyncAcceptHandler(nextSocket, ec); } );
    
    // Serve connected client on separate thread
    thread serveSessionThread( [this, socket] // copy by value to keep socket alive
    {
        shared_ptr<IProtoBufNetworkSession> session( new ProtoBufTcpStreamSession(socket) );
        try
        {
            shared_ptr<IProtoBufRequestDispatcher> dispatcher(
                _dispatcherFactory->Create(session) );

            bool endMessageLoop = false;
            while (! endMessageLoop && ! _shutdownRequested)
            {
                uint32_t messageId = 0;
                shared_ptr<iop::locnet::MessageWithHeader> requestMsg;
                unique_ptr<iop::locnet::Response> response;
                
                try
                {
                    LOG(TRACE) << "Reading request";
                    requestMsg.reset( session->ReceiveMessage() );

                    if ( ! requestMsg || ! requestMsg->has_body() || ! requestMsg->body().has_request() )
                        { throw LocationNetworkError(ErrorCode::ERROR_BAD_REQUEST, "Missing message body or request"); }
                    
                    LOG(TRACE) << "Serving request";
                    messageId = requestMsg->body().id();
                    response = dispatcher->Dispatch( requestMsg->body().request() );
                    response->set_status(iop::locnet::Status::STATUS_OK);
                    
                    // TODO somehow this should be abstracted away and implemented in a nicer way
                    if ( requestMsg->has_body() && requestMsg->body().has_request() &&
                        requestMsg->body().request().has_localservice() &&
                        requestMsg->body().request().localservice().has_getneighbournodes() &&
                        requestMsg->body().request().localservice().getneighbournodes().keepaliveandsendupdates() )
                    {
                        LOG(DEBUG) << "GetNeighbourhood with keepalive is requested, ending dispatch loop and serve only notifications through ChangeListener";
                        // NOTE Session will be still kept alive because its ahared_ptr is copied by the ChangeListener that sends notifications
                        endMessageLoop = true;
                    }
                }
                catch (LocationNetworkError &lnex)
                {
                    LOG(WARNING) << "Failed to serve request with code "
                        << static_cast<uint32_t>( lnex.code() ) << ": " << lnex.what();
                    response.reset( new iop::locnet::Response() );
                    response->set_status( Converter::ToProtoBuf( lnex.code() ) );
                    response->set_details( lnex.what() );
                    endMessageLoop = true;
                }
                catch (exception &ex)
                {
                    LOG(WARNING) << "Failed to serve request: " << ex.what();
                    response.reset( new iop::locnet::Response() );
                    response->set_status(iop::locnet::Status::ERROR_INTERNAL);
                    response->set_details( ex.what() );
                    endMessageLoop = true;
                }
                
                LOG(TRACE) << "Sending response";
                iop::locnet::MessageWithHeader responseMsg;
                responseMsg.mutable_body()->set_allocated_response( response.release() );
                responseMsg.mutable_body()->set_id(messageId);
                
                session->SendMessage(responseMsg);
            }
        }
        catch (exception &ex)
        {
            LOG(WARNING) << "Request dispatch loop failed: " << ex.what();
        }
        
        LOG(INFO) << "Request dispatch loop for session " << session->id() << " finished";
    } );
    
    // Keep thread running independently, don't block io_service here by joining it
    serveSessionThread.detach();
}




ProtoBufTcpStreamSession::ProtoBufTcpStreamSession(shared_ptr<tcp::socket> socket) :
    _id(), _stream(), _socket(socket)
{
    if (! _socket)
        { throw LocationNetworkError(ErrorCode::ERROR_INTERNAL, "No socket instantiated"); }
    
    _id = socket->remote_endpoint().address().to_string() + ":" + to_string( socket->remote_endpoint().port() );
    // TODO consider possible ownership problems of this construct
    _stream.rdbuf()->assign( socket->local_endpoint().protocol(), socket->native_handle() );
    // TODO handle session expiration
    //_stream.expires_after(NormalStreamExpirationPeriod);
}


ProtoBufTcpStreamSession::ProtoBufTcpStreamSession(const NetworkInterface &contact) :
    _id( contact.address() + ":" + to_string( contact.port() ) ),
    _stream()
{
    _stream.expires_after(NormalStreamExpirationPeriod);
    _stream.connect( contact.address(), to_string( contact.port() ) );
    if (! _stream)
        { throw LocationNetworkError(ErrorCode::ERROR_CONNECTION, "Session failed to connect: " + _stream.error().message() ); }
    LOG(DEBUG) << "Connected to " << contact;
}

ProtoBufTcpStreamSession::~ProtoBufTcpStreamSession()
{
    LOG(DEBUG) << "Session " << id() << " closed";
}


const SessionId& ProtoBufTcpStreamSession::id() const
    { return _id; }



uint32_t GetMessageSizeFromHeader(const char *bytes)
{
    // Adapt big endian value from network to local format
    const uint8_t *data = reinterpret_cast<const uint8_t*>(bytes);
    return data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24);
}



iop::locnet::MessageWithHeader* ProtoBufTcpStreamSession::ReceiveMessage()
{
    if ( _stream.eof() )
        { throw LocationNetworkError(ErrorCode::ERROR_INVALID_STATE,
            "Session " + id() + " connection is already closed, cannot read message"); }
        
    // Allocate a buffer for the message header and read it
    string messageBytes(MessageHeaderSize, 0);
    _stream.read( &messageBytes[0], MessageHeaderSize );
    if ( _stream.fail() )
        { throw LocationNetworkError(ErrorCode::ERROR_PROTOCOL_VIOLATION,
            "Session " + id() + " failed to read message header, connection may have been closed by remote peer"); }
    
    // Extract message size from the header to know how many bytes to read
    uint32_t bodySize = GetMessageSizeFromHeader( &messageBytes[MessageSizeOffset] );
    
    if (bodySize > MaxMessageSize)
        { throw LocationNetworkError(ErrorCode::ERROR_BAD_REQUEST,
            "Session " + id() + " message size is over limit: " + to_string(bodySize) ); }
    
    // Extend buffer to fit remaining message size and read it
    messageBytes.resize(MessageHeaderSize + bodySize, 0);
    _stream.read( &messageBytes[0] + MessageHeaderSize, bodySize );
    if ( _stream.fail() )
        { throw LocationNetworkError(ErrorCode::ERROR_PROTOCOL_VIOLATION,
            "Session " + id() + " failed to read full message body"); }

    // Deserialize message from receive buffer, avoid leaks for failing cases with RAII-based unique_ptr
    unique_ptr<iop::locnet::MessageWithHeader> message( new iop::locnet::MessageWithHeader() );
    message->ParseFromString(messageBytes);
    
    string buffer;
    google::protobuf::TextFormat::PrintToString(*message, &buffer);
    LOG(TRACE) << "Session " << id() << " received message " << buffer;
    
    return message.release();
}



void ProtoBufTcpStreamSession::SendMessage(iop::locnet::MessageWithHeader& message)
{
    message.set_header(1);
    message.set_header( message.ByteSize() - MessageHeaderSize );
    _stream << message.SerializeAsString();
    
    string buffer;
    google::protobuf::TextFormat::PrintToString(message, &buffer);
    LOG(TRACE) << "Session " << id() << " sent message " << buffer;
}


void ProtoBufTcpStreamSession::KeepAlive()
{
    // TODO handle session expiration
    //_stream.expires_after(KeepAliveStreamExpirationPeriod);
}
    
// // TODO CHECK This doesn't really seem to work as on "normal" std::streamss
// bool ProtoBufTcpStreamSession::IsAlive() const
//     { return _stream.good(); }
// 
// void ProtoBufTcpStreamSession::Close()
//     { _stream.close(); }




ProtoBufRequestNetworkDispatcher::ProtoBufRequestNetworkDispatcher(shared_ptr<IProtoBufNetworkSession> session) :
    _session(session) {}

    

unique_ptr<iop::locnet::Response> ProtoBufRequestNetworkDispatcher::Dispatch(const iop::locnet::Request& request)
{
    iop::locnet::Request *clonedReq = new iop::locnet::Request(request);
    clonedReq->set_version({1,0,0});
    
    iop::locnet::MessageWithHeader reqMsg;
    reqMsg.mutable_body()->set_allocated_request(clonedReq);
    
    _session->SendMessage(reqMsg);
    unique_ptr<iop::locnet::MessageWithHeader> respMsg( _session->ReceiveMessage() );
    if ( ! respMsg || ! respMsg->has_body() || ! respMsg->body().has_response() )
        { throw LocationNetworkError(ErrorCode::ERROR_BAD_RESPONSE, "Got invalid response from remote node"); }
        
    unique_ptr<iop::locnet::Response> result(
        new iop::locnet::Response( respMsg->body().response() ) );
    if ( result && result->status() != iop::locnet::Status::STATUS_OK )
    {
        LOG(WARNING) << "Session " << _session->id() << " received response code " << result->status()
                     << ", error details: " << result->details();
        throw LocationNetworkError( ErrorCode::ERROR_BAD_RESPONSE, result->details() );
    }
    return result;
}



shared_ptr<INodeMethods> TcpStreamConnectionFactory::ConnectTo(const NetworkInterface& address)
{
    LOG(DEBUG) << "Connecting to " << address;
    shared_ptr<IProtoBufNetworkSession> session( new ProtoBufTcpStreamSession(address) );
    shared_ptr<IProtoBufRequestDispatcher> dispatcher( new ProtoBufRequestNetworkDispatcher(session) );
    shared_ptr<INodeMethods> result( new NodeMethodsProtoBufClient(dispatcher) );
    return result;
}



IncomingRequestDispatcherFactory::IncomingRequestDispatcherFactory(shared_ptr<Node> node) :
    _node(node) {}


shared_ptr<IProtoBufRequestDispatcher> IncomingRequestDispatcherFactory::Create(
    shared_ptr<IProtoBufNetworkSession> session )
{
    shared_ptr<IChangeListenerFactory> listenerFactory(
        new ProtoBufTcpStreamChangeListenerFactory(session) );
    return shared_ptr<IProtoBufRequestDispatcher>(
        new IncomingRequestDispatcher(_node, listenerFactory) );
}



ProtoBufTcpStreamChangeListenerFactory::ProtoBufTcpStreamChangeListenerFactory(
        shared_ptr<IProtoBufNetworkSession> session) :
    _session(session) {}



shared_ptr<IChangeListener> ProtoBufTcpStreamChangeListenerFactory::Create(
    shared_ptr<ILocalServiceMethods> localService)
{
    shared_ptr<IProtoBufRequestDispatcher> dispatcher(
        new ProtoBufRequestNetworkDispatcher(_session) );
    return shared_ptr<IChangeListener>(
        new ProtoBufTcpStreamChangeListener(_session, localService, dispatcher) );
}



ProtoBufTcpStreamChangeListener::ProtoBufTcpStreamChangeListener(
        shared_ptr<IProtoBufNetworkSession> session,
        shared_ptr<ILocalServiceMethods> localService,
        shared_ptr<IProtoBufRequestDispatcher> dispatcher ) :
    _sessionId( session->id() ), _localService(localService), _dispatcher(dispatcher)
{
    session->KeepAlive();
}


ProtoBufTcpStreamChangeListener::~ProtoBufTcpStreamChangeListener()
{
    Deregister();
    LOG(DEBUG) << "ChangeListener destroyed";
}

void ProtoBufTcpStreamChangeListener::Deregister()
{
    if ( ! _sessionId.empty() )
    {
        LOG(DEBUG) << "ChangeListener deregistering for session " << _sessionId;
        _localService->RemoveListener(_sessionId);
        _sessionId.clear();
    }
}



const SessionId& ProtoBufTcpStreamChangeListener::sessionId() const
    { return _sessionId; }



void ProtoBufTcpStreamChangeListener::AddedNode(const NodeDbEntry& node)
{
    if ( node.relationType() == NodeRelationType::Neighbour )
    {
        try
        {
            iop::locnet::Request req;
            iop::locnet::NeighbourhoodChange *change =
                req.mutable_localservice()->mutable_neighbourhoodchanged()->add_changes();
            iop::locnet::NodeInfo *info = change->mutable_addednodeinfo();
            Converter::FillProtoBuf(info, node);
            
            unique_ptr<iop::locnet::Response> response( _dispatcher->Dispatch(req) );
            // TODO what to do with response status codes here?
        }
        catch (exception &ex)
        {
            LOG(ERROR) << "Failed to send change notification";
            Deregister();
        }
    }
}


void ProtoBufTcpStreamChangeListener::UpdatedNode(const NodeDbEntry& node)
{
    if ( node.relationType() == NodeRelationType::Neighbour )
    {
        try
        {
            iop::locnet::Request req;
            iop::locnet::NeighbourhoodChange *change =
                req.mutable_localservice()->mutable_neighbourhoodchanged()->add_changes();
            iop::locnet::NodeInfo *info = change->mutable_addednodeinfo();
            Converter::FillProtoBuf(info, node);
            
            unique_ptr<iop::locnet::Response> response( _dispatcher->Dispatch(req) );
            // TODO what to do with response status codes here?
        }
        catch (exception &ex)
        {
            LOG(ERROR) << "Failed to send change notification";
            Deregister();
        }
    }
}


void ProtoBufTcpStreamChangeListener::RemovedNode(const NodeDbEntry& node)
{
    if ( node.relationType() == NodeRelationType::Neighbour )
    {
        try
        {
            iop::locnet::Request req;
            iop::locnet::NeighbourhoodChange *change =
                req.mutable_localservice()->mutable_neighbourhoodchanged()->add_changes();
            change->set_removednodeid( node.profile().id() );
            
            unique_ptr<iop::locnet::Response> response( _dispatcher->Dispatch(req) );
            // TODO what to do with response status codes here?
        }
        catch (exception &ex)
        {
            LOG(ERROR) << "Failed to send change notification";
            Deregister();
        }
    }
}



} // namespace LocNet