/***************** ESP32-C3 Super Mini - Time-Scheduled Pomodoro + Google Sheets *****************
 * Features:
 *  - Sheet (start/end) থেকে schedule পড়ে সময় হলে task auto-start
 *  - শুরুতে: GREEN flash + (beep×2 + vib×2)
 *  - শেষে:  RED   flash + (beep×2 + vib×2) + sheet status=done
 *  - মাঝে Clock + "Next … in mm:ss" কাউন্টডাউন
 *  - BTN1: Stop/Done, BTN2: Pause/Resume
 *************************************************************************************************/

#include <WiFi.h>                // WiFi connect
#include <WiFiClientSecure.h>    // HTTPS client
#include <HTTPClient.h>          // HTTP GET/POST
#include <ArduinoJson.h>         // JSON parse
#include <Wire.h>                // I2C
#include <Adafruit_GFX.h>        // OLED gfx
#include <Adafruit_SSD1306.h>    // OLED driver
#include <time.h>                // NTP + localtime

/* ---------------- OLED / I2C ---------------- */
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
#define SDA_PIN 8                 // ESP32-C3 Super Mini SDA
#define SCL_PIN 9                 // ESP32-C3 Super Mini SCL
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

/* ---------------- GPIO pins ----------------- */
#define LED_RED     4             // End cue (flash only at END)
#define LED_YELLOW  0             // (unused now; keep free)
#define LED_BLUE    6             // (unused now)
#define LED_GREEN   7             // Start cue (flash only at START)
#define BUZZER_PIN  10            // Passive buzzer (software square-wave)
#define VIB_PIN     5             // Vibration motor (prefer transistor driver)
#define BTN_STOP    2             // BTN1: Stop/Done
#define BTN_PAUSE   3             // BTN2: Pause/Resume

/* ---------------- WiFi & Script URLs -------- */
const char* WIFI_SSID = "B6";          // << তোমার WiFi SSID
const char* WIFI_PASS = "94778100";    // << তোমার WiFi Password

// GET: googleusercontent echo URL (যেটা JSON দেখায়)
const char* API_URL_GET =
"https://script.googleusercontent.com/macros/echo?user_content_key=AehSKLiiFz3B0B33Qu1NfnWajQ_Dk58486lvOJUnsns9fTWEGnWHI81HhFktovxTa53WmSUQQFUskqdG9I0x3vUus2pZYr1ENYmGSv2FEXc__8sY_2WTYkaZOMMyKxFZBIgfeK9cO1HAek4noTc9E04OUmUBjeQxnlh20QSGLxfFYyVY1kcfvD2fn1Jo5Ztm7Zlll0KjjzwR9jOoN81SKjJqlVeGdjBTsgNFJ9GqdQCDfau1bNe1bK-fAWiwvo3bw-kcaddaUukY_ofWXwnfJoVNWgxk6WneVhf_nDI_2_s73zReBXIg1ZD_8FrRtheyVQ&lib=M3jNLyLL_37SxGTOQShzhG9J2pwj9FzdV";

// POST: Apps Script Web App exec URL (status=done পাঠাতে)
const char* API_URL_POST =
"https://script.google.com/macros/s/AKfycbyn6SQUPbBhYeToTiNasEt-m9TJdbvPd43d4f3lHA7AGYb_EW0goDAd4t77Q46u5Sk8Dg/exec";

const char* TOKEN = "us12345678";          // Apps Script token
const bool  USE_GET_TOKEN = false;         // echo URL-এ token লাগলে true করো

/* ---------------- Pomodoro constants -------- */
#define DEFAULT_FOCUS_SECONDS (25*60)      // Sheet-এ না থাকলে fallback
#define SPLASH_MS             3000         // Splash screen সময়
#define REFRESH_MS            200          // OLED refresh rate
#define BUZZ_FREQ_HZ          2200         // Passive buzzer frequency

/* ---------------- Timezone/NTP -------------- */
const long   gmtOffset_sec      = 6 * 3600; // BD UTC+6
const int    daylightOffset_sec = 0;
const char*  ntpServer1 = "pool.ntp.org";
const char*  ntpServer2 = "time.nist.gov";
const char*  DAY_NAME[7] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};

/* ---------------- App state ----------------- */
enum RunState { SPLASH, RUNNING, PAUSED, IDLE };
RunState state = SPLASH;

unsigned long sessionStartMs = 0;          // session শুরু হওয়ার millis()
unsigned long pausedAccumMs  = 0;          // pause সহ elapsed যোগফল
unsigned long lastRefreshMs  = 0;          // OLED redraw throttling

