## 로그인 서버 / 게임 서버 DB 분리 (Docker 활용)
**날짜**: 2026-04-24

### 현상
로컬 환경에서 로그인 서버와 게임 서버가 단일 DB(`loginserver`, 3306)를 공유하고 있었음. 서비스 책임 경계가 불명확하고, 두 서버가 같은 DB에 의존하는 구조.

### 원인
초기 개발 단계에서 빠른 구현을 위해 하나의 DB에 모든 테이블(`item_data`, `characters`, `player_inventory`)을 몰아넣었음. 로컬에서 두 DB 인스턴스를 동시에 띄우려면 포트 충돌 문제가 발생.

### 추론 및 시도 과정
- 로그인 서버(인증·티켓): 포트 3306 MySQL 유지
- 게임 서버(캐릭터·인벤토리·아이템): 포트 3307에 별도 MySQL 인스턴스 필요
- 로컬에서 포트 분리를 위해 Docker로 독립 MySQL 컨테이너를 구성

### 해결 방안
- 게임 서버 전용 DB(`gameserver`)를 Docker 컨테이너(3307)로 분리 구성
- `ServerConfig.h` 수정:
  ```cpp
  constexpr uint16_t    DB_PORT = 3307;
  constexpr const char* DB_NAME = "gameserver";
  ```
- 게임 서버 DB 스키마 신규 작성:
  - `item_data` (item_id, name, type, max_stack)
  - `characters` (account_id, pos_x, pos_y, hp, map_id)
  - `player_inventory` (account_id, slot_index, item_id, count) — characters FK + CASCADE

### 배운 점
- 서비스 분리는 설계 초기부터 DB 단위로 경계를 나눠야 나중에 비용이 적음
- Docker를 이용하면 로컬에서도 운영 환경과 동일한 멀티 DB 구조를 쉽게 재현 가능
- 포트폴리오 관점에서 "왜 분리했는가(책임 경계) + 어떻게 해결했는가(Docker)" 흐름으로 서술하면 설계 판단력을 보여줄 수 있음

---

## 인증 상태 관리 구조 개선 — unauthenticated / authenticated 이중 리스트
**날짜**: 2026-04-24

### 현상
- `JobQueue`(작업 스케줄링 레이어)에 `isAuthenticated` 플래그가 있어 네트워크 레이어와 콘텐츠 레이어의 관심사가 혼재
- `_sessionAuthStates`(accountId, displayName)와 `jobQueue->isAuthenticated` 두 곳에 인증 상태가 분산되어 일관성 유지가 번거로움
- `MAXPAYLOAD = 100`으로 인해 displayName이 길거나 필드 추가 시 버퍼 오버플로 위험

### 원인
- 초기 구현 시 인증 게이트를 빠르게 붙이기 위해 JobQueue에 플래그를 직접 추가
- 이후 SessionAuthState가 추가되며 중복 관리 구조가 형성됨
- MAXPAYLOAD는 초기 에코 서버 기준 값을 그대로 유지한 것

### 추론 및 시도 과정
- 인증 상태를 "인증 전 세션 목록 + 인증 후 플레이어 목록"으로 명확히 이분하면 단일 진실 공급원 확보 가능
- JobQueue는 순수 작업 스케줄링만 담당하도록 책임 분리
- `_sessionAuthStates`에 없으면 미인증, 있으면 인증 완료로 단순화

### 해결 방안
**`MAXPAYLOAD` 수정** (`ServerConfig.h`):
```cpp
constexpr int MAXPAYLOAD = 4096;
```

**`JobQueue.h`**: `isAuthenticated` 제거

**`GameServer` 인증 상태 구조**:
```
connect   →  _unauthSessions (unordered_set<SessionID>) 에 insert
auth 성공 →  _unauthSessions erase + _sessionAuthStates[sessionID] = { accountId, displayName }
leave     →  _unauthSessions + _sessionAuthStates 양쪽 erase
```

**`OnRecv` 인증 게이트** (`GameServer.cpp`):
```cpp
std::shared_lock lock(_authStatesMutex);
bool isAuth = _sessionAuthStates.count(sessionID) > 0;
if (!isAuth && packet->GetType() != PKT_CS_LOGIN_AUTH)
{
    _packetPool.Free(packet);
    ReleaseContentRef(sessionID);
    Disconnect(sessionID);
    return;
}
```

### 배운 점
- 인증 상태는 네트워크/스케줄링 레이어가 아닌 게임 서버 레이어에서 단일 맵으로 관리해야 추적이 명확함
- "없으면 미인증, 있으면 인증" 패턴은 별도 플래그 없이 맵 존재 여부만으로 상태 표현 가능 — 상태 불일치 버그 원천 차단
- 포트폴리오 관점: 단순 기능 구현이 아닌 레이어 책임 경계를 의식한 리팩터링 사례로 서술 가능
