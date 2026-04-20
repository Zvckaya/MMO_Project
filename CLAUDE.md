# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Collaboration Style

이 프로젝트는 **포트폴리오용**이며, 구조 완성을 우선으로 한다.

- **역할**: 적극적인 구현 파트너. 코드 제안은 자유롭게 하되, 실제 파일 수정은 사용자 승인 후에만 한다. 파일을 수정했을 경우 반드시 변경 내용을 명시한다.
- **모던 C++ 우선**: C++17/20 문법(`std::optional`, `std::jthread`, `std::unique_ptr`, `std::atomic`, `std::mutex`, `std::lock_guard` 등)을 적극 권장한다. C-style API 래핑 시에도 RAII, smart pointer를 활용하도록 안내한다.
- **네이밍**: camelCase를 기본으로 사용한다. (변수, 함수, 멤버 모두)
- **`#define` 금지**: 상수는 `constexpr`, 타입 별칭은 `using`, 조건부 컴파일 외 매크로 사용 금지.
- **주석 금지**: 코드에 주석을 자체적으로 추가하지 않는다. 사용자가 직접 달아둔 주석은 절대 수정하거나 삭제하지 않는다.

### 트러블슈팅 기록

- 버그 수정 또는 복잡한 로직(메모리 누수, 동기화 문제 등) 완성 시 → 기록 여부를 사용자에게 먼저 물어본다.
- 사용자가 **`#log`** 또는 **"기록해 줘"** 라고 하면 → 직전 대화 맥락과 수정 코드를 바탕으로 아래 양식으로 기록한다.
- 기록 파일 규칙:
  - 현재 날짜 기준 `debug_log/YYYY-MM-DD-TROUBLESHOOTING.md` 파일에 덧붙인다.
  - 해당 날짜 파일이 없으면 새로 생성하고, 이미 있으면 기존 파일에 덧붙인다.
  - `debug_log/` 폴더가 없으면 먼저 생성한다.
  - 루트의 `TROUBLESHOOTING.md` (날짜 없는 파일)는 더 이상 사용하지 않는다.

```
## [제목]
**날짜**: YYYY-MM-DD

### 현상
### 원인
### 추론 및 시도 과정
### 해결 방안
### 배운 점
```

## Project Overview

Windows IOCP (Input/Output Completion Ports) network server — 상용 서비스 투입 가능한 수준의 고성능 비동기 TCP 서버. 단순 예제가 아닌 안정성·확장성·성능을 갖춘 프로덕션 품질을 지향한다.

## Build System

**Visual Studio 2022 (MSVC v143), C++20** — Windows only.

```
msbuild IOCPNetworkServer.sln /p:Configuration=Debug /p:Platform=x64
msbuild IOCPNetworkServer.sln /p:Configuration=Release /p:Platform=x64
```

## 레이어 분리 원칙

**네트워크 레이어(`IOCPServer`, `Session`)와 콘텐츠 레이어(`GameServer`, `Player`)는 서로의 존재를 모른다.**

- 두 레이어 사이에 포인터를 직접 넘기지 않는다. (`Session*`을 콘텐츠 레이어로, `Player*`를 네트워크 레이어로 전달 금지)
- 인터페이스는 **요청 → 응답** 형태로만 통신한다: 네트워크 레이어는 `SessionID` + `Packet*`만 콘텐츠 레이어로 전달하고, 콘텐츠 레이어는 `SendPacket(SessionID, Packet*)` / `Disconnect(SessionID)` 로만 네트워크에 응답한다.
- `OnRecv`는 `SessionID`만 전달한다: `OnRecv(SessionID sessionID, Packet* packet)`

## Architecture

### 레이어 구조

```
[ 콘텐츠 게임 서버 ]  ← IOCPServer를 상속, virtual 함수만 구현
        ↓
[   IOCPServer    ]  ← 네트워크 라이브러리 레이어
        ↓
[  Session 배열   ]  ← std::array<Session, MAXSESSIONSIZE>, 서버 시작 시 전체 생성
        ↓
[   RingBuffer    ]  ← Session당 recvBuffer / sendBuffer 각 1개
```

