#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include "esp_sntp.h"
#include <FirebaseESP32.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#define PH_PIN       34
#define TDS_PIN      35
#define ONE_WIRE_BUS  4
#define BUZZER_PIN   23

#define TRIG_PIN     18
#define ECHO_PIN     19
#define SERVO_PIN    13

const char* WIFI_SSID     = "Hjhj";
const char* WIFI_PASSWORD = "sany1256";

#define DATABASE_SECRET "c3ucOcFKOOhaxqULPtHJwVvnSZEaKSpeQ86Bit0c"
#define DATABASE_URL "https://monitoring-air-udang-gal-4d618-default-rtdb.firebaseio.com/"

// ======================================================================
// NTP
// ======================================================================
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "id.pool.ntp.org";
const char* NTP_SERVER_3 = "time.google.com";
const long  GMT_OFFSET_SEC      = 25200; // WIB = UTC+7
const int   DAYLIGHT_OFFSET_SEC = 0;

const long EPOCH_VALID_THRESHOLD = 1700000000UL;
const long EPOCH_UPPER_THRESHOLD = 2000000000UL;

volatile bool ntpSudahSinkron = false;

bool epochValid(long epoch) {
  return (epoch >= EPOCH_VALID_THRESHOLD && epoch <= EPOCH_UPPER_THRESHOLD);
}

long epochSekarang() {
  return (long)time(nullptr);
}

bool waktuValidSekarang() {
  return epochValid(epochSekarang());
}

void ntpCallback(struct timeval *tv) {
  ntpSudahSinkron = true;
  Serial.println(">> NTP callback: waktu tersinkron -> " + String((long)tv->tv_sec));
}

void mulaiNTP() {
  sntp_set_time_sync_notification_cb(ntpCallback);
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC,
             NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
}

bool tungguSinkronNTP(int maxPercobaan, int delayMsTiapPercobaan) {
  for (int i = 0; i < maxPercobaan; i++) {
    if (ntpSudahSinkron || waktuValidSekarang()) return true;
    delay(delayMsTiapPercobaan);
  }
  return ntpSudahSinkron || waktuValidSekarang();
}

// ======================================================================
// FALLBACK: ambil waktu via HTTP time API kalau SNTP gagal/lambat.
// ======================================================================
bool ambilWaktuViaHTTP() {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.setTimeout(5000);
  http.begin("http://worldtimeapi.org/api/timezone/Asia/Jakarta");
  int httpCode = http.GET();
  bool berhasil = false;

  if (httpCode == 200) {
    String payload = http.getString();
    int posKunci = payload.indexOf("\"unixtime\":");
    if (posKunci >= 0) {
      int mulai = posKunci + strlen("\"unixtime\":");
      int akhir = payload.indexOf(",", mulai);
      if (akhir > mulai) {
        long epochUtc = payload.substring(mulai, akhir).toInt();
        if (epochValid(epochUtc)) {
          struct timeval tv;
          tv.tv_sec  = epochUtc; // epoch UTC, offset WIB sudah diatur lewat configTime() sebelumnya
          tv.tv_usec = 0;
          settimeofday(&tv, nullptr);
          ntpSudahSinkron = true;
          berhasil = true;
          Serial.println(">> Fallback HTTP berhasil, waktu di-set -> " + buatTimestampLengkap());
        }
      }
    }
  } else {
    Serial.println(">> Fallback HTTP gagal, kode: " + String(httpCode));
  }

  http.end();
  return berhasil;
}

bool pastikanWaktuValid(int percobaanSNTP, int delayMsSNTP) {
  if (waktuValidSekarang()) return true;
  if (tungguSinkronNTP(percobaanSNTP, delayMsSNTP)) return true;
  Serial.println(">> SNTP belum sinkron, coba fallback HTTP...");
  return ambilWaktuViaHTTP();
}

String buatWaktuLabel() {
  time_t t = time(nullptr);
  struct tm *timeinfo = localtime(&t);
  char bufJam[9];
  strftime(bufJam, sizeof(bufJam), "%H:%M:%S", timeinfo);
  String label = "[" + String(bufJam) + "]";
  return label;
}

String buatWaktuLabelPolos() {
  time_t t = time(nullptr);
  struct tm *timeinfo = localtime(&t);
  char bufJam[9];
  strftime(bufJam, sizeof(bufJam), "%H:%M:%S", timeinfo);
  return String(bufJam);
}

