/*
  WHO'S HOME — hook buttons + visitor counter + guest greeting
  --------------------------------------------------------------
  Hook Button 1 (pin 2) -> continuously LOW while YOUR keys hang
  Hook Button 2 (pin 4) -> same, for your ROOMMATE
  Visitor Button (pin 9) -> each press increments the visitor count
  Greeting Button (pin 3) -> shows a random fun message on the LCD
                              for guests to press themselves
*/

#include <LiquidCrystal.h>

#define HOOK1_PIN 2
#define HOOK2_PIN 4
#define VISITOR_PIN 9
#define GREETING_PIN 3
#define RGB_R 5
#define RGB_G 6
#define RGB_B 11

// Display / shift register pins
#define SHIFT_DATA 10
#define SHIFT_CLOCK A2
#define SHIFT_LATCH A3
#define DIGIT3_PIN A4   // 3rd digit (tens place)
#define DIGIT4_PIN A5   // 4th/rightmost digit (ones place)

LiquidCrystal lcd(7, 8, 12, 13, A0, A1);

bool youHome = false;
bool roommateHome = false;
int visitorCount = 0;

const char* nameLine1 = "Audrey:";
const char* nameLine2 = "Crystal:";

char lineBuffer1[17];
char lineBuffer2[17];

// Tracks what's currently shown on each row so flips spin FORWARD
// from wherever each column last stopped, like a real mechanical board
char currentDisplay[2][17];

bool lastYouHome = false;
bool lastRoommateHome = false;

unsigned long lastVisitorPress = 0;
const unsigned long VISITOR_DEBOUNCE_MS = 500;

unsigned long lastGreetingPress = 0;
const unsigned long GREETING_DEBOUNCE_MS = 500;

// Short, fun messages for guests -- keep each line 16 characters or less
const char* greetings[][2] = {
  {"Hook 'em Horns!"},
  {"Go Horns Go!"},
  {"Alright alright", "alright!"},
  {"Texas Fight!"},
  {"Keep Austin", "weird!"},
  {"Y'all come back", "now!"},
  {"You're always", "welcome here!"},
  {"Thanks for", "stopping by!"},
};
const int NUM_GREETINGS = 8;