### IOCPServer

- `Start()` → WSAStartup → IOCP 핸들 생성 → 소켓 bind/listen → 빈 슬롯 초기화 → Accept/Worker 스레드 시작
- `CreateWorkerThread()`, `CreateAcceptThread()` — `private`. 외부에서 직접 호출 불가.
- `FindSession(SessionID)` — `private`. slotIndex 추출 → 세션 배열 접근 → sessionID 일치 확인 → `Session*` 반환 (불일치 시 nullptr). `SendPacket`, `Disconnect`, `ReleaseContentRef`에서 공통으로 사용.
- `ReleaseContentRef(SessionID)` — `protected`. 콘텐츠 레이어(GameServer)의 job 람다 끝에서 호출. `FindSession`으로 세션 확인 후 `session->ReleaseRef()`.
- `_hIOCP = nullptr`, `_listenSock = INVALID_SOCKET` — 멤버 선언 시 초기화.
- `_emptySlot` (`std::stack<int>`) — 사용 가능한 세션 배열 인덱스 보관. `std::mutex _emptyLock` + `std::lock_guard`로 보호.
- `_generationCnt` (`std::atomic<uint64_t>`) — 세션 할당 시 단조 증가. SessionID의 상위 48비트에 사용.
- `listen()` — `SOMAXCONN_HINT(65535)` 사용 (Windows에서 최대 backlog 65535 명시).
- **Accept 스레드** (`std::jthread`) — `accept()` 루프. 접속 시 `OnConnectionRequest` → 슬롯 확보 → generation 기반 SessionID 생성 → 3개 콜백 등록(`ReturnIndex`, `OnSessionReleased`, `OnSessionCleared`) → 소켓 옵션 적용 → `_ioCount++`(소유권 ref) → IOCP 등록 → `OnClientJoin` → `RecvPost()`. 리슨 소켓 에러(`WSAENOTSOCK`, `WSAEINVAL`, `WSAEINTR`)시에만 루프 탈출.
  - 소켓 옵션: `TCP_NODELAY`(nagle off 선택), `SO_LINGER=1`(hard reset — TIME_WAIT 없음), `SO_SNDBUF=0` / `SO_RCVBUF=0`(커널 버퍼 제거 → zero-copy).
- **Worker 스레드** (`std::vector<std::jthread>`) — `GetQueuedCompletionStatus` 루프. completion key = `Session*`. `overlapped` 포인터를 `_recvOverLapped`/`_sendOverLapped`와 비교해 recv/send 완료 구분.
- **스레드 종료 신호**: `PostQueuedCompletionStatus(hIOCP, 0, 0, nullptr)` → GQCS에서 session==nullptr && overlapped==nullptr → break.
- **`Stop()` 순서**: 리슨 소켓 닫기 → Accept 스레드 중단 → 모든 활성 세션 `Disconnect()` → `WaitForSessionDrain()` (모든 `_ioCount == 0`까지 `_drainCv` 대기) → Worker 스레드 수만큼 `PostQueuedCompletionStatus` 종료 신호 → Worker 스레드 join → IOCP 핸들 닫기 → `WSACleanup`.

### SessionID

`using SessionID = uint64_t`. 구성:
```
[ 상위 48bit: generation ] [ 하위 16bit: slot index ]
```
- generation: `IOCPServer::_generationCnt.fetch_add(1)` — 서버 전체 단조 증가. Session 멤버로 관리하지 않음.
- 슬롯 재사용 시 generation이 달라지므로 이전 세션의 ID로는 SendPacket이 드롭됨.
- 추출: `slotIndex = sessionID & 0xFFFF`, `generation = sessionID >> 16`

### Session — 생명주기 (ioCount 기반)

