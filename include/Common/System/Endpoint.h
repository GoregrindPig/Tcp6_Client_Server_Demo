#if !defined (__ENDPOINT_H__)
#define __ENDPOINT_H__

#include "CommonDefinitions.h"

#if defined(_WIN64)

// Interface of endpoint to be controlled by completion port.
struct IEndpoint
{
	virtual ~IEndpoint() = default;

	virtual SOCKET Get() = 0;
	virtual LPWSAOVERLAPPED GetContext() = 0;
	virtual void ResetContext() = 0;

	virtual void Complete(ULONG dataTransferred) = 0;
};

struct IConnection : IEndpoint
{
	virtual ~IConnection() = default;

	virtual void ReadAsync() = 0;
	virtual void WriteAsync(const std::string& data) = 0;
	virtual std::string GetInputData() = 0;
};

struct IAcceptor : IEndpoint
{
	virtual ~IAcceptor() = default;

	virtual bool AcceptAsync(IConnection* connection) = 0;
	virtual std::string GetPeerInfo() = 0;
};
template <typename Impl, typename Interface = IEndpoint>
class CEndpointBase : public Interface
{
public:
	template<typename... Args>
	CEndpointBase(Args&&... args)
	: m_impl(std::forward<Args>(args)...)
	{}

	virtual ~CEndpointBase() = default;

	SOCKET Get() override { return m_impl.Get(); }
	LPWSAOVERLAPPED GetContext() override { return m_impl.GetContext(); }
	void ResetContext() override { m_impl.ResetContext(); }

protected:
	Impl m_impl;
};

template <typename Impl>
class CAcceptorBase : public CEndpointBase<Impl, IAcceptor>
{
	using Base_t = CEndpointBase<Impl, IAcceptor>;
public:
	template<typename... Args>
	CAcceptorBase(Args&&... args)
	: Base_t(std::forward<Args>(args)...)
	{}

	virtual ~CAcceptorBase() = default;

	bool AcceptAsync(IConnection* connection) override
	{
		m_impl.Accept(connection);
		return true;
	}

	std::string GetPeerInfo() override
	{
		return m_impl.GetPeerInfo();
	}
};

template <typename Impl>
class CConnectionBase : public CEndpointBase<Impl, IConnection>
{
	using Base_t = CEndpointBase<Impl, IConnection>;
public:
	template<typename... Args>
	CConnectionBase(Args&&... args)
	: Base_t(std::forward<Args>(args)...)
	{}

	virtual ~CConnectionBase() = default;

	void ReadAsync() override {m_impl.Read();}
	void WriteAsync(const std::string& data) override {m_impl.Write(data);}
	std::string GetInputData() override {return m_impl.GetInputData();}
};

static auto SocketDeleter = [](SOCKET x)->void {if (x) closesocket(x); };

template 
<
    typename Derived,
    typename HandleType = SOCKET,
    typename HandleDeleterType = decltype(SocketDeleter),
    typename ContextType = WSAOVERLAPPED
>
class CEndpointImplBase
{
protected:
	static const size_t MAX_BUF_SIZE = 1024;
	ContextType m_context;
	HandleType m_hEndpoint;
	HandleDeleterType& m_deleter;

	CRTP_SELF(Derived)

public:
	CEndpointImplBase(HandleDeleterType& deleter = SocketDeleter)
		: m_hEndpoint((HandleType)0)
		, m_deleter(deleter)
	{}

	~CEndpointImplBase()
	{
		m_deleter(m_hEndpoint);
	}

	HandleType Get()
	{
		return m_hEndpoint;
	}

	ContextType* GetContext()
	{
		return &m_context;
	}

	void ResetContext()
	{
		Self().ResetContext();
	}
};

using OperationCallback_t = boost::function<void (IConnection*)>;
class CAcceptorImpl final : public CEndpointImplBase<CAcceptorImpl>
{
    addrinfo* m_addrInfo;
	OperationCallback_t m_acceptCallback;
	// Buffer must be big enough to hold following info:
	// 1. The number of bytes reserved for the local address information.
	// 	  This value must be at least 16 bytes more than
	//    the maximum address length for the transport protocol in use.
	// 2. The number of bytes reserved for the remote address information.
	// 	  This value must be at least 16 bytes more than
	//    the maximum address length for the transport protocol in use.
	// See AcceptEx API docummentation for more details.
	boost::array<BYTE, 2 * (sizeof(SOCKADDR_IN6) + 16)> m_acceptData;
	SOCKADDR_IN6* m_peerAddr;
	LPFN_ACCEPTEX m_pfnAcceptEx;
	LPFN_GETACCEPTEXSOCKADDRS m_pfnGetAcceptExSockaddrs;
	IConnection* m_newConnection;

