/*
  Cyberdeck Music Player — ESP32 DevKit V1
  DFPlayer Mini + IR remote + HT16K33 8x8 + GC9A01 round TFT

  Controls: 0-9 track entry, PLAY/PAUSE, PREV/NEXT, VOL-/+, MUTE,
  MODE shuffle, repeat, EQ, POWER sleep, USB/SCAN stats.

  Wiring:
    DFPlayer 5V/GND, TX->1k->D16, RX<-1k<-D17, SPK1/SPK2->speaker
    IR OUT->D27
    GC9A01 3V3: SCL D18, SDA(MOSI) D23, CS D5, DC D19, RST D4 (BL->3V3)
    8x8 5V: SDA D21, SCL D22 (DIP all OFF = I2C 0x70)

  Libs: DFRobotDFPlayerMini, IRremote v4, Arduino_GFX
*/

#include <DFRobotDFPlayerMini.h>
#include <IRremote.hpp>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_system.h>
#include <string.h>
#include <math.h>

const int IR_RECEIVE_PIN = 27;
const int TFT_CS = 5;
const int TFT_DC = 19;
const int TFT_RST = 4;
const int MATRIX_SDA = 21;
const int MATRIX_SCL = 22;
uint8_t MATRIX_I2C_ADDR = 0x70; // DIP A0/A1/A2; all OFF = 0x70
const int NUM_COLS = 8;

#define IR_0          0xE916FF00
#define IR_1          0xF30CFF00
#define IR_2          0xE718FF00
#define IR_3          0xA15EFF00
#define IR_4          0xF708FF00
#define IR_5          0xE31CFF00
#define IR_6          0xA55AFF00
#define IR_7          0xBD42FF00
#define IR_8          0xAD52FF00
#define IR_9          0xB54AFF00
#define IR_PLAY_PAUSE 0xBB44FF00
#define IR_PREV       0xBF40FF00
#define IR_NEXT       0xBC43FF00
#define IR_VOL_DOWN   0xEA15FF00
#define IR_VOL_UP     0xF609FF00
#define IR_MUTE       0xB847FF00
#define IR_MODE       0xB946FF00
#define IR_POWER      0xBA45FF00
#define IR_REPEAT     0xE619FF00
#define IR_EQ         0xF807FF00
#define IR_USB_SCAN   0xF20DFF00

DFRobotDFPlayerMini dfPlayer;
int currentVolume = 20; // 0-30
bool isPlaying = false;
bool muted = false;
bool repeatOn = false;
bool shuffleOn = false;
bool sleepMode = false;
bool wasPlayingBeforeSleep = false;
bool statsMode = false;
int currentTrack = 1;
int shuffleNextTrack = 1;
int currentEQ = 0;

const uint8_t STATS_MAGIC = 0xCD;
const uint8_t STATS_VERSION = 1;
const unsigned long LISTEN_FLUSH_MS = 60000;

struct StatsStore {
  uint8_t magic;
  uint8_t version;
  uint32_t listenSeconds;
  uint16_t trackPlays[77];
};

StatsStore stats;
Preferences statsPrefs;
unsigned long listenSecondMarkMs = 0;
unsigned long lastListenFlushMs = 0;
bool listenTimeDirty = false;

enum IrHoldAction { IR_HOLD_NONE, IR_HOLD_PREV, IR_HOLD_NEXT, IR_HOLD_VOL_DOWN, IR_HOLD_VOL_UP };
IrHoldAction lastIrHoldAction = IR_HOLD_NONE;
unsigned long lastIrRepeatMs = 0;
unsigned long irHoldStartMs = 0;
const unsigned long IR_HOLD_INITIAL_MS = 550;
const unsigned long IR_HOLD_TRACK_MS = 400;
const unsigned long IR_HOLD_VOL_MS = 140;
const unsigned long IR_FRESH_DEBOUNCE_MS = 280;
unsigned long lastFreshIrMs = 0;
unsigned long lastFreshIrCode = 0;

const char* eqNames[] = {"Normal", "Pop", "Rock", "Jazz", "Classic", "Bass"};
const uint8_t eqValues[] = {
  DFPLAYER_EQ_NORMAL, DFPLAYER_EQ_POP, DFPLAYER_EQ_ROCK,
  DFPLAYER_EQ_JAZZ, DFPLAYER_EQ_CLASSIC, DFPLAYER_EQ_BASS
};
const int totalEQs = 6;

// Synthetic 8x8 spectrum
const float BAR_ATTACK = 0.40f;
const float BAR_DECAY = 0.88f;
const float CAP_DECAY = 0.96f;
const int SOUND_MAX = 800;
const float AGC_FLOOR = 50.0f;
const float AGC_ATTACK = 0.12f;
const float AGC_RELEASE = 0.04f;
const float CONTRAST = 3.2f;
const float GATE = 0.12f;
const unsigned long VIS_FRAME_MS = 90;

// SD mp3/0001.mp3 … — track 33 is a missing gap
const char* trackNames[] = {
  "Growing Pains", "Dog Days", "Misuse, Oh", "Antlers",                                    // 1-4   Carpet Bed
  "Sunday Morning", "Casings", "Lilies", "Head in the Wall", "Knuckle Velvet",              // 5-9   Golden Age
  "Golden Age", "Selby Wall", "Child of Cain", "Sunday Morning (Demo)",                     // 10-13 Golden Age
  "Golden Age (Original Demo)", "Knuckle Velvet (Early Demo)",                              // 14-15 Unreleased
  "Powerline Valley (Early Demo)", "Powerline Valley (Piano Demo)", "Verona (Demo)",         // 16-18 Unreleased
  "Doe Hunting (Demo)", "Highway Horses", "Chapel Hill", "Death Rattle",                     // 19-22 Unreleased
  "Vultures", "Starvation", "Room 209",                                                      // 23-25 Unreleased
  "Arsony", "Eden", "Plague", "The Epitaph", "Churchyard", "Hospital Beds II",               // 26-31 Unreleased Two
  "Virginity (Piano Demo)",                                                                  // 32    Unreleased Two
  "(Missing Track)",                                                                         // 33    (no 0033 file)
  "Casey", "Eight Hour Days",                                                                // 34-35 Unreleased Two
  "Michelle Pfeiffer", "Crush", "God's Country", "Unpunishable", "Inbred",                   // 36-40 Inbred
  "Two-Headed Mother", "Crying During Sex", "Earnhardt", "Age of Delilah",                   // 41-44 Inbred
  "Michelle Pfeiffer (Solo)",                                                                // 45    Inbred
  "Family Tree (Intro)", "American Teenager", "A House in Nebraska", "Western Nights",       // 46-49 Preacher's Daughter
  "Family Tree", "Hard Times", "Thoroughfare", "Gibson Girl", "Ptolemaea",                   // 50-54 Preacher's Daughter
  "August Underground", "Televangelism", "Sun Bleached Flies", "Strangers",                  // 55-58 Preacher's Daughter
  "Perverts", "Punish", "Housofpsychoticwomn", "Vacillator", "Onanist",                       // 59-63 Perverts
  "Pulldrone", "Etienne", "Thatorchia", "Amber Waves",                                        // 64-67 Perverts
  "Janie", "Willoughby's Theme", "Fuck Me Eyes", "Nettles", "Willoughby's Interlude",         // 68-72 Willoughby Tucker
  "Dust Bowl", "A Knock at the Door", "Radio Towers", "Tempest", "Waco, Texas"                // 73-77 Willoughby Tucker
};
const int totalTracks = 77;