멤버 (private):
- `std::atomic<SessionID> _playerId` — acquire/release 메모리 순서. `Clear()`의 store(0)와 `FindSession()`의 load 사이 data race 제거. `SetSessionID` = store(release), `GetSessionID` = load(acquire).
- `std::atomic<int> _ioCount` — ref count. 0이 되면 `Clear()` 호출.
- `std::atomic<bool> _isDisconnected` — 첫 번째 에러를 감지한 스레드가 CAS로 독점.
- `std::function<void()> ReturnIndex` — `Clear()` 마지막에 호출. 슬롯을 `_emptySlot`으로 반환. Accept 스레드에서 람다로 주입.
- `std::function<void(SessionID)> OnSessionReleased` — `Clear()` 내에서 `OnClientLeave` 전달용. Accept 스레드에서 주입.
- `std::function<void()> OnSessionCleared` — `Clear()` 완료 시 `_drainCv` 깨우기용. Accept 스레드에서 주입.

**ioCount 증감 규칙**:
```
Accept 시              → _ioCount++ (소유권 ref = 1)
RecvPost/SendPost 전   → _ioCount++ (I/O ref)
OnRecv 호출 전         → AddContentRef() = _ioCount++ (콘텐츠 job ref)
완료 루틴 정상 처리    → ReleaseRef() (I/O ref 반납)
job 실행 완료          → ReleaseContentRef(sessionID) (콘텐츠 job ref 반납)
OnRecv early-exit 시   → ReleaseContentRef(sessionID) 즉시 반납
첫 에러 감지           → Disconnect() → CAS 성공 시 closesocket + ReleaseRef(소유권) + ReleaseRef(이 I/O)
                         CAS 실패 시 ReleaseRef(이 I/O)만
_ioCount == 0          → Clear()
```

> **콘텐츠 job ref의 목적**: ImmediateThread가 `TryEnqueueSend`를 실행하는 동안 `ioCount > 0`을 보장해 `Clear()`를 억제. 이 ref가 없으면 job 실행 중 세션이 Clear되어 sendBuffer에 stale write가 발생할 수 있다.

**ReleaseRef()**:
```cpp
void Session::ReleaseRef() {
    if (_ioCount.fetch_sub(1) == 1) Clear();
}
```

**Disconnect()**:
```cpp
void Session::Disconnect() {
    bool expected = false;
    if (_isDisconnected.compare_exchange_strong(expected, true)) {
        SOCKET sockToClose = INVALID_SOCKET;
        {
            std::lock_guard<std::mutex> lock(_sendLock);
            sockToClose = _sock;
            _sock = INVALID_SOCKET;   // TryEnqueueSend가 _sendLock 안에서 검사 — 원자적 차단
        }
        if (sockToClose != INVALID_SOCKET)
            closesocket(sockToClose);
        ReleaseRef(); // 소유권 ref
    }
    ReleaseRef(); // 이 I/O ref
}
```

**Clear()** — 콜백을 로컬로 이동한 뒤 순서대로 호출. `returnIndex`가 반드시 마지막. sendBuffer 초기화는 `_sendLock` 안에서:
```cpp
void Session::Clear() {
    auto returnIndex       = std::move(ReturnIndex);
    auto onSessionReleased = std::move(OnSessionReleased);
    auto onSessionCleared  = std::move(OnSessionCleared);
    SessionID releasedID   = _playerId.load(std::memory_order_acquire);

    _playerId.store(0, std::memory_order_release);
    _recvBuffer.ClearBuffer();
    {
        std::lock_guard<std::mutex> lock(_sendLock);
        _sendBuffer.ClearBuffer();   // TryEnqueueSend와 상호 배타 보장
        _isSending = false;
    }
    _isDisconnected = false;

    if (onSessionReleased && releasedID != 0) onSessionReleased(releasedID); // OnClientLeave
    if (onSessionCleared)  onSessionCleared();                               // _drainCv.notify_all()
    if (returnIndex)       returnIndex();    // 반드시 마지막 — 앞에 두면 새 클라이언트 race
}
```