uint32_t focusLimitMs    = (uint32_t)DEFAULT_FOCUS_SECONDS * 1000UL; // session duration ms
uint32_t sessionsDone    = 0;              // মোট completed sessions
uint32_t focusTimeTotalMs= 0;              // মোট focus সময়

/* ---------------- Sheet task model ---------- */
struct Task { String id, title, start, end, status, notes; };
const int MAX_TASKS = 20;
Task tasks[MAX_TASKS];                     // tasks buffer
int  taskCount = 0, currentTask = 0;       // মোট টাস্ক/কারেন্ট ইন্ডেক্স

// Scheduling arrays (seconds-of-day)
int32_t  taskStartS[MAX_TASKS];            // start time in seconds (midnight থেকে)
uint32_t taskDurS[MAX_TASKS];              // duration (seconds)
int      nextUpcomingIdx = -1;             // idle view-এর জন্য

/* ---------------- Net client ---------------- */
WiFiClientSecure secureClient;             // HTTPS client

/* ---------------- Small helpers ------------- */

// সব LED অফ করে দেওয়া
void ledsOff(){ 
  digitalWrite(LED_RED,LOW); 
  digitalWrite(LED_YELLOW,LOW); 
  digitalWrite(LED_BLUE,LOW); 
  digitalWrite(LED_GREEN,LOW); 
}

// OLED-এ কেন্দ্রে টেক্সট আঁকা
void drawCenteredText(int16_t y, const String& txt, uint8_t size){
  int16_t x1,y1; uint16_t w,h;
  display.setTextSize(size);
  display.getTextBounds(txt, 0, y, &x1, &y1, &w, &h);
  int16_t x = (SCREEN_WIDTH - w)/2;
  display.setCursor(x, y);
  display.println(txt);
}

// Splash দেখানো
void showSplash(const String& subtitle="Pomodoro Ready"){
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(8,  "Time Management", 1);
  drawCenteredText(22, "System", 2);
  drawCenteredText(48, subtitle, 1);
  display.display();
}

/*** Passive buzzer: সফটওয়্যার স্কয়ার-ওয়েভ ***/
void buzzTone(unsigned freq, unsigned ms){
  if (freq < 50) {                      // খুব কম হলে DC ক্লিক
    digitalWrite(BUZZER_PIN, HIGH); 
    delay(ms); 
    digitalWrite(BUZZER_PIN, LOW); 
    return;
  }
  unsigned long period_us = 1000000UL / freq; // এক সাইকেলের সময়
  unsigned long half = period_us / 2;         // হাফ-সাইকেল
  unsigned long cycles = (ms * 1000UL) / period_us; // মোট সাইকেল
  for (unsigned long i=0; i<cycles; i++){
    digitalWrite(BUZZER_PIN, HIGH);  delayMicroseconds(half);
    digitalWrite(BUZZER_PIN, LOW);   delayMicroseconds(half);
  }
}
void shortBuzz(int ms=120){ buzzTone(BUZZ_FREQ_HZ, (unsigned)ms); }
void beepDouble(){ shortBuzz(120); delay(80); shortBuzz(120); } // ডাবল বীপ

// Vibration utility
void vibOn(){  digitalWrite(VIB_PIN, HIGH); }
void vibOff(){ digitalWrite(VIB_PIN, LOW);  }
void vibPulse(int ms=200){ vibOn(); delay(ms); vibOff(); }
void vibDouble(){ vibPulse(200); delay(80); vibPulse(200); }     // ডাবল ভাইব

// "HH:MM" থেকে ঘন্টা:মিনিট বের করা
bool extractHHMM(const String& s, int& hh, int& mm){
  int p = s.indexOf(':');
  if (p < 1) return false;
  if (p >= 2 && p+2 < (int)s.length()){
    String h = s.substring(p-2, p);
    String m = s.substring(p+1, p+3);
    if (!isDigit(h[0])||!isDigit(h[1])||!isDigit(m[0])||!isDigit(m[1])) return false;
    hh = h.toInt(); mm = m.toInt();
    return (hh>=0 && hh<24 && mm>=0 && mm<60);
  }
  return false;
}

// start/end থেকে duration (seconds) হিসাব করা
uint32_t computeDurationFromTask(const Task& t){
  int sh, sm, eh, em;
  if (extractHHMM(t.start, sh, sm) && extractHHMM(t.end, eh, em)){
    int startM = sh*60 + sm;
    int endM   = eh*60 + em;
    int diff   = endM - startM;
    if (diff <= 0) diff += 24*60; // রাত পার হলে
    return (uint32_t)diff * 60UL;
  }
  return DEFAULT_FOCUS_SECONDS;
}

