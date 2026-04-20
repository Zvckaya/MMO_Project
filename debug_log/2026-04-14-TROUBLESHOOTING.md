# TROUBLESHOOTING - 2026-04-14

---

## Echo 서버 데이터 값 변조 — `_isSending` race condition
**날짜**: 2026-04-14

### 현상
단일 세션이 접속을 유지한 채 데이터를 연속으로 전송할 때, 클라이언트가 수신한 echo 데이터 값이 송신한 값과 다른 경우가 발생함.

### 원인
`Session::_isSending`이 일반 `bool` 타입이라 atomic하지 않음. 워커 스레드가 여러 개일 때 다음 시나리오가 발생 가능:

```
[스레드A] recv 완료 → SendPost() → _isSending 체크(false) → _isSending = true → WSASend 등록
[스레드B] send 완료 → MoveFront() → _isSending = false → GetUseSize() > 0 → SendPost()
[스레드A] recv 완료 → SendPost() → _isSending 체크(false, 스레드B가 방금 false로 바꿈) → WSASend 또 등록
```

결과적으로 같은 소켓에 `WSASend`가 동시에 2개 걸리면서, 두 send가 동시에 sendBuffer의 동일한 front 위치에서 데이터를 읽어 중복 전송 또는 데이터 뒤섞임 발생.

### Race Condition 정확한 시나리오
동시 진입이 아닌 **순차적이지만 타이밍이 겹친** 상황이다.

```
[스레드A] recv 완료 → _isSending 체크 → false 확인
                                            ↓ (컨텍스트 스위치)
[스레드B]            send 완료 → MoveFront → _isSending = false → SendPost() → _isSending = true → WSASend 등록
                                            ↓
[스레드A] (재개) → _isSending = true → WSASend 또 등록  ← 이미 스레드B가 등록한 상태
```

스레드A가 `_isSending` **읽기**와 **쓰기** 사이에 스레드B가 끼어든 것이 핵심. 이 시점에 sendBuffer의 front를 두 스레드가 동시에 참조하여:
- 같은 데이터가 두 번 전송되거나
- A가 읽는 도중 B가 `MoveFront`를 해버려 A가 이미 전송된 영역까지 걸쳐 읽는 상황 발생

클라이언트 입장에서는 데이터가 중복되거나 뒤섞여 보임.

`compare_exchange_strong`이 이를 해결하는 이유는 **체크와 변경을 하나의 atomic 명령**으로 묶어 중간에 끼어들 틈을 없애기 때문.

### 추론 및 시도 과정
- 처음에는 RingBuffer wrap-around 시 `GetFrontBufferPtr()` + `cbTransferredBytes`로 연속 메모리를 가정한 `Enqueue` 호출을 의심
- 이를 `DirectDequeueSize()` 기반 분기 처리로 수정했으나 문제 지속
- 워커 스레드 수를 고려했을 때 `_isSending` 플래그에 대한 동시 접근이 근본 원인으로 확인

### 해결 방안
`Session.h`에서 `bool` → `std::atomic<bool>` 교체:
```cpp
std::atomic<bool> _isSending = false;
```

`SendPost()` 내부에서 CAS(Compare-And-Swap) 패턴 적용:
```cpp
bool expected = false;
if (!_isSending.compare_exchange_strong(expected, true))
    return;  // 이미 sending 중인 스레드가 있으면 진입 차단
```

send 완료 처리 순서 유지:
1. `MoveFront(cbTransferredBytes)`
2. `_isSending = false`
3. `GetUseSize() > 0` 이면 `SendPost()` 재호출

### 최종 해결 방안 (확정)
외부에서 `_isSending`을 건드리지 않고, `SendPost()` 내부에서 CAS로 단일 진입을 보장하는 구조로 확정:

```cpp
void Session::SendPost()
{
    bool expected = false;
    if (!_isSending.compare_exchange_strong(expected, true))
        return;  // CAS 실패 = 이미 sending 중인 스레드 존재

    int totalSize = _sendBuffer.GetUseSize();
    if (totalSize == 0)
    {
        _isSending = false;
        return;
    }
    // ... WSABUF 구성 및 WSASend
}
```

send 완료 루틴:
```cpp
session->_sendBuffer.MoveFront(cbTransferredBytes);
session->_isSending = false;
if (session->_sendBuffer.GetUseSize() > 0)
    session->SendPost();  // 내부 CAS가 중복 진입 차단
```

