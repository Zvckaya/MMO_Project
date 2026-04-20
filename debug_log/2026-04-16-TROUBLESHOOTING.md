## [Echo 부하 테스트 중 WSASend 10014와 비정상 Disconnect 발생]
**날짜**: 2026-04-16

### 현상
- 더미 클라이언트가 연결을 끊지 않은 상태에서, 클라이언트가 overlapped I/O 여러 개로 패킷을 몰아서 보내면 서버가 먼저 `Disconnect()`를 수행했다.
- 서버 로그에는 아래와 같은 패턴이 반복됐다.

```text
[SendPost] sessionID=... err=10014 ... len=4294947xxx isDisconnected=0
[GQCS] sessionID=... ret=0 bytes=0 err=1236
```

- 이후 `err=64`, `err=10054` 같은 후속 에러도 같이 관찰됐다.

### 원인
- 최종 원인은 `sendBuffer`에 대한 동시 접근 경쟁 조건이었다.
- 즉답 스레드는 `SendPacket()`에서 `sendBuffer.Enqueue()`를 수행하고, 워커 스레드는 send 완료 처리에서 `sendBuffer.MoveFront()`와 `SendPost()` 재호출을 수행하고 있었다.
- 이 상태에서 `SendPost()`가 `GetUseSize()`, `DirectDequeueSize()`, `GetFrontBufferPtr()`를 각각 따로 읽으면서 `WSABUF`를 구성했기 때문에, `front/rear` 값이 읽는 도중 다른 스레드에 의해 바뀌면 일관성 없는 snapshot이 만들어질 수 있었다.
- 그 결과 `WSABUF.len`이 음수에 해당하는 값으로 깨졌고, 이것이 `ULONG`으로 해석되면서 `4294947xxx` 같은 비정상 길이가 로그에 찍혔다.
- `WSASend()`는 잘못된 버퍼 주소/길이로 인해 `10014(WSAEFAULT)`를 반환했고, 서버는 즉시 `Disconnect()`를 수행했다.

### 추론 및 시도 과정
- 처음에는 "즉답 스레드로 작업을 넘긴 뒤 세션 수명을 잡지 않아서, 세션이 먼저 `Clear()`되고 슬롯이 재사용되는 문제"를 의심했다.
- 실제로 현재 구조는 예전 `CLanServer`와 달리 콘텐츠/즉답 큐에 세션 포인터를 들고 있지 않았고, 큐에 넘기는 시점에 `ioCount`를 추가로 증가시키지도 않았다.
- 그래서 1차 수정으로:
  - 워커 스레드가 `OnRecv()` 호출 직전에 `ioCount`를 하나 더 증가시키고
  - immediate queue가 `SessionID` 대신 `Session*`를 보관하게 바꾸고
  - 즉답 스레드가 처리 완료 후 `ReleaseRef()` 하도록 수정했다.
- 이 수정은 "큐에 올라간 동안 세션이 사라지거나 슬롯이 재사용되는 문제"를 막는 데는 맞는 방향이었다.
- 하지만 수정 후에도 동일하게 `10014`가 재현됐고, 특히 로그의 `len=4294947xxx`는 세션 수명 문제만으로는 설명이 되지 않았다.
- 이 숫자는 실제로 음수 길이가 `ULONG`으로 출력된 형태였고, 따라서 send 경로에서 `WSABUF`가 잘못 조립되고 있다고 판단했다.
- 이후 `sendBuffer` 접근 지점을 다시 대조한 결과, enqueue 쪽과 send completion 쪽이 서로 다른 스레드에서 동시에 버퍼 메타데이터를 건드리고 있다는 점을 확인했다.

### 해결 방안
- `sendBuffer` 전체를 하나의 mutex로 보호하는 방향으로 수정했다.
- 적용 내용:
  - `Session`에 `_sendLock` 추가
  - `SendPost()`에서 `sendBuffer` 상태를 읽고 `WSABUF` snapshot을 만드는 구간을 lock으로 감쌈
  - `TryEnqueueSend()`를 추가해 enqueue도 같은 lock 아래에서 처리
  - `CompleteSend()`를 추가해 send 완료 후 `MoveFront()`와 `_isSending = false`도 같은 lock 아래에서 처리
  - `IOCPServer`에서는 `_sendBuffer`를 직접 만지지 않고 위 헬퍼만 사용하도록 변경
