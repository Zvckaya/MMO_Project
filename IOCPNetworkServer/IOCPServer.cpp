#include "IOCPServer.h"
#include "Logger.h"
#include "PacketTypes.h"
#include <iostream>

static const wchar_t* PacketTypeName(uint16_t type)
{
	switch (type)
	{
	case PKT_ECHO:                 return L"ECHO";
	case PKT_CS_LOGIN_AUTH:        return L"CS_LOGIN_AUTH";
	case PKT_SC_LOGIN_AUTH_RESULT: return L"SC_LOGIN_AUTH_RESULT";
	case PKT_CS_MOVE:              return L"CS_MOVE";
	case PKT_SC_MOVE:              return L"SC_MOVE";
	case PKT_SC_CREATE_MY_CHARACTER:    return L"SC_CREATE_MY_CHARACTER";
	case PKT_SC_CREATE_OTHER_CHARACTER: return L"SC_CREATE_OTHER_CHARACTER";
	case PKT_SC_DESPAWN:           return L"SC_DESPAWN";
	case PKT_SC_MOVE_CORRECT:      return L"SC_MOVE_CORRECT";
	case PKT_SC_WORLD_ENTER:       return L"SC_WORLD_ENTER";
	case PKT_CS_STOP:              return L"CS_STOP";
	case PKT_CS_ATTACK:            return L"CS_ATTACK";
	case PKT_SC_ATTACK:            return L"SC_ATTACK";
	case PKT_CS_SKILL:             return L"CS_SKILL";
	case PKT_SC_SKILL:             return L"SC_SKILL";
	case PKT_CS_ITEM_PICKUP:       return L"CS_ITEM_PICKUP";
	case PKT_CS_ITEM_DROP:         return L"CS_ITEM_DROP";
	case PKT_SC_ITEM_APPEAR:       return L"SC_ITEM_APPEAR";
	case PKT_SC_ITEM_DISAPPEAR:    return L"SC_ITEM_DISAPPEAR";
	case PKT_SC_INVENTORY_UPD:     return L"SC_INVENTORY_UPD";
	case PKT_SC_NPC_SPAWN:         return L"SC_NPC_SPAWN";
	case PKT_SC_NPC_DESPAWN:       return L"SC_NPC_DESPAWN";
	case PKT_SC_NPC_MOVE:          return L"SC_NPC_MOVE";
	case PKT_SC_NPC_ATTACK:        return L"SC_NPC_ATTACK";
	default:                       return L"UNKNOWN";
	}
}

IOCPServer::~IOCPServer()
{
	Stop();
}

bool IOCPServer::Start(std::optional<std::string_view> openIp, uint16_t port,
	int workerThreadCount, int runningThreadCount,
	bool nagleOption, int maxSessionCount
)
{
	if (_isRunning.load())
	{
		return false;
	}


	WSADATA wsaData;
	int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != NO_ERROR) {
		std::cerr << "WSAStartup failed with error" << WSAGetLastError();
		return false;
	}

	_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, runningThreadCount);
	if (!_hIOCP)
	{
		std::cerr << "iocp handle create failed with error" << GetLastError();
		return false;
	}

	_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (_listenSock == INVALID_SOCKET) {  
		std::cerr << "socket failed with error" << WSAGetLastError();
		CloseHandle(_hIOCP);
		_hIOCP = nullptr;
		WSACleanup();
		return false;
	}

	sockaddr_in service;
	service.sin_family = AF_INET;
	service.sin_port = htons(port);
	if (!openIp.has_value()) //std::optional 값을 이용하여 ip가 없을때 대응 
	{
		service.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
	}
	else {  //ip 값을 전달했을때 
		inet_pton(AF_INET, std::string(*openIp).c_str(), &service.sin_addr);
	}

	if (bind(_listenSock,
		reinterpret_cast<sockaddr*>(&service), sizeof(service)) == SOCKET_ERROR) {
		std::cerr << "bind failed with error " << WSAGetLastError();
		closesocket(_listenSock);
		_listenSock = INVALID_SOCKET;
		CloseHandle(_hIOCP);
		_hIOCP = nullptr;
		WSACleanup();
		return false;
	}

	if (listen(_listenSock, SOMAXCONN_HINT(65535)) == SOCKET_ERROR) {
		std::cerr << "listen failed with error" << WSAGetLastError();
		closesocket(_listenSock);
		_listenSock = INVALID_SOCKET;
		CloseHandle(_hIOCP);
		_hIOCP = nullptr;
		WSACleanup();
		return false;
	}

		for (int i = 0; i < MAXSESSIONSIZE; i++)
		{
			_emptySlot.push(i);
		}
	

	offNagle = nagleOption; 
	_isRunning.store(true); //서버 가동을 flush 

	_acceptThread = std::jthread([this](std::stop_token stopToken) //accept 스레드 생성 
		{
			CreateAcceptThread(stopToken);
		});

	for (int j = 0; j < workerThreadCount; j++) //worker 스레드 생성 
	{
		_workerThreads.emplace_back([this]()
			{
				CreateWorkerThread(); //stok_token을 주지 않는 이유는 GQSC에 있는 할일을 마친후 종료 패킷을 넣어 종료한다.
			});
	}

	return true;
}