> `sendBuffer.ClearBuffer()`를 `_sendLock` 안에 넣은 이유: `TryEnqueueSend`도 `_sendLock`을 잡으므로, Clear와 TryEnqueueSend가 sendBuffer를 동시에 건드리는 것을 방지. `_playerId`는 `std::atomic<SessionID>`이므로 `Clear()`의 store와 `FindSession()`의 load 사이의 data race는 제거됨. 단, FrameThread 등 **콘텐츠 job ref 없이** `SendPacket`을 호출하는 경로에서는 `FindSession` 통과 후 `Clear` 완료 + 슬롯 재사용 window가 여전히 존재. 현재는 recv-triggered job만 사용하므로 `AddContentRef`로 커버됨.

**RecvPost()** — `_ioCount.fetch_add(1)` 후 WSARecv. 즉시 에러 시 `Disconnect()`. `GetFreeSize()` 기준으로 wrap-around 시 WSABUF 2개 등록:
- `bufs[0]`: `GetRearBufferPtr()`, `DirectEnqueueSize()`
- `bufs[1]`: `GetBufferStartPtr()`, 나머지 (wrap 구간)

**SendPost()** — `_sendLock` 획득 후 CAS로 단일 진입 보장, `_ioCount.fetch_add(1)`, WSASend. 즉시 에러 시 `_sendLock` 해제 **후** `Disconnect()` (`needDisconnect` 플래그 패턴):
```cpp
bool needDisconnect = false;
{
    std::lock_guard<std::mutex> lock(_sendLock);
    bool expected = false;
    if (!_isSending.compare_exchange_strong(expected, true)) return;
    // totalSize == 0이면 _isSending = false 후 return (필수)
    // WSASend 즉시 에러 시 needDisconnect = true
}
if (needDisconnect) Disconnect(); // 락 밖에서 호출 — 데드락 방지
```
> `_sendLock` 잡은 채로 `Disconnect()` 호출 금지: `Disconnect()` → `ReleaseRef()` → `Clear()` → `_sendLock` 재획득 시도 → 같은 스레드 데드락.

**TryEnqueueSend() / CompleteSend()** — sendBuffer 접근은 `_sendLock`으로 보호:
- `TryEnqueueSend(ownerSessionID, data, size)` — `_sendLock` 잠금 후 `GetSessionID() == ownerSessionID`, `_isDisconnected == false`, `_sock != INVALID_SOCKET` 재검증 → `GetFreeSize()` 확인 → `Enqueue`. `SendPacket`에서 호출.
  - `Disconnect()`도 `_sendLock` 안에서 `_sock = INVALID_SOCKET`을 먼저 반영하므로, 이 검증이 stale enqueue를 완전히 차단한다.
- `CompleteSend(bytes)` — `_sendLock` 잠금 후 `MoveFront(bytes)`, `_isSending = false`, 잔여 데이터 유무 반환. GQCS send 완료에서 호출.

> `_sendLock`이 `SendPost` + `TryEnqueueSend` + `CompleteSend` + `Disconnect`의 `_sock` 변경을 모두 보호하므로, 다중 ImmediateThread에서 `SendPacket`을 호출해도 sendBuffer 접근은 안전하다.

**GQCS recv 완료 처리 순서**:
1. `MoveRear(bytes)` — rear 포인터 전진
2. 패킷 파싱 루프: `Peek(header)` → payload 크기 확인 → `new Packet()` → `MoveFront(sizeof(uint16_t))` → `PutData` (wrap-around 시 temp 버퍼 경유, `DirectDequeueSize` 기준 분기) → `MoveFront(payloadSize)` → **`session->AddContentRef()`** → `OnRecv(sessionID, packet)` (Packet* 소유권 이전)
3. `RecvPost()` 재호출 → **마지막에** `ReleaseRef()`

> `AddContentRef()`는 패킷 1개당 1회 호출. `OnRecv` → job 람다 끝의 `ReleaseContentRef(sessionID)`와 반드시 1:1 대응해야 한다. `OnRecv` 내부에서 early-exit(게임 미실행, JobQueue 없음 등)하는 모든 경로에서도 `ReleaseContentRef`를 호출해야 한다.

