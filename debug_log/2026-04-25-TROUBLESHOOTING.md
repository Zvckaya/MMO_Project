## CS_STOP 지연 도착으로 인한 서버 플레이어 이동 중단 버그
**날짜**: 2026-04-25

### 현상
클라이언트에서 A* 웨이포인트 기반으로 이동 시, 서버의 플레이어 위치가 클라이언트보다 항상 한 세그먼트씩 뒤처짐.
STOP 로그에서 서버 위치가 직전 CS_STOP 처리 후 위치에 고정된 채 움직이지 않는 패턴이 반복됨.

```
[STOP] client(-7.50, 2.50) server(-10.51, 5.50)
[STOP] client(-4.50, 2.50) server(-7.50, 2.50)  ← 서버가 이전 client 위치에 머묾
[STOP] client(-4.50, -0.50) server(-4.50, 2.50)
```

### 원인
클라이언트의 A* 이동은 세그먼트 단위로 동작한다.
웨이포인트 B 도착 시 CS_STOP(B)과 CS_MOVE(curX=B, destX=C)를 거의 동시에 전송하지만,
네트워크 지연으로 인해 두 패킷이 서로 다른 FrameThread 사이클에 처리될 수 있다.

```
[FrameThread 사이클 N]
  CS_MOVE(B→C) dirty=true, CS_STOP(B) 아직 미도착
  → destX=C, isMoving=true 처리 완료

[FrameThread 사이클 N+1]
  CS_STOP(B) 도착, dirty=true
  → posX=B, destX=B, isMoving=false ← CS_MOVE(B→C) 결과를 덮어씀
  → CS_MOVE는 이미 dirty=false → 재처리 없음
  → 서버가 B에서 멈춰버림
```

CS_STOP 핸들러가 `destX/destY`와 `isMoving`을 무조건 덮어써서,
이미 처리된 CS_MOVE의 상태를 되돌리는 것이 핵심 원인.

### 추론 및 시도 과정
- 처음에는 CS_MOVE의 curX/curY 거리 검증 로직(speed * elapsed * 1.5f) 문제로 CS_MOVE 자체가 reject되는 것으로 의심했으나, destX/destY와 isMoving은 검증 블록 바깥에서 항상 적용되므로 해당 원인은 아님.
- STOP 로그에서 "서버 위치 = 이전 CS_STOP의 client 위치"가 반복되는 패턴으로 CS_STOP이 이미 처리된 CS_MOVE 상태를 덮어쓰는 것임을 확인.

### 해결 방안
CS_STOP 핸들러에서 stop 위치와 현재 `destX/destY`의 거리를 비교해,
서버가 이미 다른 목적지를 향하고 있으면 `posX/posY` 동기화만 하고 `isMoving`과 `destX/destY`는 건드리지 않는다.

```cpp
player->posX = jobQueue->pendingStop.curX;
player->posY = jobQueue->pendingStop.curY;

float sdx    = jobQueue->pendingStop.curX - player->destX;
float sdy    = jobQueue->pendingStop.curY - player->destY;
bool  atDest = (sdx * sdx + sdy * sdy) < 0.1f * 0.1f;
if (atDest)
{
    player->destX    = jobQueue->pendingStop.curX;
    player->destY    = jobQueue->pendingStop.curY;
    player->isMoving = false;
}
```

- stop 위치 ≈ destX/destY (0.1f 이내): 정상 도착 → isMoving=false, 이동 종료
- stop 위치 ≠ destX/destY: 늦게 도착한 CS_STOP → posX/posY만 동기화, 이동 상태 유지

### 배운 점
- 웨이포인트 기반 이동에서 CS_STOP과 CS_MOVE는 거의 동시에 전송되지만, 서버의 FrameThread 사이클 경계에 걸리면 처리 순서가 역전될 수 있다.
- 이동 상태(isMoving, destX/destY)는 CS_STOP이 아닌 서버의 목적지 도달 여부로 판단하는 것이 안전하다.
- CS_STOP의 역할을 "위치 동기화"와 "이동 종료" 두 가지로 분리해서, 이동 종료는 목적지 일치 여부로만 결정해야 한다.