const char* albumNames[] = {
  "Carpet Bed", "Carpet Bed", "Carpet Bed", "Carpet Bed",                                    // 1-4
  "Golden Age", "Golden Age", "Golden Age", "Golden Age", "Golden Age",                      // 5-9
  "Golden Age", "Golden Age", "Golden Age", "Golden Age",                                    // 10-13
  "Unreleased", "Unreleased",                                                                 // 14-15
  "Unreleased", "Unreleased", "Unreleased",                                                   // 16-18
  "Unreleased", "Unreleased", "Unreleased", "Unreleased",                                     // 19-22
  "Unreleased", "Unreleased", "Unreleased",                                                   // 23-25
  "Unreleased Two", "Unreleased Two", "Unreleased Two", "Unreleased Two", "Unreleased Two", "Unreleased Two", // 26-31
  "Unreleased Two",                                                                            // 32
  "(N/A)",                                                                                    // 33 (no 0033 file)
  "Unreleased Two", "Unreleased Two",                                                          // 34-35
  "Inbred", "Inbred", "Inbred", "Inbred", "Inbred",                                            // 36-40
  "Inbred", "Inbred", "Inbred", "Inbred",                                                      // 41-44
  "Inbred",                                                                                    // 45
  "Preacher's Daughter", "Preacher's Daughter", "Preacher's Daughter", "Preacher's Daughter",  // 46-49
  "Preacher's Daughter", "Preacher's Daughter", "Preacher's Daughter", "Preacher's Daughter", "Preacher's Daughter", // 50-54
  "Preacher's Daughter", "Preacher's Daughter", "Preacher's Daughter", "Preacher's Daughter",  // 55-58
  "Perverts", "Perverts", "Perverts", "Perverts", "Perverts",                                  // 59-63
  "Perverts", "Perverts", "Perverts", "Perverts",                                              // 64-67
  "Willoughby Tucker, I'll Always Love You", "Willoughby Tucker, I'll Always Love You", "Willoughby Tucker, I'll Always Love You", "Willoughby Tucker, I'll Always Love You", "Willoughby Tucker, I'll Always Love You", // 68-72
  "Willoughby Tucker, I'll Always Love You", "Willoughby Tucker, I'll Always Love You", "Willoughby Tucker, I'll Always Love You", "Willoughby Tucker, I'll Always Love You", "Willoughby Tucker, I'll Always Love You" // 73-77
};

const char* artistNames[] = {
  "Ethel Cain", "Ethel Cain", "Ethel Cain", "Ethel Cain",                                    // 1-4
  "Ethel Cain", "Ethel Cain", "Ethel Cain", "Ethel Cain", "Ethel Cain",                      // 5-9
  "Ethel Cain", "Ethel Cain", "Ethel Cain", "Ethel Cain",                                    // 10-13
  "Ethel Cain", "Ethel Cain",                                                                 // 14-15
  "Ethel Cain", "Ethel Cain", "Ethel Cain",                                                   // 16-18
  "Ethel Cain", "Ethel Cain", "Ethel Cain", "Ethel Cain",                                     // 19-22
  "Ethel Cain", "Ethel Cain", "Ethel Cain",                                                   // 23-25
  "Ethel Cain", "Ethel Cain", "Ethel Cain", "Ethel Cain", "Ethel Cain", "Ethel Cain",         // 26-31
  "Ethel Cain",                                                                                // 32
  "(N/A)",                                                                                    // 33
  "Ethel Cain", "Ethel Cain",                                                                  // 34-35
  "Ethel Cain", "Ethel Cain", "Ethel Cain", "Ethel Cain", "Ethel Cain",                        // 36-40
  "Ethel Cain", "Ethel Cain", "Ethel Cain", "Ethel Cain",                                      // 41-44
  "Ethel Cain",                                                                                // 45
  "Ethel Cain", "Ethel Cain", "Ethel Cain", "Ethel Cain",                                      // 46-49
  "Ethel Cain", "Ethel Cain", "Ethel Cain", "Ethel Cain", "Ethel Cain",                        // 50-54
  "Ethel Cain", "Ethel Cain", "Ethel Cain", "Ethel Cain",                                      // 55-58
  "Ethel Cain", "Ethel Cain", "Ethel Cain", "Ethel Cain", "Ethel Cain",                        // 59-63
  "Ethel Cain", "Ethel Cain", "Ethel Cain", "Ethel Cain",                                      // 64-67
  "Ethel Cain", "Ethel Cain", "Ethel Cain", "Ethel Cain", "Ethel Cain",                        // 68-72
  "Ethel Cain", "Ethel Cain", "Ethel Cain", "Ethel Cain", "Ethel Cain"                         // 73-77
};

// MISO unused so DC can stay on GPIO19 (VSPI default MISO)
Arduino_DataBus *displayBus = new Arduino_HWSPI(
  TFT_DC, TFT_CS, 18 /* SCK */, 23 /* MOSI */, GFX_NOT_DEFINED /* MISO */);
Arduino_GFX *gfx = new Arduino_GC9A01(displayBus, TFT_RST, 0, true);

#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define COLOR_BLACK     RGB565(0, 0, 0)
#define COLOR_WHITE     RGB565(255, 255, 255)
#define COLOR_HUB_HOLE  RGB565(220, 220, 225)
#define COLOR_GROOVE    RGB565(90, 90, 100)

const int DISP_CX = 120;
const int DISP_CY = 120;

const int CD_R_OUTER = 118;
const int CD_R_OUTER2 = 114;
const int CD_R_FACE = 112;
const int CD_R_HUB = 38;
const int CD_R_HOLE = 14;
const int CD_R_EQ = 27;

