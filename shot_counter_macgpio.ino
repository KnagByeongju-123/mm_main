/* ============================================================
   ESP32 SHOT 카운터 펌웨어 (MAC+GPIO 방식, 생성기 출력)
   ============================================================
   ★ ESP32 #1 (SET1) - DI1~DI4 원시 신호 모드 ★
   
   변경점 (v3.3.1 → v4.2):
     - machine_code = {MAC}_{GPIO} (의미부여는 HTML 매핑에서)
     - iptime SSID 비번 "" (오픈 네트워크, 비번 없음)
     - 고정 IP 제거 → DHCP 자동 할당 (서브넷 자동 매칭)
     - 부팅 시 Supabase 연결 테스트 추가
     - 5초마다 DI 진단 출력 추가
   
   베이스: v3.3.1 (FastEdge + Queue) - 검증된 안정 버전
   ============================================================ */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "esp_mac.h"

// ════════════════════════════════════════════════════════════
//  ▼▼▼▼▼ ESP32 #1 (SET1 - DI1~DI4 원시) 전용 설정 ▼▼▼▼▼
// ════════════════════════════════════════════════════════════

// ════════════════════════════════════════════════════════════
//  ▼▼▼▼▼ WiFi 멀티 SSID 설정 ▼▼▼▼▼
// ════════════════════════════════════════════════════════════

WiFiMulti wifiMulti;

// ════════════════════════════════════════════════════════════
//  ▼▼▼▼▼ 공통 설정 ▼▼▼▼▼
// ════════════════════════════════════════════════════════════

const char* SUPABASE_URL = "https://xrmwfsuitikqibwqxede.supabase.co/rest/v1/shot_count";
const char* SUPABASE_KEY = "sb_publishable_0mgJ2cQeYpIDcfJi_9p4pA_dlaknATN";

const int DI_PINS[1] = { 26 };  // 26,27,32,33 중 사용 핀

const unsigned long MIN_INTERVAL_MS  = 30;
const unsigned long WIFI_CHECK_MS    = 10000;
const unsigned long WIFI_REPORT_MS   = 60000;
const unsigned long DIAG_PERIOD_MS   = 5000;

#define QUEUE_SIZE 50

// ════════════════════════════════════════════════════════════
//  큐 데이터 (병렬 배열)
// ════════════════════════════════════════════════════════════
int           q_diIdx[QUEUE_SIZE];
unsigned long q_count[QUEUE_SIZE];
unsigned long q_interval[QUEUE_SIZE];
int q_head = 0;
int q_tail = 0;
int q_dropped = 0;

// ════════════════════════════════════════════════════════════
//  내부 상태
// ════════════════════════════════════════════════════════════
byte mac_addr[6];
char mac_str[18];
char ip_str[20];
String currentSSID = "";
String macNoColon = "";   // MAC(콜론제거) → machine_code 생성용

unsigned long shot_count[10]   = {0};
unsigned long last_shot_ms[10] = {0};
int last_state[10];

unsigned long last_wifi_check  = 0;
unsigned long last_wifi_report = 0;
unsigned long last_diag        = 0;

// ════════════════════════════════════════════════════════════
//  함수 프로토타입
// ════════════════════════════════════════════════════════════
void registerWiFiAPs();
void updateIPInfo();
void setupWiFi();
void checkWiFi();
void testSupabase();
void printDiag();
bool enqueueShot(int diIdx, unsigned long cnt, unsigned long interval);
bool processOneQueueItem();
int queueSize();
void checkDI(int diIdx);

// ════════════════════════════════════════════════════════════
//  WiFi
// ════════════════════════════════════════════════════════════
void registerWiFiAPs(){
  wifiMulti.addAP("KY", "KY");
  wifiMulti.addAP("iptime", "");
}

void updateIPInfo(){
  IPAddress ip = WiFi.localIP();
  snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  currentSSID = WiFi.SSID();
}

// machine_code = {MAC(콜론제거)}_{GPIO}  (의미부여는 HTML에서)
String codeFor(int i){ return macNoColon + "_" + String(DI_PINS[i]); }

