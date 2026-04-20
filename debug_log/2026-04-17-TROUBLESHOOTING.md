## [재접속 폭주에서 세션 재사용과 send 경합으로 인한 응답 유실 및 잘못된 Echo 데이터]
**날짜**: 2026-04-17

### 현상
- 평상시에는 Echo 서버가 안정적으로 동작했지만, `disconnect on`, `disconnect delay 0`, `loop delay 0`, `oversend 200` 같은 극단 부하에서 문제가 발생했다.
- 초기에는 서버가 한동안 처리하다가 나중에는 Echo 응답이 아예 오지 않는 현상이 나타났다.
- 이후 일부 수정을 적용하자 응답 유실은 크게 줄었지만, 이번에는 클라이언트가 보낸 값과 다른 Echo 응답을 받는 현상이 새로 드러났다.
- Debug 빌드에서 테스트하거나 전송 패턴을 단순화하면 잘못된 Echo 빈도가 크게 줄어드는 경향이 있었다.

### 원인
- 첫 번째 본질 문제는 **세션 lifetime과 콘텐츠 handoff lifetime이 분리되어 있었다는 점**이다.
- worker thread가 패킷을 파싱한 뒤 콘텐츠 `JobQueue`로 넘기고 있었지만, 그 잡이 끝날 때까지 세션 슬롯 재사용을 막는 ref가 없었다.
- 그 결과 stale job이 남아 있는 상태에서 `Clear()`와 slot 재사용이 먼저 일어날 수 있었고, 재접속 폭주 시 같은 `Session` 슬롯 객체를 새 논리 세션이 다시 쓰는 race가 발생할 수 있었다.
- 두 번째 문제는 **`SendPacket` 경로와 `Session::Clear()`가 `sendBuffer`를 동일한 락 규약으로 보호하지 않았다는 점**이다.
- `TryEnqueueSend()`, `SendPost()`, `CompleteSend()`는 `_sendLock`을 사용했지만, `Clear()`는 `_sendLock` 없이 `_sendBuffer.ClearBuffer()`와 `_isSending = false`를 수행하고 있었다.
- 이로 인해 release/clear 타이밍이 빡빡한 상황에서 send 경로와 clear 경로가 경합하며 응답 유실 또는 잘못된 데이터 전송으로 이어졌을 가능성이 높았다.

### 추론 및 시도 과정
- 처음에는 `OnClientJoin()`보다 `RecvPost()`가 먼저 호출되어 첫 패킷이 `JobQueue` 생성 전에 도착하는 race를 의심했다.
- accept 경로의 순서를 `OnClientJoin(sessionID)` 후 `RecvPost()`로 바꿨지만 증상은 그대로였다.
- 이후 문제를 `generation 검사만으로는 막히지 않는 세션 reuse race`로 재정리했다.
- `IOCP_ChatServer`를 참고해 보니, 이 프로젝트는 단순 generation 비교가 아니라 다음 세 가지를 함께 사용하고 있었다.
  - `sessionID` 전체 일치 검사
  - `ioCount` 기반 lifetime 관리
  - release 시작 여부를 나타내는 flag
- 이 비교를 통해 현재 구조에서 가장 부족한 부분이 **콘텐츠 handoff를 세션 lifetime에 포함하지 않은 점**이라는 결론에 도달했다.
- 그래서 worker가 완성 패킷을 콘텐츠로 넘기기 직전에 세션 ref를 하나 더 잡고, 즉답 job이 끝날 때 ref를 반환하도록 바꿨다.
- 이 수정 이후 `500ms 이상 응답 없음` 계열 문제는 사실상 사라졌고, 응답 유실 쪽은 크게 개선되었다.
- 하지만 이후에는 **응답은 오되 값이 틀리는 문제**가 남았다.
- 이 시점에서 `Session::Clear()`와 send 경로가 같은 락을 쓰지 않는 점을 다시 주목했고, `_sendLock`으로 `Clear()`의 send state 초기화를 감싸는 수정이 필요하다고 판단했다.
- 실제로 `Clear()`에 `_sendLock`을 넣고 나서는 Debug 빌드 기준 잘못된 Echo 빈도가 `10초에 1회 수준`에서 `7분에 1회 수준`으로 크게 줄었다.
- 이 결과는 `sendBuffer` 경합이 실제 원인 중 하나였다는 강한 증거가 되었다.

