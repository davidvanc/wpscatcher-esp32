// wpscatcher-esp32 -- WPS-knop indrukken, wifi-gegevens als QR tonen.
// Zelfde idee als de Pi-versie in ../wpscatcher, maar op een M5Stack-bordje
// met LCD: dat scherm is leeg zodra de stroom weg is, dus geen "wachten tot
// het uitvalt" zoals bij e-ink.

#include <M5Unified.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <time.h>
#include "esp_wps.h"
#include "esp_wifi.h"
#include "qrcode.h"
#include "config.h"

enum class State {
  WpsSearching,
  WpsSuccess,
  WpsFailed,
};

static State state = State::WpsSearching;
static esp_wps_config_t wps_config;
static uint8_t attempt = 1;
static uint32_t qrShownAtMs = 0;
static String foundSsid;
static String foundPassword;

// Vlaggen gezet vanuit de wifi-event-task, afgehandeld in loop().
// De Arduino-voorbeeldcode waarschuwt expliciet: "WiFiEvent is called from a
// separate FreeRTOS task (thread)". Arduino String doet heap-allocatie, dus
// die vanuit twee taken tegelijk aanraken is een echte race. Daarom doet de
// handler niets meer dan een vlag zetten.
static volatile bool wpsSuccessFlag = false;
static volatile bool wpsFailFlag = false;

// ---------------------------------------------------------------- WPS ----

// Opzet 1-op-1 gebaseerd op de officiele voorbeelden van Espressif zelf:
// espressif/arduino-esp32 libraries/WiFi/examples/WPS/WPS.ino en
// espressif/esp-idf examples/wifi/wps/main/wps.c (beide opgehaald en
// gecontroleerd op 2026-08-28).
static void wpsConfigInit() {
  memset(&wps_config, 0, sizeof(esp_wps_config_t));
  wps_config.wps_type = WPS_TYPE_PBC;
  snprintf(wps_config.factory_info.manufacturer, sizeof(wps_config.factory_info.manufacturer), "ESPRESSIF");
  snprintf(wps_config.factory_info.model_number, sizeof(wps_config.factory_info.model_number), "ESP32");
  snprintf(wps_config.factory_info.model_name, sizeof(wps_config.factory_info.model_name), "wpscatcher-esp32");
  snprintf(wps_config.factory_info.device_name, sizeof(wps_config.factory_info.device_name), "wpscatcher");
}

static void wpsStart() {
  esp_err_t err = esp_wifi_wps_enable(&wps_config);
  if (err != ESP_OK) {
    Serial.printf("WPS Enable Failed: 0x%x: %s\n", err, esp_err_to_name(err));
    return;
  }
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
  err = esp_wifi_wps_start();
#else
  err = esp_wifi_wps_start(0);
#endif
  if (err != ESP_OK) {
    Serial.printf("WPS Start Failed: 0x%x: %s\n", err, esp_err_to_name(err));
  }
}

static void wpsRestart() {
  esp_wifi_wps_disable();
  delay(10);
  attempt++;
  wpsStart();
}

// Leest ssid/wachtwoord uit de wifi-driverconfig.
//
// Waarom dit werkt, letterlijk uit esp-idf/examples/wifi/wps/main/wps.c:
// "If only one AP credential is received from WPS, there will be no event
// data and esp_wifi_set_config() is already called by WPS modules". Bij een
// gewone router met een enkel netwerk staat de plaintext passphrase dus al
// in de sta-config zodra WPS slaagt -- precies zoals wpa_supplicant op de Pi
// psk="..." wegschreef.
//
// OPEN PUNT: geeft de router meerdere credentials terug, dan roept WPS
// esp_wifi_set_config() juist NIET zelf aan (dat doet het IDF-voorbeeld met
// de hand vanuit de event-data) en lezen we hier mogelijk een lege config.
// Zeldzaam bij consumentenrouters, niet afgedekt, niet getest.
static void readWpsCredentials() {
  wifi_config_t conf;
  memset(&conf, 0, sizeof(conf));
  esp_wifi_get_config(WIFI_IF_STA, &conf);

  char ssid[33] = {0};
  char pass[65] = {0};
  memcpy(ssid, conf.sta.ssid, sizeof(conf.sta.ssid));
  memcpy(pass, conf.sta.password, sizeof(conf.sta.password));

  foundSsid = String(ssid);
  foundPassword = String(pass);

  Serial.printf("WPS ok -- ssid='%s' (%u tekens), wachtwoord %u tekens\n",
                foundSsid.c_str(), foundSsid.length(), foundPassword.length());
  if (foundPassword.length() == 0) {
    Serial.println("LET OP: leeg wachtwoord uit de config -- QR wordt onbruikbaar.");
  }
}

