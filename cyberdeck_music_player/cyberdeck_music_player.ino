/*
  Cyberdeck Music Player
  Arduino Uno R4 + DFPlayer Mini + speaker + IR remote + built-in 12x8
  LED matrix + sound sensor + RGB LED + round GC9A01 display

  Round display (CD disc theme on GC9A01 240x240):
    - Smooth rainbow CD diffraction fill + faint groove rings (not foil)
    - Dual thin black outer rings; thick black hub with spindle hole
    - Hub: white EQ preset text only (Normal/Pop/Rock/Jazz/Classic/Bass)
      — no bar/synth icons
    - Marker-style track title (top) + album (under); wide-spaced tech
      stacked TRACK/NN left and VOL right (CD-RW label feel)
    - Long track titles marquee-scroll in a clipped window (album stays
      static/truncated). Title-band only redraw — no full-CD flicker.
    - Sleep: muted (dimmed) rainbow CD with no labels; soft power-off
      also pauses audio until wake.
    - Stats (USB/SCAN): rainbow CD + hub; top track above, top album
      below; PLAYS/TIME as side stacks; top artist in the footer.
      EEPROM-backed; survives power-off. Music keeps playing.
    - Bottom: PREV: / NEXT: names above two tapered black rules
    - Full screen redraw on track/volume/EQ/mute change — Uno R4 lacks
      RAM for a 240x240 framebuffer, so brief flicker on redraw is
      expected. Album names come from the mp3 naming; track 33 is a
      placeholder gap on the card.

  The IR remote is the entire physical interface now. The joystick, the
  6-button analog ladder, the standalone Play/Pause button, and the
  rotary encoder were all removed after persistent flaky-connection
  issues - nothing functional was lost, since the remote already covers
  every one of those functions directly (including digit-based track
  selection, which replaces what the joystick's browsing mode used to do).

  LED matrix behavior:
    - Spectrum-only: 12 solid bars driven by the A4 sound sensor while
      playing (Beatbox-style analyzer on the mono 12x8 matrix — no color
      gradient on this hardware). Blank while paused / asleep.
    - No pause icon, volume bar, or track-number overlays on the matrix.

  RGB LED mood lighting: color matches current EQ preset, brightness
  pulses with the sound level, off while paused.

  Controls (remote - the only physical input now):
    0-9         type a 1 or 2 digit track number (auto-plays after ~1s pause)
    PLAY/PAUSE  play/pause
    PREV/NEXT   previous/next track (hold to keep stepping)
    VOL- / VOL+ volume (hold to keep adjusting)
    MUTE        toggle mute
    MODE        toggle Shuffle (does not jump tracks — current song
                keeps playing; NEXT label shows the pre-picked random
                upcoming track. Next and auto-advance-on-finish use that
                pick. Repeat takes priority if both are on. Default play
                is sequential through the list.)
    ⇄ (repeat)  toggle Repeat
    EQ          cycle EQ preset
    POWER       soft power-off / wake — muted rainbow CD (no labels),
                RGB + matrix + audio off; press again to wake and resume
                if it was playing. While asleep only POWER works.
    USB/SCAN    toggle stats page (top track/album, plays, listen time).
                Music and visualizer keep going; press again for CD face.

  Wiring:
    DFPlayer VCC/GND -> external 5V supply (NOT Arduino 5V), shared ground
      with Arduino. 100uF + 0.1uF ceramic across VCC/GND at the DFPlayer pins.
    DFPlayer TX  -> Arduino D0  (RX1, hardware serial - R4 only)
    DFPlayer RX  -> 1k resistor -> Arduino D1 (TX1)
    DFPlayer SPK1/SPK2 -> speaker

    IR receiver: VCC -> 5V, GND -> GND, OUT -> D9

    Sound sensor: VCC -> 5V, GND -> GND, AOUT -> A4
    RGB LED module: R -> D3, G -> D5, B -> D6, GND -> GND
      (if colors look inverted, swap each analogWrite value to 255-value)
      (D3 is free again now that the standalone Play/Pause button is gone)

    LED matrix: built into the board, no wiring needed.

    Round display (GC9A01): VCC -> 3.3V (check silkscreen before trying
      5V), GND -> GND, SCL/SCK -> D13, SDA/MOSI -> D11, RES/RST -> A3,
      DC -> D2, CS -> D10

    D4, D7, D8, D12, A0, A1, A2, A5 all free now too.

  SD card setup: folder named "mp3" at the root, files named 0001.mp3,
  0002.mp3 ... 9999.mp3 (extra text after the 4 digits is fine).
  On macOS: run `dot_clean /Volumes/YourCardName` if playback seems off.

  Libraries needed (Library Manager): DFRobotDFPlayerMini, IRremote (v4.x,
  by Armin Joachimsmeyer), GFX Library for Arduino (by moononournation).
  Arduino_LED_Matrix ships with the Uno R4 board core - no separate
  install, and ArduinoGraphics is NOT needed.
*/