recv 완료 루틴에서도 그냥 `SendPost()` 호출만 하면 되어 외부 코드가 단순해짐.

### 배운 점
- IOCP 워커 스레드가 여러 개일 때, 같은 세션에 대한 완료 이벤트가 서로 다른 스레드에서 동시에 처리될 수 있다. 세션의 상태 플래그는 반드시 atomic으로 보호해야 한다.
- `bool` 타입의 플래그는 단일 스레드 환경에서는 문제없지만, 멀티 스레드 환경에서는 read-modify-write가 atomic하지 않아 race condition이 발생한다.
- CAS(`compare_exchange_strong`)를 호출부가 아닌 함수 내부에 두면, 호출부가 상태 플래그를 직접 건드리지 않아도 되어 인터페이스가 훨씬 깔끔해진다.
- `compare_exchange_strong`은 락 없이 "먼저 진입한 스레드만 통과"를 보장하는 표준적인 패턴이다.

---

## 세션 안전 종료 — `ioCount` 기반 생명주기 관리
**날짜**: 2026-04-14

### 현상
클라이언트 접속 종료 시 세션이 정리되지 않음. `ret == false || cbTransferredBytes == 0` 조건에서 `continue`로 넘어가 슬롯이 반환되지 않고, 시간이 지날수록 사용 가능한 세션 슬롯이 고갈됨.

### 원인
비동기 I/O 특성상 소켓을 닫는 시점에 아직 GQCS 큐에 완료 이벤트가 남아 있을 수 있음. 소켓만 닫고 즉시 슬롯을 반환하면, 나중에 도착하는 완료 이벤트가 이미 다른 세션이 사용 중인 슬롯을 건드리는 문제 발생.

추가로 `closesocket`이 여러 워커 스레드에서 중복 호출될 수 있는 문제도 존재.

### 추론 및 시도 과정

**① `closesocket` 중복 호출 방지**

`std::atomic<bool> _isDisconnected` + CAS 패턴으로 해결. CAS에 성공한 스레드만 `closesocket`을 호출하며, 나머지 스레드는 소켓을 건드리지 않음.

```cpp
bool expected = false;
if (_isDisconnected.compare_exchange_strong(expected, true))
{
    closesocket(_sock);
    _sock = INVALID_SOCKET;
}
```

초기에 CAS 방향을 반대로 작성하는 실수(`expected = true` → `false`)가 있었음. `_isDisconnected`는 초기값이 `false`이므로 `false → true` 방향이 맞음.

**② ioCount 설계 — 소유권 레퍼런스 패턴**

처음에는 `RecvPost`/`SendPost` 호출 전후로만 증감. 그러나 정상 완료 시에도 `ioCount`가 0이 되어 `Clear()`가 불필요하게 호출되는 문제 발견.

근본 원인: 세션이 살아있는 동안 유지되는 **소유권 ref** 개념 누락.

```
Accept 시        → ioCount = 1  (소유권 ref)
RecvPost/SendPost → ioCount++
완료 루틴         → ioCount--
첫 번째 에러 감지 → ioCount--   (소유권 ref 반납)
ioCount == 0     → Clear()
```

이로써 정상 동작 중 recv 완료 시:
```
ioCount: 1(소유권) + 1(RecvPost) = 2
recv 완료 → ioCount-- = 1  →  Clear 안 됨 ✓
```

**③ `fetch_sub` 리턴값 실수**

`fetch_sub(1)`은 감소 **전** 값을 반환. 따라서 ioCount가 1→0으로 내려가는 시점을 잡으려면 `== 1` 체크가 맞음.

```cpp
// 틀림
if (_ioCount.fetch_sub(1) == 0)

// 맞음
if (_ioCount.fetch_sub(1) == 1)
```

**④ `ReleaseRef` / `Disconnect` 추출 — 중복 제거**

동일한 CAS + closesocket + fetch_sub 패턴이 IOCPServer.cpp, Session.cpp 세 곳에 중복. 두 메서드로 추출:

```cpp
void Session::ReleaseRef()
{
    if (_ioCount.fetch_sub(1) == 1)
        Clear();
}

void Session::Disconnect()
{
    bool expected = false;
    if (_isDisconnected.compare_exchange_strong(expected, true))
    {
        closesocket(_sock);
        _sock = INVALID_SOCKET;
        ReleaseRef(); // 소유권 ref 반납
    }
    ReleaseRef(); // 이 I/O의 ref 반납
}
```