const int TITLE_Y = 48;
const int TITLE_TEXT_SIZE = 2;
const int TITLE_MAX_W = 150;
const int TITLE_CLIP_X = DISP_CX - TITLE_MAX_W / 2;
const int TITLE_CHAR_H = 8 * TITLE_TEXT_SIZE;
const unsigned long MARQUEE_PAUSE_MS = 1200;
const unsigned long MARQUEE_STEP_MS = 50;
const int MARQUEE_STEP_PX = 2;
enum { MARQUEE_PAUSE_START, MARQUEE_SCROLL, MARQUEE_PAUSE_END };
int titleScrollPx = 0;
int marqueePhase = MARQUEE_PAUSE_START;
unsigned long marqueePhaseStart = 0;

char entryBuffer[4] = "";
byte entryLen = 0;
unsigned long lastDigitTime = 0;
const unsigned long AUTO_CONFIRM_MS = 1000;

bool matrixBlanked = false;
bool matrixPresent = false;

void ht16Cmd(uint8_t cmd) {
  Wire.beginTransmission(MATRIX_I2C_ADDR);
  Wire.write(cmd);
  Wire.endTransmission();
}

uint8_t probeHt16Address() {
  for (uint8_t addr = 0x70; addr <= 0x77; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      return addr;
    }
  }
  return 0;
}

void matrixTestPattern() {
  uint8_t bitmap[8][NUM_COLS];
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < NUM_COLS; col++) bitmap[row][col] = 1;
  }
  drawBitmap(bitmap);
  delay(400);
  clearMatrix();
}

void matrixBegin() {
  Wire.begin(MATRIX_SDA, MATRIX_SCL);
  Wire.setClock(100000);
  delay(20);

  uint8_t found = probeHt16Address();
  if (found == 0) {
    matrixPresent = false;
    Serial.println("8x8 matrix not found (check 5V, D21/D22, DIP).");
    return;
  }

  MATRIX_I2C_ADDR = found;
  matrixPresent = true;
  Serial.print("8x8 matrix at 0x");
  Serial.println(MATRIX_I2C_ADDR, HEX);

  ht16Cmd(0x21); // oscillator on
  ht16Cmd(0x81); // display on
  ht16Cmd(0xEF); // max brightness
  matrixTestPattern();
}

void showResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();
  const char* label;
  uint16_t color;
  switch (reason) {
    case ESP_RST_POWERON:   label = "POWERON";   color = RGB565(60, 60, 60);   break;
    case ESP_RST_BROWNOUT:  label = "BROWNOUT";  color = RGB565(255, 0, 0);    break;
    case ESP_RST_PANIC:     label = "PANIC";     color = RGB565(255, 0, 0);    break;
    case ESP_RST_INT_WDT:   label = "INT WDT";   color = RGB565(255, 120, 0);  break;
    case ESP_RST_TASK_WDT:  label = "TASK WDT";  color = RGB565(255, 120, 0);  break;
    case ESP_RST_WDT:       label = "WDT";       color = RGB565(255, 120, 0);  break;
    case ESP_RST_SW:        label = "SW RESET";  color = RGB565(255, 220, 0);  break;
    case ESP_RST_DEEPSLEEP: label = "DEEPSLEEP"; color = RGB565(0, 150, 255);  break;
    case ESP_RST_EXT:       label = "EXT/EN";    color = RGB565(0, 150, 255);  break;
    default:                label = "OTHER";     color = RGB565(150, 150, 150); break;
  }
  Serial.print("Reset reason: ");
  Serial.println(label);

  gfx->fillScreen(color);
  drawCenteredText(label, DISP_CY - 8, 2, COLOR_BLACK);
  delay(reason == ESP_RST_POWERON ? 400 : 2000); // linger longer on anything abnormal
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial2.begin(9600, SERIAL_8N1, 16, 17);

  IrReceiver.begin(IR_RECEIVE_PIN, ENABLE_LED_FEEDBACK);

  randomSeed(millis());

  if (!gfx->begin()) {
    Serial.println("Display init failed.");
  } else {
    gfx->fillScreen(COLOR_BLACK);
    showResetReason();
  }

  Serial.println("Initializing DFPlayer...");
  delay(1500); // let DFPlayer finish its own boot/SD mount before we talk to it

  bool dfPlayerReady = false;
  for (int attempt = 1; attempt <= 5 && !dfPlayerReady; attempt++) {
    if (dfPlayer.begin(Serial2)) {
      dfPlayerReady = true;
    } else {
      Serial.print("DFPlayer not found, attempt ");
      Serial.println(attempt);
      delay(1000);
    }
  }
  if (!dfPlayerReady) {
    Serial.println("DFPlayer not found after retries.");
    while (true) delay(1000);
  }

  dfPlayer.volume(currentVolume);
  delay(1000); // cold-boot settle before first play
  Serial.println("Ready.");
  loadStats();
  listenSecondMarkMs = millis();
  lastListenFlushMs = millis();
  playTrack(1);
  delay(200);
  dfPlayer.playMp3Folder(currentTrack); // first play often ignored cold

  delay(500); // let playback current settle before the matrix's power-on flash
  matrixBegin();
}

void loop() {
  handleIR();
  handleAutoConfirm();
  handleDFPlayerEvents();
  updateListenTime();
  updateVisualizer();
  updateTitleMarquee();
}