#include <DFRobotDFPlayerMini.h>
#include <IRremote.hpp>
#include "Arduino_LED_Matrix.h"
#include <Arduino_GFX_Library.h>
#include <EEPROM.h>
#include <string.h>
#include <math.h>

const int IR_RECEIVE_PIN = 9;

// Button codes identified from this specific remote via the code-finder sketch
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
int shuffleNextTrack = 1; // pre-picked upcoming track while shuffle is on
int currentEQ = 0; // index into eqNames/eqValues below

// ---- Persistent listening stats (EEPROM) ----
const uint8_t STATS_MAGIC = 0xCD;
const uint8_t STATS_VERSION = 1;
const int STATS_EEPROM_ADDR = 0;
const unsigned long LISTEN_FLUSH_MS = 60000;

struct StatsStore {
  uint8_t magic;
  uint8_t version;
  uint32_t listenSeconds;
  uint16_t trackPlays[77];
};

StatsStore stats;
unsigned long listenSecondMarkMs = 0;
unsigned long lastListenFlushMs = 0;
bool listenTimeDirty = false;

// IR hold-to-repeat for PREV/NEXT/VOL (NEC repeat frames)
enum IrHoldAction { IR_HOLD_NONE, IR_HOLD_PREV, IR_HOLD_NEXT, IR_HOLD_VOL_DOWN, IR_HOLD_VOL_UP };
IrHoldAction lastIrHoldAction = IR_HOLD_NONE;
unsigned long lastIrRepeatMs = 0;
unsigned long irHoldStartMs = 0; // when the key was first pressed
const unsigned long IR_HOLD_INITIAL_MS = 550; // ignore NEC repeats until held this long
const unsigned long IR_HOLD_TRACK_MS = 400;   // then step tracks at this pace
const unsigned long IR_HOLD_VOL_MS = 140;
const unsigned long IR_FRESH_DEBOUNCE_MS = 280; // ignore duplicate "fresh" decodes of one tap
unsigned long lastFreshIrMs = 0;
unsigned long lastFreshIrCode = 0;

const char* eqNames[] = {"Normal", "Pop", "Rock", "Jazz", "Classic", "Bass"};
const uint8_t eqValues[] = {
  DFPLAYER_EQ_NORMAL, DFPLAYER_EQ_POP, DFPLAYER_EQ_ROCK,
  DFPLAYER_EQ_JAZZ, DFPLAYER_EQ_CLASSIC, DFPLAYER_EQ_BASS
};
const int totalEQs = 6;

// RGB color per EQ preset (0-255 each), mood-lighting ties to whichever
// preset is active
const uint8_t eqColors[6][3] = {
  {255, 255, 255}, // Normal - white
  {255, 60, 180},  // Pop - pink
  {255, 40, 20},   // Rock - red/orange
  {150, 60, 220},  // Jazz - purple
  {60, 100, 255},  // Classic - blue
  {255, 20, 20}    // Bass - deep red
};

// ---- Sound sensor, RGB LED ----
const int SOUND_PIN = A4;

// Real A4 mic module. Set false only for desk testing without the sensor
// (falls back to a synthetic envelope — not song-reactive).
const bool USE_REAL_SOUND_SENSOR = true;
const int RGB_R_PIN = 3;
const int RGB_G_PIN = 5;
const int RGB_B_PIN = 6;

