# AGENTS.md

This file provides guidance to Codex when working with code in this repository.

## Collaboration Style

이 프로젝트는 포트폴리오용이며, 결과 코드뿐 아니라 문제를 어떻게 추론하고 해결했는지가 중요하다.

- 역할: 페어 프로그래머. 방향, 개념, 설계 힌트를 먼저 제시하고 사용자가 스스로 구현할 수 있게 돕는다.
- 코드 작성 트리거: 사용자가 명시적으로 `작성해줘`라고 말할 때만 실제 코드를 작성한다.
- 모던 C++ 우선: C++17/20 문법, RAII, smart pointer, `std::atomic`, `std::mutex`, `std::shared_mutex`, `std::jthread` 사용을 우선한다.
- 네이밍: camelCase를 기본으로 사용한다.
- `#define` 금지: 상수는 `constexpr`, 타입 별칭은 `using`을 우선한다.
- 주석 금지: 자체적으로 주석을 달지 않는다.

## 트러블슈팅 기록

- 버그 수정이나 복잡한 동기화 문제를 해결했을 때는 기록 여부를 먼저 사용자에게 묻는다.
- 사용자가 `#log` 또는 `기록해줘`라고 하면 현재 날짜 기준 `debug_log/YYYY-MM-DD-TROUBLESHOOTING.md`에 덧붙인다.
- `debug_log/`가 없으면 먼저 생성한다.
- 루트의 `TROUBLESHOOTING.md`는 더 이상 사용하지 않는다.

기록 형식:

```md
## [제목]
**날짜**: YYYY-MM-DD

### 현상
### 원인
### 추론 및 시도 과정
### 해결 방안
### 배운 점
```

## Repository Overview

현재 저장소에는 성격이 다른 두 축이 함께 있다.

- `IOCPNetworkServer/`
  - 현재 메인 실행 타깃이다.
  - 커스텀 IOCP 기반 TCP 서버 위에 `tcp_fighter` 성격의 실시간 대전 게임 서버 로직이 올라가 있다.
  - 루트 솔루션 `IOCPNetworkServer.sln`에 포함되어 실제로 바로 빌드되는 대상은 이쪽이다.
- `ServerCore/`
  - 별도 실험용 네트워크 라이브러리다.
  - IOCP와 RIO를 모두 감싼 공용 static library 구조다.
  - 현재 루트 솔루션에는 포함되어 있지 않고, `.gitignore`에도 포함된 로컬 참조 코드다.
- `CompletionPortTCPServer/`
  - 예전 스타일의 별도 참조 프로젝트다.
  - 현재 메인 작업 타깃으로 보지 않는다.

작업 우선순위는 기본적으로 `IOCPNetworkServer/`를 기준으로 잡고, 사용자가 명시할 때만 `ServerCore/`나 참조 프로젝트를 직접 수정한다.

## Main Project Overview

`IOCPNetworkServer/`는 Windows IOCP 기반 비동기 TCP 서버이며, 현재는 echo 서버가 아니라 `tcp_fighter` 게임 서버 구조를 가지고 있다.

- generation 기반 `SessionID`
- `ioCount` 기반 세션 생명주기 관리
- 네트워크 레이어와 콘텐츠 레이어 분리
- `GlobalQueue + per-session JobQueue + immediate threads + frame thread`
- `Packet`용 커스텀 메모리 풀 재사용
- sector grid 기반 주변 브로드캐스트
- 패킷 수신 시간 기반 heartbeat timeout disconnect

## Current Build Targets

Visual Studio 2022, MSVC v143, C++20, Windows only.

```powershell
msbuild IOCPNetworkServer.sln /p:Configuration=Debug /p:Platform=x64
msbuild IOCPNetworkServer.sln /p:Configuration=Release /p:Platform=x64
msbuild ServerCore\ServerCore.vcxproj /p:Configuration=Debug /p:Platform=x64
msbuild ServerCore\ServerCore.vcxproj /p:Configuration=Release /p:Platform=x64
```

## IOCPNetworkServer Runtime Layout

```text
[ GameServer ]
    |- immediate threads x4
    |- frame thread x1
    |- _players
    |- sector grid
    v
[ GlobalQueue ]
    v
[ per-session JobQueue ]
    v
[ IOCPServer ]
    |- accept thread x1
    |- worker threads
    |- packet pool
    v
[ std::array<Session, MAXSESSIONSIZE> ]
    v
[ RingBuffer ]
```

`main.cpp`에서는 현재 `gameServer.Start(std::nullopt, PORT, 16, 8, true, 20000)`로 서버를 시작한다.

## IOCPNetworkServer Implementation Notes

### Network Layer