void handleIR() {
  if (!IrReceiver.decode()) return;

  unsigned long code = IrReceiver.decodedIRData.decodedRawData;
  bool isRepeat = (code == 0) ||
                  (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT);
  IrReceiver.resume();

  if (sleepMode) {
    if (!isRepeat && code == IR_POWER) {
      exitSleep();
    }
    return;
  }

  if (isRepeat) {
    if (lastIrHoldAction == IR_HOLD_NONE) return;
    if (millis() - irHoldStartMs < IR_HOLD_INITIAL_MS) return;
    unsigned long interval = (lastIrHoldAction == IR_HOLD_VOL_DOWN ||
                              lastIrHoldAction == IR_HOLD_VOL_UP)
                                 ? IR_HOLD_VOL_MS
                                 : IR_HOLD_TRACK_MS;
    if (millis() - lastIrRepeatMs < interval) return;
    switch (lastIrHoldAction) {
      case IR_HOLD_PREV: prevTrack(); break;
      case IR_HOLD_NEXT: nextTrack(); break;
      case IR_HOLD_VOL_DOWN: changeVolume(-1); break;
      case IR_HOLD_VOL_UP: changeVolume(1); break;
      default: break;
    }
    lastIrRepeatMs = millis();
    return;
  }

  if (code == 0) return;

  if ((code == IR_PREV || code == IR_NEXT || code == IR_VOL_DOWN || code == IR_VOL_UP) &&
      code == lastFreshIrCode &&
      (millis() - lastFreshIrMs) < IR_FRESH_DEBOUNCE_MS) {
    return;
  }

  lastIrHoldAction = IR_HOLD_NONE;

  switch (code) {
    case IR_0: enterDigit('0'); break;
    case IR_1: enterDigit('1'); break;
    case IR_2: enterDigit('2'); break;
    case IR_3: enterDigit('3'); break;
    case IR_4: enterDigit('4'); break;
    case IR_5: enterDigit('5'); break;
    case IR_6: enterDigit('6'); break;
    case IR_7: enterDigit('7'); break;
    case IR_8: enterDigit('8'); break;
    case IR_9: enterDigit('9'); break;

    case IR_PLAY_PAUSE: togglePlayPause(); break;
    case IR_PREV:
      lastFreshIrCode = code;
      lastFreshIrMs = millis();
      lastIrHoldAction = IR_HOLD_PREV;
      irHoldStartMs = millis();
      prevTrack();
      lastIrRepeatMs = millis();
      break;
    case IR_NEXT:
      lastFreshIrCode = code;
      lastFreshIrMs = millis();
      lastIrHoldAction = IR_HOLD_NEXT;
      irHoldStartMs = millis();
      nextTrack();
      lastIrRepeatMs = millis();
      break;
    case IR_VOL_DOWN:
      lastFreshIrCode = code;
      lastFreshIrMs = millis();
      lastIrHoldAction = IR_HOLD_VOL_DOWN;
      irHoldStartMs = millis();
      changeVolume(-1);
      lastIrRepeatMs = millis();
      break;
    case IR_VOL_UP:
      lastFreshIrCode = code;
      lastFreshIrMs = millis();
      lastIrHoldAction = IR_HOLD_VOL_UP;
      irHoldStartMs = millis();
      changeVolume(1);
      lastIrRepeatMs = millis();
      break;
    case IR_MUTE:
      muted = !muted;
      dfPlayer.volume(muted ? 0 : currentVolume);
      Serial.println(muted ? "Muted" : "Unmuted");
      updateDisplay();
      break;
    case IR_MODE: {
      shuffleOn = !shuffleOn;
      Serial.println(shuffleOn ? "Shuffle ON" : "Shuffle OFF");
      if (shuffleOn) {
        pickShuffleNext();
      }
      updateDisplay();
      break;
    }
    case IR_REPEAT:
      repeatOn = !repeatOn;
      Serial.println(repeatOn ? "Repeat ON" : "Repeat OFF");
      updateDisplay();
      break;
    case IR_EQ:
      currentEQ = (currentEQ + 1) % totalEQs;
      dfPlayer.EQ(eqValues[currentEQ]);
      Serial.print("EQ: ");
      Serial.println(eqNames[currentEQ]);
      updateDisplay();
      break;
    case IR_POWER:
      enterSleep();
      break;
    case IR_USB_SCAN:
      toggleStatsMode();
      break;
  }
}

void clearMatrix() {
  uint8_t blank[8][NUM_COLS];
  memset(blank, 0, sizeof(blank));
  drawBitmap(blank);
  matrixBlanked = true;
}

void enterSleep() {
  flushListenTime(true);
  statsMode = false;
  sleepMode = true;
  wasPlayingBeforeSleep = isPlaying;
  if (isPlaying) {
    dfPlayer.pause();
    isPlaying = false;
  }
  lastIrHoldAction = IR_HOLD_NONE;
  clearMatrix();
  drawSleepCdFace();
  Serial.println("Sleep");
}

void exitSleep() {
  sleepMode = false;
  updateDisplay();
  if (wasPlayingBeforeSleep) {
    dfPlayer.start();
    isPlaying = true;
  }
  listenSecondMarkMs = millis();
  matrixBlanked = false;
  Serial.println("Wake");
}

void enterDigit(char digit) {
  if (entryLen < 2) {
    entryBuffer[entryLen++] = digit;
    entryBuffer[entryLen] = '\0';
    Serial.print("Track entry: ");
    Serial.println(entryBuffer);
  }
  lastDigitTime = millis();
}

void handleAutoConfirm() {
  if (entryLen == 0) return;
  if (millis() - lastDigitTime < AUTO_CONFIRM_MS) return;

  int n = atoi(entryBuffer);
  if (n >= 1 && n <= totalTracks) {
    playTrack(n);
  } else {
    Serial.println("No such track.");
  }
  entryLen = 0;
  entryBuffer[0] = '\0';
}

void handleDFPlayerEvents() {
  if (sleepMode) return;
  if (!dfPlayer.available()) return;

  uint8_t type = dfPlayer.readType();
  if (type == DFPlayerPlayFinished) {
    recordTrackFinished(currentTrack);
    isPlaying = false;
    if (repeatOn) {
      playTrack(currentTrack);
    } else {
      nextTrack();
    }
  }
}

void playTrack(int n) {
  currentTrack = n;
  muted = false;
  dfPlayer.volume(currentVolume);
  dfPlayer.playMp3Folder(n);
  isPlaying = true;
  if (shuffleOn) {
    pickShuffleNext();
  }
  printNowPlaying();
  if (sleepMode) return;
  resetTitleMarquee();
  updateDisplay();
}

void pickShuffleNext() {
  do {
    shuffleNextTrack = random(1, totalTracks + 1);
  } while (shuffleNextTrack == 33 || shuffleNextTrack == currentTrack);
}

void nextTrack() {
  if (shuffleOn) {
    if (shuffleNextTrack < 1 || shuffleNextTrack > totalTracks ||
        shuffleNextTrack == 33 || shuffleNextTrack == currentTrack) {
      pickShuffleNext();
    }
    currentTrack = shuffleNextTrack;
  } else {
    do {
      currentTrack = (currentTrack % totalTracks) + 1;
    } while (currentTrack == 33);
  }
  playTrack(currentTrack);
}

void prevTrack() {
  do {
    currentTrack = ((currentTrack - 2 + totalTracks) % totalTracks) + 1;
  } while (currentTrack == 33);
  playTrack(currentTrack);
}

void togglePlayPause() {
  if (isPlaying) {
    dfPlayer.pause();
    Serial.println("Paused");
  } else {
    dfPlayer.start();
    Serial.println("Playing");
  }
  isPlaying = !isPlaying;
}