int soundBaseline = 512;  // recalibrated in setup()
const float BAR_ATTACK = 0.40f;  // ease up — no instant blink to new height
const float BAR_DECAY = 0.88f;   // slow fall so neighbor gaps stay visible
const float CAP_DECAY = 0.96f;   // caps hang longer
const int SOUND_MAX = 200;       // base sensitivity; AGC adapts on top
const int SAMPLES_PER_COL = 8;   // ADC reads per column window each frame
const float AGC_FLOOR = 35.0f;   // don't over-amplify silence
const float AGC_ATTACK = 0.12f;  // slower — avoid slamming everything to full
const float AGC_RELEASE = 0.04f;
const float CONTRAST = 3.2f;     // strong neighbor height differences
const float GATE = 0.12f;        // normalized noise gate (quiet = empty)
const unsigned long VIS_FRAME_MS = 90; // ~11 fps — readable, not strobing

// Filenames on the SD card ("mp3" folder) are numbered to match these
// indices, e.g. 0001_carpet_bed_growing_pains.mp3 -> track 1. File 0033
// doesn't exist (gap in the source recordings), so index 33 is a
// placeholder - the DFPlayer will just no-op if that track is selected.
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

// Parallel artist credits (this library is all Ethel Cain — top artist is the bit)
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

// ---- Round display (GC9A01, 240x240) ----
#define TFT_CS   10
#define TFT_DC   2
#define TFT_RST  A3
Arduino_DataBus *displayBus = new Arduino_HWSPI(TFT_DC, TFT_CS);
Arduino_GFX *gfx = new Arduino_GC9A01(displayBus, TFT_RST, 0, true);

// RGB565 color macro - converts standard 0-255 RGB to the display's format
#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define COLOR_BLACK     RGB565(0, 0, 0)
#define COLOR_WHITE     RGB565(255, 255, 255)
#define COLOR_HUB_HOLE  RGB565(220, 220, 225)
#define COLOR_GROOVE    RGB565(90, 90, 100)

const int DISP_CX = 120;
const int DISP_CY = 120;

// CD face geometry (GC9A01 240x240)
const int CD_R_OUTER = 118;
const int CD_R_OUTER2 = 114;
const int CD_R_FACE = 112;
const int CD_R_HUB = 38;
const int CD_R_HOLE = 14;
const int CD_R_EQ = 27; // radius for hub EQ character placement

// Track title marquee (Spotify-style clip scroll; album stays static)
const int TITLE_Y = 48;
const int TITLE_TEXT_SIZE = 2;
const int TITLE_MAX_W = 150;
const int TITLE_CLIP_X = DISP_CX - TITLE_MAX_W / 2; // 45
const int TITLE_CHAR_H = 8 * TITLE_TEXT_SIZE;
const unsigned long MARQUEE_PAUSE_MS = 1200;
const unsigned long MARQUEE_STEP_MS = 50;
const int MARQUEE_STEP_PX = 2;
enum { MARQUEE_PAUSE_START, MARQUEE_SCROLL, MARQUEE_PAUSE_END };
int titleScrollPx = 0;
int marqueePhase = MARQUEE_PAUSE_START;
unsigned long marqueePhaseStart = 0;

// ---- Track number entry buffer (type digits, auto-confirms after a pause) ----
char entryBuffer[4] = "";
byte entryLen = 0;
unsigned long lastDigitTime = 0;
const unsigned long AUTO_CONFIRM_MS = 1000;

// ---- LED matrix ----
ArduinoLEDMatrix matrix;
bool matrixBlanked = false; // true after we cleared for pause/sleep

