#include "System/Endpoint.h"
#include "System/Exception.h"

#if defined(_WIN64)

 CAcceptorImpl::CAcceptorImpl(USHORT port, OperationCallback_t&& acceptCallback)
 : m_addrInfo(nullptr)
 , m_acceptCallback(acceptCallback)
 , m_peerAddr(nullptr)
 , m_newConnection(nullptr)
 {
	 ResetContext();

     // Create acceptor endpoint.
     m_hEndpoint = WSASocket(AF_INET6, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
	if (m_hEndpoint == INVALID_SOCKET) throw CWindowsException(WSAGetLastError());
    
	// Obtain advanced socket API supporting IO completion port principle.

	// AcceptEx API needed to accept peer connections asynchronously.
	GUID funcId = WSAID_ACCEPTEX;
	ULONG bytesRet = 0;

	if (WSAIoctl(m_hEndpoint, SIO_GET_EXTENSION_FUNCTION_POINTER, &funcId, sizeof(GUID),
		&m_pfnAcceptEx, sizeof(PVOID), &bytesRet, nullptr, nullptr) == SOCKET_ERROR)
	{
		throw CWindowsException(WSAGetLastError());
	}

	// GetAcceptExSockaddrs needed to obtain peer information.
	funcId = WSAID_GETACCEPTEXSOCKADDRS;
	bytesRet = 0;

	if (WSAIoctl(m_hEndpoint, SIO_GET_EXTENSION_FUNCTION_POINTER, &funcId, sizeof(GUID),
		&m_pfnGetAcceptExSockaddrs, sizeof(PVOID), &bytesRet, nullptr, nullptr) == SOCKET_ERROR)
	{
		throw CWindowsException(WSAGetLastError());
	}

	// Reuse server address to get read of possible errors the previous connection has not been fully disconnected.
	BOOL reuseAddr = true;
	if (setsockopt(m_hEndpoint, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuseAddr, sizeof(BOOL)) == SOCKET_ERROR)
		throw CWindowsException(WSAGetLastError());

	// Initialize IPv6 address data.
    addrinfo hint = {};
    hint.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
    hint.ai_family = AF_INET6;
    hint.ai_socktype = SOCK_STREAM;
    hint.ai_protocol = IPPROTO_TCP;

	std::stringstream portHint;
	portHint << port;

    int res = getaddrinfo("::1", portHint.str().c_str(), &hint, &m_addrInfo);
    if(res) throw CWindowsException(res);

	// Bind endpoint to the IPv6 address.
    if (bind(m_hEndpoint, m_addrInfo->ai_addr, static_cast<int>(m_addrInfo->ai_addrlen)) == SOCKET_ERROR)
        throw CWindowsException(WSAGetLastError());

	// Start listening to peer connections.
    if (listen(m_hEndpoint, 1) == SOCKET_ERROR)
		throw CWindowsException(WSAGetLastError());
 }

 CAcceptorImpl::~CAcceptorImpl()
 {
     if(m_addrInfo)
        freeaddrinfo(m_addrInfo);
 }

void CAcceptorImpl::Complete(ULONG)
{
	// This callback triggded only when a new connection accepted.
	// In this case a callback from the server called.
	// We use server callback because accept operation should be handled
	// by the whole server, not an acceptor only. Thereby only the acceptor
	// is able to track accept operation completion.
	m_acceptCallback(m_newConnection);
}

void CAcceptorImpl::ResetContext()
{
	memset(&m_context, 0, sizeof(WSAOVERLAPPED));
	std::fill(std::begin(m_acceptData), std::end(m_acceptData), 0);
	m_newConnection = nullptr;
}

void CAcceptorImpl::Accept(IConnection* connection)
{
	ResetContext();

	m_newConnection = connection;
	PBYTE acceptBuf = &m_acceptData[0];
	ULONG addrLen = static_cast<ULONG>(sizeof(SOCKADDR_IN6) + 16);
	if (!m_pfnAcceptEx(m_hEndpoint, connection->Get(), acceptBuf, 0,
		addrLen, addrLen, nullptr, &m_context))
	{
		ULONG err = WSAGetLastError();
		if (err != ERROR_IO_PENDING) throw CWindowsException(err);
	}
}

std::string CAcceptorImpl::GetPeerInfo()
{
	// Client has been just connected.

	PBYTE acceptBuf = &m_acceptData[0];
	ULONG addrLen = static_cast<ULONG>(sizeof(SOCKADDR_IN6) + 16);
	SOCKADDR_IN6* localAddr = nullptr;
	int localAddrLen = 0;
	int peerAddrLen = 0;

	// Retain the peer to be inspected later.

	m_pfnGetAcceptExSockaddrs(acceptBuf, 0, addrLen, addrLen, 
		reinterpret_cast<LPSOCKADDR*>(&localAddr), &localAddrLen,
		reinterpret_cast<LPSOCKADDR*>(&m_peerAddr), &peerAddrLen);

	std::string hostName;
	std::string serviceName;

	hostName.resize(NI_MAXHOST);
	serviceName.resize(NI_MAXSERV);

	// Print peer data.

	if (getnameinfo(reinterpret_cast<LPSOCKADDR>(m_peerAddr), peerAddrLen,
		const_cast<char*>(hostName.c_str()), static_cast<ULONG>(hostName.length()),
		const_cast<char*>(serviceName.c_str()), static_cast<ULONG>(serviceName.length()), 0) == SOCKET_ERROR)
	{
		throw CWindowsException(WSAGetLastError());
	}

	hostName.resize(strlen(hostName.c_str()));
	serviceName.resize(strlen(serviceName.c_str()));

	std::stringstream peerInfo;
	peerInfo << "Peer " << hostName << ":" << serviceName << " connected.";
	return peerInfo.str();
}

CConnectionImpl::CConnectionImpl(
	OperationCallback_t&& readCallback,
	OperationCallback_t&& writeCallback,
	OperationCallback_t&& disconnectCallback
	) : m_curState(initial)
{
	ResetContext();

	std::fill(std::begin(m_readBuf), std::end(m_readBuf), 0);
	std::fill(std::begin(m_writeBuf), std::end(m_writeBuf), 0);

	// Establish state callbacks to be called as the IO opration got completed.
	m_callbacks.emplace(readPending, readCallback);
	m_callbacks.emplace(writePending, writeCallback);
	m_callbacks.emplace(disconnectPending, disconnectCallback);

     // Create connection endpoint.
     m_hEndpoint = WSASocket(AF_INET6, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
	if (m_hEndpoint == INVALID_SOCKET) throw CWindowsException(WSAGetLastError());

	GUID funcId = WSAID_DISCONNECTEX;
	ULONG bytesRet = 0;

	if (WSAIoctl(m_hEndpoint, SIO_GET_EXTENSION_FUNCTION_POINTER, &funcId, sizeof(GUID),
		&m_pfnDisconnectEx, sizeof(PVOID), &bytesRet, nullptr, nullptr) == SOCKET_ERROR)
	{
		throw CWindowsException(WSAGetLastError());
	}
}

CConnectionImpl::~CConnectionImpl()
{
	Reset();
}

void CConnectionImpl::Read()
{
	WSABUF dataBuf;
	dataBuf.buf = &m_readBuf[0];
	dataBuf.len = MAX_BUF_SIZE;
	ULONG flags = 0;

	ResetContext();

	if (WSARecv(m_hEndpoint, &dataBuf, 1, nullptr, &flags, &m_context, nullptr) == SOCKET_ERROR)
	{
		ULONG err = WSAGetLastError();
		if (err != WSA_IO_PENDING)
			throw CWindowsException(err);
	}

	SwitchTo(readPending);
}
	
void CConnectionImpl::Write(const std::string& data)
{
	// Copy output data into the buffer without buffer reallocation.
	ULONG dataSize = static_cast<ULONG>(data.length());
	std::copy(std::begin(data), std::end(data), std::begin(m_writeBuf));

	WSABUF dataBuf;
	dataBuf.buf = &m_writeBuf[0];
	dataBuf.len = dataSize;
	ULONG flags = 0;

	ResetContext();

	if (WSASend(m_hEndpoint, &dataBuf, 1, nullptr, flags, &m_context, nullptr) == SOCKET_ERROR)
	{
		ULONG err = WSAGetLastError();
		if(err != WSA_IO_PENDING)
			throw CWindowsException(err);
	}

	SwitchTo(writePending);
}

std::string CConnectionImpl::GetInputData()
{
	assert(m_curState == readPending);

	// Copy input data until the \0 symbol occurred.
	auto endPos = std::find(std::begin(m_readBuf), std::end(m_readBuf), 0);
	std::string data;
	data.resize(std::distance(std::begin(m_readBuf), endPos));
	std::copy(std::begin(m_readBuf), endPos, std::begin(data));

	return data;
}

void CConnectionImpl::Complete(IConnection* connection, ULONG dataTransferred)
{
	if(dataTransferred)
	{
		bool dataExchange = (m_curState == readPending) || (m_curState == writePending);
		assert(dataExchange);

		// Here we're gonna initiate data writing if it has just been read.
		// Or we'll start reading next data portion if previous portion has been written.
		(m_callbacks[m_curState])(connection);

		// At operation complete stage zero out only part of buffer containing letters.
		// Other part is zeroed initially, thus no need to do it again spending CPU time.
		Buffer_t& buf = (m_curState == readPending) ? m_readBuf : m_writeBuf;
		auto endPos = std::begin(buf) + std::min<ULONG>(dataTransferred, MAX_BUF_SIZE);
		std::fill(std::begin(buf), endPos, 0);
	}
	else
	{
		// Asynchronous disconnect completed so we need to reset connection. Now it's ready to reuse.
		if (m_curState == disconnectPending)
		{
			// Here we're gonna carry connection instance from the active list into the list of those being reused.  
			(m_callbacks[m_curState])(connection);
			Reset();
		}
		else
		{
			// Peer closed connection so let's do asynchronouse disconnect, thereby initializing connection reuse.
			Disconnect();
		}
	}
}
	
void CConnectionImpl::ResetContext()
{
	memset(&m_context, 0, sizeof(WSAOVERLAPPED));
}

void CConnectionImpl::Disconnect()
{
	ResetContext();
	if (!m_pfnDisconnectEx(m_hEndpoint, &m_context, TF_REUSE_SOCKET, 0))
	{
		ULONG err = WSAGetLastError();
		if (err != ERROR_IO_PENDING) throw CWindowsException(err);
	}
	SwitchTo(disconnectPending);
}

void CConnectionImpl::Reset()
{
	m_curState = initial;
	ResetContext();
}

#elif defined(__linux__)

AcceptorImpl::AcceptorImpl(uint16_t port, AcceptCallback_t&& acceptCallback,
	StartAsyncIoCallback_t&& startAsyncIoCallback,
	StopAsyncIoCallback_t&& stopAsyncIoCallback)
: Base_t(std::forward<StartAsyncIoCallback_t>(startAsyncIoCallback),
	std::forward<StopAsyncIoCallback_t>(stopAsyncIoCallback))
, m_acceptCallback(acceptCallback)
, m_newConnection(nullptr)
{
     // Create acceptor endpoint.
    m_endpoint = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
	if (m_endpoint < 0) throw SystemException(errno);

	// Switch socket to non-blocking mode to take advantage of epoll API.
	int nonBlockMode = 1;
	if (ioctl(m_endpoint, FIONBIO, &nonBlockMode) < 0) throw SystemException(errno);

	// Reuse server address to get read of possible errors the previous connection has not been fully disconnected.
	int reuseAddr = 1;
	if (setsockopt(m_endpoint, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuseAddr, sizeof(int)) < 0)
		throw SystemException(errno);

	// Initialize IPv6 address data.
    addrinfo hint = {};
    hint.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
    hint.ai_family = AF_INET6;
    hint.ai_socktype = SOCK_STREAM;
    hint.ai_protocol = IPPROTO_TCP;

	std::stringstream portHint;
	portHint << port;

    int res = getaddrinfo("::1", portHint.str().c_str(), &hint, &m_addrInfo);
    if(res) throw SystemException(res);

	// Bind endpoint to the IPv6 address.
    if (bind(m_endpoint, m_addrInfo->ai_addr, static_cast<int>(m_addrInfo->ai_addrlen)) < 0)
        throw SystemException(errno);

	// Start listening to peer connections.
    if (listen(m_endpoint, 1) < 0)
		throw SystemException(errno);
}
    
AcceptorImpl::~AcceptorImpl()
{
	if(m_addrInfo)
		freeaddrinfo(m_addrInfo);
}

bool AcceptorImpl::Complete()
{
	// This callback triggded only when a new connection accepted.
	// In this case a callback from the server called.
	// We use server callback because accept operation should be handled
	// by the whole server, not an acceptor only. Thereby only the acceptor
	// is able to track accept operation completion.
	m_acceptCallback(m_newConnection);
	return true;
}

bool AcceptorImpl::Accept(IConnection* connection)
{
	socklen_t peerAddrLen = static_cast<socklen_t>(sizeof(m_peerAddr));
	int res = accept(m_endpoint, reinterpret_cast<sockaddr*>(&m_peerAddr), &peerAddrLen);
	if (res < 0)
	{
		// Triggered with empty queue of listening sockets, or socket has just been closed.
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED)
			return false;
		else
			throw SystemException(errno);
	}
	else if (!res)
	{
		// Socket closed.
		return false;
	}

	// Now connection instance got associated with socket descriptor and switched to non-blocking mode.
	m_newConnection = connection;
	m_newConnection->Set(res);
	return true;
}
	
std::string AcceptorImpl::GetPeerInfo()
{
	std::string hostName;
	std::string serviceName;

	hostName.resize(NI_MAXHOST);
	serviceName.resize(NI_MAXSERV);

	int res = getnameinfo(reinterpret_cast<sockaddr*>(&m_peerAddr), static_cast<int>(sizeof(m_peerAddr)),
		const_cast<char*>(hostName.c_str()), static_cast<int>(hostName.length()),
		const_cast<char*>(serviceName.c_str()), static_cast<int>(serviceName.length()), 0);
	if (res) throw SystemException(res);

	hostName.resize(strlen(hostName.c_str()));
	serviceName.resize(strlen(serviceName.c_str()));

	std::stringstream peerInfo;
	peerInfo << "Peer " << hostName << ":" << serviceName << " connected.";
	return peerInfo.str();
}

void AcceptorImpl::StartAsyncIo(IEndpoint* endpoint)
{
	m_startAsyncIoCallback(endpoint);
}

void AcceptorImpl::StopAsyncIo(IEndpoint* endpoint)
{
	m_stopAsyncIoCallback(endpoint);
}

ConnectionImpl::ConnectionImpl(
	OperationCallback_t&& dataExchangeCallback,
	StartAsyncIoCallback_t&& startAsyncIoCallback,
	StopAsyncIoCallback_t&& stopAsyncIoCallback) 
: Base_t(std::forward<StartAsyncIoCallback_t>(startAsyncIoCallback),
	std::forward<StopAsyncIoCallback_t>(stopAsyncIoCallback))
, m_dataExchange(false)
, m_dataExchangeCallback(std::forward<OperationCallback_t>(dataExchangeCallback))
{
	std::fill(std::begin(m_readBuf), std::end(m_readBuf), 0);
	std::fill(std::begin(m_writeBuf), std::end(m_writeBuf), 0);
}

void ConnectionImpl::Set(int fd)
{
	assert(fd);
	m_endpoint = fd;

	int nonBlockMode = 1;
	if (ioctl(m_endpoint, FIONBIO, &nonBlockMode) < 0) throw SystemException(errno);

	SetDataExchangeMode(true);
}

size_t ConnectionImpl::Read()
{
	int bytesRead = read(m_endpoint, &m_readBuf[0], MAX_BUF_SIZE);
	if (bytesRead < 0)
	{
		// Nothing to read yet.
		if (errno == EAGAIN) return bytesRead;
		else throw SystemException(errno);
	}

	return bytesRead;
}
	
size_t ConnectionImpl::Write(const std::string& data)
{
	// Copy output data into the buffer without buffer reallocation.
	size_t dataSize = data.length();
	std::copy(std::begin(data), std::end(data), std::begin(m_writeBuf));

	int bytesWritten = write(m_endpoint, &m_writeBuf, dataSize);
	if (bytesWritten < 0) throw SystemException(errno);

	return bytesWritten;
}

std::string ConnectionImpl::GetInputData()
{
	assert(m_dataExchange);

	// Copy input data until the \0 symbol occurred.
	auto endPos = std::find(std::begin(m_readBuf), std::end(m_readBuf), 0);
	std::string data;
	data.resize(std::distance(std::begin(m_readBuf), endPos));
	std::copy(std::begin(m_readBuf), endPos, std::begin(data));

	return data;
}

bool ConnectionImpl::Complete(IConnection* connection)
{
	assert(m_dataExchange);

	size_t dataSize = m_dataExchangeCallback(connection);
	if (!dataSize) return true;

	// After IO operation processing only part of data zeroed out.
	// A size of this part is the same as length of data portin just processed.
	boost::function<void(Buffer_t&, size_t)> ClearBuffer =
	[](Buffer_t& buf, size_t dataSize)
	{
		auto endPos = std::begin(buf) + dataSize;
		std::fill(std::begin(buf), endPos, 0);
	};

	ClearBuffer(m_readBuf, dataSize);
	ClearBuffer(m_writeBuf, dataSize);

	return true;
}

void ConnectionImpl::Reset()
{
	if (IsInitialState()) return;
	close(m_endpoint);
	m_endpoint = 0;
	SetDataExchangeMode(false);
}

void ConnectionImpl::StopAsyncIo(IEndpoint* endpoint)
{
	if (IsInitialState()) return;
	m_stopAsyncIoCallback(endpoint);
}

#endif