**GQCS send 완료 처리 순서**:
1. `session->CompleteSend(bytes)` — sendBuffer `MoveFront` + `_isSending = false`, 잔여 데이터 있으면 true 반환
2. true이면 `SendPost()` 재호출 → **마지막에** `ReleaseRef()`

> **주의**: `ReleaseRef()`는 반드시 `SendPost()`/`RecvPost()` 등 새 I/O 등록 완료 **이후**에 호출해야 한다. 앞에 두면 다른 스레드의 에러로 ioCount가 0이 되어 `Clear()` 후 새 클라이언트 소켓에 I/O가 등록되는 race가 발생한다.

**GQCS 에러/0바이트 처리**:
```cpp
if (ret == false || cbTransferredBytes == 0) {
    session->Disconnect();
    continue;
}
```

### RingBuffer

- `std::array<char, BUFFERSIZE>` 기반. Resize 없음 (고정 크기, 버퍼 초과 시 세션 종료 정책).
- Direct 계열 메서드(`GetRearBufferPtr`, `DirectEnqueueSize`, `MoveRear`, `GetBufferStartPtr` 등) — WSARecv/WSASend zero-copy 인터페이스.
- `Enqueue`는 **쓰는 쪽(rear)** wrap-around만 처리. **읽는 쪽(chpData)**은 연속 메모리를 가정하므로, wrap-around된 recvBuffer를 `GetFrontBufferPtr()` 하나로 Enqueue하면 안 됨 → `DirectDequeueSize` 기준으로 Enqueue 2회 분리 필요.

### 가상 콜백 (콘텐츠 서버 구현 대상)

| 콜백 | 시점 | 비고 |
|------|------|------|
| `OnConnectionRequest(ip, port)` | accept 직후 | false 반환 시 즉시 거부 |
| `OnClientJoin(SessionID)` | IOCP 등록 + RecvPost 후, Accept 스레드에서 호출 | |
| `OnClientLeave(SessionID)` | `Clear()` 내 `OnSessionReleased` 콜백으로 호출 | |
| `OnRecv(SessionID, Packet*)` | 워커 스레드에서 패킷 파싱 완료 후 즉시 호출 | Packet* 소유권 이전. SessionID만 전달 — Session* 노출 없음 |

### 게임 스레드 구조 (GameServer)

`GameServer`는 두 개의 전용 스레드 유형을 운용한다:

**ImmediateThread** (`CreateImmediateThread`) — 현재 6개 (`constexpr int immediateThreadCount = 6`)
- `GlobalQueue::Pop(stopToken)` 대기 → `JobQueue` 획득 → `Execute(endTick)` (64ms 타임슬라이스)
- per-session `JobQueue` 기반 Actor 모델: 같은 세션의 패킷은 항상 하나의 스레드만 순차 처리
- `_sessionJobQueues: unordered_map<SessionID, shared_ptr<JobQueue>>` + `shared_mutex`로 보호

**FrameThread** (`CreateFrameThread`)
- `FRAME_RATE` (ServerConfig.h) 기준 고정 프레임 루프
- `OnClientJoin` / `OnClientLeave`에서 enqueue된 `FrameTask`를 `std::swap`으로 localQueue에 옮겨 처리
- `_players` 맵 조작은 이 스레드에서만 → lock 불필요
- 프레임 처리 후 `Update(deltaMs)` 호출

```cpp
// GameServer 내부 (private)
enum class FrameTaskType { clientJoin, clientLeave };
struct FrameTask { FrameTaskType type; SessionID sessionID; };
```