void setup() {
  Serial.begin(115200);
  Serial1.begin(9600);

  IrReceiver.begin(IR_RECEIVE_PIN, ENABLE_LED_FEEDBACK);

  matrix.begin();
  randomSeed(millis());

  if (!gfx->begin()) {
    Serial.println("Display init failed - check wiring!");
  } else {
    gfx->fillScreen(COLOR_BLACK);
  }

  pinMode(RGB_R_PIN, OUTPUT);
  pinMode(RGB_G_PIN, OUTPUT);
  pinMode(RGB_B_PIN, OUTPUT);

  // Quick baseline calibration - assumes it's quiet at boot (nothing
  // playing yet), averages some samples to find the sensor's resting level.
  // Skipped entirely if the real sensor isn't wired up.
  if (USE_REAL_SOUND_SENSOR) {
    long sum = 0;
    for (int i = 0; i < 50; i++) {
      sum += analogRead(SOUND_PIN);
      delay(2);
    }
    soundBaseline = sum / 50;
    Serial.print("Sound sensor baseline: ");
    Serial.println(soundBaseline);
  } else {
    Serial.println("Sound sensor not wired - using synthetic visualizer.");
  }

  Serial.println("Initializing DFPlayer...");
  if (!dfPlayer.begin(Serial1)) {
    Serial.println("DFPlayer not found. Check wiring and SD card.");
    while (true) delay(1000);
  }
  dfPlayer.volume(currentVolume);
  // Brief settle so the first playMp3Folder after begin() is reliable
  // (cold PREV/play right after boot was otherwise flaky).
  delay(300);
  Serial.println("Ready.");
  loadStats();
  listenSecondMarkMs = millis();
  lastListenFlushMs = millis();
  playTrack(1);
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

  // Sleep lockout: consume every code, but only POWER can wake.
  if (sleepMode) {
    if (!isRepeat && code == IR_POWER) {
      exitSleep();
    }
    return;
  }

  // NEC hold frames: scrub only PREV/NEXT/VOL while the matching key is held.
  // Require an initial hold before the first repeat so a quick tap can't
  // double-skip (full CD redraw often outlasts the first NEC repeat gap).
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
    // Stamp after the action so a slow display redraw doesn't eat the gap
    lastIrRepeatMs = millis();
    return;
  }

  if (code == 0) return; // garbage with no usable code

  // Duplicate fresh decode of the same tap (common with some remotes)
  if ((code == IR_PREV || code == IR_NEXT || code == IR_VOL_DOWN || code == IR_VOL_UP) &&
      code == lastFreshIrCode &&
      (millis() - lastFreshIrMs) < IR_FRESH_DEBOUNCE_MS) {
    return;
  }

  // Fresh press: only these four keys arm hold-repeat.
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
      // Stay on the current song; preview the upcoming pick on the NEXT label.
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
  uint8_t blank[8][12];
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
  setRGBColor(0, 0, 0, 0);
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

// Plays the entered track number once ~1 second has passed with no new
// digit press, so no separate "confirm" button is needed.
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

// Watches for the DFPlayer's "finished playing" event. Repeat replays
// the same track; otherwise advance (sequential by default, shuffle-aware).
void handleDFPlayerEvents() {
  if (sleepMode) return; // soft-off: ignore finish events while paused for sleep
  if (!dfPlayer.available()) return;

  uint8_t type = dfPlayer.readType();
  if (type == DFPlayerPlayFinished) {
    // Count this completion even if Repeat will play the same track again
    recordTrackFinished(currentTrack);
    isPlaying = false;
    if (repeatOn) {
      playTrack(currentTrack); // repeat takes priority over shuffle
    } else {
      nextTrack(); // sequential, or random when shuffle is on
    }
  }
}

void playTrack(int n) {
  currentTrack = n;
  dfPlayer.playMp3Folder(n);
  isPlaying = true;
  muted = false;
  if (shuffleOn) {
    pickShuffleNext();
  }
  printNowPlaying();
  // While asleep, keep the dim CD face — don't flash matrix or full UI.
  if (sleepMode) return;
  resetTitleMarquee();
  updateDisplay();
}

// Pick a random upcoming track for shuffle preview / next advance.
void pickShuffleNext() {
  do {
    shuffleNextTrack = random(1, totalTracks + 1);
  } while (shuffleNextTrack == 33 || shuffleNextTrack == currentTrack);
}

