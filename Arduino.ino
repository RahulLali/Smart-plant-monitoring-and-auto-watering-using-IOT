/*
  Smart Plant - Corrected Final Sketch
  - Fixes manual override race (serial processed early)
  - Manual override sticky with optional timeout
  - LCD float formatting fix (dtostrf)
  - DHT caching, robust reads, JSON reporting
  - Auto-watering with hysteresis & safety timers
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <string.h>

// ---------------- CONFIG ----------------
#define MQ_PIN      A0
#define SOIL_PIN    A1
#define DHT_PIN     2
#define DHTTYPE     DHT11      // change to DHT22 if you use DHT22
#define RELAY_PIN   8

#define LCD_ADDR    0x27       // change to 0x3F if needed
#define LCD_COLS    16
#define LCD_ROWS    2

// If your relay module is active LOW (LOW == ON), set true.
#define RELAY_ACTIVE_LOW true

// Soil moisture thresholds (percent)
const int SOIL_THRESHOLD_LOW  = 35;   // start watering when soil% <= this
const int SOIL_THRESHOLD_HIGH = 50;   // stop watering when soil% >= this

// Safety timers (ms)
const unsigned long MAX_PUMP_ON_MS  = 2UL * 60UL * 1000UL;   // 2 minutes
//const unsigned long MIN_PUMP_OFF_MS = 5UL * 60UL * 1000UL;   // 5 minutes
const unsigned long MIN_PUMP_OFF_MS = 10UL * 1000UL; // 10 seconds (for testing)

// Timing
const unsigned long REPORT_INTERVAL_MS  = 1000UL;  // JSON every 1s
const unsigned long DHT_MIN_INTERVAL_MS = 2000UL;  // read DHT every >=2s
const int           DHT_RETRY_COUNT     = 3;
const unsigned long DHT_RETRY_DELAY_MS  = 250UL;

// Manual override timeout (set max duration manual control can persist automatically)
// Set to 0 to require explicit MANUAL:0 to clear (no auto-expire)
const unsigned long MANUAL_OVERRIDE_MAX_MS = 30UL * 60UL * 1000UL; // 30 minutes (0 disables auto-expire)
// -----------------------------------------

LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
DHT dht(DHT_PIN, DHTTYPE);

// state variables
bool pumpState = false;        // logical pump state (true = ON)
bool autoMode = true;          // auto watering enabled
bool manualOverride = false;   // set true by manual RELAY command
unsigned long manualOverrideUntilMs = 0; // expiry time for manual override; 0 means none (or disabled if max=0)

unsigned long pumpOnStartMs = 0;
unsigned long lastPumpOffMs = 0;

unsigned long lastReportMs = 0;
unsigned long lastDHTReadMs = 0;

// DHT cache
float lastTemp = NAN;
float lastHum = NAN;
unsigned long lastDHTSuccessMs = 0;  // millis of last successful DHT read

// Serial input buffer
String inputBuffer = "";

// ---------------- helpers ----------------
void setPump(bool on) {
  if (RELAY_ACTIVE_LOW) digitalWrite(RELAY_PIN, on ? LOW : HIGH);
  else digitalWrite(RELAY_PIN, on ? HIGH : LOW);

  pumpState = on;
  if (on) {
    pumpOnStartMs = millis();
    // Optionally log that pump was turned on
  } else {
    lastPumpOffMs = millis();
    pumpOnStartMs = 0;
  }

  // ack
  Serial.print("{\"ack\":\"RELAY\",\"state\":"); Serial.print(on ? 1 : 0); Serial.println("}");
}

int analogToSoilPercent(int raw) {
  if (raw < 0) raw = 0;
  if (raw > 1023) raw = 1023;
  int pct = map(raw, 0, 1023, 0, 100);
  // If your sensor gives higher reading when wet (inverted), uncomment:
  // pct = 100 - pct;
  return pct;
}

bool readDHTWithRetries(float &outTemp, float &outHum) {
  for (int i = 0; i < DHT_RETRY_COUNT; ++i) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      outTemp = t;
      outHum = h;
      return true;
    }
    delay(DHT_RETRY_DELAY_MS);
  }
  return false;
}

// --- updateLCD uses dtostrf to format floats for AVR compatibility ---
void updateLCD(int mq_raw, int soilPct, float temp, float hum, bool pump) {
  char line1[17];
  char line2[17];

  // Prepare temperature/humidity strings
  char tempBuf[8]; // e.g. "29.3"
  char humBuf[8];  // e.g. "45"

  bool haveTemp = (!isnan(temp) && temp > -100.0);
  bool haveHum  = (!isnan(hum));

  if (haveTemp) {
    // dtostrf(value, minWidth, precision, buffer)
    dtostrf(temp, 4, 1, tempBuf); // width 4, 1 decimal -> "29.3"
  } else {
    strcpy(tempBuf, "N/A");
  }

  if (haveHum) {
    dtostrf(hum, 3, 0, humBuf); // integer humidity
  } else {
    strcpy(humBuf, "N/A");
  }

  // Build line1: "T:29.3C H:45% P:1" or "T:N/A    H:N/A  P:0"
  if (haveTemp && haveHum) {
    // ensure spacing fits 16 chars
    snprintf(line1, sizeof(line1), "T:%4sC H:%3s%% P:%d", tempBuf, humBuf, pump ? 1 : 0);
  } else {
    // when missing show N/A
    snprintf(line1, sizeof(line1), "T:%4s H:%3s  P:%d", tempBuf, humBuf, pump ? 1 : 0);
  }

  // Build line2: "MQ:0990 S:057%"
  snprintf(line2, sizeof(line2), "MQ:%4d S:%3d%%", mq_raw, soilPct);

  // Print to LCD and pad
  lcd.setCursor(0, 0);
  lcd.print(line1);
  int l1 = strlen(line1);
  for (int i = l1; i < LCD_COLS; ++i) lcd.print(' ');

  lcd.setCursor(0, 1);
  lcd.print(line2);
  int l2 = strlen(line2);
  for (int i = l2; i < LCD_COLS; ++i) lcd.print(' ');
}

// Process serial text commands
void processSerialCommand(const String &cmd) {
  if (cmd.startsWith("RELAY:")) {
    int v = cmd.substring(6).toInt();
    // set manual override and optionally set expiry
    manualOverride = true;
    if (MANUAL_OVERRIDE_MAX_MS > 0) {
      manualOverrideUntilMs = millis() + MANUAL_OVERRIDE_MAX_MS;
    } else {
      manualOverrideUntilMs = 0; // 0 means persist until explicit clear
    }
    setPump(v == 1);
    Serial.print("{\"info\":\"manual_override_set\",\"state\":"); Serial.print(v); Serial.println("}");
  } else if (cmd.startsWith("AUTO:")) {
    int v = cmd.substring(5).toInt();
    autoMode = (v != 0);
    Serial.print("{\"ack\":\"AUTO\",\"state\":"); Serial.print(autoMode?1:0); Serial.println("}");
  } else if (cmd.startsWith("MANUAL:")) {
    int v = cmd.substring(7).toInt();
    if (v == 0) {
      manualOverride = false;
      manualOverrideUntilMs = 0;
      Serial.println("{\"ack\":\"MANUAL\",\"cleared\":1}");
    } else {
      // if explicit MANUAL:1 allow setting manual override without changing pump?
      manualOverride = true;
      if (MANUAL_OVERRIDE_MAX_MS > 0) manualOverrideUntilMs = millis() + MANUAL_OVERRIDE_MAX_MS;
      Serial.println("{\"ack\":\"MANUAL\",\"set\":1}");
    }
  } else {
    Serial.print("{\"info\":\"unknown_cmd\",\"cmd\":\"");
    Serial.print(cmd);
    Serial.println("\"}");
  }
}

void checkSerialInput() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      inputBuffer.trim();
      if (inputBuffer.length() > 0) processSerialCommand(inputBuffer);
      inputBuffer = "";
    } else if (c != '\r') {
      inputBuffer += c;
      if (inputBuffer.length() > 256) inputBuffer = inputBuffer.substring(inputBuffer.length()-256);
    }
  }
}

// ---------------- setup & loop ----------------
void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  if (RELAY_ACTIVE_LOW) digitalWrite(RELAY_PIN, HIGH);
  else digitalWrite(RELAY_PIN, LOW);

  Serial.begin(115200);
  dht.begin();

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Smart Plant");
  lcd.setCursor(0,1);
  lcd.print("Initializing...");
  delay(1000);
  lcd.clear();

  pumpState = false;
  manualOverride = false;
  manualOverrideUntilMs = 0;
  autoMode = true;
  lastPumpOffMs = millis();
  lastReportMs = 0;
  lastDHTReadMs = 0;
  lastDHTSuccessMs = 0;
}

void loop() {
  unsigned long now = millis();

  // ---------- Process serial commands FIRST so manualOverride is set immediately ----------
  checkSerialInput();

  // Auto-expire manual override if configured
  if (manualOverride && manualOverrideUntilMs != 0 && (long)(now - manualOverrideUntilMs) > 0) {
    // if now > manualOverrideUntilMs -> clear
    manualOverride = false;
    manualOverrideUntilMs = 0;
    Serial.println("{\"info\":\"manual_override_expired\"}");
  }

  // Read sensors
  int mq_raw = analogRead(MQ_PIN);
  int soil_raw = analogRead(SOIL_PIN);
  int soilPct = analogToSoilPercent(soil_raw);

  // DHT read (every >= DHT_MIN_INTERVAL_MS)
  if ((now - lastDHTReadMs) >= DHT_MIN_INTERVAL_MS) {
    lastDHTReadMs = now;
    float t = NAN, h = NAN;
    bool ok = readDHTWithRetries(t, h);
    if (ok) {
      lastTemp = t;
      lastHum = h;
      lastDHTSuccessMs = now;
      // debug
      Serial.print("{\"debug\":\"DHT_ok\",\"temp\":"); Serial.print(lastTemp,2);
      Serial.print(",\"hum\":"); Serial.print(lastHum,2);
      Serial.println("}");
    } else {
      Serial.println("{\"warn\":\"DHT_read_failed\"}");
      // do NOT overwrite lastTemp/lastHum - keep cached values
    }
  }

  // Auto-watering logic (only if autoMode and not manual override)
  if (autoMode && !manualOverride) {
    if (!pumpState) {
      if (soilPct <= SOIL_THRESHOLD_LOW) {
        if ((now - lastPumpOffMs) >= MIN_PUMP_OFF_MS) {
          setPump(true);
          Serial.println("{\"auto\":\"start_watering\"}");
        }
      }
    } else {
      if (soilPct >= SOIL_THRESHOLD_HIGH) {
        setPump(false);
        Serial.println("{\"auto\":\"stop_watering_by_moisture\"}");
      } else if ((now - pumpOnStartMs) >= MAX_PUMP_ON_MS) {
        setPump(false);
        Serial.println("{\"auto\":\"stop_watering_by_timeout\"}");
      }
    }
  } // end auto logic
// After computing soil_raw and soilPct, add:
/*Serial.print("{\"debug_soil_raw\":"); Serial.print(soil_raw);
Serial.print(",\"debug_soil_pct\":"); Serial.print(soilPct);
Serial.print(",\"lastPumpOffAgo_ms\":"); Serial.print((unsigned long)(millis() - lastPumpOffMs));
Serial.print(",\"MIN_PUMP_OFF_MS\":"); Serial.print(MIN_PUMP_OFF_MS);
Serial.print(",\"autoMode\":"); Serial.print(autoMode?1:0);
Serial.print(",\"manualOverride\":"); Serial.print(manualOverride?1:0);
Serial.println("}");*/

  // Periodic JSON reporting
  if ((now - lastReportMs) >= REPORT_INTERVAL_MS) {
    lastReportMs = now;

    float tOut = isnan(lastTemp) ? -1.0 : lastTemp;
    float hOut = isnan(lastHum)  ? -1.0 : lastHum;
    unsigned long dhtAge = (lastDHTSuccessMs == 0) ? 4294967295UL : (now - lastDHTSuccessMs); // big if never success

    Serial.print("{\"mq\":"); Serial.print(mq_raw);
    Serial.print(",\"soil\":"); Serial.print(soilPct);
    Serial.print(",\"temp\":"); Serial.print(tOut, 2);
    Serial.print(",\"hum\":"); Serial.print(hOut, 2);
    Serial.print(",\"relay\":"); Serial.print(pumpState ? 1 : 0);
    Serial.print(",\"dht_age_ms\":"); Serial.print(dhtAge);
    Serial.println("}");
  }

  // Update LCD using cached values (so it won't flash)
  updateLCD(mq_raw, soilPct, lastTemp, lastHum, pumpState);

  // small friendly delay
  delay(100);
}