void changeVolume(int delta) {
  currentVolume = constrain(currentVolume + delta, 0, 30);
  muted = false;
  dfPlayer.volume(currentVolume);
  Serial.print("Volume: ");
  Serial.println(currentVolume);
  updateDisplay();
}

void printNowPlaying() {
  Serial.print("Now playing #");
  Serial.print(currentTrack);
  Serial.print(": ");
  Serial.println(trackNames[currentTrack - 1]);
}


void clearStatsRam() {
  memset(&stats, 0, sizeof(stats));
  stats.magic = STATS_MAGIC;
  stats.version = STATS_VERSION;
}

void saveStatsBlob() {
  stats.magic = STATS_MAGIC;
  stats.version = STATS_VERSION;
  statsPrefs.putBytes("blob", &stats, sizeof(stats));
  listenTimeDirty = false;
  lastListenFlushMs = millis();
}

void loadStats() {
  if (!statsPrefs.begin("cyberdeck", false)) {
    Serial.println("Stats: Preferences begin failed");
  }
  size_t len = statsPrefs.getBytesLength("blob");
  if (len == sizeof(stats)) {
    statsPrefs.getBytes("blob", &stats, sizeof(stats));
  } else {
    clearStatsRam();
  }
  if (stats.magic != STATS_MAGIC || stats.version != STATS_VERSION) {
    clearStatsRam();
    saveStatsBlob();
    Serial.println("Stats: NVS initialized");
  } else {
    Serial.print("Stats: loaded, listens=");
    Serial.println(stats.listenSeconds);
  }
}

void flushListenTime(bool force) {
  if (!force && !listenTimeDirty) return;
  saveStatsBlob();
}

void updateListenTime() {
  if (sleepMode || !isPlaying) {
    listenSecondMarkMs = millis();
    return;
  }
  unsigned long now = millis();
  while (now - listenSecondMarkMs >= 1000UL) {
    listenSecondMarkMs += 1000UL;
    if (stats.listenSeconds < 0xFFFFFFFFu) {
      stats.listenSeconds++;
      listenTimeDirty = true;
    }
  }
  if (listenTimeDirty && (now - lastListenFlushMs >= LISTEN_FLUSH_MS)) {
    flushListenTime(false);
  }
}

void recordTrackFinished(int n) {
  if (n < 1 || n > totalTracks || n == 33) return;
  if (stats.trackPlays[n - 1] < 65535u) {
    stats.trackPlays[n - 1]++;
  }
  saveStatsBlob();
  Serial.print("Stats: track ");
  Serial.print(n);
  Serial.print(" plays=");
  Serial.println(stats.trackPlays[n - 1]);
  if (statsMode) {
    drawStatsScreen();
  }
}

void toggleStatsMode() {
  statsMode = !statsMode;
  if (statsMode) {
    flushListenTime(true);
    drawStatsScreen();
    Serial.println("Stats ON");
  } else {
    updateDisplay();
    Serial.println("Stats OFF");
  }
}

uint32_t totalPlayCount() {
  uint32_t t = 0;
  for (int i = 0; i < totalTracks; i++) {
    t += stats.trackPlays[i];
  }
  return t;
}

int topTrackIndex() {
  int best = -1;
  uint16_t bestC = 0;
  for (int i = 0; i < totalTracks; i++) {
    if (i + 1 == 33) continue;
    if (stats.trackPlays[i] > bestC) {
      bestC = stats.trackPlays[i];
      best = i;
    }
  }
  return best;
}

void topAlbum(const char** nameOut, uint32_t* countOut) {
  *nameOut = "---";
  *countOut = 0;
  for (int i = 0; i < totalTracks; i++) {
    if (i + 1 == 33) continue;
    const char* alb = albumNames[i];
    bool seen = false;
    for (int j = 0; j < i; j++) {
      if (j + 1 == 33) continue;
      if (strcmp(albumNames[j], alb) == 0) {
        seen = true;
        break;
      }
    }
    if (seen) continue;

    uint32_t sum = 0;
    for (int k = 0; k < totalTracks; k++) {
      if (k + 1 == 33) continue;
      if (strcmp(albumNames[k], alb) == 0) {
        sum += stats.trackPlays[k];
      }
    }
    if (sum > *countOut) {
      *countOut = sum;
      *nameOut = alb;
    }
  }
}

void topArtist(const char** nameOut, uint32_t* countOut) {
  *nameOut = "---";
  *countOut = 0;
  for (int i = 0; i < totalTracks; i++) {
    if (i + 1 == 33) continue;
    const char* art = artistNames[i];
    bool seen = false;
    for (int j = 0; j < i; j++) {
      if (j + 1 == 33) continue;
      if (strcmp(artistNames[j], art) == 0) {
        seen = true;
        break;
      }
    }
    if (seen) continue;

    uint32_t sum = 0;
    for (int k = 0; k < totalTracks; k++) {
      if (k + 1 == 33) continue;
      if (strcmp(artistNames[k], art) == 0) {
        sum += stats.trackPlays[k];
      }
    }
    if (sum > *countOut) {
      *countOut = sum;
      *nameOut = art;
    }
  }
}

void formatListenTime(char* buf, int bufSize) {
  uint32_t s = stats.listenSeconds;
  uint32_t h = s / 3600UL;
  uint32_t m = (s % 3600UL) / 60UL;
  if (h > 0) {
    snprintf(buf, bufSize, "%luh%lum", (unsigned long)h, (unsigned long)m);
  } else {
    snprintf(buf, bufSize, "%lum", (unsigned long)m);
  }
}

void drawBitmap(const uint8_t bitmap[8][NUM_COLS]) {
  Wire.beginTransmission(MATRIX_I2C_ADDR);
  Wire.write((uint8_t)0x00);
  for (int row = 0; row < 8; row++) {
    uint8_t bits = 0;
    for (int col = 0; col < NUM_COLS; col++) {
      if (bitmap[row][col]) {
        bits |= (1 << col);
      }
    }
    Wire.write(bits);
    Wire.write((uint8_t)0x00);
  }
  Wire.endTransmission();
}

float columnBars[NUM_COLS] = {0};
float columnCaps[NUM_COLS] = {0};
float agcCeiling = 120.0f;
unsigned long lastVisFrameMs = 0;

float getSyntheticEnvelope() {
  unsigned long t = millis();
  float wave1 = sin(t * 0.003) * 0.5 + 0.5;
  float wave2 = sin(t * 0.011) * 0.3;
  float noise = (random(0, 100) / 100.0 - 0.5) * 0.4;
  return constrain(wave1 + wave2 + noise, 0.0, 1.0) * SOUND_MAX;
}