static void onWifiEvent(WiFiEvent_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_WPS_ER_SUCCESS:
      esp_wifi_wps_disable();
      wpsSuccessFlag = true;
      break;

    case ARDUINO_EVENT_WPS_ER_FAILED:
    case ARDUINO_EVENT_WPS_ER_TIMEOUT:
      wpsFailFlag = true;
      break;

    default:
      break;
  }
}

// ------------------------------------------------------------ logboek ----

static bool fsReady = false;

// De RTC loopt door op de batterij, maar staat na een volledige ontlading
// weer op zijn beginwaarde. Dan liever "onbekend" wegschrijven dan een
// verzonnen datum.
static bool rtcLooksSet() {
  auto dt = M5.Rtc.getDateTime();
  return dt.date.year >= 2026;
}

// CSV met dubbele aanhalingstekens: ssid en wachtwoord mogen komma's,
// puntkomma's en aanhalingstekens bevatten zonder het bestand te breken.
static String csvField(const String &in) {
  String out = "\"";
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '\r' || c == '\n') { out += ' '; continue; }
    if (c == '"') out += '"';  // verdubbelen, zo wil CSV het
    out += c;
  }
  out += "\"";
  return out;
}

static void logAppend(const String &ssid, const String &password) {
  if (!LOG_ENABLED) return;
  if (!fsReady) {
    Serial.println("logboek: geen bestandssysteem, regel niet bewaard");
    return;
  }

  char datum[16] = "onbekend";
  char tijd[16] = "onbekend";
  if (rtcLooksSet()) {
    auto dt = M5.Rtc.getDateTime();
    snprintf(datum, sizeof(datum), "%04d-%02d-%02d",
             (int)dt.date.year, (int)dt.date.month, (int)dt.date.date);
    snprintf(tijd, sizeof(tijd), "%02d:%02d:%02d",
             (int)dt.time.hours, (int)dt.time.minutes, (int)dt.time.seconds);
  } else {
    Serial.println("logboek: RTC niet gezet -- tijd wordt 'onbekend'. "
                   "Zet ze met: tijd JJJJ-MM-DD UU:MM:SS");
  }

  // Vooraf kijken of er nog plaats is. Een vol bestandssysteem mag het
  // toestel niet breken: de QR is de hoofdzaak, het logboek is bijzaak, dus
  // bij plaatsgebrek slaan we de regel over en gaat de rest gewoon door.
  size_t vrij = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (vrij < LOG_MIN_FREE_BYTES) {
    Serial.printf("logboek VOL: nog %u bytes vrij, regel NIET bewaard. "
                  "Maak plaats met het commando 'wis'.\n", (unsigned)vrij);
    return;
  }

  File f = LittleFS.open(LOG_PATH, FILE_APPEND);
  if (!f) {
    Serial.println("logboek: kon het bestand niet openen");
    return;
  }
  String regel = csvField(datum) + "," + csvField(tijd) + "," +
                 csvField(ssid) + "," + csvField(password) + "\n";
  size_t geschreven = f.print(regel);
  f.close();

  if (geschreven != regel.length()) {
    Serial.printf("logboek: slechts %u van %u bytes weggeschreven -- "
                  "waarschijnlijk vol\n",
                  (unsigned)geschreven, (unsigned)regel.length());
  } else {
    Serial.printf("logboek: bijgeschreven (%s %s)\n", datum, tijd);
  }
}

