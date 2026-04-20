## sendBuffer 동시 접근 문제와 스레드 구조 설계 고민
**날짜**: 2026-04-15

### 현상
워커 스레드(IO)와 컨텐츠 스레드가 동시에 같은 세션의 sendBuffer에 Enqueue할 경우 race condition 발생 가능. RingBuffer의 m_iRear, m_iFront가 plain int라 동기화 없음.

### 원인
- 워커 스레드: recv 완료 시 에코 Enqueue, send 완료 시 MoveFront
- 컨텐츠 스레드: SendPacket에서 Enqueue
- Enqueue가 두 군데서 일어나면 lock 없이는 안전하지 않음

### 추론 및 시도 과정
**"IO에서 처리 가능한 건 바로 send하면 낫지 않나?"** 를 고민했으나,
게임 서버 기준으로 워커가 즉시 처리할 만한 것은 핑퐁 정도뿐.
핑퐁도 sendBuffer Enqueue가 필요하므로 결국 lock 문제로 귀결됨.

스레드 구조 후보:
- 방식 A. 컨텐츠 스레드만 Enqueue → sendBuffer lock 불필요, 구조 단순
- 방식 B. 워커 스레드만 Enqueue → 컨텐츠가 워커에게 "send 요청 큐"로 전달 → 구조 복잡

### 해결 방안
**방식 A 채택**: Enqueue 주체를 컨텐츠 스레드 단일로 고정.

```
워커 스레드  → recvBuffer 파싱 → 패킷 큐에 push (sendBuffer 미접촉)
컨텐츠 스레드 → 큐 소비 → 게임 로직 → SendPacket → sendBuffer Enqueue
워커 스레드  → send 완료 → MoveFront (sendBuffer 읽기 포인터만)
```

Enqueue(쓰기)는 컨텐츠 스레드만, MoveFront(앞 포인터 이동)는 워커만 → 서로 다른 포인터 접근이라 lock 불필요.

나중에 DB 작업 큐만 추가하면 자연스럽게 확장 가능.

### 배운 점
- lock을 없애는 가장 좋은 방법은 동시 접근 자체를 설계로 막는 것
- 게임 서버에서 IO 스레드가 즉시 처리할 만한 작업은 생각보다 거의 없음
- 단순해 보이는 에코/핑퐁도 sendBuffer 공유 문제로 이어짐

---

## IO 스레드 에코 → 컨텐츠 스레드 분리 설계
**날짜**: 2026-04-15

### 현상
기존 구조는 워커 스레드(GQCS)에서 recv 완료 즉시 sendBuffer에 Enqueue하는 에코 방식.
실제 게임 서버에서는 게임 로직(상태 갱신, DB 조회 등)이 필요하므로 워커에서 처리 불가.

### 고민 과정

**"IO 스레드에서 빠르게 처리 가능한 건 바로 send하면 낫지 않나?"**
→ 게임 서버 기준으로 워커가 즉시 처리할 만한 것은 핑퐁(heartbeat) 정도뿐.
→ 핑퐁조차 sendBuffer Enqueue가 필요 → 컨텐츠 스레드와 동시 접근 문제로 귀결.
→ 결론: 워커에서 즉시 처리할 의미 있는 작업이 없음.

**"그러면 IO 스레드와 컨텐츠 스레드가 sendBuffer를 동시에 건드리면 어떻게 막지?"**
→ lock을 추가하거나, Enqueue 주체를 하나로 고정하는 방법 검토.
→ lock은 고성능 서버에서 비용 부담. Enqueue 주체를 단일화하는 게 더 나음.

**"컨텐츠 스레드를 싱글로 가면 Player 관련 자료구조에 lock이 필요없어지는 장점도 있다"**
→ 싱글 컨텐츠 스레드 채택. _players unordered_map을 lock 없이 안전하게 사용 가능.

**큐 소비 방식 고민**
→ condition_variable로 패킷이 들어올 때 깨우는 방식 vs 프레임 레이트 기반 루프
→ 프레임 레이트 기반 채택: 게임 서버는 어차피 매 프레임 Update 로직이 필요하므로 자연스러운 구조.