Session* IOCPServer::FindSession(SessionID sessionID)
{
	uint16_t slotIndex = static_cast<uint16_t>(sessionID & 0xFFFF);// 하위 16비트를 슬롯 인덱스로 사용함 

	Session& session = _sessions[slotIndex]; 
	if (session.GetSessionID() != sessionID)   //만약 요청한 세션id와 현재 세션의 id가 다르면 재사용된것
	{
		return nullptr;
	}

	return &session; // 세션 참조를 return함 
}

bool IOCPServer::ReleaseContentRef(SessionID sessionID) //콘텐츠레이어 job 람다 끝에서 해당 세션을 사용하기 위함.
{
	Session* session = FindSession(sessionID);
	if (session == nullptr)
	{
		return false;
	}

	session->ReleaseRef();
	return true;
}

void IOCPServer::CreateWorkerThread() //워커 io 스레드 생성 
{
	while (true)
	{
		LPOVERLAPPED overlapped;
		DWORD cbTransferredBytes = 0;
		Session* session = nullptr; //키로 사용할 세션 포인터

		bool ret = GetQueuedCompletionStatus(_hIOCP, &cbTransferredBytes, reinterpret_cast<PULONG_PTR>(&session), &overlapped, INFINITE);

		if (session == nullptr && overlapped == nullptr) //종료 패킷 
		{
			std::cout << "get thread delete\n";
			break;
		}

		if (ret == false || cbTransferredBytes == 0) //소켓 연결이 끊겼거나 0바이트 수신시 
		{
			int err = WSAGetLastError();
			session->Disconnect();
			continue;
		}

		if (session->IsRecvOverlapped(overlapped)) // recv 시 
		{
			RingBuffer& recvBuffer = session->GetRecvBuffer();
			recvBuffer.MoveRear(cbTransferredBytes); //받은 bytes만큼 rear증가 -> atomic함 

			constexpr int headerSize = static_cast<int>(sizeof(uint16_t) * 2);
			while (true)
			{
				if (recvBuffer.GetUseSize() < headerSize)
					break;

				char headerBuf[sizeof(uint16_t) * 2];
				recvBuffer.Peek(headerBuf, headerSize);
				uint16_t pktType     = 0;
				uint16_t payloadSize = 0;
				memcpy(&pktType,     headerBuf,                    sizeof(uint16_t));
				memcpy(&payloadSize, headerBuf + sizeof(uint16_t), sizeof(uint16_t));

				if (recvBuffer.GetUseSize() < headerSize + static_cast<int>(payloadSize))
					break;

				Packet* packet = _packetPool.Alloc();
				packet->Clear();
				recvBuffer.MoveFront(headerSize);

				int directSize = recvBuffer.DirectDequeueSize();
				if (payloadSize <= directSize)
				{
					packet->PutData(recvBuffer.GetFrontBufferPtr(), payloadSize);
				}
				else //랩어라운드시
				{
					packet->PutData(recvBuffer.GetFrontBufferPtr(), directSize);
					packet->PutData(recvBuffer.GetBufferStartPtr(), payloadSize - directSize);
				}

				recvBuffer.MoveFront(payloadSize);
				packet->SetType(pktType);

				SessionID sessionID = session->GetSessionID();
				Log(L"NET", Logger::Level::DEBUG,
					L"[RECV] sessionID=%llu  type=0x%04X(%s)  payload=%d",
					sessionID, pktType, PacketTypeName(pktType), payloadSize);
				session->AddContentRef(); //job을 꺼낼 동안은 사라지지 않게 ioCount를 올려줘야한다.
				OnRecv(sessionID, packet); //컨텐츠로 던지기 
			}

			session->RecvPost(); //recv 1회 제한 
			session->ReleaseRef(); //이제 해당 recv의 ioCount는 감소해도됨 
		}
		else if (session->IsSendOverlapped(overlapped))
		{
			if (session->CompleteSend(static_cast<int>(cbTransferredBytes))) //sendflag를 끄고, lock을 통해 front를 수정한다 
				session->SendPost();

			session->ReleaseRef(); //해당 send의 ioCOunt는 감소해도됨 
		}
	}
}