초기 구현 시 `Disconnect()` 안에서 소유권 ref만 반납하고, 트리거가 된 I/O의 ref를 반납하는 두 번째 `ReleaseRef()` 호출이 빠져 있었음.

**⑤ `ReleaseRef` 호출 순서**

정상 완료 경로에서 `ReleaseRef()`를 `SendPost()`/`RecvPost()` 앞에 두면, 다른 스레드에서 동시에 에러가 발생해 소유권 ref까지 반납될 경우 `Clear()` 이후에 I/O 등록이 시도될 수 있음.

`ReleaseRef()`를 `SendPost()`/`RecvPost()` 뒤로 이동하여 새 I/O가 등록된 후에 현재 I/O ref를 반납하도록 수정:

```cpp
session->SendPost();
session->RecvPost();
OnRecv(session->GetSessionID(), nullptr);
session->ReleaseRef();
```

### 해결 방안

**Session에 추가된 멤버**
```cpp
std::atomic<int>  _ioCount        = 0;
std::atomic<bool> _isDisconnected = false;
```

**Accept 스레드**: IOCP 등록 성공 후 `session._ioCount++` (소유권 ref 획득)

**RecvPost / SendPost**: `WSARecv`/`WSASend` 호출 전 `_ioCount.fetch_add(1)`, 즉시 에러 시 `Disconnect()`

**GQCS 워커 스레드**:
- 에러/0바이트: `session->Disconnect()`
- 정상 완료: 처리 후 `session->ReleaseRef()`

**Clear()**:
```cpp
void Session::Clear()
{
    _playerId  = 0;
    _isSending = false;
    _isDisconnected = false;
    _recvBuffer.ClearBuffer();
    _sendBuffer.ClearBuffer();
}
```

### 배운 점
- IOCP에서 소켓을 닫으면 pending I/O가 에러 완료로 GQCS에 도착한다. 따라서 "I/O가 모두 끝난 뒤에 정리"하는 ref count 패턴이 필수다.
- `fetch_sub` / `fetch_add`는 연산 **전** 값을 반환한다. "0이 됐는지" 확인하려면 `== 1` 체크가 맞다.
- 소유권 ref 개념이 없으면 정상 동작 중에도 `Clear()`가 호출된다. Accept 시점에 ref를 1 추가하고, 첫 에러 감지 시 반납하는 구조가 올바른 생명주기를 보장한다.
- 반복되는 atomic 패턴은 `ReleaseRef()` / `Disconnect()` 같은 메서드로 추출하면 호출부가 단순해지고 실수도 줄어든다.

---

## 고유 SessionID 설계 — 슬롯 인덱스 + generation 카운터
**날짜**: 2026-04-14

### 현상
슬롯 기반 세션 배열에서 슬롯이 재사용될 경우, 이전 세션의 슬롯 인덱스로 `SendPacket`을 호출하면 새로 접속한 세션에게 잘못된 데이터가 전송될 수 있음.

### 원인
슬롯 인덱스만으로는 "지금 이 슬롯에 있는 세션이 내가 의도한 세션인가"를 구분할 수 없음.

### 해결 방안

**ID 구성 — `uint64_t` 64비트**

`uint64_t`는 8바이트(64비트). 20000개 슬롯을 표현하려면 15비트 필요(`2^15 = 32768`). 넉넉하게 하위 16비트를 인덱스로 사용.

```
[ 상위 48bit: generation ] [ 하위 16bit: slot index ]
```

- 슬롯 인덱스: 최대 65535 (20000 충분히 커버)
- generation: 48비트 → 서버 운영 중 wrap-around 사실상 불가능

**generation 관리**

Session 멤버로 generation을 두고 `Clear()`마다 증가시키는 방식도 가능하나, 서버 전체 단조 증가 카운터를 `IOCPServer` 멤버로 두는 것이 더 단순함.

```cpp
// IOCPServer.h
std::atomic<uint64_t> _sessionCounter = 0;

// Accept 스레드 — 슬롯 할당 시
uint64_t generation = _sessionCounter.fetch_add(1);
uint64_t sessionID  = (generation << 16) | static_cast<uint64_t>(slotIndex);
session.SetSessionID(sessionID);
```

Session은 받은 ID를 저장만 하면 되고, generation 관리 책임이 IOCPServer에 집중됨.

**검증 — `SendPacket` 진입부**

```cpp
bool IOCPServer::SendPacket(SessionID sessionID, CPacket* packet)
{
    int slotIndex = sessionID & 0xFFFF;
    if (_sessions[slotIndex].GetSessionID() != sessionID) return false; // 슬롯 재사용됨
    // ...
}
```

