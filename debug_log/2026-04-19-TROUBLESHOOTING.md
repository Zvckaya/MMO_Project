## LAN 환경 패킷 지터 (Nagle 알고리즘)
**날짜**: 2026-04-19

### 현상
로컬(루프백)에서는 부드럽게 동작하나, 같은 LAN(공유기) 환경에서 다른 클라이언트의 움직임이 심하게 끊김.

### 원인
Nagle 알고리즘 ON 상태. 작은 패킷이 ACK 대기 중 버퍼링되어 전송 지연 발생. 로컬에서는 루프백 레이턴시가 거의 0이라 증상이 없었음.

### 해결 방안
`main.cpp`의 `nagleOption` 파라미터를 `true`로 변경 → 소켓 옵션 `TCP_NODELAY` 적용.

### 배운 점
실시간 게임 서버에서는 Nagle을 반드시 끄고 시작해야 한다. 로컬 테스트만으로는 이 문제를 재현할 수 없다.

---

## SYNC 패킷 과다 발생
**날짜**: 2026-04-19

### 현상
100명 더미 클라이언트 연결 시 SYNC 패킷이 단일 스레드 Select 서버 대비 훨씬 많이 발생.

### 원인
`MOVE_STOP` 처리가 `FrameTask` 큐를 거쳐 FrameThread에서 실행되는 구조였음. FrameThread가 큐를 처리하기 전에 `Update()`가 먼저 실행되면 서버 포지션이 클라이언트의 정지 위치를 넘어서 진행됨 → 클라가 보낸 stop 좌표와 서버 좌표 불일치 → SYNC 발생.

### 해결 방안
`MOVE_START` / `MOVE_STOP` 처리를 FrameTask 큐에서 ImmediateThread 직접 처리로 이관. `shared_lock(_playersMutex)` + Player 필드 `std::atomic` 변경으로 ImmediateThread와 FrameThread의 안전한 동시 접근 보장. FrameThread는 `Update()`(이동 루프) + 섹터 변경 + 입장/퇴장만 담당.

### 배운 점
지연 처리(큐 경유)와 실시간 상태 변경은 분리해야 한다. 상태 변경은 최대한 즉시(ImmediateThread) 처리하고, 월드 시뮬레이션(Update)만 프레임 단위로 돌리는 것이 SYNC를 줄이는 핵심.

---

## 그리드/섹터 시스템 설계 및 구현
**날짜**: 2026-04-19

### 현상
전체 브로드캐스트로 인해 플레이어 수 증가 시 SendPacket 비용이 O(N) 급증.

### 원인
`BroadcastAround`가 `_players` 전체를 순회하는 구조.

### 해결 방안
6400×6400 맵을 200px 단위 32×32 그리드로 분할. 플레이어는 자신의 섹터 좌표(`sectorX`, `sectorY`)를 보유. 브로드캐스트는 주변 9섹터 내 플레이어에게만 전송. 섹터 경계 진입 시 새로 보이는 섹터 플레이어에게 CREATE + MOVE_START, 벗어난 섹터 플레이어에게 DELETE 전송. `_gridMutex`(shared_mutex)로 ImmediateThread(read) / FrameThread(write) 분리.

### 배운 점
공간 분할은 MMO 서버의 기본 최적화. 섹터 크기가 너무 작으면 경계 진입/퇴장 패킷이 과다해지므로 이동 속도 × 프레임 레이트를 고려해 결정해야 한다.

---

## IOCPServer::Disconnect(SessionID) ioCount 불균형 버그
**날짜**: 2026-04-19

### 현상
콘텐츠 레이어(FrameThread)에서 `Disconnect(sessionID)`를 호출하면, 이후 GQCS에서 이전 소켓의 에러 완료 통지가 새 클라이언트 세션에 적중할 수 있음.

### 원인
`Session::Disconnect()`는 마지막에 "이 I/O의 ref"를 반납하는 `ReleaseRef()`를 무조건 호출함. 이는 GQCS 워커가 I/O ref를 들고 호출한다는 전제. 그러나 `IOCPServer::Disconnect(SessionID)`는 ioCount를 증가시키지 않고 바로 `session->Disconnect()`를 호출하므로, 콘텐츠 레이어에서 호출 시 ioCount가 예상보다 1 더 감소.

결과: `closesocket` 직후 ioCount가 0이 되어 `Clear()` 호출 → 슬롯 반환 → 새 클라이언트가 같은 슬롯 재사용 → 뒤늦게 GQCS에 등록된 이전 소켓의 에러 완료 통지가 새 클라이언트 세션에 도달 → 새 클라이언트 강제 종료.

IOCP completion key는 소켓이 아닌 등록 시점의 Session 포인터이므로, 슬롯(Session 객체 주소)이 재사용되면 구분 불가.

### 해결 방안
`IOCPServer::Disconnect(SessionID)`에서 `session->AddContentRef()`를 먼저 호출해 I/O ref를 미리 확보.

```cpp
bool IOCPServer::Disconnect(SessionID sessionID)
{
    Session* session = FindSession(sessionID);
    if (session == nullptr) return false;
    session->AddContentRef();
    session->Disconnect();
    return true;
}
```

