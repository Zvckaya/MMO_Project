## 락프리 메모리 풀 도입 및 성능 분석
**날짜**: 2026-04-18

### 현상
Packet 객체를 Worker 스레드에서 `new`로 생성하고 ImmediateThread에서 `delete`로 해제하는 구조였다. 패킷 처리량(TPS)이 70만을 넘는 상황에서 힙 할당/해제 비용이 병목이 될 수 있다고 판단, 락프리 메모리 풀 도입을 검토했다.

### 원인 (도입 전 구조의 한계)
- `new Packet()` / `delete packet` — 매 패킷마다 힙 할당/해제 발생
- Debug 빌드 힙은 매우 느려서 체감 가능한 오버헤드
- Release 빌드에서도 멀티스레드 힙 경합이 발생할 수 있다고 예상

### 추론 및 시도 과정

**1단계 — 락프리 메모리 풀 설계**
- 침습형(intrusive) 노드 구조: `alignas(DATA) unsigned char data[sizeof(DATA)]`를 Node 내부에 embed
- `Free(DATA*)` 시 `offsetof(Node, data)`로 포인터 역산 → 별도 맵/해시 불필요
- ABA 방지: x64 가상 주소는 48비트만 사용하므로, `std::atomic<uint64_t> _head`의 상위 16비트를 버전 태그로 사용
  - Push/Pop마다 태그 단조 증가 → 같은 포인터 재사용 시에도 ABA 불발
- `placementNew=false`: 생성자는 최초 1회만, 이후 재사용 시 `packet->Clear()`만 호출 → memset 비용 절감

**2단계 — shared_ptr 제거**
- 기존: job 람다에서 `shared_ptr<Packet>` 캡처 → 컨트롤 블록 힙 할당 발생 → 메모리 풀 도입 의미 반감
- 변경: raw 포인터 캡처 + `_packetPool.Free(packet)` 명시적 호출
- 근거: `TryEnqueueSend`에서 sendBuffer로 데이터 복사가 완료되므로, Dispatch 반환 직후 packet 해제 가능

**3단계 — PACKET_POOL_SIZE 결정**
- Packet 크기: ~1008 bytes, Node 크기: ~1036 bytes
- 20000개 → 약 20MB, 30000개 → 약 30MB
- TPS 70만 기준으로 여유분 확보 위해 30000으로 결정

**4단계 — Release 빌드 오류**
- Debug는 빌드 성공, Release만 대규모 파싱 에러 (~200개) 발생
- 원인: MemoryPool.h에 한글 주석 포함 → MSVC가 Release 구성에서 /utf-8 플래그 없이 CP949로 읽어 UTF-8 멀티바이트 시퀀스 오염 → 템플릿 파싱 실패
- 해결: vcxproj Release|Win32, Release|x64 양쪽에 `/utf-8` 추가

### 성능 테스트 결과

**Debug 빌드 — 300초 / 100클라이언트 / OverSend 200**

| 버전 | 평균 SendTPS | Connect Total | 에러 |
|------|-------------|--------------|------|
| aafe56a (new/delete) | 285,666 | 285,901 | 모두 0 |
| 3f27873 (MemoryPool) | 291,585 | 302,372 | 모두 0 |
| 차이 | **+2.1%** | | |

**Release 빌드 — 180초 / 100클라이언트 / OverSend 200**

| 버전 | 평균 SendTPS | Connect Total | 에러 |
|------|-------------|--------------|------|
| aafe56a (new/delete) | 737,838 | 357,738 | 모두 0 |
| 3f27873 (MemoryPool) | 443,066 | 249,975 | 모두 0 |
| 차이 | **-39.9%** (new/delete가 빠름) | | |

### 해결 방안 (현재 결론)

Release에서 메모리 풀이 오히려 느린 원인:
- MSVC Release의 `malloc`/`free`는 LFH(Low-Fragmentation Heap)로 자동 최적화되어 이미 매우 빠름
- 현재 구현한 글로벌 락프리 스택은 Alloc/Free마다 CAS가 발생 → 멀티스레드 경합 시 spin 오버헤드가 LFH보다 큼
- Debug에서 +2.1% 이득이 난 이유: Debug 힙이 원래 느려서 상대적 이득 발생한 것

근본 원인은 **Worker(Alloc)와 ImmediateThread(Free)가 다른 스레드**라는 점:
- Alloc과 Free가 항상 같은 스레드에서 일어나지 않으므로, 단순 TLS 캐시 적용 불가
- 해결책: **버킷 단위 TLS + SharedPool** 구조 (ChatServer의 CTLSMemoryPool 방식)
  - 각 스레드가 TLS에 버킷(예: 64개) 단위로 캐싱
  - 버킷이 비면 SharedPool에서 뭉텅이로 가져옴, 차면 뭉텅이로 반납
  - 글로벌 CAS 접근이 노드 1개당 → 버킷 64개당 1회로 감소
  - Free한 스레드의 TLS에 쌓이다가 버킷이 차면 SharedPool 경유로 재순환