// Gebufferd tellen, niet byte per byte: dit draait bij elke start, en
// f.read() per byte kost bij een gegroeid logboek merkbaar tijd.
static uint32_t logCountEntries() {
  if (!fsReady || !LittleFS.exists(LOG_PATH)) return 0;
  File f = LittleFS.open(LOG_PATH, FILE_READ);
  if (!f) return 0;
  uint32_t n = 0;
  uint8_t buf[512];
  while (f.available()) {
    size_t got = f.read(buf, sizeof(buf));
    for (size_t i = 0; i < got; i++) { if (buf[i] == '\n') n++; }
  }
  f.close();
  return n;
}

static void logDump() {
  Serial.println("=== logboek ===");
  if (!fsReady) { Serial.println("(geen bestandssysteem)"); return; }
  if (!LittleFS.exists(LOG_PATH)) {
    Serial.println("(nog leeg)");
    Serial.println("=== einde ===");
    return;
  }
  File f = LittleFS.open(LOG_PATH, FILE_READ);
  if (!f) { Serial.println("(kon niet openen)"); return; }
  Serial.println("datum,tijd,ssid,wachtwoord");
  uint8_t buf[256];
  while (f.available()) {
    size_t got = f.read(buf, sizeof(buf));
    Serial.write(buf, got);
  }
  f.close();
  Serial.printf("=== einde, %u regels ===\n", (unsigned)logCountEntries());
}

static void logWipe() {
  if (fsReady && LittleFS.exists(LOG_PATH)) {
    LittleFS.remove(LOG_PATH);
    Serial.println("logboek gewist");
  } else {
    Serial.println("logboek was al leeg");
  }
}

// --------------------------------------------------- seriele commando's ----

static void runCommand(const String &cmd) {
  if (cmd == "dump") {
    logDump();
  } else if (cmd == "wis") {
    logWipe();
  } else if (cmd.startsWith("tijd ")) {
    int y, mo, d, h, mi, s;
    if (sscanf(cmd.c_str(), "tijd %d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) {
      struct tm t = {};
      t.tm_year = y - 1900;
      t.tm_mon = mo - 1;
      t.tm_mday = d;
      t.tm_hour = h;
      t.tm_min = mi;
      t.tm_sec = s;
      t.tm_isdst = -1;
      mktime(&t);  // vult tm_wday in, want de BM8563 bewaart de weekdag apart
      M5.Rtc.setDateTime(&t);
      Serial.printf("tijd gezet op %04d-%02d-%02d %02d:%02d:%02d\n", y, mo, d, h, mi, s);
    } else {
      Serial.println("gebruik: tijd JJJJ-MM-DD UU:MM:SS");
    }
  } else if (cmd == "tijd") {
    auto dt = M5.Rtc.getDateTime();
    Serial.printf("RTC staat op %04d-%02d-%02d %02d:%02d:%02d%s\n",
                  (int)dt.date.year, (int)dt.date.month, (int)dt.date.date,
                  (int)dt.time.hours, (int)dt.time.minutes, (int)dt.time.seconds,
                  rtcLooksSet() ? "" : "  (lijkt niet gezet)");
  } else {
    Serial.println("commando's: dump | wis | tijd | tijd JJJJ-MM-DD UU:MM:SS");
  }
}

static void handleSerial() {
  static String buf;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (buf.length()) { runCommand(buf); buf = ""; }
    } else if (buf.length() < 64) {
      buf += c;
    }
  }
}

// ----------------------------------------------------------------- QR ----

static String escapeForWifiQr(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') {
      out += '\\';
    }
    out += c;
  }
  return out;
}

static String buildWifiQrPayload(const String &ssid, const String &password) {
  return "WIFI:T:WPA;S:" + escapeForWifiQr(ssid) +
         ";P:" + escapeForWifiQr(password) + ";;";
}

// Capaciteit in bytes per QR-versie, byte-mode op ECC_LOW (QR-standaard).
// Nodig omdat qrcode.c dit zelf NIET controleert -- in de broncode staat
// letterlijk "@TODO: Return error if data is too big", dus een te lange
// payload geeft daar stille geheugenschade in plaats van een foutcode.
static const uint16_t QR_BYTE_CAPACITY_ECC_LOW[] = {
    0,    // versie 0 bestaat niet
    17,   // 1
    32,   // 2
    53,   // 3
    78,   // 4
    106,  // 5
    134,  // 6
    154,  // 7
    192,  // 8
    230,  // 9
    271,  // 10
};

