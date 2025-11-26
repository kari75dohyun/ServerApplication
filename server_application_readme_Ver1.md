# ServerApplication – README (v1)

# ServerApplication (High-Performance C++ Game Server)

![C++](https://img.shields.io/badge/C++-17-blue.svg?logo=c%2B%2B)
![Boost.Asio](https://img.shields.io/badge/Boost.Asio-1.88.0-green.svg)
![Protobuf](https://img.shields.io/badge/Protobuf-v5.29.5-orange.svg?logo=google-protobuf)
![License](https://img.shields.io/badge/License-MIT-yellow.svg)
![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-lightgrey.svg)

> **C++ Boost.Asio 기반의 고성능 비동기 TCP/UDP 게임/채팅 서버 프레임워크**입니다.
> 세션 샤딩, 정교한 UDP 토큰 인증, 글로벌/세션별 레이트 리밋, Keepalive 타임아웃, DB 미들웨어 연동 구조를 포함합니다.

---

##  Key Features (핵심 기능)

* ** High Performance:** `Boost.Asio` 비동기 I/O와 `Lock Partitioning`(Session Sharding)을 적용하여 락 경합 최소화.
* ** Secure UDP:** 단순 UDP가 아닌, **3-way Handshake(토큰 발급)** 및 **엄격한 엔드포인트 검증**을 통한 스푸핑 방지.
* ** Smart Session Management:** `Generation ID`를 활용한 Stale Session 방지 및 `weak_ptr` 기반의 안전한 생명주기 관리.
* ** Scalable Architecture:** 존(Zone) 기반 브로드캐스팅 시스템 및 외부 DB Middleware(DBMW) 연동을 위한 Protobuf 라우팅 구조.

##  목차 (Table of Contents)
1. [빠른 시작 (Quick Start)](#1-빠른-시작-quick-start)
2. [아키텍처 및 흐름도](#2-Architecture)
    - [UDP 인증 및 통신 흐름](#udp-인증-및-통신-흐름-reliable-udp)
    - [Keepalive & Lifecycle](#keepalive--lifecycle)
3. [프로젝트 구조](#3-프로젝트-구조)
4. [설정 및 프로토콜](#4-설정-메시지-프로토콜-json-예시)
5. [유지/모니터링](#5-유지모니터링)
6. [DB Middleware (옵션)](#6-db-middleware옵션)
7. [운영 팁](#7-운영-팁)
8. [라이선스/크레딧](#8-라이선스크레딧)
9. [중요 함수 설명서 (API Reference)](#9-중요-함수-설명서-api-reference)

---

## 1. 빠른 시작 (Quick Start)

### 📋 요구사항 (Prerequisites)
* **OS**: Windows 10/11 (Visual Studio 2019+), Linux (Docker/CMake)
* **Dependencies** (vcpkg 권장):
    * `boost-asio`, `spdlog`, `nlohmann-json`, `protobuf`, `libcurl`
* **Environment**: `MY_SERVER_SECRET` 환경변수 설정 필수.

### ⚙️ 설정 파일 (`config.json`)

아래 내용을 `config.json`으로 저장하여 사용합니다.

```json
{
  "tcp_port": 12345,
  "udp_port": 54321,
  "session_pool_size": 1,
  "max_session_pool_size": 10000,
  "max_udp_queue_size": 10000,
  "zone_max_sessions": 500,
  "udp_shard_count": 16,
  "session_max_close_retries": 3,
  "session_retry_delay_ms": 100,
  "max_task_queue": 1000,
  "login_timeout_seconds": 90,
  "max_write_queue_size": 100,
  "write_queue_warn_threshold": 80,
  "write_queue_overflow_limit": 10,
  "udp_expire_timeout_seconds": 300,
  "user_limit_per_sec": 10,
  "total_limit_per_sec": 1000,
  "max_zone_count": 10,
  "max_zone_session_count": 500,
  "dbmw_host": "127.0.0.1",
  "dbmw_port": 6789,
  "dbmw_heartbeat_sec": 20,
  "udp_token_ttl_seconds": 300,
  "max_udp_packet_size": 1024
}


### 빌드
1. VS에서 `ServerApplication.vcxproj` 오픈
2. vcpkg 또는 프로젝트 설정으로 필요한 라이브러리 포함
3. `x64-Release` 빌드 권장

### 실행
bash
> set MY_SERVER_SECRET=your-secret
> ServerApplication.exe

 로그: `Server.log`
-기본 포트: TCP `12345`, UDP `54321` (config로 변경)

## 2. 아키텍처 및 흐름도 (Architecture)

### UDP 인증 및 통신 흐름 (Reliable UDP)
서버는 **토큰(Token)과 엔드포인트(IP:Port) 이중 검증**을 통해 신뢰할 수 있는 UDP 통신을 보장합니다.

```mermaid
sequenceDiagram
    participant Client
    participant Server (UdpRegisterHandler)
    participant Session (DataHandler)

    Note over Client, Server: 1. 초기 등록 (Handshake)
    Client->>Server: {"type": "udp_register", "nickname": "UserA"}
    Server->>Server: Generate Random Token (TTL 300s)
    Server-->>Client: {"type": "udp_register_ack", "token": "XYZ123"}

    Note over Client, Server: 2. 보안 통신 (Secure Transport)
    Client->>Session: {"type": "udp", "token": "XYZ123", "msg": "Hello"}
    
    Session->>Session: Validate Token & Endpoint
    alt Token/Endpoint Mismatch
        Session-->>Client: Error: ENDPOINT_MISMATCH
        Note right of Client: 클라이언트는 재등록(Register) 시도
    else Valid
        Session->>Session: Rate Limit Check
        Session->>Server: Process Message
    end

### Keepalive & Lifecycle
```mermaid
graph TD
    A[Start Keepalive Loop] --> B{Check All Sessions}
    B -->|Active| C[Update Timestamp]
    B -->|Timeout (>60s)| D[Collect Stale Sessions]
    D --> E[Close Session & Release Resource]
    E --> F[Session Pool]
    B --> G[Wait 60s]
    G --> B    

    갱신: keepalive 패킷 수신 시 Session::update_alive_time() 호출.

    감시: 글로벌 타이머 DataHandler::start_keepalive_loop()가 60초 간격으로 실행.

    정리: now - last_alive_time > 60s 이면 close_session() 수행.

### TCP 흐름
1) 클라이언트 TCP 접속 -> `Server`가 `Session` 생성/등록
2) 수신 JSON의 `type`에 따라 `MessageDispatcher`가 핸들러 호출
   - 지원 타입: `login`, `logout`, `chat`, `chat_zone`, `keepalive`, (기타는 에러)
3) `keepalive` 수신 시 `Session::update_alive_time()`으로 마지막 활동 갱신
4) 글로벌 타이머 `DataHandler::start_keepalive_loop()`가 **60초** 간격으로 `do_keepalive_check()` 호출
   - `now - last_alive_time > 60s` 이면 `close_session()`

 **권장 값**: 클라 keepalive 20s, 서버 타임아웃 ≥ 60s (현재 코드 그대로면 10s 단절은 세션 유지)

### UDP 흐름 (토큰+엔드포인트 이중 인증)
- 최초: 클라가 UDP로 `{"type":"udp_register","nickname":"<nick>"}` 전송
  1. 서버(`UdpRegisterHandler`)가 **엔드포인트 바인딩** + **랜덤 토큰 발급(기본 TTL=300s)**
  2. 응답 `udp_register_ack`에 `token` 포함
- 이후 모든 UDP에는 `nickname` + `token` + `type` 포함
- `DataHandler::on_udp_receive()` 검증 단계
  1. 서버 전체 레이트리밋(샤드)
  2. JSON 파싱
  3. 닉네임 존재 확인 -> 세션 조회
  4. (udp_register 제외) **토큰 일치 & TTL** 검사
  5. (udp_register 제외) **엔드포인트 일치** 검사 -> 불일치 시 `udp_error: ENDPOINT_MISMATCH`
  6. 세션별 토큰버킷 레이트리밋
  7. (변화 감지) 엔드포인트 스냅샷과 다르면 **전역맵/세션 값 갱신**
  8. UDP 핸들러 호출(`udp`, `broadcast_udp`, `broadcast_udp_zone` 등)

> **네트워크 전환(Wi‑Fi↔LTE)**: 엔드포인트(IP:포트)가 바뀌면 서버가 `ENDPOINT_MISMATCH`를 보내므로 **클라는 `udp_register` 재요청**으로 새 토큰을 수령해야 합니다.


## 3. 프로젝트 구조

ServerApplication/
  AppContext.h            - 전역 컨텍스트(로거, config, 약한참조 등)
  ServerApplication.cpp   - main(), io_context, Server/UDPManager/DBMW 초기화
  Server.{h,cpp}          - TCP acceptor, 세션 생성
  Session{,.cpp,.h}       - 1 TCP 연결 = 1 세션. write 큐/상태/타임스탬프
  SessionManager{,.cpp}   - 세션 등록/검색/순회/정리
  SessionPool{,.cpp}      - 세션 객체 풀
  DataHandler{,.cpp,.h}   - 상위 오케스트레이터(브로드캐스트, UDP 수신, keepalive 루프)
  MessageDispatcher{,.h,cpp} - type별 핸들러 라우팅(TCP/UDP)

  MessageHandlers/
    Chat(Zone)Handler.*   - 채팅/존 채팅
    CertifyHandler.*      - 인증/로그인 흐름 일부
    KeepaliveHandler.*    - 클라->서버 keepalive 수신

  UDPMessageHandlers/
    UdpHandler.*          - 기본 에코/샘플
    UdpRegisterHandler.*  - UDP 엔드포인트 바인딩 + 토큰 발급/TTL 적용
    UdpBroadcast(Zone)Handler.* - UDP 브로드캐스트(전체/존)

  Zone{,Manager}.{h,cpp}  - 존 관리/브로드캐스트
  UDPManager{,.h,cpp}     - UDP 소켓 생성/수신 루프 -> DataHandler로 전달
  DBMiddlewareClient.*    - (옵션) 외부 DBMW 연동 (라우터 DBmwRouter와 함께)
  DBmwRouter.* / DBmwHandlerRegistry.* - DBMW 메시지 라우팅

  Logger.{h,cpp}          - spdlog 초기화(파일 로그)
  Utility.{h,cpp}         - 토큰 생성, 레이트리밋, JSON 파싱, config 로드 등
  MemoryTracker.*         - 메모리 사용량 로그
  MessageBufferManager.*  - 송신 버퍼 관리
  generated/
    wire.pb.{h,cc}        - protobuf 메시지(옵션 경로)
  wire.proto              - PB 정의
  config.json             - 서버 설정


## 4. 설정 및 프로토콜 (Configuration)

- `tcp_port`, `udp_port`: 포트
- `udp_token_ttl_seconds`: UDP 토큰 TTL(기본 300s)
- `udp_expire_timeout_seconds`: UDP idle 로깅 기준(현재는 TCP 살아있으면 유지)
- `total_limit_per_sec`, `user_limit_per_sec`: UDP 레이트리밋
- `max_zone_count`, `zone_max_sessions`/`max_zone_session_count`: 존 규모
- `dbmw_*`: DBMW 접속/하트비트 (현재 main에서 초기화 코드 동작)

### 메시지 프로토콜 예시 (JSON)

### TCP
 **keepalive (클라 -> 서버)**
  json
  {"type":"keepalive"}

- **chat (클라 -> 서버)**
  json
  {"type":"chat","nickname":"neo","msg":"hello"}

- **chat_zone (클라 -> 서버)**
  json
  {"type":"chat_zone","nickname":"neo","zone_id":1,"msg":"hi zone"}


### UDP
 **등록 (클라 -> 서버)**
  json
  {"type":"udp_register","nickname":"neo"}

 **등록 응답 (서버 -> 클라)**
  json
  {"type":"udp_register_ack","token":"<RANDOM>","nickname":"neo"}

 **일반 전송 (클라 -> 서버)**
  json
  {"type":"udp","nickname":"neo","token":"<RANDOM>","msg":"..."}

 **에러 (서버 -> 클라)**
  json
  {"type":"udp_error","code":"ENDPOINT_MISMATCH","msg":"Endpoint mismatch; please udp_register again"}


## 5. 유지/모니터링 (Configuration)
- 글로벌 keepalive 루프: `start_keepalive_loop()` -> `do_keepalive_check()`
  1. 현재 **체크 주기 = 60s**, 타임아웃 임계치도 60s
  2. 더 촘촘히 보려면 체크 주기를 1–5s로 낮추고, 임계치는 60s 유지 권장
- 서버 상태 모니터: `start_monitor_loop()` (10s마다 활성 세션 수/메모리 로그)
- 세션 클린업: `start_cleanup_loop()` (60s마다, 5분 이상 비활성 세션 종료)
- 로그: spdlog -> `Server.log`


## 6. DB Middleware(옵션) (Configuration)
- `DBMiddlewareClient` + `DBmwRouter` 구조
- Protobuf: `wire.pb.*` 등록됨, `set_on_message_pb()`로 PB 라우팅
- 현재 main에서 라우터/클라이언트를 생성/등록하고 하트비트로 구동


## 7. 운영 팁
- 모바일 품질 향상: `ENDPOINT_MISMATCH` 수신 시 클라가 **자동 재등록**(백오프 0.2->0.5->1->1.5->2s) 구현
- 커널 TCP keepalive는 보조(앱 레벨 keepalive가 주력)


## 8. 라이선스/크레딧
- Boost, spdlog, nlohmann/json, Protocol Buffers, libcurl 사용


### 변경 이력 (to‑do)
- README 보강: 빌드 스텝(Windows/vcpkg, Linux), 의존성 버전 명시
- API 상세 문서화(로그인/인증 흐름, 에러코드 표)
- 자동 리바인딩(옵션) 플래그화 예시 추가
- 샘플 클라이언트/테스트 스크립트 추가


## 9. 중요 함수 설명서 (API Reference)

> 서명/역할/입출력/부작용/주의사항 중심의 요약. 실제 소스 기준 명칭이 다르면 가까운 위치의 함수로 이해하세요.

### AppContext
- **`static AppContext& instance()`**
  - 전역 싱글턴 접근. 설정/로거/매니저 포인터를 보유.
- **`void load_config(const std::string& path)`**
  - `config.json` 로드/검증. 실패 시 예외 또는 종료.

### Server / ServerApplication
- **`void Server::start_accept()`**
  - TCP `acceptor` 비동기 수락 루프 시작. 수락 시 `Session` 생성/등록 -> `async_read` 시작.
- **`int main()` / `ServerApplication.cpp`**
  - `io_context` 생성, `Server`/`UDPManager`/DBMW 초기화, 시그널 처리, `run()`.

### Session / SessionManager / SessionPool
- **`void Session::start()`**
  - 읽기 루프 시작, 세션 상태 활성화, 매니저 등록.
- **`void Session::post_write(std::string_view data)`**
  - 쓰기 큐에 데이터 push 후 비동기 전송. 큐 과다 시 경고/드롭(설정값 기준).
- **`void Session::close_session()`**
  - 안전 종료: 소켓 cancel/close, 매니저에서 제거, 자원 정리.
- **`void Session::update_alive_time()`**
  - 마지막 활동 시각을 `steady_clock::now()`로 업데이트(keepalive/정상메시지 수신 시 호출 권장).
- **`void Session::set_udp_endpoint(udp::endpoint ep)` / `std::optional<udp::endpoint> get_udp_endpoint()`**
  - 세션에 UDP 엔드포인트 바인딩/조회.
- **`void Session::set_udp_token(std::string token, std::chrono::steady_clock::time_point expire)` / `bool is_udp_token_valid()`**
  - UDP 토큰/만료 설정과 유효성 검사.
- **`SessionManager::find_by_nickname(const std::string&)`**
  - 닉네임 -> 세션 조회. 없으면 `nullptr`.
- **`SessionPool::acquire()/release()`**
  - 세션 객체 풀 관리(성능/할당 최적화).

### DataHandler (핵심 오케스트레이션)
- **`void DataHandler::start_keepalive_loop()`**
  - `keepalive_timer_` 설정 후 `async_wait`로 **주기 실행**. 만료 시 `do_keepalive_check()` 호출 후 **재무장**.
- **`void DataHandler::do_keepalive_check()`**
  - 모든 세션 순회 -> `now - last_alive > keepalive_timeout_` 이면 임시 벡터에 수집 -> 루프 밖에서 `close_session()` 실행.
- **`void DataHandler::on_udp_receive(const std::string& data, const udp::endpoint& from, udp::socket& sock)`**
  - UDP 파이프라인: 파싱 -> 세션 조회 -> (udp_register 제외) 토큰/TTL 확인 -> **엔드포인트 일치 검사(불일치 시 `udp_error: ENDPOINT_MISMATCH`)** -> 레이트리밋 -> 핸들러 디스패치.
- **`void DataHandler::dispatch_udp_parsed(const nlohmann::json& j, ...)`**
  - `type`별로 `Udp*Handler` 호출.
- **`void DataHandler::broadcast_to_zone(int zone_id, std::string_view msg)` / `broadcast_all(...)`**
  - 존/전체 브로드캐스트. 세션 필터링/큐 크기 주의.

### MessageDispatcher
- **`void MessageDispatcher::dispatch_tcp(Session&, std::string_view json)`**
  - TCP JSON 메시지의 `type` 분기(`keepalive`, `chat`, `chat_zone`, `login` 등) -> 각 핸들러 호출.
- **`void MessageDispatcher::dispatch_udp(...)`**
  - UDP `type` 분기(`udp`, `broadcast_udp`, `broadcast_udp_zone`, `udp_register`).

### TCP Message Handlers
- **`KeepaliveHandler::handle(Session&, const json&)`**
  - keepalive 수신 시 `session.update_alive_time()` 호출. 응답은 선택(보통 무응답).
- **`ChatHandler::handle(Session&, const json&)`**
  - 일반 채팅. 필드 검증(`nickname`,`msg`) -> 전체/필터 대상 브로드캐스트.
- **`ChatZoneHandler::handle(Session&, const json&)`**
  - 존 채팅. `zone_id`로 대상 그룹 선택 후 전송.
- **`CertifyHandler::handle(Session&, const json&)` / UseCases::LoginFlow**
  - 로그인/닉네임 등록/인증 흐름. 성공 시 `Session`에 닉네임/상태 세팅.

### UDP Message Handlers
- **`UdpRegisterHandler::handle(Session&, const json&, const udp::endpoint& from, udp::socket&)`**
  - **엔드포인트 바인딩** + **랜덤 토큰 발급**(TTL= `udp_token_ttl_seconds`) -> `udp_register_ack` 응답.
- **`UdpHandler::handle(...)`**
  - 기본 UDP 에코/메시지 처리 샘플. 토큰/엔드포인트 검증 통과가 전제.
- **`UdpBroadcastHandler::handle(...)` / `UdpBroadcastZoneHandler::handle(...)`**
  - UDP 브로드캐스트(전체/존). 크기/빈도 레이트리밋 주의.

### UDPManager
- **`void UDPManager::start_receive()`**
  - `socket_.async_receive_from(...)`로 비동기 수신 루프. 수신 시 `DataHandler::on_udp_receive(...)` 호출 후 **재무장**.

### Zone / ZoneManager
- **`ZoneManager::add_session_to_zone(int zone_id, Session&)` / `remove_session_from_zone(...)`**
  - 세션의 존 가입/탈퇴 관리, 맵/컨테이너 갱신.
- **`Zone::broadcast(std::string_view msg)`**
  - 해당 존에 속한 세션들에게 메시지 전송.

### Logger / Utility / MemoryTracker
- **`Logger::init()`**
  - spdlog 파일 로거 초기화. 로그 레벨/패턴 구성.
- **`Utility::generate_random_token(size_t n)`**
  - 고엔트로피 랜덤 토큰 생성(UDP 토큰 등).
- **`Utility::rate_limiter_xxx(...)`**
  - 총량/유저별 레이트리밋 헬퍼.
- **`MemoryTracker::log_usage()`**
  - 주기적으로 메모리 사용량 기록.

### DB Middleware (옵션)
- **`DBMiddlewareClient::start()` / `stop()` / `send()`**
  - 외부 DBMW 서버와의 TCP 연결/하트비트/송수신.
- **`DBmwRouter::route(json/pb)`**
  - 들어온 메시지를 등록된 핸들러에게 전달.

---

### 주의 사례
- **타이머 루프**: `async_wait` 콜백에서 **반드시 재무장**(다시 `expires_after`+`async_wait`).
- **객체 수명**: 비동기 콜백에서 `this` 캡처 시 `shared_from_this()` 사용.
- **엔드포인트 정책**: UDP는 항상 **토큰+엔드포인트 이중 검증**. 불일치 시 `ENDPOINT_MISMATCH` -> 클라 `udp_register` 필요.
- **타임아웃 튜닝**: 클라 keepalive 20s 기준, 서버 keepalive_timeout_ ≥ 60s 권장.


### Session class (한 명의 클라 연결 단위)

- 역할

1. TCP 소켓 I/O 비동기 처리 (async_read_some, 길이프리픽스 write 등)

2. 메시지 누적/분리(MessageBufferManager)와 JSON 파싱 -> DataHandler/Dispatcher로 전달

3. 송신 직렬화 큐(Write Queue)와 세대(generation_) 체크로 stale 콜백 방지

4. 로그인/닉네임 상태, 로그인 타임아웃, keepalive 타임스탬프

5. UDP 관련 상태(토큰, TTL, 클라의 UDP endpoint, 레이트리밋 토큰버킷)

6. 생명주기 플래그(닫힘/활성/해제 등)와 cleanup(), close_session()

-  핵심 포인트

1. generation_: reset() 후 증가시켜 이전 비동기 콜백이 현재 세션을 건드리지 못하게 함.

2. post_write() -> 내부 큐에 넣고 do_write_queue()가 하나씩 전송(직렬화·흐름제어).

3. on_nickname_registered()에서 상태를 Ready로 바꾸고 로그인 타이머 취소.

4. UDP는 Session이 토큰/엔드포인트/TTL을 들고 있고, 실제 매핑 테이블은 DataHandler가 관리.

### SessionManager class (세션 저장소/인덱서/락 관리)

- 역할

1. add_session(session): 샤딩된 버킷(예: std::vector<std::unordered_map<int, shared_ptr<Session>>>)에 세션ID로 저장

2. 샤드별 mutex로 잠금 범위를 좁혀 성능↑

3. 같은 session_id가 이미 있으면 세대 비교 후 교체(이전 세대면 replaced_session를 잠금 밖에서 종료)

4. remove_session(session_id): 안전하게 제거하고 포인터 반환(필요 시 호출자가 후처리)

5. find_session(session_id), find_session_by_nickname(nick): 조회

5. 닉네임은 nickname_index_ (unordered_map<string, weak_ptr<Session>>)로 O(1) 인덱스 제공

6. for_each_session(fn): 락 짧게 -> 스냅샷 만들고 -> 락 해제 후 순회(브로드캐스트 I/O 병목 방지)

7. 보조 유지보수: cleanup_expired_nicknames()(만료 weak_ptr 청소), cleanup_inactive_sessions() 등

- 핵심 포인트

1. 동시성: “샤드 + 샤드별 mutex”로 락 경합 줄임. 순회는 스냅샷 후 락 해제가 원칙.

2. 대체 로직: add_session에서 같은 ID 충돌 시 generation으로 재사용/재연결을 구분.

3. 동일 generation이면 같은 객체 재사용 -> 교체/종료 안 함

4. 다른 generation이면 이전 것을 교체 대상으로 잡고 락 밖에서 close_session() 호출

5. 닉네임 인덱스: 로그인 완료 시 register_nickname(nick, session)로 색인. 로그아웃/종료 시 unregister.


### 둘의 상호작용(로그인 기준 흐름)

1. 서버가 새 TCP 연결 수락 -> Session 생성(또는 SessionPool.acquire) -> SessionManager.add_session

2. 클라가 {"type":"login","nickname":"..."} 전송

3. MessageDispatcher.login_handler -> (DB인증 성공 혹은 임시 우회 정책)

4. 중복 로그인 처리: find_session_by_nickname(nick)로 이전 세션 있으면 알림 후 종료

5. session -> set_nickname(nick), on_nickname_registered()

6. SessionManager.register_nickname(nick, session)

7. 존 배정 등 부가 처리 후 입장 notice 브로드캐스트

8. 이때 SessionManager.for_each_session은 스냅샷 후 락 해제 -> 각 세션에 post_write(I/O는 락 밖)

9. 이후 채팅/UDP 등은 각 Session의 상태/큐를 통해 송수신