	using Base_t = CEndpointImplBase<CAcceptorImpl>;

public:
    CAcceptorImpl(USHORT port, OperationCallback_t&& acceptCallback);
    ~CAcceptorImpl();

    void Complete(ULONG dataTransferred);
	void ResetContext();
	void Accept(IConnection* connection);
	std::string GetPeerInfo();
};

class CConnectionImpl final :  public CEndpointImplBase<CConnectionImpl>
{
	enum State
	{
		initial,
		readPending,
		writePending,
		disconnectPending	
	};

	using StateCallbackSequence_t = boost::unordered_map<State, OperationCallback_t>;

public:
	using Buffer_t = boost::array<char, MAX_BUF_SIZE>;

	CConnectionImpl(
		OperationCallback_t&& readCallback,
		OperationCallback_t&& writeCallback,
		OperationCallback_t&& disconnectCallback
		);

	~CConnectionImpl();

	void Read();
	void Write(const std::string& data);
	std::string GetInputData();

	void Complete(IConnection* connection, ULONG dataTransferred);
	void ResetContext();

private:
	void SwitchTo(State s) {m_curState = s;}
	void Disconnect();
	void Reset();

	State m_curState;
	Buffer_t m_readBuf;
	Buffer_t m_writeBuf;
	StateCallbackSequence_t m_callbacks;
	LPFN_DISCONNECTEX m_pfnDisconnectEx;
};

class CAcceptor final : public CAcceptorBase<CAcceptorImpl>
{
	using Base_t = CAcceptorBase<CAcceptorImpl>;
public:
	CAcceptor(USHORT port, OperationCallback_t&& acceptCallback)
	: Base_t(port, std::forward<OperationCallback_t>(acceptCallback)) {}

	void Complete(ULONG dataTransferred) override
	{
		m_impl.Complete(dataTransferred);
	}
};

class CConnection final : public CConnectionBase<CConnectionImpl>
{
	using Base_t = CConnectionBase<CConnectionImpl>;
public:
	CConnection(
		OperationCallback_t&& readCallback,
		OperationCallback_t&& writeCallback,
		OperationCallback_t&& disconnectCallback)
	: Base_t(
		std::forward<OperationCallback_t>(readCallback),
		std::forward<OperationCallback_t>(writeCallback),
		std::forward<OperationCallback_t>(disconnectCallback)) {}

	void Complete(ULONG dataTransferred) override
	{
		m_impl.Complete(this, dataTransferred);
	}
};

#elif defined(__linux__)

struct IEndpoint
{
	virtual ~IEndpoint() = default;

	virtual int Get() = 0;
	virtual bool Complete() = 0;
};

struct IConnection : IEndpoint
{
	virtual ~IConnection() = default;

	virtual void Set(int fd) = 0;
	virtual size_t ReadAsync() = 0;
	virtual size_t WriteAsync(const std::string& data) = 0;
	virtual std::string GetInputData() = 0;
	virtual void Disconnect() = 0;
};

struct IAcceptor : IEndpoint
{
	virtual ~IAcceptor() = default;

	virtual bool AcceptAsync(IConnection* connection) = 0;
	virtual std::string GetPeerInfo() = 0;
};

template <typename Impl, typename Interface = IEndpoint>
class EndpointBase : public Interface
{
public:
	template<typename... Args>
	EndpointBase(Args&&... args)
	: m_impl(std::forward<Args>(args)...)
	{}

	virtual ~EndpointBase() = default;

	int Get() override { return m_impl.Get(); }

protected:
	Impl m_impl;
};

template <typename Impl>
class AcceptorBase : public EndpointBase<Impl, IAcceptor>
{
	using Base_t = EndpointBase<Impl, IAcceptor>;
public:
	template<typename... Args>
	AcceptorBase(Args&&... args)
	: Base_t(std::forward<Args>(args)...)
	{}

	virtual ~AcceptorBase() = default;

	bool AcceptAsync(IConnection* connection) override { return this->m_impl.Accept(connection); }
	std::string GetPeerInfo() override { return this->m_impl.GetPeerInfo(); }
};

template <typename Impl>
class ConnectionBase : public EndpointBase<Impl, IConnection>
{
	using Base_t = EndpointBase<Impl, IConnection>;
public:
	template<typename... Args>
	ConnectionBase(Args&&... args)
	: Base_t(std::forward<Args>(args)...)
	{}