void nextTrack() {
  if (shuffleOn) {
    // Use the pre-picked track shown on the NEXT label
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

// ---------------- Persistent stats (EEPROM) ----------------

void clearStatsRam() {
  memset(&stats, 0, sizeof(stats));
  stats.magic = STATS_MAGIC;
  stats.version = STATS_VERSION;
}

void loadStats() {
  EEPROM.get(STATS_EEPROM_ADDR, stats);
  if (stats.magic != STATS_MAGIC || stats.version != STATS_VERSION) {
    clearStatsRam();
    EEPROM.put(STATS_EEPROM_ADDR, stats);
    Serial.println("Stats: EEPROM initialized");
  } else {
    Serial.print("Stats: loaded, listens=");
    Serial.println(stats.listenSeconds);
  }
}

void flushListenTime(bool force) {
  if (!force && !listenTimeDirty) return;
  stats.magic = STATS_MAGIC;
  stats.version = STATS_VERSION;
  EEPROM.put(STATS_EEPROM_ADDR, stats);
  listenTimeDirty = false;
  lastListenFlushMs = millis();
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
  stats.magic = STATS_MAGIC;
  stats.version = STATS_VERSION;
  EEPROM.put(STATS_EEPROM_ADDR, stats);
  listenTimeDirty = false;
  lastListenFlushMs = millis();
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

// Returns 0-based track index of most-played, or -1 if none.
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
  // Compact for the side tech stack (TRACK/VOL style)
  if (h > 0) {
    snprintf(buf, bufSize, "%luh%lum", (unsigned long)h, (unsigned long)m);
  } else {
    snprintf(buf, bufSize, "%lum", (unsigned long)m);
  }
}

// ---------------- LED matrix helpers ----------------

// Converts our readable uint8_t[8][12] bitmap (1 = lit pixel) into the
// packed uint32_t[3] format loadFrame() actually expects: 96 bits total,
// row-major, MSB-first, split across 3 words.
void bitmapToFrame(const uint8_t bitmap[8][12], uint32_t frame[3]) {
  frame[0] = frame[1] = frame[2] = 0;
  int bitIndex = 0;
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 12; col++) {
      if (bitmap[row][col]) {
        int wordIndex = bitIndex / 32;
        int bitInWord = 31 - (bitIndex % 32);
        frame[wordIndex] |= (1UL << bitInWord);
      }
      bitIndex++;
    }
  }
}

void drawBitmap(const uint8_t bitmap[8][12]) {
  uint32_t frame[3];
  bitmapToFrame(bitmap, frame);
  matrix.loadFrame(frame);
}

// ---------------- Audio-reactive visualizer + mood lighting ----------------

float columnBars[12] = {0};
float columnCaps[12] = {0};
float agcCeiling = 120.0f;
unsigned long lastVisFrameMs = 0;

// Synthetic envelope for desk testing without a mic (not song-reactive).
float getSyntheticEnvelope() {
  unsigned long t = millis();
  float wave1 = sin(t * 0.003) * 0.5 + 0.5;
  float wave2 = sin(t * 0.011) * 0.3;
  float noise = (random(0, 100) / 100.0 - 0.5) * 0.4;
  return constrain(wave1 + wave2 + noise, 0.0, 1.0) * SOUND_MAX;
}

// Raw column energies from successive ADC windows (or synthetic).
float sampleColumnLevels(float levels[12]) {
  float overall = 0;
  if (USE_REAL_SOUND_SENSOR) {
    for (int col = 0; col < 12; col++) {
      long sum = 0;
      for (int i = 0; i < SAMPLES_PER_COL; i++) {
        sum += abs(analogRead(SOUND_PIN) - soundBaseline);
      }
      levels[col] = (float)sum / (float)SAMPLES_PER_COL;
      overall += levels[col];
    }
  } else {
    float env = getSyntheticEnvelope();
    for (int col = 0; col < 12; col++) {
      float j = 0.55f + 0.45f * (random(0, 100) / 100.0f);
      levels[col] = env * j;
      overall += levels[col];
    }
  }
  return overall / 12.0f;
}

// Mid-fill EQ silhouette: strong neighbor contrast, peaks hit the top only
// on real hits — not a solid wall of LEDs.
void stylizeSpectrum(float levels[12]) {
  // Light neighbor blend (heavy blur kills the contrast we want)
  float smooth[12];
  for (int i = 0; i < 12; i++) {
    float left = levels[i > 0 ? i - 1 : i];
    float right = levels[i < 11 ? i + 1 : i];
    smooth[i] = levels[i] * 0.75f + left * 0.125f + right * 0.125f;
  }

  float mx = AGC_FLOOR;
  float mean = 0;
  for (int i = 0; i < 12; i++) {
    if (smooth[i] > mx) mx = smooth[i];
    mean += smooth[i];
  }
  mean /= 12.0f;

  if (mx > agcCeiling) {
    agcCeiling += (mx - agcCeiling) * AGC_ATTACK;
  } else {
    agcCeiling += (mx - agcCeiling) * AGC_RELEASE;
  }
  if (agcCeiling < AGC_FLOOR) agcCeiling = AGC_FLOOR;

  float meanN = mean / agcCeiling;
  for (int i = 0; i < 12; i++) {
    float n = smooth[i] / agcCeiling;
    n = meanN + (n - meanN) * CONTRAST;
    if (n < GATE) {
      n = 0;
    } else {
      n = (n - GATE) / (1.0f - GATE);
      if (n < 0) n = 0;
      if (n > 1) n = 1;
      n = powf(n, 0.85f);
      // Most music sits mid-matrix; only strong columns reach the top
      float rows = n * 6.5f;
      if (n > 0.85f) {
        rows = 6.5f + (n - 0.85f) / 0.15f * 1.5f; // 6.5 .. 8
      }
      levels[i] = rows;
      continue;
    }
    levels[i] = 0;
  }
}