- 추가로 `TryEnqueueSend()`에서 free size를 먼저 확인하여 partial enqueue가 일어나지 않게 막았다.

### 배운 점
- `std::atomic<int>`로 `front`, `rear`를 각각 관리한다고 해서 링버퍼 전체 상태가 thread-safe해지는 것은 아니다.
- `front/rear`를 여러 번 따로 읽는 구조에서는, 각 load가 atomic이어도 "같은 시점의 일관된 snapshot"은 보장되지 않는다.
- 세션 수명 관리(`ioCount`) 문제와 버퍼 동시 접근 문제는 서로 다른 계층의 버그다.
- 이번 케이스에서는 먼저 세션 ref 누락을 의심한 것이 자연스러웠지만, `WSASend 10014 + 비정상 len` 로그가 "송신 버퍼 snapshot race"를 더 직접적으로 가리키는 증거였다.
- 부하 상황에서 재현되는 네트워크 버그는 "세션 생명주기", "버퍼 ownership", "I/O 완료 후 재진입"을 분리해서 봐야 원인을 좁힐 수 있다.

## [CompletionPortTCPServer와 비교해 본 sendBuffer 경쟁 조건 정리]
**날짜**: 2026-04-16

### 현상
- 상위 폴더의 `CompletionPortTCPServer`는 콘텐츠 스레드에서 큐잉 후 send 하는 구조인데도 별도 send lock 없이 동작해 왔다.
- 반면 현재 `IOCPNetworkServer`는 동일하게 "recv -> 콘텐츠/즉답 스레드 큐잉 -> send" 구조로 보이는데, send lock 없이 실행하면 `WSASend 10014`와 비정상 `len=4294947xxx`가 재현됐다.
- 그래서 "왜 비슷한 구조인데 한쪽은 안 터지고 다른 쪽은 터지는가"를 코드 기준으로 비교했다.

### 원인
- 두 프로젝트의 본질적인 구조는 완전히 다르지 않았다. 둘 다 결국:
  - worker가 recv 완료 처리와 send completion 처리를 담당하고
  - 콘텐츠/즉답 스레드가 send 요청을 발생시키는 구조였다.
- 차이는 `RingBuffer` 구현 디테일에 있었다.
- `CompletionPortTCPServer`의 `CRingBuffer`는 `front/rear`가 plain `int`였고, 현재 `IOCPNetworkServer`의 `RingBuffer`는 `std::atomic<int>`였다.
- `std::atomic<int>`로 바꾼 현재 구현은 `GetUseSize()`, `DirectDequeueSize()`, `GetFrontBufferPtr()` 같은 함수 안에서 `front/rear`를 여러 번 따로 읽을 때, 각 load가 서로 다른 시점 값을 볼 수 있었다.
- 이 때문에 조건식에서는 정상처럼 보이지만 계산식에서는 음수 길이가 나오는 식의 snapshot race가 더 직접적으로 표면화됐다.
- 반면 `CompletionPortTCPServer`는 이론적으로는 동일한 경쟁 조건을 안고 있어도, plain `int` 기반 구현에서 컴파일러가 값을 레지스터에 들고 계산하면서 같은 문제가 덜 드러났을 가능성이 높다.
- 즉 `CompletionPortTCPServer`가 구조적으로 안전해서 안 터진 것이 아니라, 같은 취약점이 있어도 덜 드러난 쪽에 가깝다고 판단했다.