// Kleinste versie die de payload aankan. Kleiner = minder modules = dikkere
// blokjes op hetzelfde scherm = beter scanbaar. Een vaste hoge versie kiezen
// zou de modules onnodig klein maken.
static uint8_t chooseQrVersion(size_t payloadLength) {
  for (uint8_t v = 1; v <= QR_MAX_VERSION; v++) {
    if (payloadLength <= QR_BYTE_CAPACITY_ECC_LOW[v]) return v;
  }
  return 0;  // past niet
}

// -------------------------------------------------------------- scherm ----

// Tekstgrootte hangt af van de BREEDTE, niet de hoogte: "WPS zoeken" op
// grootte 3 is 180 px breed en past dus niet op een smal scherm, ook al is
// dat scherm hoog genoeg.
static bool narrowScreen() { return M5.Display.width() < 200; }

static void drawBatteryIndicator() {
  auto &d = M5.Display;
  int32_t level = M5.Power.getBatteryLevel();  // 0-100, negatief = onbekend
  bool charging = M5.Power.isCharging() == m5::Power_Class::is_charging;

  char buf[16];
  if (level < 0) {
    snprintf(buf, sizeof(buf), "batt ?");
  } else if (charging) {
    snprintf(buf, sizeof(buf), "%ld%% +", (long)level);
  } else {
    snprintf(buf, sizeof(buf), "%ld%%", (long)level);
  }

  d.setTextColor(TFT_BLACK, TFT_WHITE);
  d.setTextDatum(top_right);
  d.setTextSize(1);
  d.drawString(buf, d.width() - 4, 4);
}

// Tekstregels worden verhoudingsgewijs geplaatst, niet op vaste pixels:
// het bord kan een Core Basic (320x240) of een StickC (240x135) zijn en
// vaste coordinaten vallen op de kleine schermen buiten beeld.
static void drawSearchingScreen() {
  auto &d = M5.Display;
  d.fillScreen(TFT_WHITE);
  d.setTextColor(TFT_BLACK, TFT_WHITE);
  d.setTextDatum(middle_center);

  int cx = d.width() / 2;
  int h = d.height();
  bool small = narrowScreen();

  d.setTextSize(small ? 2 : 3);
  d.drawString("WPS zoeken", cx, h * 25 / 100);

  d.setTextSize(small ? 1 : 2);
  d.drawString("druk op de knop", cx, h * 48 / 100);
  d.drawString("van de modem", cx, h * 62 / 100);

  char buf[24];
  snprintf(buf, sizeof(buf), "poging %u", attempt);
  d.drawString(buf, cx, h * 82 / 100);

  drawBatteryIndicator();
}

static void drawFailedScreen() {
  auto &d = M5.Display;
  d.fillScreen(TFT_WHITE);
  d.setTextColor(TFT_BLACK, TFT_WHITE);
  d.setTextDatum(middle_center);

  int cx = d.width() / 2;
  int h = d.height();
  bool small = narrowScreen();

  d.setTextSize(small ? 2 : 3);
  d.drawString("Niet gelukt", cx, h * 28 / 100);

  d.setTextSize(small ? 1 : 2);
  d.drawString("knop A: opnieuw", cx, h * 58 / 100);
  d.drawString("andere knop: uit", cx, h * 75 / 100);

  drawBatteryIndicator();
}

static void drawMessageScreen(const char *line1, const char *line2) {
  auto &d = M5.Display;
  d.fillScreen(TFT_WHITE);
  d.setTextColor(TFT_BLACK, TFT_WHITE);
  d.setTextDatum(middle_center);
  d.setTextSize(narrowScreen() ? 1 : 2);
  d.drawString(line1, d.width() / 2, d.height() * 40 / 100);
  if (line2) d.drawString(line2, d.width() / 2, d.height() * 58 / 100);
  drawBatteryIndicator();
}