String buatTimestampLengkap() {
  time_t t = time(nullptr);
  struct tm *timeinfo = localtime(&t);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", timeinfo);
  return String(buf);
}

String buatJamMenitSingkat() {
  time_t t = time(nullptr);
  struct tm *timeinfo = localtime(&t);
  char buf[6];
  strftime(buf, sizeof(buf), "%H:%M", timeinfo);
  return String(buf);
}

int getJamSekarang() {
  time_t t = time(nullptr);
  struct tm *timeinfo = localtime(&t);
  return timeinfo->tm_hour;
}

int getMenitSekarang() {
  time_t t = time(nullptr);
  struct tm *timeinfo = localtime(&t);
  return timeinfo->tm_min;
}
// ======================================================================
// AKHIR BAGIAN NTP
// ======================================================================

const float TINGGI_TOTAL_WADAH = 12.0;
const float TINGGI_MAKS_PAKAN  = 10.0;

const float SUHU_MIN = 27.0;
const float SUHU_MAX = 30.0;
const float PH_MIN   = 6.5;
const float PH_MAX   = 8.5;
const float TDS_MIN  = 300.0;
const float TDS_MAX  = 600.0;
const float PAKAN_MIN = 20.0;
const float PAKAN_MAX = 100.0;

// ======================================================================
// KALIBRASI SENSOR — REGRESI LINIER (Y = SLOPE * X + INTERCEPT)
// ------------------------------------------------------------------
// X = nilai mentah dari sensor, Y = nilai akhir yang sudah dikalibrasi.
// SLOPE dan INTERCEPT di bawah ini adalah hasil regresi linier kamu
// sendiri (mis. dari Excel: SLOPE = fungsi SLOPE(), INTERCEPT = fungsi
// INTERCEPT()) berdasarkan data pasangan (nilai mentah, nilai acuan).
// Nilai default 1.0 / 0.0 = belum dikalibrasi (Y = X apa adanya).
//
// X untuk tiap sensor:
//   SUHU       : pembacaan DS18B20 mentah, satuan °C
//   pH         : nilai pH mentah hasil rumus manual (titik kalibrasi pH 7 +
//                kemiringan elektroda), sebelum diregresi, satuan pH
//   TDS        : nilai ppm mentah hasil konversi tegangan + kompensasi
//                suhu otomatis (rumus polinomial bawaan), satuan ppm
//   ULTRASONIK : jarak hasil konversi durasi pulsa echo, satuan cm
// ======================================================================
const float SUHU_SLOPE     = 0.9849;
const float SUHU_INTERCEPT = 0.4424;

const float PH_SLOPE     = 1.0565;
const float PH_INTERCEPT = -0.2542;

// Konstanta rumus manual pH (titik kalibrasi pH 7 & sensitivitas elektroda dalam
// Volt per unit pH), dipakai di bacaPHMentah() untuk menghasilkan NILAI PH MENTAH
// (bukan tegangan) sebelum nilai itu diregresi lewat PH_SLOPE/PH_INTERCEPT di atas.
const float TEGANGAN_PH7   = 2.5010;
const float VOLT_PER_PH    = 0.18;

const float TDS_SLOPE     = 1.0904;
const float TDS_INTERCEPT = 36.2096;

const float ULTRASONIK_SLOPE     = 0.9971;
const float ULTRASONIK_INTERCEPT = -0.0257;
const float JARAK_MINIMUM_VALID  = 2.0; // cm — di bawah ini pembacaan ultrasonik dianggap tidak valid

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo myServo;

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

Preferences prefs;

const unsigned long INTERVAL_FIREBASE = 5000;
unsigned long lastFirebaseSend = 0;

const unsigned long INTERVAL_SERIAL_CLOCK = 1000;
unsigned long lastSerialClock = 0;

bool pakanSudahDiberikan = false;
bool alertSebelumnya = false;

bool buzzerAlertAktif       = false;
unsigned long buzzerAlertMulaiMs = 0;
const unsigned long DURASI_BUZZER_ALERT_MS = 5000;

int jadwalJam[3]   = {-1, -1, -1};
int jadwalMenit[3] = {-1, -1, -1};
int durasiServoDetik = 0;
String jadwalTeks = "(belum ada jadwal dari Firebase)";

