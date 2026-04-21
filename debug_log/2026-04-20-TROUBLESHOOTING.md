# 2026-04-20 작업 기록

---

## 티켓 기반 로그인 인증 시스템 구현
**날짜**: 2026-04-20

### 현상
게임서버에 접속하는 모든 클라이언트가 즉시 게임 패킷을 보낼 수 있는 상태였음. 인증 없이 이동/공격 등 모든 패킷 처리.

### 원인
인증 게이트 및 로그인 서버 연동 미구현.

### 추론 및 시도 과정
- 인증 흐름 설계: 클라 → 로그인서버 (id/pw) → ticket 발급 → 게임서버 접속 → CS_LOGIN_AUTH(ticket) → 게임서버가 로그인서버에 HTTP POST로 ticket consume 요청 → 성공 시 Authenticated 전환
- 인증 전 상태에서는 CS_LOGIN_AUTH 패킷만 허용, 나머지 전부 차단
- 실패 시 SC_LOGIN_AUTH_RESULT(fail) 전송 후 Disconnect

### 해결 방안

**1. PacketTypes.h — CS_LOGIN_AUTH 구조체 변경**
- 기존: `{ uint16_t ticketLength; }` + 가변 길이 ticket
- 변경: `{ char ticket[32]; }` — 고정 32바이트 ASCII, length 필드 제거

**2. ServerConfig.h — 인증 서버 상수 추가**
```cpp
constexpr const wchar_t* AUTH_SERVER_HOST = L"127.0.0.1";
constexpr uint16_t       AUTH_SERVER_PORT = 5150;
constexpr const wchar_t* AUTH_VERIFY_PATH = L"/verify";
```
- WinHTTP API가 `wchar_t*` 요구 → 게임 로직 문자열은 `std::string` 유지, WinHTTP 경계에서만 wide string 사용

**3. AuthClient.h / AuthClient.cpp 신규 생성**
- `VerifyTicket(const char* ticket, int ticketLen)` → `AuthResult{ valid, accountId, displayName }`
- WinHTTP 동기 POST: `POST /verify` body `{"ticket":"<32chars>"}`
- 응답 JSON 파싱: `{"valid":true,"accountId":1001,"displayName":"name"}`
- 세션당 WinHTTP 세션 재사용 (`static HINTERNET s_hSession`)
- include 순서: `winsock2.h` → `windows.h` → `winhttp.h` (순서 틀리면 `HINTERNET` 미정의 빌드 에러)
- `#pragma comment(lib, "winhttp.lib")`

**4. 인증 게이트 최적화**
- 초기: `OnRecv`마다 `shared_lock(_authStatesMutex)` + `unordered_map::find` → 매 패킷 오버헤드
- 개선: `JobQueue`에 `std::atomic<bool> isAuthenticated = false` 추가 (public)
- `OnRecv`에서 `jobQueue->isAuthenticated` 직접 체크 → lock/map lookup 제거
- 인증 성공 시 `OnCS_LoginAuth`에서 `jobQueue->isAuthenticated = true` 세팅
- `SessionAuthState`에서 `isAuthenticated` 필드 제거, `accountId`/`displayName` 보관 전용으로 변경

**5. PacketHandler.cpp — OnCS_LoginAuth 로직 교체**
- 기존: `ticket.empty()` 여부로 stub 성공 판단
- 변경: `VerifyTicket()` 호출 → HTTP 검증 → 실패 시 `Disconnect(sessionID)` 추가

### 배운 점
- 인증 게이트를 per-session 객체(JobQueue)에 두면 글로벌 뮤텍스 없이 O(1) 체크 가능
- WinHTTP는 Windows 내장 라이브러리로 외부 의존성 없이 HTTP 클라이언트 구현 가능. 단 include 순서(`winsock2` → `windows` → `winhttp`) 주의
- ticket 길이가 고정이면 가변 길이 필드 없애는 게 직렬화/역직렬화 단순화에 유리