### 추론 및 시도 과정
- 먼저 `CompletionPortTCPServer`의 `CLanServer::SendPost(SessionID, CPacket*)`, `CLanServer::SendPost(Session*)`, worker send completion 처리, `CRingBuffer` 구현을 대조했다.
- 확인 결과:
  - `CompletionPortTCPServer`도 콘텐츠 스레드에서 `sendBuffer.Enqueue()` 후 `SendPost(session)`를 호출한다.
  - worker는 send completion에서 `sendBuffer.MoveFront()` 후, 데이터가 남아 있으면 다시 `SendPost(session)`를 호출한다.
  - 즉 sendBuffer ownership이 엄밀히는 단일 스레드가 아니며, 현재 서버와 마찬가지로 콘텐츠/worker가 함께 관여한다.
- 따라서 단순히 "컨텐츠 스레드로 뺐기 때문에 안전하다"는 설명은 성립하지 않았다.
- 이후 `CRingBuffer`와 현재 `RingBuffer`를 비교했고, 현재 구현에서 atomic 기반 `front/rear`를 여러 번 읽는 패턴이 snapshot race를 더 쉽게 유발한다는 결론에 도달했다.
- 실제로 현재 서버 로그의 `len=4294947xxx`는 `m_iRear - m_iFront` 계산이 음수가 된 경우와 정확히 맞아떨어졌다.

### 해결 방안
- 단기 해결:
  - `sendBuffer` 접근을 mutex로 감싸서 enqueue, send snapshot 생성, send completion 후 `MoveFront`를 같은 임계 구역으로 묶었다.
  - 이 방식으로 `WSABUF.len`이 깨지는 문제는 실제로 재현이 멈췄다.
- 장기 방향:
  - lock 없이 가려면 `sendBuffer`를 여러 스레드가 같이 만지지 않게 ownership을 한쪽으로 몰아야 한다.
  - 권장 방향은 "worker가 send 상태를 전부 소유"하는 구조다.
  - 즉답/콘텐츠 스레드는 `sendBuffer`를 직접 만지지 않고, 송신 요청 큐만 생성한다.
  - worker만:
    - 송신 요청 큐에서 패킷을 꺼내 `sendBuffer`에 적재
    - `SendPost()`
    - send completion 후 `MoveFront()`
    - 필요 시 다음 전송 이어가기
- 핵심은 atomic 사용 여부가 아니라, `sendBuffer`, `sendFlag`, `sendOverlapped`, `WSABUF snapshot`의 ownership을 단일 스레드로 고정하는 것이다.

### 배운 점
- "락이 없는데 안 터진다"는 사실만으로 구조가 안전하다고 결론 내리면 안 된다.
- 같은 경쟁 조건도 자료형(`int` vs `std::atomic<int>`), 컴파일러 코드 생성, 스케줄 타이밍에 따라 드러나는 방식이 달라질 수 있다.
- lock-free를 목표로 할 때 핵심은 atomic 자체가 아니라 ownership 설계다.
- `sendBuffer`를 lock 없이 유지하려면 "누가 rear를 쓰는가", "누가 front를 쓰는가", "누가 SendPost를 호출하는가"를 먼저 한 스레드 책임으로 고정해야 한다.

## [sendBuffer를 worker 단독 소유 구조로 바꾸는 방향 확정]
**날짜**: 2026-04-16

### 현상
- `sendBuffer`에 lock을 걸자 `WSASend 10014`와 비정상 `WSABUF.len` 문제는 멈췄다.
- 하지만 이 방식은 임시 안정화에 가깝고, 장기적으로는 구조 자체를 바꾸는 쪽이 더 낫다고 판단했다.
- 특히 앞으로 콘텐츠 쪽 스레드가 여러 개가 될 가능성을 고려하면, 현재처럼 콘텐츠 스레드가 세션의 `sendBuffer`를 직접 건드리는 구조는 계속 부담이 된다.

### 원인
- 현재 문제의 본질은 `sendBuffer`를 여러 스레드가 공유하는 데 있었다.
- 즉답/콘텐츠 스레드가 enqueue와 `SendPost()`를 담당하고, worker가 send completion 후 `MoveFront()`와 다음 `SendPost()`를 담당하면, 결국 한 세션의 송신 상태를 여러 스레드가 함께 관리하게 된다.
- 이 공유 구조가 있으면 lock으로 막을 수는 있어도, 멀티 콘텐츠 스레드 환경으로 갈수록 동기화 비용과 추론 난이도가 커진다.

