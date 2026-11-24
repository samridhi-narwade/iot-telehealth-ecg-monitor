#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

const char* WIFI_SSID   = "";
const char* WIFI_PASS   = "";
const char* TS_API_KEY  = "";
const char* NTFY_TOPIC  = "";

#define BPM_LOW   50
#define BPM_HIGH  120
#define SEND_TEST_MSG_ON_BOOT true
#define PLOT_MODE false

#define ECG_PIN   36
#define LO_MINUS  26
#define LO_PLUS   4
#define GREEN_LED 23
#define RED_LED   19

Adafruit_SSD1306 display(128, 64, &Wire, -1);

int threshold = 2000;
int winMax = 0, winMin = 4095;
bool above = false;
int bpm = 0;
int raw = 0;
unsigned long lastBeat = 0, winStart = 0, lastDraw = 0;
unsigned long intervals[5];
int idx = 0, count = 0;

volatile int  sharedBpm = 0;
volatile bool sharedLeadsOff = true;

void resetBpm() {
  bpm = 0; count = 0; idx = 0; lastBeat = 0;
}

bool sendAlert(String text) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(8);
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, String("https://ntfy.sh/") + NTFY_TOPIC)) {
    Serial.println("ntfy begin failed");
    return false;
  }
  http.addHeader("Title", "ECG Monitor");
  http.addHeader("Priority", "high");
  int code = http.POST(text);
  Serial.print("ntfy response: ");
  Serial.println(code);
  http.end();
  return code == 200;
}

void sendThingSpeak(int value) {
  HTTPClient http;
  http.begin(String("http://api.thingspeak.com/update?api_key=") + TS_API_KEY + "&field1=" + value);
  int code = http.GET();
  Serial.print("ThingSpeak response: ");
  Serial.println(code);
  http.end();
}

void netTask(void* pv) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long lastTS = 0, lastAlert = 0, lastDbg = 0;
  bool testSent = false;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      continue;
    }

    unsigned long now = millis();
    int b = sharedBpm;

    if (SEND_TEST_MSG_ON_BOOT && !testSent) {
      sendAlert("ECG monitor is online");
      testSent = true;
    }

    if (b > 0 && (lastTS == 0 || now - lastTS > 20000)) {
      sendThingSpeak(b);
      lastTS = now;
    }

    // Alert when BPM is outside range, repeat every 60 s
    bool abnormal = (b > 0) && (b < BPM_LOW || b > BPM_HIGH);
    if (abnormal && (lastAlert == 0 || now - lastAlert > 60000)) {
      String msg = (b < BPM_LOW) ? "ALERT: LOW heart rate " : "ALERT: HIGH heart rate ";
      msg += String(b) + " BPM";
      if (sendAlert(msg)) lastAlert = now;
    }

    if (now - lastDbg > 2000) {
      lastDbg = now;
      Serial.printf("[net] bpm=%d abnormal=%d\n", b, abnormal);
    }

    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LO_PLUS, INPUT);
  pinMode(LO_MINUS, INPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  analogSetPinAttenuation(ECG_PIN, ADC_11db);

  digitalWrite(GREEN_LED, HIGH); delay(400); digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, HIGH);   delay(400); digitalWrite(RED_LED, LOW);

  Wire.begin(21, 22);
  Wire.setClock(400000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found (try address 0x3D)");
  }
  display.setTextColor(WHITE);
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(10, 20);
  display.println("IoT ECG MONITOR");
  display.setCursor(30, 40);
  display.println("Starting...");
  display.display();
  delay(1500);

  winStart = millis();

  xTaskCreatePinnedToCore(netTask, "net", 16384, NULL, 1, NULL, 0);
}

void loop() {
  // Type 't' in Serial Monitor to send a test message
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 't') {
      Serial.println("Manual test alert...");
      sendAlert("TEST: manual alert from ESP32");
    }
  }

  unsigned long now = millis();
  bool loP = digitalRead(LO_PLUS);
  bool loM = digitalRead(LO_MINUS);
  bool leadsOff = (loP || loM);

  raw = analogRead(ECG_PIN);
  if (PLOT_MODE) Serial.println(raw);

  if (!leadsOff) {
    if (raw > winMax) winMax = raw;
    if (raw < winMin) winMin = raw;
    if (now - winStart >= 2000) {
      if (winMax - winMin > 200)
        threshold = winMin + 0.65 * (winMax - winMin);
      winMax = 0; winMin = 4095;
      winStart = now;
    }

    if (raw > threshold && !above && now - lastBeat > 300) {
      above = true;
      if (lastBeat > 0) {
        unsigned long iv = now - lastBeat;
        if (iv <= 2000) {
          intervals[idx] = iv;
          idx = (idx + 1) % 5;
          if (count < 5) count++;
          unsigned long sum = 0;
          for (int i = 0; i < count; i++) sum += intervals[i];
          bpm = 60000UL * count / sum;
          if (!PLOT_MODE) { Serial.print("BPM: "); Serial.println(bpm); }
        }
      }
      lastBeat = now;
    }
    if (raw < threshold - 100) above = false;

    if (lastBeat > 0 && now - lastBeat > 3000) resetBpm();
  } else {
    resetBpm();
  }

  sharedBpm = bpm;
  sharedLeadsOff = leadsOff;

  // ---- LEDs: red only when BPM is abnormal ----
  bool normal   = (bpm >= BPM_LOW && bpm <= BPM_HIGH);
  bool abnormal = (bpm > 0 && !normal);
  digitalWrite(GREEN_LED, normal ? HIGH : LOW);
  digitalWrite(RED_LED, abnormal ? HIGH : LOW);

  // ---- OLED (every 500 ms) ----
  if (now - lastDraw > 500) {
    lastDraw = now;
    display.clearDisplay();

    display.setTextSize(1);
    display.setCursor(10, 0);
    display.print("IoT ECG MONITOR");
    display.drawLine(0, 10, 127, 10, WHITE);

    display.setTextSize(2);
    display.setCursor(0, 18);
    display.print("BPM: ");
    if (bpm > 0) display.print(bpm); else display.print("--");

    display.setTextSize(1);
    display.setCursor(0, 40);
    if (leadsOff)             display.print("Attach electrodes");
    else if (bpm == 0)        display.print("Measuring...");
    else if (bpm < BPM_LOW)   display.print("LOW");
    else if (bpm > BPM_HIGH)  display.print("HIGH");
    else                      display.print("NORMAL");

    display.setCursor(0, 54);
    display.print(WiFi.status() == WL_CONNECTED ? "WiFi OK" : "WiFi...");

    display.display();
  }

  delay(4);
}