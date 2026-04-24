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