// আজকের দিনের 00:00:00 থেকে কত সেকেন্ড গেছে
int secondsSinceMidnight(){
  time_t now = time(nullptr);
  tm tminfo; localtime_r(&now, &tminfo);
  return tminfo.tm_hour*3600 + tminfo.tm_min*60 + tminfo.tm_sec;
}

/* ---------------- Idle clock + next task -------- */
// Clock + “Next … in mm:ss” ব্যানার দেখানো
void renderClockIdle(){
  time_t now = time(nullptr);
  tm tminfo; localtime_r(&now, &tminfo);

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  drawCenteredText(0, DAY_NAME[tminfo.tm_wday], 1);

  char dateBuf[16];
  sprintf(dateBuf,"%02d/%02d/%04d", tminfo.tm_mday, tminfo.tm_mon+1, tminfo.tm_year+1900);
  drawCenteredText(16, String(dateBuf), 2);

  char timeBuf[16];
  sprintf(timeBuf,"%02d:%02d:%02d", tminfo.tm_hour, tminfo.tm_min, tminfo.tm_sec);
  drawCenteredText(36, String(timeBuf), 3);

  // নিচে: Next task কাউন্টডাউন
  display.setTextSize(1);
  display.setCursor(0,56);
  if (nextUpcomingIdx >= 0){
    int nowS = secondsSinceMidnight();
    int startS = taskStartS[nextUpcomingIdx];
    int diff = max(0, startS - nowS);
    int m = diff/60, s = diff%60;
    String title = tasks[nextUpcomingIdx].title;
    if (title.length()>10) title = title.substring(0,10)+"...";
    char buf[64];
    sprintf(buf, "Next: %s %02d:%02d in %02d:%02d",
            title.c_str(), startS/3600, (startS/60)%60, m, s);
    display.print(buf);
  } else {
    display.print("No upcoming task");
  }
  display.display();
}

/* ---------------- Task screen render ------------ */
// স্ট্যাটাস টেক্সট (FOCUS/PAUSED) সহ কাউন্টডাউন দেখানো
void renderTask(const String& titleLine, const char* statusText){
  unsigned long now = millis();
  if (now - lastRefreshMs < REFRESH_MS) return; // খুব ঘনঘন না আঁকা
  lastRefreshMs = now;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // হেডার: sessions + title
  display.setTextSize(1);
  display.setCursor(0,0);
  display.print("Sessions: "); display.print(sessionsDone);

  if (titleLine.length()){
    display.setCursor(70,0);
    String tt = titleLine; if (tt.length()>9) tt = tt.substring(0,9) + "...";
    display.print(tt);
  }

  // অবশিষ্ট সময় হিসাব
  uint32_t remainS = 0;
  if (state == RUNNING || state == PAUSED){
    uint32_t elapsed = (state==RUNNING ? (millis()-sessionStartMs)+pausedAccumMs : pausedAccumMs);
    remainS = (elapsed >= focusLimitMs) ? 0 : (focusLimitMs - elapsed)/1000UL;
  }
  char buf[8]; sprintf(buf,"%02lu:%02lu",(unsigned long)(remainS/60),(unsigned long)(remainS%60));
  drawCenteredText(18, String(buf), 3);     // বড় টাইম
  drawCenteredText(48, statusText, 1);      // স্ট্যাটাস

  // নিচে বোতাম হিন্ট
  display.setTextSize(1);
  display.setCursor(0,56);
  display.print("BTN1:Stop/Done  BTN2:Pause/Resume");
  display.display();
}

/* ---------------- Networking (GET/POST) -------- */
// token দরকার হলে echo URL-এ এটাচ করা
String getUrlForGet(){
  if (!USE_GET_TOKEN) return String(API_URL_GET);
  return String(API_URL_GET) + (String(API_URL_GET).indexOf('?')>=0 ? "&" : "?") + "token=" + TOKEN;
}