**큐 swap 패턴**
→ lock 구간 안에서 패킷을 하나씩 꺼내면 워커 스레드가 push할 때 대기 시간 증가.
→ `std::swap`으로 큐 전체를 O(1)에 교환 후 lock 즉시 해제 → lock 구간 최소화.

### 해결 방안 — 최종 구조

```
워커 스레드 (여러 개)
  → GQCS recv 완료
  → recvBuffer에서 헤더(2byte) 파싱 + 패킷 완성 여부 확인
  → Packet 생성 후 PacketTask 큐에 push (sendBuffer 미접촉)

컨텐츠 스레드 (1개, 60fps 루프)
  → swap으로 큐 전체 꺼내기 (lock 최소화)
  → OnRecv(sessionID, packet) 호출 → 게임 로직 처리
  → SendPacket → sendBuffer Enqueue → SendPost
  → OnUpdate(deltaMs) 호출

워커 스레드
  → send 완료 → MoveFront만 (Enqueue 없음)
```

sendBuffer Enqueue는 컨텐츠 스레드만, MoveFront는 워커만
→ 서로 다른 포인터를 건드리므로 lock 불필요.

### 배운 점
- 스레드 간 공유 자원 문제는 lock보다 접근 주체를 설계로 분리하는 게 더 깔끔함
- 싱글 컨텐츠 스레드는 단순히 lock을 없애는 것 이상으로, Player 등 게임 상태 관리 전체를 단순하게 만들어줌
- 프레임 루프 기반 컨텐츠 스레드는 게임 서버의 자연스러운 구조이며 나중에 DB 큐 추가도 용이함

---

## OnClientJoin에 잘못된 인자 전달
**날짜**: 2026-04-15

### 현상
클라이언트 접속 시 `OnClientJoin`에 `sessionID` 대신 `slotIndex`(int)가 전달됨.

### 원인
Accept 스레드에서 `sessionID`를 생성해두고 `OnClientJoin(slotIndex)`로 잘못 호출.
`SessionID`는 `uint64_t`이고 `slotIndex`는 `int`라 암묵적 변환으로 컴파일 에러 없이 통과됨.

### 해결 방안
```cpp
// 수정 전
OnClientJoin(slotIndex);

// 수정 후
OnClientJoin(sessionID);
```

### 배운 점
- `uint64_t` ← `int` 암묵적 변환이 되기 때문에 컴파일러가 잡아주지 않음
- SessionID를 인자로 받는 콜백은 항상 실제 sessionID를 넘기는지 확인 필요

---

## Packet.cpp 프로젝트 미등록으로 인한 빌드 에러
**날짜**: 2026-04-15

### 현상
`Packet.cpp`를 VS 외부(Claude Code)에서 생성했더니 `.vcxproj`에 자동 등록이 안 됨.
이후 `PacketTask`, `localQueue` 등 관련 타입이 `선언되지 않은 식별자` 에러로 cascade 발생.

### 원인
VS는 프로젝트 외부에서 생성된 파일을 자동으로 `.vcxproj`에 추가하지 않음.
구현부가 없는 클래스를 참조하는 코드들이 연쇄 에러를 일으킴.

### 해결 방안
`Packet.cpp` 를 제거하고 모든 구현을 `Packet.h` 인라인으로 옮겨 헤더 온리 클래스로 전환.
고정 배열(`char _buffer[MAXPAYLOAD]`) 기반이라 헤더 온리로 두어도 풀 연동에 문제 없음.

### 배운 점
- Claude Code로 `.cpp` 파일 생성 시 VS 프로젝트에 수동 등록 필요
- 또는 처음부터 헤더 온리로 설계하면 이 문제를 피할 수 있음
- cascade 에러는 첫 번째 에러가 항상 표시되지 않으므로, 에러 목록 전체를 확인해야 함
---

## 네트워크 레이어와 게임 루프 결합으로 인한 스레드 책임 충돌
**날짜**: 2026-04-15