**OnRecv 처리 흐름**:
```cpp
// Worker가 AddContentRef() 후 OnRecv 호출
void GameServer::OnRecv(SessionID sessionID, Packet* packet)
{
    // early-exit 모든 경로에서 ReleaseContentRef 필수
    if (!_isGameRunning) { delete packet; ReleaseContentRef(sessionID); return; }

    jobQueue->Post([this, sessionID, pkt = shared_ptr<Packet>(packet)]()
    {
        _packetProc.Dispatch(sessionID, pkt.get());
        ReleaseContentRef(sessionID);   // AddContentRef와 1:1 대응
    });
}
```

### MemoryPool

`MemoryPool.h` — `MemoryPool<DATA>` 템플릿. 태그드 포인터 기반 락프리 구현.

**노드 구조 (침습형)**:
```cpp
struct Node {
    MemoryPool*                owner;     // 소유 풀 주소 — 크로스풀 Free 감지
    uint32_t                   checkCode; // CODE_ALLOC(0x99999999) / CODE_FREE(0xDEADDEAD)
    Node*                      next;
    bool                       isConstructed;
    alignas(DATA) unsigned char data[sizeof(DATA)];
};
```

- `Free(DATA*)` — `offsetof(Node, data)`로 포인터 역산해 노드 복구. 별도 맵/해시 불필요.
- `placementNew` 플래그: `true`이면 Alloc/Free마다 생성자/소멸자 호출, `false`이면 최초 1회만 생성자 호출 (이후 재사용 시 raw 반환).
- 풀 소진 시 동적 확장 (`new Node`) — 상한 없음.
- **스레드 안전: 락프리** — `std::atomic<uint64_t> _head`에 상위 16비트 태그 + 하위 48비트 포인터 패킹. Push/Pop 모두 CAS 루프. ABA는 태그 단조 증가로 방지.
- `checkCode` 이중 해제·오염 감지 — 위반 시 `std::abort()`.
- 카운터(`_capacity`, `_useCount`) — `std::atomic<int>`.

**Packet 연동**:
- `IOCPServer` protected 멤버: `MemoryPool<Packet> _packetPool{ PACKET_POOL_SIZE }` (30000개 선할당 ≈ 30MB)
- Worker 스레드: `_packetPool.Alloc()` + `packet->Clear()` — `new` 대체
- `GameServer::OnRecv` job 람다: `Dispatch` 후 `_packetPool.Free(packet)` — `TryEnqueueSend`에서 sendBuffer로 복사 완료 후 즉시 반납
- `shared_ptr<Packet>` 사용 안 함 — 컨트롤 블록 힙 할당 비용 제거, raw 포인터 + 수동 Free

### Packet

`Packet.h` / `Packet.cpp` — 고정 배열(`char _buffer[MAXPAYLOAD]`) 기반 직렬화 버퍼.

```
_buffer: [ size(2byte) | payload... ]
                ↑
         _readPos / _writePos = sizeof(uint16_t) 에서 시작
```

- `operator<<` / `operator>>` — 타입별 명시적 오버로딩
- `PutData(const char*, int)` — 워커 스레드에서 recvBuffer → Packet 복사 시 사용
- `EncodeHeader()` — `IOCPServer::SendPacket` 내부에서 자동 호출. 호출자가 직접 부를 필요 없음.
- `Clear()` — `_readPos` / `_writePos` 리셋. 풀 재사용 시 `Alloc()` 직후 호출 (`placementNew=false`이므로 생성자 재호출 없음)

### ServerConfig.h

상수 전용 파일. Winsock 헤더 포함 금지. `<cstdint>` 필수.

```cpp
constexpr int      BUFFERSIZE      = 20000;
constexpr int      MAXSESSIONSIZE  = 20000;
constexpr uint16_t PORT            = 6000;
constexpr int      MAXPAYLOAD      = 1000;
constexpr int      FRAME_RATE      = 60;
constexpr int      PACKET_POOL_SIZE = 30000;
```

### GameServer / Player