### 추론 및 시도 과정
- 먼저 세션 lifetime 문제를 의심해 `ioCount`를 immediate queue 경로에 추가했고, 이것은 별도 문제를 막는 데 유효했다.
- 그 다음 실제 `10014`의 직접 원인이 `sendBuffer snapshot race`라는 점을 확인하고 `_sendLock`으로 안정화했다.
- 이후 `CompletionPortTCPServer`와 비교한 결과, 그쪽도 본질적으로 sendBuffer를 콘텐츠/worker가 함께 건드리는 구조이며, "락이 없는데 안전한 설계"라기보다 "동일한 위험이 덜 드러난 구현"에 가깝다는 결론에 도달했다.
- 그래서 lock을 유지한 채로 끝내기보다, send ownership 자체를 worker 쪽으로 몰아 구조를 바꾸는 것이 장기적으로 맞다고 정리했다.

### 해결 방안
- 장기 해결 방향:
  - `sendBuffer`, `sendFlag`, `sendOverlapped`, `WSABUF snapshot`은 worker 스레드만 관리한다.
  - 콘텐츠/즉답 스레드는 세션의 실제 송신 버퍼를 만지지 않고, "송신 요청 큐"에 패킷만 넣는다.
  - worker는 송신 요청 큐에서 패킷을 꺼내 `sendBuffer`에 적재하고 `SendPost()`를 호출한다.
  - send completion 이후 `MoveFront()`, 다음 `SendPost()` 여부 판단도 worker만 수행한다.
- 이 구조가 되면:
  - 멀티 콘텐츠 스레드 환경에서도 `sendBuffer` 공유 문제가 사라진다.
  - 세션별 송신 요청을 큐잉했다가 worker가 순서 있게 처리할 수 있다.
  - lock-free에 더 가까운 ownership 모델을 만들 수 있다.

### 배운 점
- lock은 증상을 빠르게 멈추게 해주는 도구지만, 구조적으로 ownership이 섞여 있으면 언젠가 다시 복잡도가 폭발한다.
- 장기적으로는 "누가 송신 상태를 소유하는가"를 먼저 정하고, 그 다음 atomic/lock 여부를 선택해야 한다.
- 멀티 콘텐츠 스레드로 갈 계획이 있다면, 콘텐츠 스레드는 송신 요청 생성만 하고 실제 네트워크 송신 상태는 worker가 책임지는 구조가 더 자연스럽다.
- 내일 이어서 구현해야 한다: `sendBuffer` worker 단독 소유 구조로 개편.

---

## [게임 서버 컨텐츠 스레드 구조 설계 고민]
**날짜**: 2026-04-16

### 현상
- 기존 구조는 `ImmediateThread` 2개가 공유 큐 하나를 소비하는 고정 구조였다.
- 스레드가 늘어도 소비 능력이 같은 큐에 묶이고, 같은 세션의 패킷이 두 스레드에서 동시에 처리되어 순서 보장이 없는 문제가 있었다.
- 이를 해결하기 위한 구조 개편 방향을 고민했다.

### 추론 및 시도 과정

**1. 레이어 분리 원칙 확립**

네트워크 레이어(`IOCPServer`, `Session`)와 콘텐츠 레이어(`GameServer`, `Player`)가 서로 포인터를 교환하면 안 된다는 원칙을 명시했다.

- 기존 `OnRecv(Session*, Packet*)`에서 콘텐츠 레이어가 `Session*`를 직접 들고 있었다.
- `OnRecv(SessionID, Packet*)`로 변경해 레이어 경계를 `SessionID`로만 넘기도록 수정했다.
- 이에 따라 `ImmediateTask::Session*`도 `SessionID`로 교체하고, 관련 `_ioCount` 증감 로직을 제거했다.
- 두 레이어의 인터페이스:
  - 콘텐츠 → IO: `SendPacket(SessionID, Packet*)`, `Disconnect(SessionID)`
  - IO → 콘텐츠: `OnConnectionRequest`, `OnClientJoin`, `OnClientLeave`, `OnRecv` (모두 SessionID만 전달)