void updateVisualizer() {
  if (sleepMode) {
    setRGBColor(0, 0, 0, 0);
    return;
  }

  if (!isPlaying) {
    if (!matrixBlanked) {
      clearMatrix();
    }
    agcCeiling = 120.0f;
    for (int i = 0; i < 12; i++) {
      columnBars[i] = 0;
      columnCaps[i] = 0;
    }
    setRGBColor(0, 0, 0, 0);
    return;
  }

  // Cap redraw rate so the EQ morphs instead of strobing every loop
  unsigned long now = millis();
  if (now - lastVisFrameMs < VIS_FRAME_MS) return;
  lastVisFrameMs = now;

  float levels[12];
  float baseLevel = sampleColumnLevels(levels);
  stylizeSpectrum(levels);

  uint8_t bitmap[8][12];
  memset(bitmap, 0, sizeof(bitmap));

  for (int col = 0; col < 12; col++) {
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

    // Floating peak tip with a gap — classic analyzer silhouette
    int capH = constrain((int)(columnCaps[col] + 0.5f), 0, 8);
    if (capH > filledRows + 1) {
      bitmap[8 - capH][col] = 1;
    } else if (filledRows >= 2 && filledRows < 8) {
      bitmap[8 - filledRows - 1][col] = 1;
    }
  }

  drawBitmap(bitmap);
  matrixBlanked = false;

  int clampedBase = constrain((int)baseLevel, 0, SOUND_MAX);
  int soundBrightness = map(clampedBase, 0, SOUND_MAX, 50, 255);
  float finalScale = soundBrightness / 255.0f;
  setRGBColor(eqColors[currentEQ][0], eqColors[currentEQ][1],
              eqColors[currentEQ][2], finalScale);
}

void setRGBColor(uint8_t r, uint8_t g, uint8_t b, float brightnessScale) {
  analogWrite(RGB_R_PIN, (int)(r * brightnessScale));
  analogWrite(RGB_G_PIN, (int)(g * brightnessScale));
  analogWrite(RGB_B_PIN, (int)(b * brightnessScale));
}

// ---------------- Round display (CD disc) ----------------

// ---- Round CD disc display helpers ----

// Interpolate a smooth CD-style rainbow (cyan → blue → magenta → gold → green).
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

// Soft shine: brighten an RGB565 color slightly toward white.
uint16_t brighten565(uint16_t c, float amount) {
  int r = ((c >> 11) & 0x1F) << 3;
  int g = ((c >> 5) & 0x3F) << 2;
  int b = (c & 0x1F) << 3;
  r = (int)(r + (255 - r) * amount);
  g = (int)(g + (255 - g) * amount);
  b = (int)(b + (255 - b) * amount);
  return RGB565(r, g, b);
}