### 배운 점
- 락프리라고 무조건 빠른 게 아니다. CAS spin 자체가 캐시 라인 invalidation을 유발하므로, 경합이 많은 환경에서는 오히려 병목이 된다.
- OS 힙(LFH)은 Release 환경에서 이미 고도로 최적화되어 있어, 단순 글로벌 락프리 풀로는 이기기 어렵다.
- 실질적인 이득을 내려면 글로벌 접근 빈도 자체를 줄여야 한다 → 버킷 TLS 구조가 필요.
- 크로스스레드 Alloc/Free 패턴에서는 "Free한 스레드의 TLS에 쌓았다가 SharedPool로 일괄 반납" 방식으로 해결 가능하다.

---

## SO_SNDBUF=0 (Zero-Copy) 제거로 TPS 3배 이상 상승
**날짜**: 2026-04-18

### 현상
`SO_SNDBUF=0` / `SO_RCVBUF=0` 소켓 옵션을 적용한 상태에서 물리 네트워크(다른 머신) 스트레스 테스트 결과:
- SO_SNDBUF=0 켬: 약 30만 TPS
- SO_SNDBUF=0 끔: 100만 TPS 이상 (3배 이상 상승)

루프백(127.0.0.1)에서는 켜나 끄나 TPS 차이 없음 — 이유는 아래 원인 참고.

### 원인

`SO_SNDBUF=0`의 이론적 목적은 **zero-copy** 이다.
- 일반 구조: 앱 버퍼 → [memcpy] → 커널 TCP 송신버퍼 → TCP 처리 → NIC
- zero-copy 구조: 앱 버퍼(page lock) → TCP 처리 → NIC DMA 직접 접근

memcpy 1회를 제거하는 대신 다음 비용이 발생한다:
- WSASend마다 IRP(I/O Request Packet) 생성 — 커널 객체 할당
- 앱 버퍼에 page lock/unlock 오버헤드
- 모든 WSASend가 IO_PENDING(진짜 비동기)으로 강제 전환 → IOCP 완료 통지 발생 → 워커 스레드 깨움 → CompleteSend → SendPost 재호출 반복

커널 송신버퍼가 있으면:
- WSASend → 커널 버퍼에 memcpy 후 즉시 리턴(fast I/O, 0 반환)
- 완료 통지는 IOCP 큐에 올라오나 거의 즉시 처리됨
- IRP 생성 및 page lock 오버헤드 없음
- 워커 스레드 CompleteSend → SendPost 사이클이 훨씬 빠르게 돌아감

### 추론 및 시도 과정

**1단계 — 루프백에서 차이 없는 이유**
- 루프백은 NIC를 거치지 않고 커널 내부에서 직접 처리됨
- IRP는 생성되지 않음이 확인됨
- page lock 여부는 미확인 — DMA가 없으므로 이론상 불필요하나, Windows TCP/IP 드라이버가 루프백 경로에서 실제로 생략하는지는 ETW 수준 분석이 필요
- 결과적으로 루프백에서는 켜나 끄나 TPS 차이가 없었음 — page lock이 없거나 있어도 루프백 전체 비용 대비 무시할 수준으로 추정

**2단계 — 물리 네트워크에서 차이가 발생하는 이유**
- 실제 NIC를 타는 경로에서는 `SO_SNDBUF=0`이 다음을 강제함:
  - WSASend마다 IRP(I/O Request Packet) 생성 — 커널 객체 할당 비용
  - 앱 버퍼에 page lock/unlock — 물리 메모리 고정 비용
- 커널 송신버퍼가 있으면(기본값): WSASend → 커널 버퍼에 memcpy → IRP/page lock 없이 빠르게 리턴
- GQCS를 통한 완료 통지 처리는 양쪽 모두 동일하게 발생 (IOCP 표준 동작)
- 게임 서버 패킷은 수십~수백 바이트 수준이라 memcpy 비용 자체가 미미 → IRP + page lock 오버헤드가 압도적으로 큼

### 해결 방안
`SO_SNDBUF=0` / `SO_RCVBUF=0` 옵션 제거. 커널 기본 송수신 버퍼 사용.

### 배운 점
- zero-copy는 "무조건 빠른 기법"이 아니라 **대용량 전송 환경에서 memcpy가 병목일 때** 의미 있는 최적화다.
- 게임 서버처럼 소형 패킷을 고빈도로 처리하는 환경에서는 IRP/page lock 오버헤드가 memcpy 절감 이득을 압도한다.
- 루프백 테스트는 NIC DMA 이득이 원천 차단되므로, zero-copy 효과 측정에 부적합하다.
- 최적화 옵션은 항상 실측 후 판단해야 한다. 이론과 실제 결과가 역전되는 경우가 빈번하다.