**2. ServerCore 분석**

참고용으로 추가한 `ServerCore` 프로젝트를 분석해 세 가지 차이점을 확인했다.

| 항목 | ServerCore | 현재 구조 |
|---|---|---|
| GQCS 세션 탐색 | `IocpEvent.m_owner` (shared_ptr) | completion key = `Session*` |
| 세션 생명주기 | shared_ptr 자동 관리 | `_ioCount` 수동 관리 |
| 컨텐츠 스레드 | Worker 스레드가 GlobalQueue 겸임 | ImmediateThread 전용 분리 |

**ServerCore의 JobQueue/GlobalQueue 스케줄링 핵심:**
- `JobQueue`는 Actor 단위(플레이어, 방 등)로 생성되며 자신만의 job 큐를 가진다.
- job이 0→1이 되는 순간 `JobQueue` 자신을 `GlobalQueue`에 push한다.
- Worker 스레드가 `GlobalQueue`에서 `JobQueue`를 꺼내 `Execute()`한다.
- `LEndTickCount`(TLS) 초과 시 선점 — 현재 `JobQueue`를 `GlobalQueue` 끝에 재등록하고 다른 큐에 양보한다.
- 이 선점 덕분에 특정 세션에 패킷이 몰려도 다른 세션의 처리가 굶지 않는다.

**같은 세션 패킷의 순서 보장 원리:**
- `Push()`는 `prevCount == 0`일 때만 `GlobalQueue`에 등록한다.
- 선점 후 재등록 시에도 `m_jobCount > 0`이므로 중복 등록이 발생하지 않는다.
- 결과적으로 같은 `JobQueue`는 항상 GlobalQueue에 최대 하나만 존재하고, 동시에 하나의 스레드만 `Execute()`한다. → **세션 내 패킷 순서 보장**

**3. 게임 서버 스레드 역할 구분**

이동 패킷을 예시로 즉답 스레드와 프레임 스레드의 역할을 정리했다.

```
IO Worker    : 패킷 파싱 → OnRecv(sessionID, packet)
즉답 스레드  : 유효성 검사 + desiredDest 설정 + 클라이언트 ACK 전송
              (플레이어 혼자 처리 가능한 것)
프레임 스레드: currentPos 갱신 + 충돌 감지 + 범위 내 브로드캐스트
              (다른 플레이어와의 관계가 필요한 것)
```

- 즉답 스레드: "이 플레이어 혼자 처리 가능한 일"
- 프레임 스레드: "다른 플레이어들과의 관계가 필요한 일" — 모든 플레이어의 `currentPos`를 동시에 보는 유일한 스레드

`Player`의 데이터를 두 종류로 분리:
```cpp
std::atomic<int> desiredDestX, desiredDestY;  // 즉답→프레임 공유, atomic
int currentPosX, currentPosY;                  // 프레임 단독, lock 불필요
```

**4. Player/Room 단위 Actor 모델 방향**

- 플레이어별 `JobQueue`: 개인 연산 (인벤토리, ACK, 채팅)
- 방/맵별 `JobQueue`: 공간 연산 (충돌, 브로드캐스트)
- 프레임 스레드 = "Room JobQueue를 고정 주기로 실행하는 것"으로 재해석 가능
- 방이 여러 개로 늘어도 스레드를 추가하지 않고 즉답스레드가 GlobalQueue에서 동적으로 처리

### 해결 방안 (다음 구현 목표)

현재 `ImmediateThread` 구조를 Player별 JobQueue + GlobalQueue 방식으로 개편한다.

**변경 내용:**
- `Job`: `std::function<void()>` 래퍼 클래스
- `JobQueue`: Player당 1개. job push 시 0→1이면 `GlobalQueue`에 self push. `LEndTickCount` 선점 포함
- `GlobalQueue`: `LockQueue<shared_ptr<JobQueue>>`
- `GameServer`:
  - `_sessionJobQueues: unordered_map<SessionID, shared_ptr<JobQueue>>` + `shared_mutex`
  - `OnClientJoin` → FrameThread에서 Player 생성 + JobQueue 생성
  - `OnRecv` → `_sessionJobQueues[sessionID]->DoAsync(...)` 로 job 등록
  - `ImmediateThread`: CV 대신 GlobalQueue에서 Pop 후 Execute 루프