bool servoAktif = false;
bool kontrolPakanTersedia = false;
long servoMulaiEpoch = 0;
long lastTriggerMinuteIndex[3] = {-1, -1, -1};
bool servoAktifTersimpan = false;
bool perluRecoveryOffPending = false;

const unsigned long JEDA_KOCOK = 350;
unsigned long waktuKocokTerakhir = 0;
bool posisiKananServo = false;

const unsigned long INTERVAL_SYNC_JADWAL = 10000;
unsigned long lastSyncJadwal = 0;

unsigned long lastPaksaResyncNtpMs = 0;
const unsigned long INTERVAL_PAKSA_RESYNC_NTP = 30000;

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  delay(1000);

  analogSetAttenuation(ADC_11db); // supaya ADC bisa baca tegangan sampai ~3.3V (untuk pH & TDS)

  pinMode(TDS_PIN,    INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN,  INPUT);
  myServo.attach(SERVO_PIN);
  myServo.write(90);

  sensors.begin();

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("pH & TDS Meter");
  lcd.setCursor(0, 1);
  lcd.print("Connecting WiFi");

  prefs.begin("pakan", false);
  lastTriggerMinuteIndex[0] = prefs.getLong("ltm0", -1);
  lastTriggerMinuteIndex[1] = prefs.getLong("ltm1", -1);
  lastTriggerMinuteIndex[2] = prefs.getLong("ltm2", -1);
  servoAktifTersimpan       = prefs.getBool("servoAktif", false);
  servoMulaiEpoch           = prefs.getLong("mulaiEpoch", 0);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int counter = 0;
  while (WiFi.status() != WL_CONNECTED && counter < 20) {
    delay(500);
    Serial.print(".");
    counter++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Terhubung! IP: " + WiFi.localIP().toString());
    lcd.setCursor(0, 1);
    lcd.print("WiFi Connected! ");
    delay(1000);

    mulaiNTP();

    bool waktuOk = pastikanWaktuValid(30, 500);

    int percobaanTambahan = 0;
    while (!waktuOk && percobaanTambahan < 5) {
      lcd.setCursor(0, 1);
      lcd.print("Sinkron waktu...");
      Serial.println(">> Waktu belum valid, percobaan tambahan ke-" + String(percobaanTambahan + 1));
      delay(2000);
      mulaiNTP();
      waktuOk = pastikanWaktuValid(6, 500);
      percobaanTambahan++;
    }

    if (!waktuOk) {
      lcd.setCursor(0, 1);
      lcd.print("Waktu gagal sync");
      Serial.println(">> GAGAL mendapatkan waktu valid setelah semua percobaan, lanjut ke loop() untuk retry berkala");
      delay(1000);
    } else {
      Serial.println(">> Waktu tersinkron: " + buatTimestampLengkap());
    }

    config.database_url = DATABASE_URL;
    config.signer.tokens.legacy_token = DATABASE_SECRET;

    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);
    Firebase.setFloatDigits(2);

    if (servoAktifTersimpan) {
      if (kirimStatusPakan(false)) {
        servoAktifTersimpan = false;
        prefs.putBool("servoAktif", false);
      } else {
        perluRecoveryOffPending = true;
      }
      servoAktif = false;
    }

    delay(500);
    sinkronJadwalPakan();

  } else {
    lcd.setCursor(0, 1);
    lcd.print("WiFi GAGAL!     ");
    delay(1000);
  }

  lcd.clear();
  Serial.println("======= SISTEM SIAP =======");
}

// ======================================================================
// SENSOR SUHU, pH, TDS, DAN ULTRASONIK
// ------------------------------------------------------------------
// Tiap sensor punya 2 fungsi:
//   1) bacaXxxMentah()  -> ambil nilai mentah langsung dari hardware (X)
//   2) bacaXxx()        -> terapkan persamaan garis Y = SLOPE*X + INTERCEPT
// Supaya kalibrasi, kamu HANYA perlu ganti nilai SUHU_SLOPE/INTERCEPT,
// PH_SLOPE/INTERCEPT, TDS_SLOPE/INTERCEPT, dan ULTRASONIK_SLOPE/INTERCEPT
// di bagian konstanta di atas — tidak perlu sentuh fungsi di bawah ini.
// ======================================================================

float bacaSuhuMentah() {
  sensors.requestTemperatures();
  float suhu = sensors.getTempCByIndex(0);
  if (suhu == DEVICE_DISCONNECTED_C) return 25.0; // fallback aman kalau sensor lepas
  return suhu;
}