float sampleColumnLevels(float levels[NUM_COLS]) {
  float env = getSyntheticEnvelope();
  float overall = 0;
  for (int col = 0; col < NUM_COLS; col++) {
    float j = 0.55f + 0.45f * (random(0, 100) / 100.0f);
    levels[col] = env * j;
    overall += levels[col];
  }
  return overall / (float)NUM_COLS;
}

void stylizeSpectrum(float levels[NUM_COLS]) {
  float smooth[NUM_COLS];
  for (int i = 0; i < NUM_COLS; i++) {
    float left = levels[i > 0 ? i - 1 : i];
    float right = levels[i < NUM_COLS - 1 ? i + 1 : i];
    smooth[i] = levels[i] * 0.75f + left * 0.125f + right * 0.125f;
  }

  float mx = AGC_FLOOR;
  float mean = 0;
  for (int i = 0; i < NUM_COLS; i++) {
    if (smooth[i] > mx) mx = smooth[i];
    mean += smooth[i];
  }
  mean /= (float)NUM_COLS;

  if (mx > agcCeiling) {
    agcCeiling += (mx - agcCeiling) * AGC_ATTACK;
  } else {
    agcCeiling += (mx - agcCeiling) * AGC_RELEASE;
  }
  if (agcCeiling < AGC_FLOOR) agcCeiling = AGC_FLOOR;

  float meanN = mean / agcCeiling;
  for (int i = 0; i < NUM_COLS; i++) {
    float n = smooth[i] / agcCeiling;
    n = meanN + (n - meanN) * CONTRAST;
    if (n < GATE) {
      n = 0;
    } else {
      n = (n - GATE) / (1.0f - GATE);
      if (n < 0) n = 0;
      if (n > 1) n = 1;
      n = powf(n, 0.85f);
      float rows = n * 6.5f;
      if (n > 0.85f) {
        rows = 6.5f + (n - 0.85f) / 0.15f * 1.5f;
      }
      levels[i] = rows;
      continue;
    }
    levels[i] = 0;
  }
}

void updateVisualizer() {
  if (sleepMode || !matrixPresent) {
    return;
  }

  if (!isPlaying) {
    if (!matrixBlanked) {
      clearMatrix();
    }
    agcCeiling = 120.0f;
    for (int i = 0; i < NUM_COLS; i++) {
      columnBars[i] = 0;
      columnCaps[i] = 0;
    }
    return;
  }

  unsigned long now = millis();
  if (now - lastVisFrameMs < VIS_FRAME_MS) return;
  lastVisFrameMs = now;

  float levels[NUM_COLS];
  sampleColumnLevels(levels);
  stylizeSpectrum(levels);

  uint8_t bitmap[8][NUM_COLS];
  memset(bitmap, 0, sizeof(bitmap));

  for (int col = 0; col < NUM_COLS; col++) {
    float lvl = levels[col];
    if (lvl > columnBars[col]) {
      columnBars[col] += (lvl - columnBars[col]) * BAR_ATTACK;
    } else {
      columnBars[col] *= BAR_DECAY;
    }
    if (columnBars[col] > columnCaps[col]) {
      columnCaps[col] = columnBars[col];
    } else {
      columnCaps[col] *= CAP_DECAY;
    }

    int filledRows = constrain((int)(columnBars[col] + 0.5f), 0, 8);
    for (int row = 8 - filledRows; row < 8; row++) {
      bitmap[row][col] = 1;
    }

    int capH = constrain((int)(columnCaps[col] + 0.5f), 0, 8);
    if (capH > filledRows + 1) {
      bitmap[8 - capH][col] = 1;
    } else if (filledRows >= 2 && filledRows < 8) {
      bitmap[8 - filledRows - 1][col] = 1;
    }
  }

  drawBitmap(bitmap);
  matrixBlanked = false;
}

uint16_t cdRainbowAt(float t) {
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  const uint8_t stops[][3] = {
    {40, 210, 255},
    {90, 110, 255},
    {255, 70, 200},
    {255, 210, 50},
    {70, 240, 140},
    {40, 210, 255}
  };
  const int nSeg = 5;
  float scaled = t * nSeg;
  int i = (int)scaled;
  if (i >= nSeg) i = nSeg - 1;
  float f = scaled - i;
  uint8_t r = (uint8_t)(stops[i][0] + (stops[i + 1][0] - stops[i][0]) * f);
  uint8_t g = (uint8_t)(stops[i][1] + (stops[i + 1][1] - stops[i][1]) * f);
  uint8_t b = (uint8_t)(stops[i][2] + (stops[i + 1][2] - stops[i][2]) * f);
  return RGB565(r, g, b);
}

uint16_t brighten565(uint16_t c, float amount) {
  int r = ((c >> 11) & 0x1F) << 3;
  int g = ((c >> 5) & 0x3F) << 2;
  int b = (c & 0x1F) << 3;
  r = (int)(r + (255 - r) * amount);
  g = (int)(g + (255 - g) * amount);
  b = (int)(b + (255 - b) * amount);
  return RGB565(r, g, b);
}

uint16_t dim565(uint16_t c, float amount) {
  int r = ((c >> 11) & 0x1F) << 3;
  int g = ((c >> 5) & 0x3F) << 2;
  int b = (c & 0x1F) << 3;
  r = (int)(r * amount);
  g = (int)(g * amount);
  b = (int)(b * amount);
  return RGB565(r, g, b);
}

void drawCdRainbowFillScaled(float dimAmount) {
  const int step = (dimAmount < 1.0f) ? 2 : 3;
  for (int deg = 0; deg < 360; deg += step) {
    float a0 = deg * DEG_TO_RAD;
    float a1 = (deg + step) * DEG_TO_RAD;
    uint16_t c = cdRainbowAt(deg / 360.0f);
    if ((deg >= 200 && deg < 245) || (deg >= 20 && deg < 65)) {
      c = brighten565(c, 0.35f);
    }
    if (dimAmount < 1.0f) {
      c = dim565(c, dimAmount);
    }
    int x1 = DISP_CX + (int)(sin(a0) * CD_R_FACE);
    int y1 = DISP_CY - (int)(cos(a0) * CD_R_FACE);
    int x2 = DISP_CX + (int)(sin(a1) * CD_R_FACE);
    int y2 = DISP_CY - (int)(cos(a1) * CD_R_FACE);
    gfx->fillTriangle(DISP_CX, DISP_CY, x1, y1, x2, y2, c);
  }
  for (int r = 48; r < CD_R_FACE; r += 10) {
    gfx->drawCircle(DISP_CX, DISP_CY, r, COLOR_GROOVE);
  }
}