**일단 echo 기준으로 구현하고, Room JobQueue는 추후 추가 예정.**

### 배운 점
- 공유 큐 + 고정 소비 스레드 구조는 스레드를 늘려도 처리량이 늘지 않고, 세션 내 순서 보장도 안 된다.
- Actor 모델(JobQueue per entity)은 세션 내 순서를 자동으로 보장하면서 스레드 추가 시 처리량이 선형으로 증가한다.
- 선점(LEndTickCount)은 job이 몰리는 세션이 스레드를 독점하는 것을 막는 핵심 장치다.
- "즉답"과 "프레임"의 본질적 기준은 속도가 아니라 "다른 플레이어 상태가 필요한가"다.

---

## [공유 큐 swap 구조 → GlobalQueue + per-player JobQueue 구조 전환 후 TPS 비교]
**날짜**: 2026-04-16

### 현상
- 기존: ImmediateThread N개가 단일 공유 큐(`_immediateTaskQueue`)를 mutex로 보호하며 소비.
  - 스레드를 늘려도 TPS 상승이 관측되지 않음.
- 개편 후: per-player `JobQueue` + `GlobalQueue` 구조로 전환.
  - 동일 클라이언트 수, 동일 스레드 수 조건에서 **TPS 3배 이상 상승** 확인.

### 두 구조의 핵심 차이

**구 구조 (공유 큐 swap)**
```
OnRecv → _immediateTaskQueue.push()  (mutex 잠금)
ImmediateThread x N → mutex 경합 → task 1개 pop → 처리 → 반복
```
- 모든 스레드가 하나의 mutex에 집중 → 스레드가 많을수록 대기 시간 비례 증가
- task 단위로 mutex를 잠그므로 TPS가 높아질수록 lock 빈도도 같은 비율로 증가
- 같은 세션의 패킷이 서로 다른 스레드에서 동시에 처리 가능 → 세션 내 순서 보장 없음

**신 구조 (GlobalQueue + per-player JobQueue)**
```
OnRecv → sessionJobQueue.DoAsync()
         └ job 0→1 전환 시에만 GlobalQueue.Push(jobQueue)
ImmediateThread x N → GlobalQueue에서 JobQueue 획득 → Execute(endTick)
                       └ 해당 JobQueue의 job을 모두 소진할 때까지 루프
                         선점(64ms) 초과 시 GlobalQueue 끝에 재등록
```
- GlobalQueue lock은 JobQueue 단위로만 발생 (task 단위 X)
  - 패킷이 1000개 쏟아져도 같은 JobQueue가 GlobalQueue에 등록되는 건 최대 1회
- 스레드가 JobQueue를 획득한 뒤에는 lock 없이 실행 → 스레드 추가 시 처리량이 선형 증가
- 같은 JobQueue는 동시에 하나의 스레드만 `Execute()` → **세션 내 패킷 순서 자동 보장**

### 원인 분석 (TPS 차이의 근거)

| 항목 | 공유 큐 구조 | GlobalQueue 구조 |
|---|---|---|
| mutex 잠금 빈도 | task 1개당 1회 (TPS 직비례) | JobQueue 등록 시만 (훨씬 드묾) |
| 스레드 추가 효과 | 거의 없음 (lock contention 증가) | 선형 증가 (각 스레드가 독립 JobQueue 처리) |
| 세션 내 순서 | 보장 없음 | 자동 보장 |
| 한 세션 폭주 시 | 다른 세션도 함께 처리 지연 | 64ms 선점으로 다른 세션에 양보 |
| 캐시 효율 | task 단위 점프 → 캐시 비효율 | 같은 JobQueue 연속 실행 → 캐시 친화적 |