### 해결 방안
- 세션 수명 모델을 `ioCount + generation(SessionID)` 중심으로 정리했다.
- 적용한 수정:
  - worker thread가 완성 패킷을 `GameServer::OnRecv()`로 넘기기 전에 `AddContentRef()`로 세션 ref를 하나 더 잡음
  - `GameServer::OnRecv()`에서 서버 종료 중이거나 `JobQueue`가 없으면 패킷을 버리면서 `ReleaseContentRef(sessionID)` 수행
  - 즉답 job이 끝날 때 `ReleaseContentRef(sessionID)` 수행
  - `IOCPServer::SendPacket()`은 generation 상위 비트만 보지 않고 `SessionID` 전체 일치로 세션을 찾도록 변경
  - 이미 끊긴 세션이면 `SendPacket()`이 바로 실패하도록 `INVALID_SOCKET` / `_isDisconnected` 체크 추가
  - `Session::Clear()`에서 `_sendLock`을 잡은 뒤 `_sendBuffer.ClearBuffer()`와 `_isSending = false`를 수행하도록 변경
  - `listen()` backlog를 `SOMAXCONN`에서 `SOMAXCONN_HINT(65535)`로 상향
- 남은 과제:
  - `IOCP_ChatServer`처럼 public session API 진입 시 짧은 ref를 잡는 구조까지 확장할지 검토
  - recv 파싱 단계에서 `payloadSize` 상한 검증과 `PutData()` 반환값 검증을 추가해 패킷 자체가 이미 틀어지는 경우를 분리해서 확인

### 배운 점
- generation 검사만으로는 충분하지 않다. **stale job이 살아 있는 동안 slot reuse 자체가 일어나지 못하게 만드는 lifetime 규약**이 함께 있어야 한다.
- 세션 reuse 문제는 `ID를 바꿨는지`보다 `old work가 끝나기 전에 clear/reuse가 가능해지는지`가 더 본질적이다.
- 같은 상태를 여러 경로에서 만질 때는 **모든 경로가 같은 락 규약을 따라야 한다**. 일부 경로만 lock을 쓰고 `Clear()` 같은 정리 경로가 lock 없이 초기화하면, Release 빌드/고부하에서 데이터 오염 형태로 터질 수 있다.
- 증상이 `응답 없음`에서 `응답은 오지만 값이 틀림`으로 바뀌었다는 것은, 한 번의 수정으로 문제가 완전히 끝난 것이 아니라 **병목과 race의 층위가 바뀌었다는 신호**일 수 있다.
- `IOCP_ChatServer` 같은 참조 프로젝트를 볼 때는 단순히 API 모양보다, `public API 진입 -> ref 획득 -> sessionID 검증 -> release 조건 확인 -> 작업 수행` 순서 같은 **수명 규약**을 읽어야 한다.

---

## [SendPost 데드락 및 `_playerId` data race 수정]
**날짜**: 2026-04-17

### 현상
- 이전 수정(`Clear()`에 `_sendLock` 추가) 이후 잘못된 Echo 데이터(`Packet Error`) 빈도가 10초/1회 → 7분/1회로 줄었지만, 여전히 산발적으로 발생.
- 특히 `disconnect delay 0`, `loop delay 0`, `oversend 200` 같은 극단 부하에서만 재현.

### 원인
두 가지 독립적인 결함이 공존하고 있었다.

**1. `SendPost()` 데드락 경로**
- `SendPost()`는 `_sendLock`을 잡은 상태에서 WSASend 즉시 에러 시 `Disconnect()`를 호출하고 있었다.
- `Disconnect()` → `ReleaseRef()` → `_ioCount == 1`이면 `Clear()` → `Clear()`가 `_sendLock`을 다시 획득 시도.
- 같은 스레드가 non-recursive `std::mutex`를 두 번 잠그는 것 → UB / 데드락.
- WSASend 즉시 에러(non-`WSA_IO_PENDING`)는 평상시 드물지만, 고부하 + 버퍼 포화 상황에서 발생 가능.

**2. `_playerId` non-atomic data race**
- `_playerId`는 `SessionID`(`uint64_t`) 평범한 멤버 변수였다.
- `Clear()`에서 `_playerId = 0` (write), `FindSession()`에서 `session.GetSessionID()` (read) 가 동시에 실행 가능.
- x64에서 64비트 정수 읽기/쓰기는 대체로 원자적이지만, C++ 표준 기준 data race = UB이며 컴파일러 최적화(레지스터 캐싱, 재순서화)로 오동작 가능.

### 추론 및 시도 과정
- 이전 수정으로 빈도가 크게 줄었지만 완전히 사라지지 않은 이유를 분석.
- `SendPost()` 내부 흐름을 다시 추적하다가 `_sendLock` 보유 중 `Disconnect()` 호출 경로 발견 → 데드락 가능성 확인.
- `_playerId`를 `std::atomic` 미적용 상태로 둔 점을 재검토 → C++ memory model 기준 UB 확인.
- 두 수정이 독립적이므로 동시에 적용.