void drawCdRainbowFill() {
  drawCdRainbowFillScaled(1.0f);
}

void drawHubRing() {
  gfx->fillCircle(DISP_CX, DISP_CY, CD_R_HUB, COLOR_BLACK);
  gfx->fillCircle(DISP_CX, DISP_CY, CD_R_HOLE, COLOR_HUB_HOLE);
  gfx->drawCircle(DISP_CX, DISP_CY, CD_R_HOLE, COLOR_BLACK);
}

void drawHubEqChar(float angDeg, char ch) {
  float ang = angDeg * DEG_TO_RAD;
  int x = DISP_CX + (int)(sin(ang) * CD_R_EQ) - 3;
  int y = DISP_CY - (int)(cos(ang) * CD_R_EQ) - 3;
  gfx->setCursor(x, y);
  gfx->print(ch);
}

void drawHubEqText() {
  char name[12];
  snprintf(name, sizeof(name), "%s", eqNames[currentEQ]);
  for (char* p = name; *p; p++) {
    if (*p >= 'a' && *p <= 'z') *p = *p - 'a' + 'A';
  }

  const float charDeg = 14.0f;
  gfx->setTextColor(COLOR_WHITE);
  gfx->setTextSize(1);

  drawHubEqChar(-charDeg / 2.0f, 'E');
  drawHubEqChar(charDeg / 2.0f, 'Q');

  int nameLen = (int)strlen(name);
  if (nameLen < 1) return;
  float span = (nameLen - 1) * charDeg;
  float angDeg = 180.0f + span / 2.0f;
  for (int i = 0; i < nameLen; i++) {
    drawHubEqChar(angDeg, name[i]);
    angDeg -= charDeg;
  }
}

void drawCdDiscBackground() {
  gfx->fillScreen(COLOR_BLACK);
  drawCdRainbowFill();
  gfx->drawCircle(DISP_CX, DISP_CY, CD_R_OUTER, COLOR_BLACK);
  gfx->drawCircle(DISP_CX, DISP_CY, CD_R_OUTER2, COLOR_BLACK);
  drawHubRing();
}

void drawSleepCdFace() {
  gfx->fillScreen(COLOR_BLACK);
  drawCdRainbowFillScaled(0.55f);
  gfx->drawCircle(DISP_CX, DISP_CY, CD_R_OUTER, COLOR_BLACK);
  gfx->drawCircle(DISP_CX, DISP_CY, CD_R_OUTER2, COLOR_BLACK);
  drawHubRing();
}

int textPixelWidth(const char* text, int textSize) {
  return (int)strlen(text) * 6 * textSize;
}