- `SessionID`는 `[상위 generation][하위 16bit slot index]` 구조다.
- `IOCPServer::Start()`는 `listen(_listenSock, SOMAXCONN_HINT(65535))`를 사용한다.
- accept 성공 후 `OnClientJoin(sessionID)`가 `RecvPost()`보다 먼저 호출된다.
- 세션 배열은 `std::array<Session, MAXSESSIONSIZE>`로 고정되어 있고, 빈 슬롯은 `_emptySlot`로 관리한다.
- worker thread는 recv 완료 시 패킷을 분해한 뒤 콘텐츠 handoff ref를 추가하고 `OnRecv(sessionID, packet)`를 호출한다.
- `SendPacket(SessionID, Packet*)`는 slot index 접근 후 full `SessionID` equality를 다시 검증한다.
- 최종 enqueue 허용 여부는 `Session::TryEnqueueSend(ownerSessionID, ...)` 안에서 `_sendLock` 아래 다시 확인한다.
- `Session::TryEnqueueSend(...)`는 아래 조건을 모두 만족해야 성공한다.
  - `GetSessionID() == ownerSessionID`
  - `_isDisconnected == false`
  - `_sock != INVALID_SOCKET`
- `Session::Disconnect()`는 `_sendLock` 안에서 `_sock = INVALID_SOCKET`를 먼저 반영하고, 락 밖에서 `closesocket()`을 호출한다.
- `SetSessionID()`는 `store(memory_order_release)`, `GetSessionID()`는 `load(memory_order_acquire)`, `Clear()`는 `store(0, memory_order_release)`를 사용한다.
- `Session::SendPost()`는 `Disconnect()`를 `_sendLock` 밖에서 호출한다.
- `Session::Clear()`는 `_sendLock`을 잡은 뒤 `_sendBuffer`와 `_isSending`을 초기화한다.

### Packet Format

- 현재 `IOCPNetworkServer`의 패킷 헤더는 3바이트다.
- 형식은 `[code(1) | payloadSize(1) | packetType(1)]`다.
- `dfPACKET_CODE`는 `0x89`다.
- worker는 recv buffer에서 헤더를 먼저 검사하고, `packetType`을 `Packet::SetType()`으로 저장한다.
- `SendPacket()` 직전에 `packet->EncodeHeader()`가 호출된다.

### Packet Memory Pool

- `IOCPServer`는 `MemoryPool<Packet> _packetPool{ PACKET_POOL_SIZE };`를 가진다.
- recv worker는 완성 패킷마다 `_packetPool.Alloc()`으로 `Packet`을 가져오고, 사용 전에 `packet->Clear()`를 호출한다.
- `GameServer::OnRecv()`는 `std::shared_ptr<Packet>`를 사용하지 않는다.
- 현재 패킷 수명은 `worker alloc -> content job dispatch -> GameServer free` 흐름이다.
- `PACKET_POOL_SIZE`는 현재 `30000`이다.
- `MemoryPool.h`는 태그드 head를 쓰는 락프리 free list 기반 구현이다.

### GameServer

- `GameServer`는 `IOCPServer`를 상속한다.
- immediate thread 수는 현재 4개다.
- frame thread는 1개다.
- `OnClientJoin()`에서는 session별 `JobQueue`를 만들고, frame task로 플레이어 스폰을 처리한다.
- 플레이어 스폰 위치는 맵 범위 안에서 랜덤하게 정해진다.
- `_players`는 `SessionID -> std::unique_ptr<Player>` 맵으로 관리한다.
- `_sessionJobQueues`는 `SessionID -> std::shared_ptr<JobQueue>` 맵으로 관리한다.
- `_grid[SECTOR_COUNT_Y][SECTOR_COUNT_X]`가 주변 플레이어 visibility의 기준이 된다.
- `OnRecv()`는 session별 `JobQueue`에 작업을 `Post(...)`한다.
- 즉답 처리 스레드는 `GlobalQueue`에서 `JobQueue`를 꺼내 `Execute(endTick)`를 수행한다.
- frame thread는 `_frameTaskQueue`를 비우고 `Update(deltaMs)`를 돌린다.

### Player / Combat / Heartbeat

- `Player`는 `posX`, `posY`, `isMoving`, `moveDir`를 atomic으로 가진다.
- `Player`는 `hp`, `sectorX`, `sectorY`, `isDead`, `dir` 상태를 가진다.
- 이동 시작/정지는 immediate thread에서 상태를 반영하고, sector 변경 반영은 frame task로 넘긴다.
- 공격 처리(`OnCS_Attack`)는 frame task에서 `HandleAttack()`로 넘긴다.
- 공격 범위 안의 플레이어는 데미지를 받고, `hp == 0`이면 `isDead = true`가 된다.
- frame thread의 `Update()`는 `isDead` 플레이어를 `Disconnect()`한다.
- 최근 수신 시각은 `Player` 내부가 아니라 `GameServer::_lastRecvTimes[MAXSESSIONSIZE]`에 slot index 기준으로 저장한다.
- `Dispatch()`가 패킷을 처리할 때마다 해당 slot의 마지막 recv 시간을 갱신한다.
- `Update()`는 `dfNETWORK_PACKET_RECV_TIMEOUT`을 넘긴 세션을 disconnect한다.