float bacaSuhu() {
  float x = bacaSuhuMentah();
  return (SUHU_SLOPE * x) + SUHU_INTERCEPT;
}

float bacaPHMentah() {
  int raw = 0;
  for (int i = 0; i < 10; i++) { raw += analogRead(PH_PIN); delay(10); }
  float avg           = raw / 10.0;
  float teganganESP32 = (avg * 3.3) / 4095.0;
  // Rekonstruksi tegangan asli modul pH sebelum pembagi tegangan (R1=1k, R2=2k -> faktor 1.5)
  float teganganModulAsli = teganganESP32 * 1.5;

  // Nilai pH mentah bawaan (rumus manual pakai titik kalibrasi pH 7 & sensitivitas
  // elektroda 0.18 V/pH), sebelum diregresi lewat PH_SLOPE/PH_INTERCEPT
  float phBawaan = 7.0 + ((TEGANGAN_PH7 - teganganModulAsli) / VOLT_PER_PH);
  return phBawaan;
}

float bacaPH() {
  float x = bacaPHMentah();
  float y = (PH_SLOPE * x) + PH_INTERCEPT;
  if (y < 0.0)  y = 0.0;   // batasi rentang skala pH standar 0-14
  if (y > 14.0) y = 14.0;
  return y;
}

float bacaTDSMentah(float suhuKalibrasi) {
  // Jeda settling ADC saat pindah channel dari PH_PIN ke TDS_PIN, supaya
  // sisa muatan dari pembacaan pH sebelumnya tidak ikut memengaruhi
  // pembacaan TDS (kedua probe ada di wadah air yang sama).
  analogRead(TDS_PIN);
  delay(20);

  int raw = 0;
  for (int i = 0; i < 20; i++) { raw += analogRead(TDS_PIN); delay(10); }
  float avg      = raw / 20.0;
  float tegangan = (avg * 3.3) / 4095.0;

  // Kompensasi suhu otomatis (acuan 25°C), pakai suhu yang sudah dikalibrasi
  float koefisienKompensasi = 1.0 + 0.02 * (suhuKalibrasi - 25.0);
  float teganganKompensasi  = tegangan / koefisienKompensasi;

  // PPM mentah bawaan (rumus pabrik), sebelum diregresi lewat TDS_SLOPE/INTERCEPT
  float ppmBawaan = (133.42 * pow(teganganKompensasi, 3) - 255.86 * pow(teganganKompensasi, 2) + 857.39 * teganganKompensasi) * 0.5;
  return ppmBawaan;
}

float bacaTDS(float suhuKalibrasi) {
  float x = bacaTDSMentah(suhuKalibrasi);
  float y = (TDS_SLOPE * x) + TDS_INTERCEPT;
  if (y < 0) y = 0; // cegah nilai negatif kalau air terlalu steril
  return y;
}

long bacaDurasiUltrasonikMentah() {
  long totalDurasi = 0;
  int  sampelValid = 0;

  for (int i = 0; i < 5; i++) {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    long dur = pulseIn(ECHO_PIN, HIGH, 26000);
    if (dur > 0) {
      totalDurasi += dur;
      sampelValid++;
    }
    delay(10);
  }

  if (sampelValid == 0) return 0;
  return totalDurasi / sampelValid;
}

float bacaJarakUltrasonik() {
  long durasiMentah = bacaDurasiUltrasonikMentah();
  if (durasiMentah == 0) return -1.0; // tidak ada echo terbaca -> tidak valid

  // Konversi durasi pulsa ke jarak (cm) dulu, baru diregresi -> X = jarak cm
  float x = durasiMentah * 0.0343 / 2.0;
  return (ULTRASONIK_SLOPE * x) + ULTRASONIK_INTERCEPT;
}

float konversiJarakKePersen(float jarak) {
  if (jarak >= TINGGI_TOTAL_WADAH) {
    return 0.0; // jarak >=12cm -> wadah dianggap kosong
  }
  float tinggiPakan = TINGGI_TOTAL_WADAH - jarak;
  tinggiPakan = constrain(tinggiPakan, 0.0, TINGGI_MAKS_PAKAN);
  float persen = (tinggiPakan / TINGGI_MAKS_PAKAN) * 100.0;
  return constrain(persen, 0.0, PAKAN_MAX);
}

float persenPakanTerakhir = 0.0; // nilai valid terakhir; dipakai kalau pembacaan baru tidak valid