// Darken for sleep-face muted rainbow.
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
  // Finer wedges when dimmed so RGB565 steps look less blocky on sleep.
  const int step = (dimAmount < 1.0f) ? 2 : 3;
  for (int deg = 0; deg < 360; deg += step) {
    float a0 = deg * DEG_TO_RAD;
    float a1 = (deg + step) * DEG_TO_RAD;
    uint16_t c = cdRainbowAt(deg / 360.0f);
    // Soft specular highlights: bottom-left + mirrored top-right
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
  // Faint concentric grooves
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

// White EQ text on hub — EQ centered on top arc, preset centered on bottom.
// Glyphs stay upright (GFX can't rotate chars); each arc is centered on its own.
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

  // Top: "EQ" centered on angle 0
  drawHubEqChar(-charDeg / 2.0f, 'E');
  drawHubEqChar(charDeg / 2.0f, 'Q');

  // Bottom: preset centered on angle 180, left-to-right
  // (angles increase clockwise: left of bottom is 180+span/2, right is 180-span/2)
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

// Same CD rainbow as active UI (including light wedges), globally dimmed; no labels.
void drawSleepCdFace() {
  gfx->fillScreen(COLOR_BLACK);
  drawCdRainbowFillScaled(0.55f);
  gfx->drawCircle(DISP_CX, DISP_CY, CD_R_OUTER, COLOR_BLACK);
  gfx->drawCircle(DISP_CX, DISP_CY, CD_R_OUTER2, COLOR_BLACK);
  drawHubRing();
}

// Default font ≈ 6px wide per char per textSize unit.
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

// Centered label at a fixed text size, truncated to maxWidth.
void drawSizedCentered(const char* text, int y, int textSize, int maxWidth, uint16_t color) {
  char buf[40];
  int maxChars = maxWidth / (6 * textSize);
  if (maxChars < 1) maxChars = 1;
  if (maxChars > (int)sizeof(buf) - 1) maxChars = (int)sizeof(buf) - 1;
  truncateToChars(text, buf, sizeof(buf), maxChars);
  drawCenteredText(buf, y, textSize, color);
}

// Wide-spaced tech label (CD-RW style), centered on cx. Fake-bold via 1px offset.
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

// Stacked TRACK/NN or VOL/value — same size, centered in left/right wedge.
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
  // 3px thick rule
  for (int t = 0; t < 3; t++) {
    gfx->drawFastHLine(x0, lineY + t, lineWidth, COLOR_BLACK);
  }
}

// Restore the rainbow CD face under the title clip window (cheap band redraw).
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

// Draw the current track title: centered if it fits, else clipped + scrolled.
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
    // Only draw glyphs fully inside the clip so edges stay clean.
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

// Stats face: rainbow CD + bare hub. Track above, album below; PLAYS/TIME
// flank the hub like TRACK/VOL; top artist sits in the bottom footer.
void drawStatsScreen() {
  drawCdDiscBackground();
  drawHubRing();

  drawSpacedCentered("STATS", DISP_CX, 22, 1, 2, COLOR_BLACK);

  // --- Top track (above hub) ---
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

  // --- PLAYS / TIME stacks (same wedges as TRACK / VOL) ---
  char playsNum[8];
  snprintf(playsNum, sizeof(playsNum), "%lu", (unsigned long)totalPlayCount());
  drawStackedTechLabel(42, DISP_CY, "PLAYS", playsNum);

  char timeBuf[12];
  formatListenTime(timeBuf, sizeof(timeBuf));
  drawStackedTechLabel(198, DISP_CY, "TIME", timeBuf);

  // --- Top album (below hub, same band as before) ---
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

  // --- Top artist (footer where PLAYS/TIME lines used to be) ---
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

// Full redraw of the CD face for current track/volume/EQ/prev/next.
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
    // Repeat: prev/next both show the track that will play again
    prevIdx = currentTrack - 1;
    nextIdx = currentTrack - 1;
  } else if (shuffleOn) {
    prevIdx = (currentTrack - 2 + totalTracks) % totalTracks;
    if (prevIdx + 1 == 33) {
      prevIdx = (prevIdx - 1 + totalTracks) % totalTracks;
    }
    // Show the pre-picked random upcoming track
    if (shuffleNextTrack < 1 || shuffleNextTrack > totalTracks) {
      pickShuffleNext();
    }
    nextIdx = shuffleNextTrack - 1;
  } else {
    prevIdx = (currentTrack - 2 + totalTracks) % totalTracks;
    nextIdx = currentTrack % totalTracks;
    // Skip the missing placeholder in the preview labels
    if (prevIdx + 1 == 33) {
      prevIdx = (prevIdx - 1 + totalTracks) % totalTracks;
    }
    if (nextIdx + 1 == 33) {
      nextIdx = (nextIdx + 1) % totalTracks;
    }
  }

  // Track title large on top (marquee if needed); album smaller underneath
  drawTitleMarqueeFrame();
  drawSizedCentered(albumNames[currentTrack - 1], 72, 1, 140, COLOR_BLACK);

  // Tech stacks flanking the hub
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

  // PREV: / NEXT: above tapered rules
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