// Sheet থেকে tasks লোড করা + schedule arrays পূরণ
bool fetchTasks(){
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  secureClient.setInsecure();
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(10000);

  // প্রধান GET (echo)
  String url = getUrlForGet();
  if (!http.begin(secureClient, url)) return false;
  int code = http.GET();
  String payload = http.getString();
  http.end();

  // ব্যাকআপ: exec?token=...
  if (code != 200){
    HTTPClient http2;
    String fb = String(API_URL_POST) + "?token=" + TOKEN;
    if (!http2.begin(secureClient, fb)) return false;
    http2.setTimeout(10000);
    int c2 = http2.GET();
    payload = http2.getString();
    http2.end();
    if (c2 != 200) return false;
  }

  // JSON parse
  StaticJsonDocument<16384> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) return false;

  // tasks অ্যারে ফিল
  JsonArray arr = doc["tasks"].as<JsonArray>();
  taskCount = 0;
  for (JsonObject t : arr){
    if (taskCount >= MAX_TASKS) break;
    tasks[taskCount].id     = (const char*) t["id"];
    tasks[taskCount].title  = (const char*) t["title"];
    tasks[taskCount].start  = (const char*) t["start"];
    tasks[taskCount].end    = (const char*) t["end"];
    tasks[taskCount].status = (const char*) t["status"];
    tasks[taskCount].notes  = (const char*) t["notes"];

    int hh, mm;
    if (extractHHMM(tasks[taskCount].start, hh, mm)) taskStartS[taskCount] = hh*3600 + mm*60;
    else taskStartS[taskCount] = -1;

    taskDurS[taskCount] = computeDurationFromTask(tasks[taskCount]);
    taskCount++;
  }
  if (currentTask >= taskCount) currentTask = 0;
  return true;
}

// status=done আপডেট করা (POST)
bool postStatusDone(const String& id){
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  secureClient.setInsecure();
  if (!http.begin(secureClient, API_URL_POST)) return false;
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> body;
  body["token"]  = TOKEN;
  body["id"]     = id;
  body["status"] = "done";
  String out; serializeJson(body, out);

  int code = http.POST(out);
  http.end();
  return (code == 200);
}

/* ---------------- Scheduling logic ------------- */
// nowS পর্যন্ত যে টাস্ক due (status!=done, start<=now), সবচেয়ে আগেরটা
int findDueTaskByClock(int nowS){
  int idx = -1, bestStart = 1e9;
  for (int i=0;i<taskCount;i++){
    if (tasks[i].status == "done") continue;
    if (taskStartS[i] < 0)        continue;
    if (taskStartS[i] <= nowS && taskStartS[i] < bestStart){
      bestStart = taskStartS[i]; idx = i;
    }
  }
  return idx;
}
// next upcoming (start>now) সবচেয়ে কাছে যেটা
int findNextUpcoming(int nowS){
  int idx = -1, bestStart = 1e9;
  for (int i=0;i<taskCount;i++){
    if (tasks[i].status == "done") continue;
    if (taskStartS[i] < 0)        continue;
    if (taskStartS[i] > nowS && taskStartS[i] < bestStart){
      bestStart = taskStartS[i]; idx = i;
    }
  }
  return idx;
}

/* ---------------- Session control -------------- */
// সেশন শুরু: শুধু START cue (GREEN flash + double beep + double vib), তারপর LED OFF
void startSession(){
  ledsOff();                              // আগে সব নিভাও
  digitalWrite(LED_GREEN, HIGH);          // GREEN flash (start cue)
  beepDouble();                           // beep ×2
  vibDouble();                            // vib ×2
  delay(250);
  digitalWrite(LED_GREEN, LOW);           // এবং নিভিয়ে দাও

  pausedAccumMs  = 0;                     // pause accumulator reset
  sessionStartMs = millis();              // এখন থেকে গণনা
  state = RUNNING;                        // state update
}

// সেশন শেষ: শুধু END cue (RED flash + double beep + double vib) + sheet=done
void endSessionAndMark(){
  uint32_t elapsed = (millis() - sessionStartMs) + pausedAccumMs;      // মোট সময়
  focusTimeTotalMs += min<uint32_t>(elapsed, focusLimitMs);            // স্ট্যাটস
  sessionsDone++;                                                      // কাউন্টার

  ledsOff();                               // সব নিভাও
  digitalWrite(LED_RED, HIGH);             // RED flash (end cue)
  beepDouble();                            // beep ×2
  vibDouble();                             // vib ×2
  delay(250);
  digitalWrite(LED_RED, LOW);              // এবং নিভিয়ে দাও

  if (taskCount>0 && tasks[currentTask].id.length())                    // Sheet update
    postStatusDone(tasks[currentTask].id);
}