### 배운 점
ref count 기반 세션 생명주기에서 Disconnect는 항상 "호출자가 ref를 하나 들고 있다"는 전제로 설계됨. 콘텐츠 레이어처럼 ref 없이 외부에서 호출하는 경우 반드시 사전에 ref를 확보해야 한다. IOCP completion key가 포인터 기반이라 슬롯 재사용 시 이전 세션 완료 통지가 오염될 수 있다는 점도 설계 시 항상 고려해야 한다.

---

## 하트비트 도입 후 5000명 접속 시 hp=100 disconnect 현상
**날짜**: 2026-04-19

### 현상
하트비트(lastRecvTime 기반 타임아웃) 도입 전에는 5000명 connect/disconnect 스트레스 테스트에서 문제 없었으나, 도입 후 hp=100(정상 상태)인 플레이어가 disconnect 되는 현상 발생.

### 원인
`lastRecvTime`을 `Player` 멤버로 두고, `Dispatch()` 진입 시 `shared_lock(_playersMutex)`를 잡아 업데이트하는 구조였음.

5000명 급속 connect/disconnect 시 FrameThread가 join/leave FrameTask에서 `unique_lock(_playersMutex)`를 대량으로 반복 획득. write lock이 잡혀 있는 동안 ImmediateThread의 `shared_lock` 획득이 모두 블로킹됨.

```
FrameThread:     unique_lock(_playersMutex) × 5000 (join/leave FrameTask)
ImmediateThread: shared_lock(_playersMutex) 대기 → Dispatch 지연
                 → lastRecvTime 업데이트 안 됨
                 → 30초 타임아웃 오발동 → hp=100 disconnect
```

### 해결 방안
`lastRecvTime`을 `_players`(unordered_map)와 완전히 분리. SessionID 하위 16비트가 슬롯 인덱스이므로, `GameServer`에 `std::atomic<uint64_t> _lastRecvTimes[MAXSESSIONSIZE]` 고정 배열을 두고 락 없이 직접 인덱싱.

```cpp
// Dispatch (ImmediateThread) — 락 없음
uint16_t slotIndex = static_cast<uint16_t>(sessionID & 0xFFFF);
_lastRecvTimes[slotIndex].store(::GetTickCount64(), std::memory_order_relaxed);

// Update (FrameThread) — 락 없음
uint16_t slotIndex = static_cast<uint16_t>(id & 0xFFFF);
if (now - _lastRecvTimes[slotIndex].load(...) > dfNETWORK_PACKET_RECV_TIMEOUT)
    Disconnect(id);
```

메모리: `atomic<uint64_t>` × 20000 = 160KB. 슬롯 재사용 시 OnClientJoin FrameTask에서 초기화.

### 배운 점
shared_mutex라도 write lock이 빈번하면 reader 전체를 블로킹한다. 핫패스(패킷 수신마다 호출)에 락을 두면 고부하 시 증폭된다. SessionID 구조(슬롯 인덱스 내장)를 활용하면 별도 자료구조 없이 O(1) 락프리 접근이 가능하다.

---

## 하트비트 timeout false positive와 unsigned underflow
**날짜**: 2026-04-19

### 현상
disconnect 원인 로그를 추가한 뒤 `reason=GameRecvTimeout`인데 `data0=18446744073709551600`, `data1=71263703`, `data2=71263687` 같은 값이 찍히며 정상 플레이어가 끊김.

### 원인
`FrameThread`의 `Update()`는 프레임 시작 시점의 `now = GetTickCount64()`를 한 번만 읽고 전체 `_players`를 순회했다. 그 사이 `ImmediateThread`의 `Dispatch()`가 `_lastRecvTimes[slotIndex]`를 더 최신 시각으로 갱신할 수 있었고, 그 결과 `lastRecvTick > now`가 발생.

이 상태에서 `uint64_t recvDelta = now - lastRecvTick;`를 계산하면 음수 대신 unsigned underflow가 발생해 매우 큰 값으로 감긴다. 이 값이 timeout 임계치보다 크다고 판단되어 false timeout disconnect가 발생했다.

### 추론 및 시도 과정
disconnect 로그의 `data1(lastRecvTick)`가 `data2(now)`보다 16ms 큰 것을 확인. 실제 timeout이 아니라 시간 역전 상황이라는 점을 확인했고, `GameServer_Logic.cpp`의 timeout 계산이 unsigned subtraction이라는 점으로 원인을 좁혔다.

### 해결 방안
timeout 체크를 다음과 같이 변경해 `lastRecvTick <= now`일 때만 차이를 계산하도록 수정.

```cpp
uint64_t lastRecvTick = _lastRecvTimes[slotIndex].load(std::memory_order_relaxed);
if (lastRecvTick <= now)
{
    uint64_t recvDelta = now - lastRecvTick;
    if (recvDelta > dfNETWORK_PACKET_RECV_TIMEOUT)
    {
        DisconnectWithReason(id, DisconnectReason::GameRecvTimeout, recvDelta, lastRecvTick, now);
        continue;
    }
}
```

추가로 `dfNETWORK_PACKET_RECV_TIMEOUT`을 다시 30초(`30000ms`)로 설정.

### 배운 점
멀티스레드에서 "현재 시각"과 "최근 갱신 시각"을 다른 스레드가 각각 갱신하면 시간 역전이 충분히 발생할 수 있다. timeout 비교는 unsigned subtraction 하나로 끝내면 안 되고, 항상 역전 가능성을 먼저 방어해야 한다.