void setupWiFi(){
  esp_read_mac(mac_addr, ESP_MAC_WIFI_STA);
  snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac_addr[0],mac_addr[1],mac_addr[2],mac_addr[3],mac_addr[4],mac_addr[5]);
  Serial.print("WiFi MAC: "); Serial.println(mac_str);
  macNoColon = String(mac_str); macNoColon.replace(":", "");

  WiFi.mode(WIFI_STA);
  // ⭐ WiFi.config() 호출 안 함 → DHCP 자동 할당 (서브넷 자동 매칭)
  registerWiFiAPs();
  
  Serial.println("📡 WiFi 검색 중 (TAEJIN + iptime)...");
  
  unsigned long t0 = millis();
  while (wifiMulti.run() != WL_CONNECTED && (millis() - t0) < 30000) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    updateIPInfo();
    Serial.print("✅ WiFi 연결!  SSID: "); Serial.println(currentSSID);
    Serial.print("   IP   : "); Serial.println(ip_str);
    Serial.print("   GW   : "); Serial.println(WiFi.gatewayIP());
    Serial.print("   RSSI : "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");
  } else {
    Serial.println("❌ WiFi 연결 실패");
    strcpy(ip_str, "0.0.0.0");
    currentSSID = "(연결안됨)";
  }
}

void checkWiFi(){
  unsigned long now = millis();
  if (now - last_wifi_check < WIFI_CHECK_MS) return;
  last_wifi_check = now;
  
  wifiMulti.run();
  
  if (WiFi.status() != WL_CONNECTED) {
    strcpy(ip_str, "0.0.0.0");
    currentSSID = "(재연결중)";
    return;
  }
  
  String newSSID = WiFi.SSID();
  if (newSSID != currentSSID) {
    updateIPInfo();
    Serial.print("🔄 WiFi 전환! SSID: "); Serial.print(currentSSID);
    Serial.print(" IP: "); Serial.print(ip_str);
    Serial.print(" RSSI: "); Serial.println(WiFi.RSSI());
  }
  
  if (now - last_wifi_report >= WIFI_REPORT_MS) {
    last_wifi_report = now;
    Serial.print("📶 ["); Serial.print(currentSSID);
    Serial.print("] IP="); Serial.print(ip_str);
    Serial.print(" RSSI="); Serial.print(WiFi.RSSI()); Serial.print("dBm");
    Serial.print(" Q="); Serial.print(queueSize());
    Serial.print("/"); Serial.print(QUEUE_SIZE);
    if (q_dropped > 0) {
      Serial.print(" ⚠️ 누락="); Serial.print(q_dropped);
    }
    Serial.println();
  }
}

// ════════════════════════════════════════════════════════════
//  Supabase 연결 테스트 (부팅 시)
// ════════════════════════════════════════════════════════════
void testSupabase(){
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[TEST] WiFi 미연결 → 건너뜀");
    return;
  }
  Serial.println("[TEST] Supabase 연결 테스트 중...");
  
  WiFiClientSecure client;
  client.setInsecure();
  
  HTTPClient https;
  String testUrl = String(SUPABASE_URL) + "?limit=1";
  if (!https.begin(client, testUrl)) {
    Serial.println("[TEST] ❌ HTTPS begin 실패");
    return;
  }
  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  https.setTimeout(10000);

  int code = https.GET();
  Serial.printf("[TEST] HTTP=%d ", code);
  if (code >= 200 && code < 300) {
    Serial.println("✅ Supabase 접속 OK");
  } else if (code == -1) {
    Serial.println("❌ 인터넷 안됨 (현재 AP에서 외부망 불가)");
  } else if (code == 401 || code == 403) {
    Serial.println("❌ 인증 실패");
  } else if (code == 404) {
    Serial.println("❌ 테이블 없음");
  } else {
    Serial.printf("⚠️ %s\n", https.getString().c_str());
  }
  https.end();
}

// ════════════════════════════════════════════════════════════
//  진단 출력 (5초 주기)
// ════════════════════════════════════════════════════════════
void printDiag(){
  Serial.printf("[DIAG] WiFi=%s IP=%s | DI: ",
    WiFi.status()==WL_CONNECTED ? currentSSID.c_str() : "OFF",
    ip_str);
  for (int i = 0; i < 1; i++) {
    int lv = digitalRead(DI_PINS[i]);
    Serial.printf("[GPIO%d]%s=%d ", DI_PINS[i], codeFor(i).c_str(), lv);
  }
  Serial.printf("| cnt=[%lu,%lu,%lu,%lu]\n",
    shot_count[0], shot_count[1], shot_count[2], shot_count[3]);
}

// ════════════════════════════════════════════════════════════
//  큐 함수
// ════════════════════════════════════════════════════════════
int queueSize(){
  return (q_head - q_tail + QUEUE_SIZE) % QUEUE_SIZE;
}