// Standard common-cathode 7-segment patterns, bit0=A ... bit6=G, bit7=DP
const byte digitPatterns[10] = {
  0x3F, 0x06, 0x5B, 0x4F, 0x66,
  0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

void setup() {
  Serial.begin(9600);

  pinMode(HOOK1_PIN, INPUT_PULLUP);
  pinMode(HOOK2_PIN, INPUT_PULLUP);
  pinMode(VISITOR_PIN, INPUT_PULLUP);
  pinMode(GREETING_PIN, INPUT_PULLUP);

  pinMode(RGB_R, OUTPUT);
  pinMode(RGB_G, OUTPUT);
  pinMode(RGB_B, OUTPUT);

  pinMode(SHIFT_DATA, OUTPUT);
  pinMode(SHIFT_CLOCK, OUTPUT);
  pinMode(SHIFT_LATCH, OUTPUT);
  pinMode(DIGIT3_PIN, OUTPUT);
  pinMode(DIGIT4_PIN, OUTPUT);
  digitalWrite(DIGIT3_PIN, HIGH); // HIGH = that digit OFF
  digitalWrite(DIGIT4_PIN, HIGH);

  lcd.begin(16, 2);
  strcpy(currentDisplay[0], "                ");
  strcpy(currentDisplay[1], "                ");

  youHome = (digitalRead(HOOK1_PIN) == LOW);
  roommateHome = (digitalRead(HOOK2_PIN) == LOW);
  lastYouHome = youHome;
  lastRoommateHome = roommateHome;

  drawFullBoard();
  updateRGB();
}

void loop() {
  // --- Hook buttons: continuous read, animate only on change ---
  bool currentYouHome = (digitalRead(HOOK1_PIN) == LOW);
  bool currentRoommateHome = (digitalRead(HOOK2_PIN) == LOW);

  if (currentYouHome != lastYouHome) {
    delay(30);
    currentYouHome = (digitalRead(HOOK1_PIN) == LOW);
    if (currentYouHome != lastYouHome) {
      youHome = currentYouHome;
      lastYouHome = currentYouHome;
      buildStatusText(lineBuffer1, nameLine1, youHome);
      flipLine(0, lineBuffer1);
      updateRGB();
    }
  }

  if (currentRoommateHome != lastRoommateHome) {
    delay(30);
    currentRoommateHome = (digitalRead(HOOK2_PIN) == LOW);
    if (currentRoommateHome != lastRoommateHome) {
      roommateHome = currentRoommateHome;
      lastRoommateHome = currentRoommateHome;
      buildStatusText(lineBuffer2, nameLine2, roommateHome);
      flipLine(1, lineBuffer2);
      updateRGB();
    }
  }

  // --- Visitor button: single press = +1, debounced ---
  if (digitalRead(VISITOR_PIN) == LOW) {
    unsigned long now = millis();
    if (now - lastVisitorPress > VISITOR_DEBOUNCE_MS) {
      lastVisitorPress = now;
      visitorCount++;
      if (visitorCount > 99) visitorCount = 0;
      Serial.print("Visitor count: ");
      Serial.println(visitorCount);
    }
  }

  // --- Greeting button: single press = show random message ---
  if (digitalRead(GREETING_PIN) == LOW) {
    unsigned long now = millis();
    if (now - lastGreetingPress > GREETING_DEBOUNCE_MS) {
      lastGreetingPress = now;
      showGreeting();
      drawFullBoard(); // return to status view after
    }
  }

  // --- Keep the display refreshed every loop pass ---
  updateDisplay();
}

void showGreeting() {
  randomSeed(micros()); // reseed using the exact moment of this press
  int index = random(NUM_GREETINGS);
  Serial.print("Greeting index: ");
  Serial.println(index);
  flipLine(0, greetings[index][0]);
  delay(150);
  flipLine(1, greetings[index][1]);
  delay(3000);
}

void buildStatusText(char* buffer, const char* label, bool home) {
  buffer[0] = '\0';
  strncat(buffer, label, 16);
  strncat(buffer, " ", 16 - strlen(buffer));
  strncat(buffer, home ? "Home" : "Out", 16 - strlen(buffer));
  int len = strlen(buffer);
  for (int i = len; i < 16; i++) {
    buffer[i] = ' ';
  }
  buffer[16] = '\0';
}

void drawFullBoard() {
  buildStatusText(lineBuffer1, nameLine1, youHome);
  buildStatusText(lineBuffer2, nameLine2, roommateHome);
  flipLine(0, lineBuffer1);
  delay(150);
  flipLine(1, lineBuffer2);
}

void flipLine(int row, const char* target) {
  // The "wheel" order every column spins through, same as a real
  // split-flap display -- space first, then letters, then numbers,
  // then common punctuation used in our messages.
  const char charset[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!'?,.:()";
  int charsetLen = strlen(charset);

  // Always animate the FULL 16 columns, padding shorter messages with
  // spaces -- otherwise leftover characters from a longer previous
  // line never get touched and stay stuck on screen
  char paddedTarget[17];
  strncpy(paddedTarget, target, 16);
  int rawLen = strlen(target);
  for (int i = rawLen; i < 16; i++) paddedTarget[i] = ' ';
  paddedTarget[16] = '\0';

  int startIndex[16];
  int stepsNeeded[16];
  int maxSteps = 0;

  for (int col = 0; col < 16; col++) {
    char currentChar = currentDisplay[row][col];

    int startIdx = 0;
    int targetIdx = 0;
    for (int i = 0; i < charsetLen; i++) {
      if (charset[i] == currentChar) startIdx = i;
      if (charset[i] == paddedTarget[col]) targetIdx = i;
    }

    startIndex[col] = startIdx;
    int steps = (targetIdx - startIdx + charsetLen) % charsetLen;
    stepsNeeded[col] = steps;
    if (steps > maxSteps) maxSteps = steps;
  }

  for (int step = 0; step <= maxSteps; step++) {
    lcd.setCursor(0, row);
    for (int col = 0; col < 16; col++) {
      int actualStep = min(step, stepsNeeded[col]);
      int charIdx = (startIndex[col] + actualStep) % charsetLen;
      lcd.print(charset[charIdx]);
    }
    delay(40);
  }

  // final clean draw, just to be safe
  lcd.setCursor(0, row);
  lcd.print(paddedTarget);

  // remember what's now on screen for next time
  strcpy(currentDisplay[row], paddedTarget);
}

void updateRGB() {
  if (youHome && roommateHome) {
    setRGB(0, 255, 0);
  } else if (youHome || roommateHome) {
    setRGB(0, 0, 255);
  } else {
    setRGB(255, 0, 0);
  }
}

void setRGB(int r, int g, int b) {
  analogWrite(RGB_R, r);
  analogWrite(RGB_G, g);
  analogWrite(RGB_B, b);
}

// Sends one digit's segment pattern to the shift register
void showDigit(byte pattern) {
  digitalWrite(SHIFT_LATCH, LOW);
  shiftOut(SHIFT_DATA, SHIFT_CLOCK, MSBFIRST, pattern);
  digitalWrite(SHIFT_LATCH, HIGH);
}

// Rapidly alternates the two digits so both appear lit at once
void updateDisplay() {
  int tens = (visitorCount / 10) % 10;
  int ones = visitorCount % 10;

  showDigit(digitPatterns[tens]);
  digitalWrite(DIGIT4_PIN, HIGH); // make sure ones digit is off
  digitalWrite(DIGIT3_PIN, LOW);  // turn tens digit on
  delay(5);
  digitalWrite(DIGIT3_PIN, HIGH); // off

  showDigit(digitPatterns[ones]);
  digitalWrite(DIGIT4_PIN, LOW);  // turn ones digit on
  delay(5);
  digitalWrite(DIGIT4_PIN, HIGH); // off
}