**ID 추출**

```cpp
int      slotIndex  = sessionID & 0xFFFF;
uint64_t generation = sessionID >> 16;
```

### 배운 점
- 슬롯 인덱스만으로는 세션의 동일성을 보장할 수 없다. generation 카운터를 상위 비트에 결합해 재사용 슬롯을 구분하는 것이 표준 패턴이다.
- generation을 Session 멤버로 관리하면 슬롯마다 상태를 따로 관리해야 하지만, IOCPServer의 전역 단조 카운터를 쓰면 Session은 ID를 저장만 하면 돼서 책임이 명확히 분리된다.
- `uint64_t`는 16바이트가 아닌 8바이트(64비트)다.

---

## 현재 미해결 버그 목록 및 작업 상태
**날짜**: 2026-04-14

### 작업 완료된 내용
1. `_isSending` race condition → `std::atomic<bool>` + CAS 패턴으로 해결 (검증 완료)
2. `ioCount` 기반 세션 생명주기 설계 및 구현
   - 소유권 ref 패턴 (accept 시 +1, 첫 에러 시 반납)
   - `ReleaseRef()` / `Disconnect()` 메서드 추출
3. `SessionID` = `(generation << 16) | slotIndex` 구조 설계 및 적용
4. `std::function<void()> ReturnIndex` 콜백으로 슬롯 반환 구현
5. `debug_log/` 폴더 분리, `.gitignore`에 추가
6. 트러블슈팅 파일 이미 원격에 올라간 것 `git rm --cached`로 제거

### 현재 확인된 버그 (미수정)

**버그 1 — `Clear()`에서 `ReturnIndex()` 호출 순서**
```cpp
// 현재 (잘못됨): 슬롯 반환 후 버퍼 정리
void Session::Clear()
{
    ReturnIndex();        // ← 먼저 슬롯 반환
    _recvBuffer.ClearBuffer();  // ← 이 시점에 새 클라이언트가 이미 슬롯 사용 중일 수 있음
    _sendBuffer.ClearBuffer();
    ...
}

// 올바른 순서: 모든 정리 완료 후 마지막에 슬롯 반환
void Session::Clear()
{
    _recvBuffer.ClearBuffer();
    _sendBuffer.ClearBuffer();
    _isSending = false;
    _isDisconnected = false;
    ReturnIndex();        // ← 맨 마지막
}
```
**영향**: 새 클라이언트가 슬롯을 받은 직후 구 `Clear()`가 버퍼를 초기화하는 race condition. 슬롯 재사용 시 데이터 오염 원인.

**버그 2 — `GetSessionID()` 반환 타입 잘림**
```cpp
// 현재 (잘못됨)
int GetSessionID() const { return _playerId; }  // uint64_t → int 잘림

// 올바름
SessionID GetSessionID() const { return _playerId; }
```

**버그 3 — `ReleaseRef()` 호출 순서 (recv/send 완료 경로)**
```cpp
// 현재: ReleaseRef가 SendPost/RecvPost 앞
session->ReleaseRef();
session->SendPost();
session->RecvPost();

// 올바름: 새 I/O 등록 후 ref 반납
session->SendPost();
session->RecvPost();
OnRecv(...);
session->ReleaseRef();
```
**영향**: 드문 race에서 `Clear()` 후 `RecvPost()`가 새 클라이언트 소켓에 이중 등록될 가능성.

### 현재 미파악 버그
- echo 자체가 동작하지 않음 (클라이언트 send 값이 돌아오지 않음)
- 연결을 끊지 않아도 발생
- 정적 분석으로는 원인 미파악 → 다음 세션에서 로그 찍어 추적 필요

**추적 방법 (다음 세션 시작 시)**:
```cpp
// recv 완료 직후
std::cout << "[RECV] bytes=" << cbTransferredBytes
          << " sendUse=" << session->_sendBuffer.GetUseSize()
          << " isSending=" << session->_isSending.load() << "\n";

// SendPost 내부, WSASend 직전
std::cout << "[SEND] posting " << totalSize << " bytes\n";
```

### 다음 작업 예정
- 위 버그 3개 수정
- echo 미동작 원인 파악 및 수정
- `OnClientLeave` 호출 연결
- `SendPacket` / `Disconnect` 외부 API 구현
- `CPacket` 설계 및 구현
- `Stop()` 구현