	virtual ~ConnectionBase() = default;

	void Set(int fd) override { this->m_impl.Set(fd); }
	size_t ReadAsync() override { return this->m_impl.Read(); }
	size_t WriteAsync(const std::string& data) override { return this->m_impl.Write(data); }
	std::string GetInputData() override { return this->m_impl.GetInputData(); }
};

using StartAsyncIoCallback_t = boost::function<void (IEndpoint*)>;
using StopAsyncIoCallback_t = boost::function<void (IEndpoint*)>;

template <typename Derived>
class EndpointImplBase
{
protected:
	static const size_t MAX_BUF_SIZE = 1024;
	int m_endpoint;
	StartAsyncIoCallback_t m_startAsyncIoCallback;
	StopAsyncIoCallback_t m_stopAsyncIoCallback;

	CRTP_SELF(Derived)

public:
	EndpointImplBase(
		StartAsyncIoCallback_t&& startAsyncIoCallback,
		StopAsyncIoCallback_t&& stopAsyncIoCallback)
	: m_endpoint(0)
	, m_startAsyncIoCallback(startAsyncIoCallback)
	, m_stopAsyncIoCallback(stopAsyncIoCallback)
	{}

	~EndpointImplBase()
	{
		close(m_endpoint);
	}

	int Get()
	{
		return m_endpoint;
	}

	bool Complete(IConnection* connection)
	{
		return Self().Complete(connection);
	}

	void StartAsyncIo(IEndpoint* endpoint)
	{
		Self().StartAsyncIo(endpoint);
	}

	void StopAsyncIo(IEndpoint* endpoint)
	{
		Self().StopAsyncIo(endpoint);
	}
};

using AcceptCallback_t = boost::function<void (IConnection*)>;
using OperationCallback_t = boost::function<size_t (IConnection*)>;

class AcceptorImpl final : public EndpointImplBase<AcceptorImpl>
{
    addrinfo* m_addrInfo;
	sockaddr_in6 m_peerAddr;
	AcceptCallback_t m_acceptCallback;
	IConnection* m_newConnection;

	using Base_t = EndpointImplBase<AcceptorImpl>;

public:
    AcceptorImpl(uint16_t port, AcceptCallback_t&& acceptCallback,
		StartAsyncIoCallback_t&& startAsyncIoCallback,
		StopAsyncIoCallback_t&& stopAsyncIoCallback);
    ~AcceptorImpl();

	void StartAsyncIo(IEndpoint* endpoint);
	void StopAsyncIo(IEndpoint* endpoint);

    bool Complete();

	bool Accept(IConnection* connection);
	std::string GetPeerInfo();
};

class ConnectionImpl final :  public EndpointImplBase<ConnectionImpl>
{
	using Base_t = EndpointImplBase<ConnectionImpl>;

public:
	using Buffer_t = boost::array<char, MAX_BUF_SIZE>;

	ConnectionImpl(
		OperationCallback_t&& dataExchangeCallback,
		StartAsyncIoCallback_t&& startAsyncIoCallback,
		StopAsyncIoCallback_t&& stopAsyncIoCallback);

	~ConnectionImpl() = default;

	void StartAsyncIo(IEndpoint*) {}
	void StopAsyncIo(IEndpoint* endpoint);

	bool Complete(IConnection* connection);

	void Set(int fd);
	size_t Read();
	size_t Write(const std::string& data);
	std::string GetInputData();
	void Reset();

private:
	void SetDataExchangeMode(bool dxm) { m_dataExchange = dxm; }
	bool IsInitialState() const { return !m_dataExchange; }

private:
	bool m_dataExchange;
	OperationCallback_t m_dataExchangeCallback;
	Buffer_t m_readBuf;
	Buffer_t m_writeBuf;
};

class Acceptor final : public AcceptorBase<AcceptorImpl>
{
	using Base_t = AcceptorBase<AcceptorImpl>;
public:
	Acceptor(unsigned short port, AcceptCallback_t&& acceptCallback,
		StartAsyncIoCallback_t&& startAsyncIoCallback,
		StopAsyncIoCallback_t&& stopAsyncIoCallback)
	: Base_t(port, std::forward<AcceptCallback_t>(acceptCallback),
		std::forward<StartAsyncIoCallback_t>(startAsyncIoCallback),
		std::forward<StopAsyncIoCallback_t>(stopAsyncIoCallback)) 
	{}

	virtual ~Acceptor() { m_impl.StopAsyncIo(this); }