void IOCPServer::CreateAcceptThread(std::stop_token stopToken)
{
	while (!stopToken.stop_requested())
	{
		sockaddr_in clientAddr = {};
		int addrLen = sizeof(clientAddr);
		char clientIp[16] = {};
		SOCKET clientSock = accept(_listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);

		if (clientSock == INVALID_SOCKET)
		{
			int err = WSAGetLastError();
			if (err == WSAENOTSOCK || err == WSAEINVAL || err == WSAEINTR)
				break;

			std::cerr << "accept failed with error: " << err << "\n";
			continue;
		}

		inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, sizeof(clientIp));
		uint16_t clientPort = ntohs(clientAddr.sin_port);

		if (!OnConnectionRequest(clientIp, clientPort))
		{
			closesocket(clientSock);
			continue;
		}

		int slotIndex = -1;
		{
			std::lock_guard<std::mutex> lock(_emptyLock); //남는 슬롯 자리를 atomic하게 접근해야함 
			if (!_emptySlot.empty())
			{
				slotIndex = _emptySlot.top();
				_emptySlot.pop();
			}
		}

		if (slotIndex == -1)
		{
			std::cerr << "session slot full, rejecting client\n";
			closesocket(clientSock);
			continue;
		}

		Session& session = _sessions[slotIndex];

		uint64_t generation = _generationCnt.fetch_add(1); //일렬로 증가되는 카운트, 동일 슬롯이 재사용되었는지 확인가능함 

		uint64_t sessionID = (generation << 16) | slotIndex; // 48bit generation | 16비트 인덱스 
		session.SetSocket(clientSock);
		session.SetSessionID(sessionID);
		Log(L"NET", Logger::Level::SYSTEM,
			L"[CONNECT] %S:%d  sessionID=%llu  slot=%d",
			clientIp, clientPort, sessionID, slotIndex);

		session.SetEventHandler({ //람다를 이용하여 세션에 대하 컨텐츠에서 수행할 이벤드 핸들러를등록 한다 
			.onReturnIndex     = [this, slotIndex]() { //인덱스 반납시 
				std::lock_guard lock(_emptyLock);
				_emptySlot.push(slotIndex);
			},
			.onSessionReleased = [this](SessionID releasedSessionID) { //클라이언트 leave시 
				OnClientLeave(releasedSessionID);
			},
			.onSessionCleared  = [this]() { //서버 종료시 모든 io가 정리될때까지 정리하는 용도 overlapped 댕글링을 막음
				_drainCv.notify_all();
			}
		});

		int opOn = 1;
		int optOff = 0;
		linger lingerOpt{};
		lingerOpt.l_onoff = 1;
		lingerOpt.l_linger = 0;

		if (offNagle) //nagle 유무 
		{
			setsockopt(clientSock, IPPROTO_TCP, TCP_NODELAY,
				reinterpret_cast<const char*>(&opOn), sizeof(opOn));
		}

		setsockopt(clientSock, SOL_SOCKET, SO_LINGER,
			reinterpret_cast<const char*>(&lingerOpt), sizeof(lingerOpt)); //링거옵션 - 안키면 np 풀 고갈 발생
		
		//측정결과 zero-cpy로 얻는 이점보다 lock이나 irp의 생성 비용이 더 컸음. 나중에 뭉텅뭉텅 보낼때 사용해볼 만함 
		//setsockopt(clientSock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&optOff), sizeof(optOff));
		//setsockopt(clientSock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&optOff), sizeof(optOff));

		session.AcquireOwnerRef(); //세션 수명 ioCount증가 

		if (!CreateIoCompletionPort(reinterpret_cast<HANDLE>(clientSock), _hIOCP,
			reinterpret_cast<ULONG_PTR>(&session), 0))
		{
			std::cerr << "CreateIoCompletionPort associate failed: " << GetLastError() << "\n";
			session.ReleaseOwnerRef();
			closesocket(clientSock);
			std::lock_guard<std::mutex> lock(_emptyLock);
			_emptySlot.push(slotIndex);
			continue;
		}

		OnClientJoin(sessionID);
		session.RecvPost();  //recv 받기시작 
	}
}