### 현상
`IOCPServer` 내부에 content thread가 들어 있어서 네트워크 라이브러리 레이어가 게임 로직 실행까지 함께 담당하고 있었다.
여기에 frame thread를 추가하려고 하자 `OnRecv`, `OnClientJoin`, `OnClientLeave`, `SendPacket`의 실행 스레드가 섞이기 시작했고, 게임 상태와 sendBuffer 접근 책임이 불명확해졌다.

### 원인
기존 구조는 `IOCPServer`가 `_packetQueue`, `_queueMutex`, `_contentThread`, `CreateContentThread()`를 직접 소유하는 형태였다.
이 상태에서 frame thread만 별도로 추가하면 다음 문제가 생긴다.

- 네트워크 레이어가 게임 루프 구현 세부사항을 알아야 한다.
- `SendPacket()`이 어느 스레드에서 호출되는지 보장이 없다.
- `_players` 같은 게임 상태를 content thread와 frame thread가 동시에 건드릴 위험이 있다.
- 종료 시 `Stop()`이 비어 있어서 스레드 분리 후 정리 순서가 더 불안정해진다.

### 추론 및 시도 과정
먼저 현재 상태를 체크포인트 커밋(`5e992c8`)으로 고정했다.
그 다음 구조를 아래 순서로 재정리했다.

1. `IOCPServer`에서 content queue/thread를 제거하고, worker thread는 패킷을 직접 처리하지 않고 `OnRecv(sessionID, packet)` 이벤트만 올리도록 변경했다.
2. `Session::Clear()`에 `OnSessionReleased` 콜백을 추가해서 세션 반환 직전에 `OnClientLeave(sessionID)`를 안전하게 올릴 수 있게 했다.
3. `GameServer`가 `contentThread`와 `frameThread`를 직접 소유하도록 옮겼다.
4. content thread는 패킷 파싱만 하고, 실제 게임 상태 변경과 `SendPacket()` 호출은 frame thread에서만 처리하도록 분리했다.
5. `IOCPServer::Stop()`과 `GameServer::Stop()`을 구현해서 accept/worker/content/frame thread가 종료 순서대로 정리되게 만들었다.
6. 이후 우선순위 큐 기반 큐잉을 붙일 수 있도록, 네트워크 이벤트를 바로 처리하지 않고 `packet queue`와 `frame task queue`를 거치는 구조로 바꿨다.

### 해결 방안
레이어 책임을 다음처럼 분리했다.

```text
IOCP worker -> GameServer packet queue -> contentThread(패킷 해석)
            -> frame task queue        -> frameThread(상태 갱신, 송신)
```

- `IOCPServer`는 네트워크 I/O와 세션 생명주기만 담당
- `GameServer`는 게임 이벤트 큐, content thread, frame thread 담당
- `SendPacket()`은 frame thread에서만 호출되도록 정리
- `OnClientJoin` / `OnClientLeave`도 frame task로 넘겨 게임 상태 변경 스레드를 단일화
- 현재는 일반 `queue`를 사용하지만, 추후 `frame task queue`를 우선순위 큐로 교체해도 네트워크 레이어를 건드리지 않고 게임 서버 레이어에서 정책만 바꿀 수 있게 분리
- 예를 들어 이동/전투/시스템 메시지처럼 처리 우선순위가 다른 작업을 도입해도, frame thread 앞단 큐 정책만 확장하면 되도록 구조를 잡음

### 배운 점
- 네트워크 라이브러리 레이어가 게임 루프까지 소유하면 이후 확장에서 책임 경계가 빠르게 무너진다.
- 스레드를 늘리는 것보다 먼저 “어떤 스레드가 어떤 상태를 소유하는가”를 고정해야 한다.
- `SendPacket()` 같은 API는 함수 자체보다도 “어느 스레드에서 불러야 안전한가”가 더 중요하다.
- 종료 경로(`Stop`)가 비어 있는 상태에서 스레드 구조를 확장하면 정상 동작보다 종료 레이스부터 먼저 터진다.
- 큐를 한 단계 분리해 두면 나중에 우선순위 큐, 지연 큐, 배치 처리 같은 정책을 붙일 때 네트워크 코드와 게임 코드를 함께 뜯지 않아도 된다.