### 배운 점
- lock contention이 TPS의 천장이 된다. 공유 큐 구조에서는 TPS가 높아질수록 lock 경쟁이 비례해 커지므로, 스레드를 아무리 늘려도 mutex가 병목이 된다.
- Actor 모델은 "lock의 단위를 줄이는 설계"다. task마다 lock을 걸었던 것을 JobQueue 등록 시점으로만 줄이면, 나머지 실행 시간은 완전히 lock-free가 된다.
- `prevCount == 0` 조건으로 GlobalQueue 등록을 제어하는 것이 핵심이다. 이 한 줄이 "중복 등록 방지 + 순서 보장 + GlobalQueue lock 빈도 최소화"를 동시에 달성한다.
- 선점(endTick)은 단순한 공정성 장치가 아니라 tail latency 제어 메커니즘이다. 특정 세션에 패킷이 폭주할 때 다른 세션이 굶는 것을 구조적으로 막는다.

---

## [재접속 부하 테스트 중 PACKET ERROR RecvPacket: 0x00 발생 — ioCount per-job 설계]
**날짜**: 2026-04-16

### 현상
- 연결 유지(단순 echo) 상태에서는 100명, 200명 모두 정상 동작.
- `disconnect delay=0`, `loop delay=0`, 클라이언트 200명 급속 재접속 조건에서 서버 측에 아래 에러 반복:
  ```
  PACKET ERROR RecvPacket: 0x00  [x27, x수백]
  ```
- 이 에러는 클라이언트가 수신한 echo 패킷의 `payloadSize`가 0인 경우를 가리킨다.
- 재접속 없이 100명 스트레스 테스트에서는 발생하지 않음 → 슬롯 재사용 경로에서만 재현.

### 원인
- **TryEnqueueSend(ImmediateThread) vs ClearBuffer(Worker) 간 경쟁 조건.**
- 흐름:
  1. ImmediateThread: `SendPacket(sessionID, packet)` → `TryEnqueueSend` → `sendBuffer.Enqueue` (sendBuffer의 `m_iRear` 갱신)
  2. 동시에 Worker: recv 에러 → `Disconnect()` → `ioCount` 감소 → `Clear()` → `sendBuffer.ClearBuffer()` (`m_iRear = 0`)
  3. Worker: `returnIndex()` → 슬롯 반환
  4. Accept 스레드: 새 클라이언트가 같은 슬롯 획득
  5. ImmediateThread가 쓴 `m_iRear` 값이 새 클라이언트의 `sendBuffer`에 잔류 → sendBuffer가 오염된 상태로 시작
- `generation` 기반 SessionID 검증은 `SendPacket` 진입 시에만 하는데, 검증 통과 후 `TryEnqueueSend`가 실행되기 전에 Clear()와 returnIndex()가 완료되면 새 세션의 버퍼를 건드리게 됨.
- ImmediateThread는 `ioCount`를 들고 있지 않으므로 `ioCount == 0` 이후에도 `TryEnqueueSend`를 실행할 수 있음.

### 추론 및 시도 과정
- 1차 추측: `_playerId`가 non-atomic이라 generation 검증이 늦게 반영된다?
  → `generation` 기반 SessionID는 monotonic counter로 보호되므로 단순 ID 오검증으로는 설명이 안 됨.
- 2차 추측: `Clear()` 안에서 `_sendLock`을 잡고 `ClearBuffer()` 호출하면?
  → `SendPost()`가 `_sendLock`을 잡은 채로 `Disconnect()` → `ReleaseRef()` → `Clear()` → 같은 스레드에서 `_sendLock` 재획득 시도 → **데드락**, exit code 3으로 즉시 종료.
- 3차 시도: `SendPost()` 안에서 Disconnect를 락 밖으로 이동 후 Clear 내부에서 락 잡기
  → 같은 에러 재현, 완전히 해결되지 않음.
- 최종 진단: 근본 원인은 "ImmediateThread가 job 실행 중 ioCount를 들고 있지 않아 세션 Clear()를 막을 방법이 없다"는 것.

### 고민: 상용 서버에서 서버 주도 send 처리