// Clock দেখে due হলে শুরু করো, নাহলে idle+countdown
void maybeStartByClock(){
  int nowS = secondsSinceMidnight();                 // এখন কয় সেকেন্ড
  int due  = findDueTaskByClock(nowS);               // due task আছে?
  if (due != -1){
    currentTask  = due;                              // এই টাস্ক
    focusLimitMs = taskDurS[currentTask] * 1000UL;   // তার duration
    showSplash(tasks[currentTask].title);            // splash শো
    delay(SPLASH_MS);
    startSession();                                  // session শুরু
  } else {
    nextUpcomingIdx = findNextUpcoming(nowS);        // idle view-এর জন্য
    state = IDLE;                                    // clock মোড
  }
}

/* ---------------- Buttons (debounce) ------------- */
bool wasBtn1 = true, wasBtn2 = true;                  // pull-up → idle=HIGH
unsigned long lastDebounce = 0;

// Pause/Resume: LED change করা হচ্ছে না (চাহিদা অনুযায়ী)
// শুধু state/accumulator আপডেট
void togglePause(){
  if (state == RUNNING){
    state = PAUSED;
    pausedAccumMs = (millis() - sessionStartMs) + pausedAccumMs;
  } else if (state == PAUSED){
    state = RUNNING;
    sessionStartMs = millis();
  }
}

// BTN হ্যান্ডলার
void handleButtons(){
  unsigned long now = millis();
  if (now - lastDebounce < 30) return;     // simple debounce
  lastDebounce = now;

  bool b1 = digitalRead(BTN_STOP);         // HIGH=not pressed, LOW=pressed
  bool b2 = digitalRead(BTN_PAUSE);

  // BTN1: Stop/Done → End cue + mark done + reschedule
  if (wasBtn1 && !b1){
    if (state == RUNNING || state == PAUSED){
      endSessionAndMark();
      maybeStartByClock();                 // সময় হলে next শুরু, নইলে idle
    }
  }
  // BTN2: Pause/Resume
  if (wasBtn2 && !b2){
    if (state == RUNNING || state == PAUSED){
      togglePause();
    }
  }
  wasBtn1 = b1; wasBtn2 = b2;
}

/* ---------------- Setup -------------------------- */
void setup(){
  Serial.begin(115200);
  delay(400);

  pinMode(LED_RED, OUTPUT); pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_BLUE, OUTPUT); pinMode(LED_GREEN, OUTPUT);
  ledsOff();

  pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);
  pinMode(VIB_PIN, OUTPUT);    digitalWrite(VIB_PIN, LOW);

  pinMode(BTN_STOP, INPUT_PULLUP);
  pinMode(BTN_PAUSE, INPUT_PULLUP);

  Wire.begin(SDA_PIN, SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)){         // OLED init
    while(true){ digitalWrite(LED_RED, !digitalRead(LED_RED)); delay(250); }
  }

  // Power-on self-test (শুধু একবার)
  showSplash("Self-test...");
  beepDouble(); vibDouble();
  delay(300);

  // WiFi + NTP
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint8_t tries=0; while (WiFi.status()!=WL_CONNECTED && tries<80){ delay(150); tries++; }
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);

  // Tasks load
  fetchTasks();                 // error হলেও চলবে → idle clock দেখাবে
  maybeStartByClock();          // due হলে start, না হলে idle + countdown
}

/* ---------------- Main loop ---------------------- */
void loop(){
  if (state == RUNNING){
    // সময় শেষ হলে end + পরের শিডিউল অনুযায়ী শুরু/idle
    uint32_t elapsed = (millis() - sessionStartMs) + pausedAccumMs;
    if (elapsed >= focusLimitMs){
      endSessionAndMark();
      maybeStartByClock();
    }
    // Screen: running view
    renderTask(tasks[currentTask].title, "FOCUS");
  }
  else if (state == PAUSED){
    // Screen: paused view
    renderTask(tasks[currentTask].title, "PAUSED");
  }
  else if (state == IDLE){
    // প্রতি ১ সেকেন্ডে চেক: next due task এসে গেছে কি?
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > 1000){
      lastCheck = millis();
      int due = findDueTaskByClock(secondsSinceMidnight());
      if (due != -1){
        currentTask  = due;
        focusLimitMs = taskDurS[currentTask] * 1000UL;
        showSplash(tasks[currentTask].title);
        delay(SPLASH_MS);
        startSession();
      } else {
        nextUpcomingIdx = findNextUpcoming(secondsSinceMidnight());
      }
    }
    // Idle clock render + countdown
    renderClockIdle();
  }

  // Buttons সব state-এ কাজ করবে
  handleButtons();
}