### 해결 방안

**`SendPost()` — `needDisconnect` 플래그 패턴**
```cpp
void Session::SendPost()
{
    bool needDisconnect = false;
    {
        std::lock_guard<std::mutex> lock(_sendLock);
        // ... CAS, totalSize 확인, WSASend ...
        if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
            needDisconnect = true;
    }
    if (needDisconnect) Disconnect(); // 락 해제 후 호출
}
```

**`_playerId` → `std::atomic<SessionID>`**
```cpp
// Session.h
std::atomic<SessionID> _playerId = 0;

void SetSessionID(SessionID id) { _playerId.store(id, std::memory_order_release); }
SessionID GetSessionID() const  { return _playerId.load(std::memory_order_acquire); }

// Session.cpp Clear()
SessionID releasedID = _playerId.load(std::memory_order_acquire);
_playerId.store(0, std::memory_order_release);
```

### 검증
- `ServerStressTest.exe`, 100 클라이언트, OverSend 200, Disconnect Test ON, 지연 0으로 90초 실행.
- `Error - Packet Error: 0`, `Error - Disconnect from Server: 0`, `Error - Echo Not Recv: 0` 전부 0.
- Connect Total 72,000+, Send/RecvTPS 약 210,000 유지.
- **추가 검증 (300초)**: 동일 조건으로 300초 실행 → `Packet Error: 3`, Connect Total 226,969, Send/RecvTPS ~190,000/195,000.
  - 에러율 약 5.3 × 10⁻⁸. Disconnect/Timeout은 0으로 유지.
  - 90초 테스트에서 0이 나온 것은 샘플이 너무 작아 통계적으로 운이 좋았던 것 (기댓값 0.85개).
  - `_playerId` atomic 수정 전후 에러율 동일 → 이 수정이 Packet Error의 직접 원인은 아님.

### 배운 점
- **락을 잡은 채로 절대 `Disconnect()`/`ReleaseRef()`를 호출하지 않는다.** `Clear()`가 그 락을 재획득하면 같은 스레드에서 non-recursive mutex 이중 잠금 → UB.
- **내부 상태를 여러 스레드가 읽는 멤버는 `std::atomic`이 기본이다.** x64에서 정렬된 64비트 읽기/쓰기가 사실상 원자적이더라도, C++ 표준 기준으로 data race = UB이며 컴파일러는 그 UB를 전제로 최적화한다.
- `acquire`/`release` 메모리 순서: `store(release)` + `load(acquire)` 조합으로 store 이전 변경사항이 load 이후에 가시성을 보장받는다. `_playerId.store(0)` 전에 일어난 sendBuffer 초기화 등이 `GetSessionID()` 이후에 보이도록 순서를 강제한다.

---

## [Packet Error 5.3×10⁻⁸ — 원인 미확정, 조사 기록]
**날짜**: 2026-04-17

### 현상
- Debug 64 클라이언트 10분: Packet Error 5개, 평균 TPS 20만 (에러율 4.2 × 10⁻⁸)
- Release 64 클라이언트 10분: Packet Error 2개, 평균 TPS 50만 (에러율 6.7 × 10⁻⁹)
- Debug < Release 에러율 → 타이밍 민감한 race condition 특성
- `Disconnect from Server`, `Echo Not Recv` 는 0 → 패킷은 도착하지만 내용이 틀림

### 원인 (미확정)
코드 분석으로 결정적 원인을 찾지 못했다. 분석한 경로는 모두 논리적으로 맞았다:

- **sendBuffer 메모리 겹침**: TryEnqueueSend가 쓰는 위치(rear)와 커널이 읽는 구간(front ~ front+totalSize-1)이 실제로 겹치지 않음 → 원인 아님
- **recvBuffer Peek wrap-around**: `Peek()`이 `BUFFERSIZE - m_iFront` 기준으로 두 구간 복사 → 정상
- **DirectDequeueSize 호출 타이밍**: `MoveFront(sizeof(uint16_t))` 이후에 호출됨 → 정상
- **`_playerId` atomic 미적용**: 수정 후 300초 재테스트에서도 Packet Error 3개 동일하게 발생 → 직접 원인 아님