void truncateToChars(const char* src, char* dst, int dstSize, int maxChars) {
  if (dstSize < 2 || maxChars < 1) {
    if (dstSize > 0) dst[0] = '\0';
    return;
  }
  int n = (int)strlen(src);
  if (n > maxChars) n = maxChars;
  if (n >= dstSize) n = dstSize - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

void drawCenteredText(const char* text, int y, int textSize, uint16_t color) {
  gfx->setTextColor(color);
  gfx->setTextSize(textSize);
  int w = textPixelWidth(text, textSize);
  gfx->setCursor(DISP_CX - w / 2, y);
  gfx->print(text);
}

void drawSizedCentered(const char* text, int y, int textSize, int maxWidth, uint16_t color) {
  char buf[40];
  int maxChars = maxWidth / (6 * textSize);
  if (maxChars < 1) maxChars = 1;
  if (maxChars > (int)sizeof(buf) - 1) maxChars = (int)sizeof(buf) - 1;
  truncateToChars(text, buf, sizeof(buf), maxChars);
  drawCenteredText(buf, y, textSize, color);
}

void drawSpacedCentered(const char* text, int cx, int y, int textSize, int letterGap, uint16_t color) {
  int n = (int)strlen(text);
  if (n < 1) return;
  int charW = 6 * textSize;
  int totalW = n * charW + (n - 1) * letterGap;
  int x = cx - totalW / 2;
  gfx->setTextColor(color);
  gfx->setTextSize(textSize);
  for (int i = 0; i < n; i++) {
    gfx->setCursor(x, y);
    gfx->print(text[i]);
    gfx->setCursor(x + 1, y);
    gfx->print(text[i]);
    x += charW + letterGap;
  }
}

void drawStackedTechLabel(int cx, int cy, const char* line1, const char* line2) {
  const int textSize = 1;
  const int letterGap = 2;
  const int lineH = 10;
  drawSpacedCentered(line1, cx, cy - lineH, textSize, letterGap, COLOR_BLACK);
  drawSpacedCentered(line2, cx, cy + 2, textSize, letterGap, COLOR_BLACK);
}

void drawRuleWithLabelAbove(const char* label, int lineY, int lineWidth) {
  char buf[36];
  int maxChars = 24;
  truncateToChars(label, buf, sizeof(buf), maxChars);
  drawCenteredText(buf, lineY - 12, 1, COLOR_BLACK);
  int x0 = DISP_CX - lineWidth / 2;
  for (int t = 0; t < 3; t++) {
    gfx->drawFastHLine(x0, lineY + t, lineWidth, COLOR_BLACK);
  }
}

void clearTitleBand() {
  int yMid = TITLE_Y + TITLE_CHAR_H / 2;
  for (int x = TITLE_CLIP_X; x < TITLE_CLIP_X + TITLE_MAX_W; x++) {
    int dx = x - DISP_CX;
    int dy = yMid - DISP_CY;
    float ang = atan2((float)dx, (float)(-dy));
    if (ang < 0) ang += 2.0f * (float)PI;
    float t = ang / (2.0f * (float)PI);
    uint16_t c = cdRainbowAt(t);
    int deg = (int)(t * 360.0f);
    if ((deg >= 200 && deg < 245) || (deg >= 20 && deg < 65)) {
      c = brighten565(c, 0.35f);
    }
    gfx->drawFastVLine(x, TITLE_Y, TITLE_CHAR_H, c);
  }
}

void resetTitleMarquee() {
  titleScrollPx = 0;
  marqueePhase = MARQUEE_PAUSE_START;
  marqueePhaseStart = millis();
}

void drawTitleMarqueeFrame() {
  const char* title = trackNames[currentTrack - 1];
  int fullW = textPixelWidth(title, TITLE_TEXT_SIZE);
  if (fullW <= TITLE_MAX_W) {
    drawCenteredText(title, TITLE_Y, TITLE_TEXT_SIZE, COLOR_BLACK);
    return;
  }

  clearTitleBand();
  int charW = 6 * TITLE_TEXT_SIZE;
  int x = TITLE_CLIP_X - titleScrollPx;
  gfx->setTextColor(COLOR_BLACK);
  gfx->setTextSize(TITLE_TEXT_SIZE);
  for (int i = 0; title[i]; i++) {
    if (x >= TITLE_CLIP_X && x + charW <= TITLE_CLIP_X + TITLE_MAX_W) {
      gfx->setCursor(x, TITLE_Y);
      gfx->print(title[i]);
    }
    x += charW;
  }
}

void updateTitleMarquee() {
  if (sleepMode || statsMode) return;

  const char* title = trackNames[currentTrack - 1];
  int fullW = textPixelWidth(title, TITLE_TEXT_SIZE);
  if (fullW <= TITLE_MAX_W) return;

  int maxScroll = fullW - TITLE_MAX_W;
  unsigned long now = millis();

  switch (marqueePhase) {
    case MARQUEE_PAUSE_START:
      if (now - marqueePhaseStart >= MARQUEE_PAUSE_MS) {
        marqueePhase = MARQUEE_SCROLL;
        marqueePhaseStart = now;
      }
      break;
    case MARQUEE_SCROLL:
      if (now - marqueePhaseStart >= MARQUEE_STEP_MS) {
        marqueePhaseStart = now;
        titleScrollPx += MARQUEE_STEP_PX;
        if (titleScrollPx >= maxScroll) {
          titleScrollPx = maxScroll;
          marqueePhase = MARQUEE_PAUSE_END;
        }
        drawTitleMarqueeFrame();
      }
      break;
    case MARQUEE_PAUSE_END:
      if (now - marqueePhaseStart >= MARQUEE_PAUSE_MS) {
        titleScrollPx = 0;
        marqueePhase = MARQUEE_PAUSE_START;
        marqueePhaseStart = now;
        drawTitleMarqueeFrame();
      }
      break;
  }
}

void drawStatsScreen() {
  drawCdDiscBackground();
  drawHubRing();

  drawSpacedCentered("STATS", DISP_CX, 22, 1, 2, COLOR_BLACK);

  drawCenteredText("TOP TRACK", 36, 1, COLOR_BLACK);
  int top = topTrackIndex();
  if (top >= 0 && stats.trackPlays[top] > 0) {
    drawSizedCentered(trackNames[top], 48, 1, 150, COLOR_BLACK);
    char cbuf[12];
    snprintf(cbuf, sizeof(cbuf), "%ux", (unsigned)stats.trackPlays[top]);
    drawCenteredText(cbuf, 60, 1, COLOR_BLACK);
  } else {
    drawCenteredText("---", 48, 1, COLOR_BLACK);
  }

  char playsNum[8];
  snprintf(playsNum, sizeof(playsNum), "%lu", (unsigned long)totalPlayCount());
  drawStackedTechLabel(42, DISP_CY, "PLAYS", playsNum);

  char timeBuf[12];
  formatListenTime(timeBuf, sizeof(timeBuf));
  drawStackedTechLabel(198, DISP_CY, "TIME", timeBuf);

  drawCenteredText("TOP ALBUM", 162, 1, COLOR_BLACK);
  const char* albName;
  uint32_t albCount;
  topAlbum(&albName, &albCount);
  if (albCount > 0) {
    drawSizedCentered(albName, 174, 1, 150, COLOR_BLACK);
    char cbuf[12];
    snprintf(cbuf, sizeof(cbuf), "%lux", (unsigned long)albCount);
    drawCenteredText(cbuf, 186, 1, COLOR_BLACK);
  } else {
    drawCenteredText("---", 174, 1, COLOR_BLACK);
  }

  drawCenteredText("TOP ARTIST", 198, 1, COLOR_BLACK);
  const char* artName;
  uint32_t artCount;
  topArtist(&artName, &artCount);
  if (artCount > 0) {
    drawSizedCentered(artName, 208, 1, 140, COLOR_BLACK);
    char cbuf[12];
    snprintf(cbuf, sizeof(cbuf), "%lux", (unsigned long)artCount);
    drawCenteredText(cbuf, 218, 1, COLOR_BLACK);
  } else {
    drawCenteredText("---", 208, 1, COLOR_BLACK);
  }
}

void updateDisplay() {
  if (sleepMode) {
    drawSleepCdFace();
    return;
  }
  if (statsMode) {
    drawStatsScreen();
    return;
  }

  drawCdDiscBackground();
  drawHubEqText();

  int prevIdx;
  int nextIdx;
  if (repeatOn) {
    prevIdx = currentTrack - 1;
    nextIdx = currentTrack - 1;
  } else if (shuffleOn) {
    prevIdx = (currentTrack - 2 + totalTracks) % totalTracks;
    if (prevIdx + 1 == 33) {
      prevIdx = (prevIdx - 1 + totalTracks) % totalTracks;
    }
    if (shuffleNextTrack < 1 || shuffleNextTrack > totalTracks) {
      pickShuffleNext();
    }
    nextIdx = shuffleNextTrack - 1;
  } else {
    prevIdx = (currentTrack - 2 + totalTracks) % totalTracks;
    nextIdx = currentTrack % totalTracks;
    if (prevIdx + 1 == 33) {
      prevIdx = (prevIdx - 1 + totalTracks) % totalTracks;
    }
    if (nextIdx + 1 == 33) {
      nextIdx = (nextIdx + 1) % totalTracks;
    }
  }

  drawTitleMarqueeFrame();
  drawSizedCentered(albumNames[currentTrack - 1], 72, 1, 140, COLOR_BLACK);

  char trackNum[8];
  snprintf(trackNum, sizeof(trackNum), "%02d", currentTrack);
  drawStackedTechLabel(42, DISP_CY, "TRACK", trackNum);

  char volNum[8];
  if (muted) {
    drawStackedTechLabel(198, DISP_CY, "VOL", "MUTE");
  } else {
    snprintf(volNum, sizeof(volNum), "%d", currentVolume);
    drawStackedTechLabel(198, DISP_CY, "VOL", volNum);
  }

  char prevLabel[36];
  char nextLabel[36];
  char prevName[24];
  char nextName[24];
  truncateToChars(trackNames[prevIdx], prevName, sizeof(prevName), 18);
  truncateToChars(trackNames[nextIdx], nextName, sizeof(nextName), 18);
  snprintf(prevLabel, sizeof(prevLabel), "PREV: %s", prevName);
  snprintf(nextLabel, sizeof(nextLabel), "NEXT: %s", nextName);
  drawRuleWithLabelAbove(prevLabel, 178, 150);
  drawRuleWithLabelAbove(nextLabel, 198, 110);
}