- `GameServer` — `IOCPServer` 상속. `_players(unordered_map<SessionID, unique_ptr<Player>>)` 관리.
- `Player` — `SessionID`를 키로 사용. 별도 PlayerID 없음(DB 연동 시 추가 예정). `posX`, `poxY` 멤버 보유. (`poxY`는 오타 — 향후 수정 예정)
- `OnClientJoin` → `EnqueueFrameTask(clientJoin)` → FrameThread에서 `make_unique<Player>` 삽입
- `OnClientLeave` → `EnqueueFrameTask(clientLeave)` → FrameThread에서 `erase`
- `_players` 접근은 FrameThread 단독 → lock 불필요.

## 현재 미완성 / 미구현 항목

### 최근 완료
- `Session::_playerId` → `std::atomic<SessionID>` (acquire/release) — `Clear()`와 `FindSession()` 사이의 data race 제거
- `SendPost()` — `Disconnect()`를 `_sendLock` 밖에서 호출하도록 리팩터 (`needDisconnect` 플래그) — 데드락 방지
- **Packet Error 해결** — `TryEnqueueSend(ownerSessionID, ...)` 내부에서 `_sendLock` 잡은 채 sessionID/isDisconnected/sock 재검증 추가. `Disconnect()`도 `_sendLock` 안에서 `_sock = INVALID_SOCKET` 선반영. `FindSession` 통과 후 enqueue 사이의 stale enqueue 창 제거. 스트레스 테스트(100 클라이언트, OverSend 200) 전 항목 0 달성.

### 잔존 미해결 버그
없음.

### 알려진 잠재 버그
- **FrameThread send race**: `FrameThread`에서 `SendPacket` 호출 시 content job ref 없음 → `FindSession` 통과 후 `Clear` + 슬롯 재사용 window 존재. `AcquireJobRef` 패턴(`ioCount++ → sessionID 재검증 → 실패 시 ReleaseRef`)으로 해결 예정. 현재는 recv-triggered echo만 있으므로 미적용.

### 미구현
- **메모리 풀** — 완료. 락프리 태그드 포인터 구현, `Packet` 연동 완료.
- **`AcquireJobRef(SessionID)`** / FrameThread 브로드캐스트 안전화 — 서버 주도 send 경로에 적용 필요.
- **패킷 type 헤더** — `PacketProc`는 `Register(uint16_t type, Handler)` / `Dispatch()` 구조 완성. `PKT_ECHO = 0` 등록됨. 추가 패킷 타입은 `Register`로 확장.
- **TPS 모니터링** — `GetSessionCount`, `GetAcceptTPS` 등 `IOCPServer.h`에 주석 처리됨.
- **`poxY` 오타** — `Player` 멤버 `poxY` → `posY` (DB 연동 시 함께 수정 예정).
- **Session 멤버 접근 제어** — `_ioCount`, `_isSending`, `_isDisconnected`, `_sock` 등이 public. 캡슐화 정리 예정.

## 스트레스 테스트

`ServerStressTest.exe` — 루트 폴더에 위치. 에코 서버 검증용 더미 클라이언트.

```
포트: 6000
입력 순서 (메뉴 번호): 127.0.0.1 → 1 → 4 → 3 → 1
  1 = Disconnect Test Yes
  4 = Client 100명
  3 = OverSend 200
  1 = Disconnect Delay 0
```

Claude Code 커스텀 커맨드: `/stress-test [초]` (`.claude/commands/stress-test.md`)
- 서버 미기동 시 자동 기동 후 테스트 실행
- 종료 후 PASS/FAIL + 핵심 에러 카운터 보고

**통과 기준** — 아래 세 항목이 모두 0:
- `Error - Disconnect from Server`
- `Error - Echo Not Recv (500ms)`
- `Error - Packet Error`

## Platform Notes

- Windows only: `<winsock2.h>`, `<WS2tcpip.h>`, `<Windows.h>`
- `<winsock2.h>`는 반드시 `<Windows.h>`보다 먼저 include
- `Ws2_32.lib` 링크 (`#pragma comment(lib, "Ws2_32.lib")`)
- C 스타일 캐스트 금지 — `reinterpret_cast` / `static_cast` 사용