float bacaPersenPakan() {
  float jarak = bacaJarakUltrasonik();

  if (jarak < JARAK_MINIMUM_VALID) {
     // Echo valid tapi jaraknya <2cm (di luar jangkauan minimum sensor) -> wadah penuh
    persenPakanTerakhir = PAKAN_MAX; // 100.0
    return persenPakanTerakhir;
  }

  persenPakanTerakhir = konversiJarakKePersen(jarak);
  return persenPakanTerakhir;
}
// ======================================================================
// AKHIR BAGIAN SENSOR
// ======================================================================

bool parseJamMenit(String teks, int &jam, int &menit) {
  teks.trim();
  teks.replace("\"", "");
  teks.replace("\\", "");
  int posTitikDua = teks.indexOf(':');
  if (posTitikDua < 1) return false;

  String strJam   = teks.substring(0, posTitikDua);
  String strMenit = teks.substring(posTitikDua + 1);

  int j = strJam.toInt();
  int m = strMenit.toInt();

  if (j < 0 || j > 23 || m < 0 || m > 59) return false;

  jam   = j;
  menit = m;
  return true;
}

void sinkronJadwalPakan() {
  if (!Firebase.ready()) return;

  int jumlahJadwalValidTotal = 0;

  for (int i = 0; i < 3; i++) {
    String namaSesi = "jadwal" + String(i + 1);
    String pathJadwal = "/Kontrol_Pakan/" + namaSesi;

    if (Firebase.getString(fbdo, pathJadwal)) {
      String nilaiTeks = fbdo.stringData();
      int jamBaru, menitBaru;
      if (parseJamMenit(nilaiTeks, jamBaru, menitBaru)) {
        jadwalJam[i]   = jamBaru;
        jadwalMenit[i] = menitBaru;
      }
    }
    delay(50);
  }

  if (Firebase.getString(fbdo, "/Kontrol_Pakan/detik_selesai")) {
    String nilaiTeks = fbdo.stringData();
    nilaiTeks.trim();
    nilaiTeks.replace("\"", "");
    nilaiTeks.replace("\\", "");
    int durasiBaru = nilaiTeks.toInt();
    if (durasiBaru > 0) {
      durasiServoDetik = durasiBaru;
    }
  }
  delay(50);

  for (int i = 0; i < 3; i++) {
    if (jadwalJam[i] >= 0 && jadwalMenit[i] >= 0) jumlahJadwalValidTotal++;
  }

  kontrolPakanTersedia = (jumlahJadwalValidTotal > 0 && durasiServoDetik > 0);

  char bufJam[6];
  String teksGabungan = "";
  bool adaJadwal = false;
  for (int i = 0; i < 3; i++) {
    if (jadwalJam[i] < 0 || jadwalMenit[i] < 0) continue;
    if (adaJadwal) teksGabungan += ", ";
    sprintf(bufJam, "%02d:%02d", jadwalJam[i], jadwalMenit[i]);
    teksGabungan += String(bufJam);
    adaJadwal = true;
  }
  if (!adaJadwal) {
    jadwalTeks = "(belum ada jadwal dari Firebase)";
  } else {
    teksGabungan += " (durasi " + String(durasiServoDetik) + " detik)";
    jadwalTeks = teksGabungan;
  }
}

bool kirimStatusPakan(bool aktif) {
  String status    = aktif ? "ON" : "OFF";
  String pathWaktu  = aktif ? "/Kontrol_Pakan/waktu_on" : "/Kontrol_Pakan/waktu_off";
  const int MAX_PERCOBAAN = 3;

  bool waktuValid     = waktuValidSekarang();
  String labelWaktu   = waktuValid ? buatWaktuLabelPolos() : "";

  for (int percobaan = 1; percobaan <= MAX_PERCOBAAN; percobaan++) {
    if (Firebase.ready()) {
      bool okStatus = Firebase.setString(fbdo, "/Kontrol_Pakan/status", status);

      bool okWaktu = true;
      if (waktuValid) {
        okWaktu = Firebase.setString(fbdo, pathWaktu, labelWaktu);
      }

      if (okStatus && okWaktu) return true;
    }
    if (percobaan < MAX_PERCOBAAN) delay(150);
  }
  return false;
}