### Protocol

현재 `Protocol.h`에는 최소한 아래 패킷들이 정의되어 있다.

- 생성/삭제
  - `dfPACKET_SC_CREATE_MY_CHARACTER`
  - `dfPACKET_SC_CREATE_OTHER_CHARACTER`
  - `dfPACKET_SC_DELETE_CHARACTER`
- 이동
  - `dfPACKET_CS_MOVE_START`
  - `dfPACKET_SC_MOVE_START`
  - `dfPACKET_CS_MOVE_STOP`
  - `dfPACKET_SC_MOVE_STOP`
- 공격/피해
  - `dfPACKET_CS_ATTACK1`
  - `dfPACKET_SC_ATTACK1`
  - `dfPACKET_CS_ATTACK2`
  - `dfPACKET_SC_ATTACK2`
  - `dfPACKET_CS_ATTACK3`
  - `dfPACKET_SC_ATTACK3`
  - `dfPACKET_SC_DAMAGE`
- 보조
  - `dfPACKET_SC_SYNC`
  - `dfPACKET_CS_ECHO`
  - `dfPACKET_SC_ECHO`

## ServerCore Summary

`ServerCore/`는 현재 메인 실행 코드와 별개로 관리되는 공용 네트워크 static library다.

- `IocpCore`
  - completion port handle을 만들고 `Register()`와 `Dispatch()`를 담당한다.
- `IocpService`
  - session 생성 팩토리, session 집합, 서비스 시작 상태를 관리한다.
- `IocpServer`
  - `IocpListener`와 worker thread를 통해 IOCP 서버를 구동한다.
- `IocpClient`
  - IOCP 기반 클라이언트 서비스 구현이 있다.
- `IocpSession`
  - `Connect/Disconnect/Send/RegisterRecv/RegisterSend`를 제공한다.
  - recv buffer는 `NetBuffer`, send는 `queue<shared_ptr<NetBuffer>>` 기반이다.
  - 콘텐츠 레벨 확장 포인트는 `OnConnected`, `OnRecvPacket`, `OnSend`, `OnDisconnected`다.
- `RioServer` / `RioSession`
  - Registered I/O 기반 서버 구현이 함께 있다.
  - send는 deferred commit 기반으로 모아 전송한다.
- `ServerProxy`
  - `ServerType::IOCP_SERVER`와 `ServerType::RIO_SERVER`를 감싼 공용 진입점이다.
  - `Start(bool useJobQueue)`로 네트워크 dispatch와 `GlobalQueue` job 실행을 섞어서 돌릴 수 있다.
- `PacketHeader`
  - `ServerCore` 쪽 wire format은 `PacketHeader { unsigned short size; unsigned short id; }`다.
  - 즉, 메인 프로젝트 `IOCPNetworkServer`의 3바이트 헤더와 형식이 다르다.
- `ObjectPool` / `Poolable`
  - Windows `SLIST` 기반 오브젝트 풀을 제공한다.
- `ThreadManager`
  - 공용 worker thread 생성과 join을 담당한다.

## Layering Rules

`IOCPNetworkServer` 기준으로 네트워크 레이어와 콘텐츠 레이어는 서로의 내부 타입을 직접 알지 않는다.

- 네트워크 레이어에서 콘텐츠 레이어로 `Session*`를 넘기지 않는다.
- 콘텐츠 레이어에서 네트워크 레이어로 `Player*`를 넘기지 않는다.
- 네트워크 레이어는 `SessionID + Packet*`만 콘텐츠 레이어로 전달한다.
- 콘텐츠 레이어는 `SendPacket(SessionID, Packet*)`와 `Disconnect(SessionID)`만 사용해서 네트워크 레이어에 응답한다.
- `OnRecv` 시그니처는 `OnRecv(SessionID sessionID, Packet* packet)` 기준으로 유지한다.

## Current Notes / Gaps

- `IOCPNetworkServer/Packet.cpp`는 헤더 기반 최신 `Packet.h`와 맞지 않는 예전 구현이 남아 있지만, 현재 프로젝트 빌드에는 포함되지 않는다.
- 루트 솔루션은 아직 `IOCPNetworkServer`만 포함하고 있어 `ServerCore`는 별도로 빌드해야 한다.
- `ServerCore`의 패킷 포맷과 `IOCPNetworkServer`의 패킷 포맷이 다르므로 두 코드를 섞어서 해석하면 안 된다.
- `AGENTS.md`를 갱신할 때는 항상 현재 실행 타깃이 `IOCPNetworkServer`인지, `ServerCore` 실험 코드인지 먼저 구분한다.

## Platform Notes

- Windows only
- `<winsock2.h>`는 `<Windows.h>`보다 먼저 include한다.
- `Ws2_32.lib`를 링크한다.
- C-style cast는 쓰지 않고 `static_cast`, `reinterpret_cast`를 사용한다.