void IOCPServer::Stop()
{
	if (!_isRunning.exchange(false))
	{
		return;
	}

	if (_listenSock != INVALID_SOCKET)
	{
		closesocket(_listenSock);
		_listenSock = INVALID_SOCKET;
	}

	_acceptThread.request_stop();
	if (_acceptThread.joinable())
	{
		_acceptThread = std::jthread();
	}

	for (Session& session : _sessions)
	{
		if (session.GetSocket() != INVALID_SOCKET && session.IsConnected())
		{
			session.Disconnect();
		}
	}

	WaitForSessionDrain(); //세션중에 ioCount가 0이 아닌 세션이 있을때까지 기다린다 .

	if (_hIOCP != nullptr)
	{
		for (size_t index = 0; index < _workerThreads.size(); ++index)
		{
			PostQueuedCompletionStatus(_hIOCP, 0, 0, nullptr); //워커스레드 숫자만큼 종료 신호 
		}
	}

	_workerThreads.clear();

	if (_hIOCP != nullptr)
	{
		CloseHandle(_hIOCP);
		_hIOCP = nullptr;
	}

	WSACleanup();
}

void IOCPServer::WaitForSessionDrain()
{
	std::unique_lock<std::mutex> lock(_drainMutex);
	_drainCv.wait(lock, [this]()
		{
			for (const Session& session : _sessions)
			{
				if (session.HasActiveIO())
				{
					return false;
				}
			}

			return true;
		});
}

bool IOCPServer::SendPacket(SessionID sessionID, Packet* packet)
{
	Session* session = FindSession(sessionID);
	if (session == nullptr)
	{
		disconnectError.fetch_add(1); 
		return false;
	}

	session->AddContentRef(); //컨텐츠에서 전송 요청을 보낼때, 세션을 찾는것과 사용하는것이 atomic하지 않음. 
	//즉 find 후 누가 disconnect 할 수 있어서 clear를막기 위해 ioCount를 올려놓는다.
	
	if (session->GetSessionID() != sessionID) //재사용된 세션이면 
	{
		session->ReleaseRef();  
		return false;
	}

	packet->EncodeHeader(); //헤더파싱 
	bool enqueued = session->TryEnqueueSend(sessionID, packet->GetBufferPtr(), packet->GetDataSize()); //내가 보내려던 세션인지 확인 
	if (enqueued)
		session->SendPost(); //큐잉 했으면 send함 

	session->ReleaseRef(); //해당 send에 대한 io 감소
	return enqueued;
}

bool IOCPServer::Disconnect(SessionID sessionID)
{
	Session* session = FindSession(sessionID);
	if (session == nullptr)
	{
		return false;
	}

	session->AddContentRef(); //이건 콘텐츠에서 사용하는 disconnect여서 ioCount를 증가시켜야함 
	if (session->GetSessionID() != sessionID)
	{
		session->ReleaseRef();

		return false;
	}

	session->Disconnect(); //진짜 disconnect 
	session->ReleaseRef(); //요청한 io감소 
	return true;
}