bool enqueueShot(int diIdx, unsigned long cnt, unsigned long interval){
  int next = (q_head + 1) % QUEUE_SIZE;
  if (next == q_tail) {
    q_dropped++;
    return false;
  }
  q_diIdx[q_head]    = diIdx;
  q_count[q_head]    = cnt;
  q_interval[q_head] = interval;
  q_head = next;
  return true;
}

bool processOneQueueItem(){
  if (q_head == q_tail) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  
  int diIdx = q_diIdx[q_tail];
  unsigned long cnt = q_count[q_tail];
  unsigned long interval = q_interval[q_tail];
  int saved_tail = q_tail;
  q_tail = (q_tail + 1) % QUEUE_SIZE;
  
  updateIPInfo();
  
  StaticJsonDocument<256> doc;
  doc["machine_code"]     = codeFor(diIdx);
  doc["shot_count"]       = cnt;
  doc["shot_interval_ms"] = interval;
  doc["esp32_mac"]        = mac_str;
  doc["esp32_ip"]         = ip_str;

  String body;
  serializeJson(doc, body);

  WiFiClientSecure client;
  client.setInsecure();
  
  HTTPClient https;
  if (!https.begin(client, SUPABASE_URL)) {
    Serial.println("❌ HTTPS begin 실패 (큐 되돌림)");
    q_tail = saved_tail;
    return false;
  }
  
  https.addHeader("Content-Type", "application/json");
  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  https.addHeader("Prefer", "return=minimal");
  
  int code = https.POST(body);
  https.end();
  
  bool ok = (code >= 200 && code < 300);
  Serial.print("POST "); Serial.print(codeFor(diIdx));
  Serial.print(" #");    Serial.print(cnt);
  Serial.print(" Q=");   Serial.print(queueSize());
  Serial.print(" → ");   Serial.println(code);
  
  return ok;
}

// ════════════════════════════════════════════════════════════
//  엣지 검출 SHOT 카운트
// ════════════════════════════════════════════════════════════
void checkDI(int diIdx){
  int raw = digitalRead(DI_PINS[diIdx]);
  unsigned long now = millis();
  
  // HIGH → LOW 엣지 (광센서 D.ON, PNP, 풀업 회로 기준)
  if (last_state[diIdx] == HIGH && raw == LOW) {
    if (now - last_shot_ms[diIdx] >= MIN_INTERVAL_MS) {
      shot_count[diIdx]++;
      unsigned long interval = (last_shot_ms[diIdx] == 0) ? 0 : (now - last_shot_ms[diIdx]);
      last_shot_ms[diIdx] = now;
      
      bool queued = enqueueShot(diIdx, shot_count[diIdx], interval);
      
      Serial.print("🎯 "); Serial.print(codeFor(diIdx));
      Serial.print(" #"); Serial.print(shot_count[diIdx]);
      if (!queued) Serial.print(" ⚠️ 큐 가득참!");
      Serial.println();
    }
  }
  
  last_state[diIdx] = raw;
}

// ════════════════════════════════════════════════════════════
//  setup / loop
// ════════════════════════════════════════════════════════════
void setup(){
  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("======================================");
  Serial.print  ("ESP32 SHOT Counter v4.2 (DI 원시)  [");
  Serial.print  (mac_str); Serial.println("]");
  Serial.println("======================================");
  Serial.print("MIN_INTERVAL: "); Serial.print(MIN_INTERVAL_MS);
  Serial.print("ms (최대 "); Serial.print(1000/MIN_INTERVAL_MS); 
  Serial.println(" SHOT/초)");

  for (int i = 0; i < 1; i++) {
    pinMode(DI_PINS[i], INPUT_PULLUP);
    last_state[i] = HIGH;
    Serial.print("CH"); Serial.print(i+1);
    Serial.print(" GPIO"); Serial.print(DI_PINS[i]);
    Serial.print(" → ");   Serial.println(codeFor(i));
  }

  setupWiFi();
  testSupabase();
  Serial.println("Ready. Waiting for SHOTs...\n");
  last_diag = millis();
}

void loop(){
  checkWiFi();
  
  // DI 신호 체크 (빠르게)
  for (int i = 0; i < 1; i++) {
    checkDI(i);
  }
  
  // 큐에서 1건씩 POST (50ms마다)
  static unsigned long last_post_attempt = 0;
  if (millis() - last_post_attempt >= 50) {
    last_post_attempt = millis();
    processOneQueueItem();
  }
  
  // 5초마다 DI/WiFi 진단 출력
  if (millis() - last_diag >= DIAG_PERIOD_MS) {
    last_diag = millis();
    printDiag();
  }
}