void updateKocokServo() {
  if (millis() - waktuKocokTerakhir >= JEDA_KOCOK) {
    waktuKocokTerakhir = millis();
    if (posisiKananServo) {
      myServo.write(180);
    } else {
      myServo.write(0);
    }
    posisiKananServo = !posisiKananServo;
  }
}

void bunyikanBuzzer(int kali) {
  for (int i = 0; i < kali; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delay(200);
    digitalWrite(BUZZER_PIN, LOW);  delay(200);
  }
}

void tampilLCD(float suhu, float ph, float tds, float persen,
               bool isAlert, String alertMsg) {
  if (isAlert) {
    lcd.setCursor(0, 0);
    lcd.print("! STATUS BAHAYA !");
    lcd.setCursor(0, 1);
    String msg = alertMsg;
    while (msg.length() < 16) msg += " ";
    lcd.print(msg.substring(0, 16));
  } else {
    static unsigned long waktuHalaman = 0;
    static bool halamanSatu = true;

    if (millis() - waktuHalaman > 3000) {
      halamanSatu = !halamanSatu;
      waktuHalaman = millis();
      lcd.clear();
    }

    if (halamanSatu) {
      lcd.setCursor(0, 0);
      lcd.print("pH:"); lcd.print(ph, 1);
      lcd.print(" T:"); lcd.print(suhu, 1);
      lcd.print((char)223); lcd.print("C  ");

      lcd.setCursor(0, 1);
      lcd.print("TDS:"); lcd.print(tds, 0);
      lcd.print("ppm     ");
    } else {
      String jam = buatJamMenitSingkat();
      lcd.setCursor(0, 0);
      lcd.print("WIB: "); lcd.print(jam);
      lcd.print("          ");

      lcd.setCursor(0, 1);
      lcd.print("Sisa Pakan:"); lcd.print(persen, 0); lcd.print("%  ");
    }
  }
}

void kirimSensorNilai(String node, float nilai) {
  if (Firebase.ready()) {
    Firebase.setFloat(fbdo, "/Sensor/" + node, nilai);
  }
}

void cekJadwalPakan() {
  if (!kontrolPakanTersedia) {
    pakanSudahDiberikan = false;
    return;
  }

  if (!waktuValidSekarang()) return;

  int jamSekarang   = getJamSekarang();
  int menitSekarang = getMenitSekarang();
  long epochNow      = epochSekarang();

  long menitSekarangIndex = epochNow / 60;

  if (!servoAktif) {
    for (int i = 0; i < 3; i++) {
      if (jadwalJam[i] < 0 || jadwalMenit[i] < 0) continue;

      bool waktuCocok = (jamSekarang == jadwalJam[i] &&
                         menitSekarang == jadwalMenit[i]);
      bool belumPernahTrigger = (lastTriggerMinuteIndex[i] != menitSekarangIndex);

      if (waktuCocok && belumPernahTrigger) {
        lastTriggerMinuteIndex[i] = menitSekarangIndex;
        String keyLtm = "ltm" + String(i);
        prefs.putLong(keyLtm.c_str(), menitSekarangIndex);

        myServo.write(0);
        posisiKananServo   = false;
        waktuKocokTerakhir = millis();

        servoAktif      = true;
        servoMulaiEpoch = epochNow;
        prefs.putBool("servoAktif", true);
        prefs.putLong("mulaiEpoch", servoMulaiEpoch);

        if (!kirimStatusPakan(true)) {
          perluRecoveryOffPending = false;
        }
        bunyikanBuzzer(1);

        Serial.println(buatWaktuLabel() + " >> TRIGGER JADWAL PAKAN ke-" + String(i + 1) +
                        " (" + String(jadwalJam[i]) + ":" + String(jadwalMenit[i]) + ") - SERVO ON, durasi " +
                        String(durasiServoDetik) + " detik");

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("=== FEEDING ===");
        lcd.setCursor(0, 1);
        lcd.print("  SERVO: ON     ");
        break;
      }
    }
  } else {
    long sudahBerjalanDetik = epochNow - servoMulaiEpoch;
    if (sudahBerjalanDetik >= (long)durasiServoDetik) {
      servoAktif    = false;
      prefs.putBool("servoAktif", false);

      myServo.write(90);

      if (kirimStatusPakan(false)) {
        perluRecoveryOffPending = false;
      } else {
        perluRecoveryOffPending = true;
      }

      Serial.println(buatWaktuLabel() + " >> FEEDING SELESAI - SERVO OFF (berjalan " +
                      String(sudahBerjalanDetik) + " detik)");

      lcd.clear();
    }
  }

  pakanSudahDiberikan = servoAktif;
}