static void drawQrScreen() {
  auto &d = M5.Display;
  d.fillScreen(TFT_WHITE);

  String payload = buildWifiQrPayload(foundSsid, foundPassword);
  uint8_t version = chooseQrVersion(payload.length());
  if (version == 0) {
    Serial.printf("Payload te lang voor QR-versie %u: %u bytes\n",
                  QR_MAX_VERSION, payload.length());
    drawMessageScreen("QR te lang", "zie serieel");
    return;
  }

  // Buffer statisch op de maat van de grootste toegestane versie, met een
  // harde controle: de library schrijft zelf niet binnen de perken.
  static uint8_t qrData[QR_BUFFER_BYTES];
  uint16_t needed = qrcode_getBufferSize(version);
  if (needed > sizeof(qrData)) {
    Serial.printf("QR-buffer te klein: %u nodig, %u beschikbaar\n",
                  needed, (unsigned)sizeof(qrData));
    drawMessageScreen("QR-buffer", "te klein");
    return;
  }

  QRCode qrcode;
  qrcode_initText(&qrcode, qrData, version, ECC_LOW, payload.c_str());

  // Past binnen zowel hoogte als breedte; met tekst eronder blijft er ruimte
  // over voor twee regels.
  int reserved = SHOW_CREDENTIALS_TEXT ? (d.height() < 200 ? 32 : 60) : 8;
  int maxQrPx = d.height() - reserved;
  if (maxQrPx > d.width() - 8) maxQrPx = d.width() - 8;

  int scale = maxQrPx / qrcode.size;
  if (scale < 1) scale = 1;

  int qrPixels = qrcode.size * scale;
  int offsetX = (d.width() - qrPixels) / 2;
  int offsetY = SHOW_CREDENTIALS_TEXT ? 6 : (d.height() - qrPixels) / 2;

  Serial.printf("QR: %u bytes payload, versie %u, %u modules, %d px/module "
                "(%.2f mm/module, blok %.1f mm)\n",
                payload.length(), version, qrcode.size, scale,
                scale * SCREEN_PITCH_MM, qrPixels * SCREEN_PITCH_MM);
  if (scale * SCREEN_PITCH_MM < 0.40f) {
    Serial.println("LET OP: modules onder 0,40 mm. In het Pi-project las 0,41 mm "
                   "nog wel en 0,29 mm niet meer -- hier dus zelf natesten.");
  }

  for (int y = 0; y < qrcode.size; y++) {
    for (int x = 0; x < qrcode.size; x++) {
      if (qrcode_getModule(&qrcode, x, y)) {
        d.fillRect(offsetX + x * scale, offsetY + y * scale, scale, scale, TFT_BLACK);
      }
    }
  }

  if (SHOW_CREDENTIALS_TEXT) {
    int textY = offsetY + qrPixels + 10;
    d.setTextColor(TFT_BLACK, TFT_WHITE);
    d.setTextDatum(top_center);
    d.setTextSize(narrowScreen() ? 1 : 2);
    d.drawString(foundSsid, d.width() / 2, textY);
    if (SHOW_PASSWORD) {
      d.drawString(foundPassword, d.width() / 2, textY + (narrowScreen() ? 12 : 24));
    }
  }

  drawBatteryIndicator();
}

static State lastDrawnState = State::WpsFailed;
static bool qrDrawn = false;

static void render() {
  if (state == State::WpsSuccess) {
    if (!qrDrawn) {
      drawQrScreen();
      qrDrawn = true;
    }
    return;
  }
  qrDrawn = false;

  // Op het zoekscherm moet de pogingteller wel mee veranderen.
  static uint8_t lastDrawnAttempt = 0;
  if (state == lastDrawnState && attempt == lastDrawnAttempt) return;
  lastDrawnState = state;
  lastDrawnAttempt = attempt;

  if (state == State::WpsSearching) drawSearchingScreen();
  else if (state == State::WpsFailed) drawFailedScreen();
}

// --------------------------------------------------------------- loop ----

static void shutdown() {
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.sleep();
  M5.Power.powerOff();
}