### 추론 및 시도 과정
- sendBuffer 메모리 겹침 가능성 → 수식으로 증명: rear = front+totalSize, 커널 읽기 끝은 rear-1 → 겹침 없음
- Peek/DirectDequeueSize/파싱 루프 코드 전수 검토 → 논리적 오류 없음
- `_playerId` → `std::atomic<SessionID>` 적용 후 300초 테스트 → 동일 에러율
- 에러는 클라이언트에서만 감지 (서버는 받은 내용을 그대로 echo) → 서버 쪽 로그만으로는 재현 불가

### 해결 방안
미해결. 현재 에러율(5 × 10⁻⁸)이 매우 낮고 Disconnect/Timeout은 0이므로 포트폴리오 기준 허용 범위로 판단.

### 배운 점
- 에러율이 10⁻⁸ 수준이면 **300초 테스트에서 기댓값 3개** — 0이 나오는 것도 통계적 변동 범위.
- 클라이언트 쪽 에러를 디버깅하려면 **클라이언트 소스가 있거나 커스텀 테스트 클라이언트**가 필요. 서버 로그만으로는 어떤 패킷이 잘못됐는지 알 수 없다.
- x86 TSO(Total Store Ordering)로 인해 C++ data race가 실제 오동작으로 이어지지 않는 경우가 있어 수정 효과 검증이 어렵다.

---

## [Packet Error 원인 추적: Disconnect와 Send enqueue 경쟁]
**날짜**: 2026-04-17

### 현상
- 스트레스 테스트에서 `Error - Packet Error`가 매우 드물게 발생했다.
- 동시에 `Disconnect from Server`, `Echo Not Recv`는 0이었고, 서버 쪽에서도 recv 파싱 이상은 보이지 않았다.
- 즉 패킷 형식이 깨졌다기보다, 정상 형식이지만 현재 세션 기준으로 오면 안 되는 echo가 도착했을 가능성이 더 커 보였다.

### 원인
- `IOCPServer::SendPacket()`은 바깥에서 `sessionID`, `_isDisconnected`, `_sock` 상태를 확인한 뒤 `Session::TryEnqueueSend()`로 들어갔다.
- 그런데 그 확인과 실제 enqueue 사이에는 공통 락이 없었다.
- 이 틈에서 다른 스레드가 같은 세션에 대해 `Disconnect()`에 먼저 진입할 수 있었고, 기존 `TryEnqueueSend()`는 `_sendLock` 안에서 버퍼 공간만 확인하고 enqueue를 허용했다.
- 그 결과 이미 끊기기 시작한 old session의 정상 echo가 send queue에 들어갈 수 있었고, 클라이언트는 이를 "내가 기대한 값과 다른 echo"로 받아 `Packet Error`를 올렸다.

### 추론 및 시도 과정
- 처음에는 recv 파싱 오류를 의심해서 payload header가 8이 아닌 경우를 로그로 찍었다.
  - 하지만 해당 경고는 발생하지 않았다.
- 다음으로 `Packet -> sendBuffer`, `sendBuffer -> WSASend 직전 snapshot`, `CompleteSend()` 소비 흐름까지 검증 로그를 추가했다.
  - 이 과정에서도 바이트 손상은 잡히지 않았다.
- 그래서 문제를 메모리 손상이 아니라, 논리적으로 이미 죽은 세션의 정상 패킷이 늦게 큐에 들어가는 경우로 재정의했다.
- 최종적으로 `FindSession()`이 성공한 뒤 `TryEnqueueSend()`에 들어가기 전까지의 경쟁 구간을 문제 지점으로 좁혔다.

### 해결 방안
- `Session::TryEnqueueSend()`가 `ownerSessionID`를 인자로 받도록 바꾸고, `_sendLock` 안에서 다시 아래 조건을 검증하도록 수정했다.
  - `GetSessionID() == ownerSessionID`
  - `_isDisconnected == false`
  - `_sock != INVALID_SOCKET`
- `Session::Disconnect()`도 같은 `_sendLock` 안에서 `_sock = INVALID_SOCKET`를 먼저 반영한 뒤 락 밖에서 `closesocket()` 하도록 바꿨다.
- 이렇게 해서 `SendPacket()`의 바깥 검사와 실제 enqueue 사이에 있던 stale enqueue 창을 막았다.

### 배운 점
- `sessionID` 검사는 슬롯이 다른 새 세션으로 재사용됐는가를 막아주지만, 같은 세션이 지금 막 disconnect 중인가까지는 보장하지 않는다.
- 상태 검사는 실제 상태를 소비하는 락 안에서 다시 확인해야 의미가 있다.
- 드문 `Packet Error`는 항상 바이트 손상을 뜻하지 않는다. 정상 형식이지만 현재 세션 소유가 아닌 응답도 같은 증상으로 보일 수 있다.