	bool Complete() override { return m_impl.Complete(); }
	bool AcceptAsync(IConnection* connection) override
	{ 
		bool res = Base_t::AcceptAsync(connection);
		if (res) m_impl.StartAsyncIo(connection);
		return res;
	}
};

class Connection final : public ConnectionBase<ConnectionImpl>
{
	using Base_t = ConnectionBase<ConnectionImpl>;
public:
	Connection(
		OperationCallback_t&& dataExchangeCallback,
		StartAsyncIoCallback_t&& startAsyncIoCallback,
		StopAsyncIoCallback_t&& stopAsyncIoCallback)
	: Base_t(
		std::forward<OperationCallback_t>(dataExchangeCallback),
		std::forward<StartAsyncIoCallback_t>(startAsyncIoCallback),
		std::forward<StopAsyncIoCallback_t>(stopAsyncIoCallback)) {}

	virtual ~Connection() { Disconnect(); }

	bool Complete() override { return m_impl.Complete(this); }
	void Disconnect() override
	{
		m_impl.StopAsyncIo(this);
		m_impl.Reset();
	}
};


#endif // _WIN64

// An adapter class for anyb kind of container.
// Currently we provide specialization for a list
// but the template class can be specialized for
// any kind of container.

template <typename Container>
class ConnectionContainer final
{
public:
	bool IsEmpty() const;
	void Add(IConnection* c);
	IConnection* Release();
	void Remove(IConnection* c);
	void Purge();
};

template <> class ConnectionContainer<std::list<IConnection*>> final
{
	std::list<IConnection*> m_container;
public:
	~ConnectionContainer() { Purge(); }

	bool IsEmpty() const { return m_container.empty(); }

	void Add(IConnection* c)
	{
		if(c) m_container.push_back(c);
	}

	IConnection* Release()
	{
		IConnection* c = m_container.front();
		m_container.pop_front();
		return c;
	}

	void Remove(IConnection* c)
	{
		if(c) m_container.remove(c);
	}

	void Purge()
	{
		std::for_each(std::begin(m_container), std::end(m_container), [](IConnection* c) { delete c; } );
	}
};

template <> class ConnectionContainer<boost::unordered_set<IConnection*>> final
{
	boost::unordered_set<IConnection*> m_container;
public:
	~ConnectionContainer() { Purge(); }

	bool IsEmpty() const { return m_container.empty(); }

	void Add(IConnection* c)
	{
		if(c) m_container.insert(c);
	}

	IConnection* Release()
	{
		auto it = std::begin(m_container);
		IConnection* c = *it;
		m_container.erase(it);
		return c;
	}

	void Remove(IConnection* c)
	{
		if(c) m_container.erase(c);
	}

	void Purge()
	{
		std::for_each(std::begin(m_container), std::end(m_container), [](IConnection* c) { delete c; } );
	}
};

using PointerList_t = ConnectionContainer<std::list<IConnection*>>;
using PointerHashTable_t = ConnectionContainer<boost::unordered_set<IConnection*>>;
template
<
	size_t DEFAULT_CONNECTION_COUNT,
	typename Container,
	typename Creator,
	typename Lock,
	template <typename> typename Locker
>
class ConnectionManager final
{
protected:
	// Both lists accessed atomically.
	Lock m_lock;
	// Connection endpoints currently is use.
	Container m_activeConnections;
	// Connections endpoints that can be used without creating new ones.
	Container m_availConnections;
	// A function object from outside creating new entries.
	Creator m_creator;

public:
	ConnectionManager(Creator&& creator)
	: m_creator(std::forward<Creator>(creator))
	{
		// Allocate some number of connections beforehand to be available.
		for(size_t i = 0; i < DEFAULT_CONNECTION_COUNT; ++i)
			m_availConnections.Add(m_creator());
	}

	IConnection* Get()
	{
		Locker<Lock> locker(m_lock);
		IConnection* e = nullptr;

		// Is there something in the list of available connections?
		if (m_availConnections.IsEmpty())
		{
			// Create new entry.
			e = m_creator();
		}
		else
		{
			// Obtain the foremost entry.
			e = m_availConnections.Release();	
		}

		// Put entry in the list of active entries and return it.
		m_activeConnections.Add(e);
		return e;
	}

	void Release(IConnection* e)
	{
		Locker<Lock> locker(m_lock);

		// Remove entry from the active list.
		m_activeConnections.Remove(e);

		// Put it into the list of available entries.
		m_availConnections.Add(e);
	}
};

#endif // __ENDPOINT_H__