**문제 의식:**
- recv-triggered job: Worker가 OnRecv 전에 ioCount++ → job 실행 중 Clear() 억제 → TryEnqueueSend 안전.
- 서버 주도 send (FrameThread에서 NPC 브로드캐스트 등): recv 페어가 없으니 ioCount를 어디서 빌려오는가?

**결론:**
- recv-triggered든 server-initiated든, `SendPacket`을 호출하기 전에 반드시 ioCount ref를 들고 있어야 한다.
- 이를 위해 `AcquireJobRef(sessionID)` 패턴을 도입한다.
  - **"검증 후 증가"가 아니라 반드시 "증가 후 검증"** — 검증 후 증가 순서이면 검증 통과와 증가 사이에 Clear()가 끼어들 수 있음.
  - ioCount를 먼저 올리면 Clear() 진입 자체가 막히므로, 이후 generation 검증이 안전해짐.
  - 검증 실패 시 즉시 `ReleaseRef()` 반납 → ioCount가 0이면 double-Clear() 진입하지만, Clear()의 콜백들은 이미 `std::move`로 비워졌으므로 null 체크로 안전하게 처리됨.

```cpp
// IOCPServer protected 추가 예정
bool IOCPServer::AcquireJobRef(SessionID sessionID)
{
    uint16_t slotIndex = static_cast<uint16_t>(sessionID & 0xFFFF);
    if (slotIndex >= MAXSESSIONSIZE) return false;

    Session& session = _sessions[slotIndex];
    session._ioCount.fetch_add(1);           // 먼저 올려서 Clear() 억제

    if (session.GetSessionID() != sessionID) // 그 다음 검증
    {
        session.ReleaseRef();                // 불일치 → 즉시 반납
        return false;
    }
    return true;
}
```

- `GetSessionID()`가 읽는 `_playerId`가 non-atomic이면 Clear()의 `_playerId = 0` 쓰기와 data race → **`_playerId`를 `std::atomic<SessionID>`로 변경 필요**.
- FrameThread 브로드캐스트 예시:
  ```cpp
  for (auto& [sessionID, player] : _players) {
      if (AcquireJobRef(sessionID)) {
          SendPacket(sessionID, packet);
          ReleaseJobRef(sessionID);
      }
      // false → 이미 나간 세션, 다음 프레임에 _players에서 제거됨
  }
  ```
- recv-triggered Job lambda 끝에도 동일하게 `ReleaseJobRef(sessionID)` 호출.
- 서버 주도 send에서 Clear된 세션에 send가 실패해도 데이터 손실은 허용 — 어차피 유저는 이미 나갔으므로 문제없음.

### 해결 방안 (구현 예정)
1. `Session.h` — `_playerId` → `std::atomic<SessionID>`
2. `IOCPServer` — `AcquireJobRef(SessionID)` / `ReleaseJobRef(SessionID)` protected 추가
3. Worker — `OnRecv` 호출 전 `session->_ioCount.fetch_add(1)` (recv-triggered job ref)
4. Job lambda 끝 — `ReleaseJobRef(sessionID)` 호출
5. FrameThread 브로드캐스트 — `AcquireJobRef` / `ReleaseJobRef` 감싸기

### 배운 점
- "ioCount가 살아있는 동안에는 Clear()가 안 된다"는 보장을 모든 send 경로에서 지켜야 한다. recv 응답이든 서버 주도 send든 예외가 없다.
- `shared_ptr` 기반(ServerCore 방식)은 이 문제를 ownership 모델로 자동 해결하지만, 수동 ioCount 관리 방식에서는 "누가 ref를 들고 있는가"를 명시적으로 설계해야 한다.
- AcquireJobRef의 "증가 후 검증" 패턴은 lock 없이 TOCTOU를 회피하는 핵심 트릭이다. atomic increment가 메모리 배리어 역할을 하여 이후 sessionID 읽기가 최신 값을 볼 수 있게 된다.
- Clear()의 double 진입은 콜백을 `std::move`로 비우는 패턴이 있어서 멱등적으로 안전하다.