void loop() {
  unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED && !waktuValidSekarang() && !ntpSudahSinkron) {
    if (millis() - lastPaksaResyncNtpMs >= INTERVAL_PAKSA_RESYNC_NTP) {
      lastPaksaResyncNtpMs = millis();
      Serial.println(buatWaktuLabel() + " >> Waktu belum sinkron, coba NTP ulang...");
      mulaiNTP();
      delay(500);
      if (!waktuValidSekarang() && !ntpSudahSinkron) {
        Serial.println(">> Masih gagal, coba fallback HTTP...");
        ambilWaktuViaHTTP();
      }
    }
  }

  if (now - lastSerialClock >= INTERVAL_SERIAL_CLOCK) {
    lastSerialClock = now;
    Serial.println(buatWaktuLabel() + " Waktu berjalan...");
  }

  static unsigned long lastCobaRecoveryOff = 0;
  if (perluRecoveryOffPending && (millis() - lastCobaRecoveryOff >= 2000)) {
    lastCobaRecoveryOff = millis();
    if (kirimStatusPakan(false)) {
      perluRecoveryOffPending = false;
    }
  }

  if (!servoAktif && (now - lastSyncJadwal >= INTERVAL_SYNC_JADWAL)) {
    sinkronJadwalPakan();
    lastSyncJadwal = now;
  }

  cekJadwalPakan();

  if (servoAktif) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println(buatWaktuLabel() + " >> Servo aktif tapi WiFi belum tersambung - mencoba reconnect...");
      WiFi.reconnect();
    }
    updateKocokServo();
    delay(50);
    return;
  }

  float suhu   = bacaSuhu();
  float ph     = bacaPH();
  float tds    = bacaTDS(suhu);
  float persen = bacaPersenPakan();

  bool   isAlert  = false;
  String alertMsg = "";

  if      (suhu < SUHU_MIN)    { isAlert = true; alertMsg = "SUHU TERLALU LOW"; }
  else if (suhu > SUHU_MAX)    { isAlert = true; alertMsg = "SUHU TERLALU HIGH"; }
  else if (ph   < PH_MIN)      { isAlert = true; alertMsg = "pH TERLALU ASAM "; }
  else if (ph   > PH_MAX)      { isAlert = true; alertMsg = "pH TERLALU BASA "; }
  else if (tds  < TDS_MIN)     { isAlert = true; alertMsg = "PPM TERLALU LOW "; }
  else if (tds  > TDS_MAX)     { isAlert = true; alertMsg = "PPM TERLALU HIGH"; }
  else if (persen <= PAKAN_MIN){ isAlert = true; alertMsg = "PAKAN HAMPIR HABIS"; }

  if (!pakanSudahDiberikan) {
    if (isAlert && !alertSebelumnya) {
      digitalWrite(BUZZER_PIN, HIGH);
      buzzerAlertAktif   = true;
      buzzerAlertMulaiMs = millis();
    } else if (!isAlert) {
      digitalWrite(BUZZER_PIN, LOW);
      buzzerAlertAktif = false;
    }
  }
  alertSebelumnya = isAlert;

  if (buzzerAlertAktif && (millis() - buzzerAlertMulaiMs >= DURASI_BUZZER_ALERT_MS)) {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerAlertAktif = false;
  }

  tampilLCD(suhu, ph, tds, persen, isAlert, alertMsg);

  if (now - lastFirebaseSend >= INTERVAL_FIREBASE) {
    String waktuKirim = buatTimestampLengkap();
    Serial.println(buatWaktuLabel() + " >> Kirim data sensor ke Firebase (" + waktuKirim + ")");
    Serial.println("    Suhu_Air   = " + String(suhu, 1) + " C");
    Serial.println("    pH_Air     = " + String(ph, 2));
    Serial.println("    TDS        = " + String(tds, 0) + " ppm");
    Serial.println("    Sisa_Pakan = " + String(persen, 0) + " %");

    kirimSensorNilai("Suhu_Air",   suhu);
    kirimSensorNilai("pH_Air",     ph);
    kirimSensorNilai("TDS",        tds);
    kirimSensorNilai("Sisa_Pakan", persen);

    lastFirebaseSend = now;
  }

  delay(1000);
}