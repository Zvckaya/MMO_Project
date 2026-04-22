## [Logger::Level::INFO 컴파일 오류]
**날짜**: 2026-04-22

### 현상
`DBClient.cpp`에서 `Logger::Level::INFO`를 사용하자 컴파일 오류 발생.

### 원인
`Logger`에 정의된 레벨 열거값에 `INFO`가 없었음. 실제로 정의된 레벨은 `DEBUG`, `WARN`, `ERR`, `SYSTEM`.

### 해결 방안
`Logger::Level::INFO` → `Logger::Level::SYSTEM`으로 교체.

### 배운 점
새 파일에서 Logger를 사용할 때 실제 열거값 목록을 먼저 확인할 것.

---

## [playerMapChange 시 플레이어 포인터 댕글링]
**날짜**: 2026-04-22

### 현상
맵 변경 처리 중 `newMap->FindPlayer()`가 nullptr을 반환하거나, 이미 해제된 플레이어에 접근하는 문제.

### 원인
`oldMap->RemovePlayer(sessionID)`로 `unique_ptr`을 erase하면 Player 객체가 즉시 소멸됨. 이후 `newMap->AddPlayer()`를 호출하기 전에 Player 포인터를 사용하거나, RemovePlayer 후 플레이어를 newMap에 추가하지 않아 플레이어가 어느 맵에도 존재하지 않는 상태가 발생.

### 해결 방안
`GameMap`에 `TakePlayer(SessionID) → unique_ptr<Player>` 메서드를 추가. `_players` map에서 erase하기 전 소유권을 로컬 변수로 이전한 뒤, `newMap->AddPlayer()`로 넘겨줌. 소유권 이전이 완료된 후에 DESPAWN 브로드캐스트 수행.

```cpp
// GameMap::TakePlayer
std::unique_ptr<Player> TakePlayer(SessionID id)
{
    auto it = _players.find(id);
    if (it == _players.end()) return nullptr;
    auto player = std::move(it->second);
    _players.erase(it);
    return player;
}
```

### 배운 점
맵 간 플레이어 이동은 RemovePlayer가 아닌 TakePlayer로 소유권을 명시적으로 이전해야 함. `unique_ptr`을 erase로 소멸시키면 포인터를 재사용할 수 없으므로, 소유권 이전 패턴을 사용할 것.

---

## [아이템 시스템 설계 — 서버 아이템 시트 필요 여부]
**날짜**: 2026-04-22

### 현상
클라이언트로부터 아이템 ID만 전달받아 처리하면 되는지, 아니면 서버에 아이템 시트가 필요한지 판단 필요.

### 원인
서버 검증 없이 클라이언트 전달값을 그대로 사용하면 존재하지 않는 아이템 ID로 위변조 가능.

### 해결 방안
서버 시작 시 DB(`item_data` 테이블)에서 아이템 시트를 `unordered_map<uint16_t, ItemData>`로 로드. 아이템 조작(드롭, 픽업 등) 처리 전 해당 맵에서 itemID 유효성 검증.

```
// DB 스키마
item_data (item_id PK, name, type, max_stack)
```

```cpp
// Start()에서 동기 로드
auto tempDB = std::make_unique<DBClient>();
tempDB->Connect(...);
_itemDataMap = tempDB->LoadItemData();
```

### 배운 점
아이템 ID 검증은 서버 책임. DB 로드는 스레드 시작 전 동기로 처리하면 별도 동기화 없이 안전하게 사용 가능.

---

## [인벤토리 DB 저장 — 트랜잭션 처리]
**날짜**: 2026-04-22

### 현상
인벤토리 저장 시 슬롯 단위 INSERT를 단순 반복하면 중간 실패 시 부분 저장 상태가 DB에 남음.

### 원인
슬롯마다 개별 INSERT를 수행할 경우 원자성 보장 없음.

### 해결 방안
`SaveInventory()` 내부에서 `START TRANSACTION` → 기존 데이터 `DELETE` → 전 슬롯 `INSERT` → `COMMIT` 패턴 적용. 실패 시 `ROLLBACK`.

### 배운 점
인벤토리처럼 다중 row를 한 단위로 교체하는 경우 반드시 트랜잭션으로 묶을 것.