void setup() {
  auto cfg = M5.config();

  // Alles uit wat dit toestel niet gebruikt. Op een 120mAh-batterij telt dat:
  // output_power voedt de 5V van de Grove-poort, en daar hangt niets aan.
  // De PLUS SE heeft sowieso geen IMU (dat is net het verschil met de PLUS).
  cfg.output_power = false;
  cfg.internal_imu = false;
  cfg.internal_mic = false;
  cfg.internal_spk = false;
  // pmic_button blijft aan: op de AXP192-borden is dat M5.BtnPWR.

  M5.begin(cfg);
  M5.Display.setRotation(1);
  Serial.begin(115200);

  Serial.printf("wpscatcher-esp32 -- scherm %dx%d\n",
                M5.Display.width(), M5.Display.height());

  if (LOG_ENABLED) {
    // true = formatteren als het bestandssysteem nog niet bestaat. Dat
    // gebeurt eenmalig bij de allereerste start en duurt een paar seconden;
    // daarna is het mounten een kwestie van milliseconden.
    fsReady = LittleFS.begin(true);
    if (fsReady) {
      // Het bestand meteen aanmaken als het er nog niet is. Anders logt de
      // VFS-laag bij elke start een rode "does not exist"-regel zodra we
      // ernaar kijken, en dat leest als een fout terwijl er niets mis is.
      if (!LittleFS.exists(LOG_PATH)) {
        File nieuw = LittleFS.open(LOG_PATH, FILE_APPEND);
        if (nieuw) nieuw.close();
      }
      // Bewust GEEN regels tellen bij het opstarten: dat leest het hele
      // bestand en zou de start dus trager maken naarmate het logboek groeit.
      // De grootte staat in de metadata en is meteen op te vragen. Tellen
      // gebeurt enkel bij 'dump', waar je toch al op uitvoer wacht.
      size_t vrij = LittleFS.totalBytes() - LittleFS.usedBytes();
      File f = LittleFS.open(LOG_PATH, FILE_READ);
      size_t gebruikt = f ? f.size() : 0;
      if (f) f.close();
      Serial.printf("logboek: %u bytes, %u van %u vrij -- "
                    "typ 'dump' om te tonen\n",
                    (unsigned)gebruikt, (unsigned)vrij,
                    (unsigned)LittleFS.totalBytes());
      if (!rtcLooksSet()) {
        Serial.println("logboek: RTC nog niet gezet. "
                       "Doe dat met: tijd JJJJ-MM-DD UU:MM:SS");
      }
    } else {
      Serial.println("logboek: bestandssysteem niet beschikbaar");
    }
  }

  wpsConfigInit();
  WiFi.mode(WIFI_MODE_STA);
  WiFi.onEvent(onWifiEvent);
  wpsStart();

  render();
}

void loop() {
  M5.update();
  handleSerial();

  // Vlaggen uit de event-task afhandelen, in deze task, waar de Strings
  // en het scherm veilig aangeraakt mogen worden.
  if (wpsSuccessFlag && state != State::WpsSuccess) {
    wpsSuccessFlag = false;
    readWpsCredentials();
    logAppend(foundSsid, foundPassword);
    state = State::WpsSuccess;
    qrShownAtMs = millis();
  }

  if (wpsFailFlag) {
    wpsFailFlag = false;
    if (GIVE_UP_AFTER_ATTEMPTS != 0 && attempt >= GIVE_UP_AFTER_ATTEMPTS) {
      state = State::WpsFailed;
    } else {
      wpsRestart();
    }
  }

  // Knop A op het mislukt-scherm probeert opnieuw; elke andere knopdruk, in
  // eender welke toestand, schakelt uit. BtnPWR staat er expliciet bij: op de
  // StickC-familie bestaat BtnC niet als echte knop (M5Unified documenteert
  // "M5Stick C/CPlus: BtnA, BtnB, BtnPWR"), daar is de aan/uitknop de derde.
  bool retried = false;
  if (state == State::WpsFailed && M5.BtnA.wasPressed()) {
    attempt = 1;
    state = State::WpsSearching;
    wpsStart();
    retried = true;
  }

  if (!retried && (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() ||
                   M5.BtnC.wasPressed() || M5.BtnPWR.wasPressed())) {
    shutdown();
  }

  if (state == State::WpsSuccess && SHUTDOWN_AFTER_MS != 0) {
    if (millis() - qrShownAtMs > SHUTDOWN_AFTER_MS) {
      shutdown();
    }
  }

  render();
  delay(20);
}
