/*
  Audio mode LED project - RGB strip + MAX7219 animation version

  This version is for a 5V analog RGB LED strip with:
  R -> D9
  G -> D10
  B -> D11

  MAX7219 pins:
  DIN -> D4
  CLK -> D5
  CS  -> D7

  Important:
  The RGB strip stays OFF until the system is ON and a mode is selected.
  The KY-037 volume controls strip brightness and color across a wide palette.
  The MAX7219 shows the mode name for 5 seconds, then plays a unique animation.
*/

#include <SoftwareSerial.h>

struct RgbColor {
  byte red;
  byte green;
  byte blue;
};

// -------------------- MAX7219 32x8 matrix --------------------
#define MATRIX_MAX_DEVICES 4

// Software SPI pins for MAX7219.
#define MATRIX_DATA_PIN 4
#define MATRIX_CLK_PIN 5
#define MATRIX_CS_PIN 7

// Matrix mapping test mode.
// Send M0, M1, ... M15 from Bluetooth to try all MAX7219 8x8 mappings.
// Default M10 usually matches 32x8 modules chained from right to left:
// - reverse the 4 module order
// - reverse columns inside each 8x8 module
byte currentMatrixMap = 10;

bool matrixReverseX = false;
bool matrixReverseY = false;

// -------------------- HC-06 Bluetooth --------------------
#define BT_RX_PIN 2
#define BT_TX_PIN 3

// -------------------- KY-037 sound sensor --------------------
#define SOUND_PIN A0

// -------------------- 5V analog RGB LED strip --------------------
#define RGB_RED_PIN 9
#define RGB_GREEN_PIN 10
#define RGB_BLUE_PIN 11

// Most 4-pin analog RGB strips use one shared +5V/VCC pin.
// In that wiring, R/G/B turn ON when the Arduino output is LOW.
// If your MOSFET driver makes the strip inverted, send RGBTYPE once.
#define RGB_STRIP_COMMON_ANODE 1

SoftwareSerial bluetooth(BT_RX_PIN, BT_TX_PIN);

bool matrixBuffer[8][32];

enum Mode {
  MODE_GAME,
  MODE_WAVE,
  MODE_RAIN,
  MODE_CHILL,
  MODE_FIRE
};

Mode currentMode = MODE_GAME;
bool displayEnabled = false;
bool modeSelected = false;
bool rgbCommonAnode = RGB_STRIP_COMMON_ANODE;

char commandBuffer[24];
byte commandIndex = 0;

unsigned long lastAudioReadMs = 0;
unsigned long lastVolumeSendMs = 0;
unsigned long lastCommandCharMs = 0;
unsigned long datasetRowNumber = 0;
unsigned long forceLedUntilMs = 0;
unsigned long modeSelectedMs = 0;
unsigned long lastMatrixAnimationMs = 0;
unsigned long matrixAnimationIntervalMs = 75;

int smoothedVolume = 0;
int beatLevel = 0;
int ambientPeak = 12;
char lastBluetoothCommand[24] = "-";
byte currentVolumeLevel = 0;
byte matrixAnimationFrame = 0;

const int SOUND_NOISE_FLOOR = 6;
const int SOUND_FULL_SCALE = 105;
const byte BEAT_RISE_STEP = 42;
const byte BEAT_FALL_STEP = 20;
const byte VOLUME_START_PERCENT = 5;
const byte STRIP_MIN_BRIGHTNESS = 8;
const byte STRIP_COLOR_LEVELS = 7;
const unsigned long MODE_NAME_HOLD_MS = 5000;
const byte MATRIX_SPEED_MIN = 0;
const byte MATRIX_SPEED_MAX = 100;
const unsigned long MATRIX_ANIMATION_FAST_MS = 25;
const unsigned long MATRIX_ANIMATION_SLOW_MS = 220;
const unsigned long DATASET_SEND_INTERVAL_MS = 200;

struct AudioData {
  int rawA0;
  int peakToPeak;
  int volumePercent;
  int frequencyHz;
};

const char *modeName(Mode mode) {
  switch (mode) {
    case MODE_GAME: return "GAME";
    case MODE_WAVE: return "WAVE";
    case MODE_RAIN: return "RAIN";
    case MODE_CHILL: return "CHILL";
    case MODE_FIRE: return "FIRE";
  }
  return "GAME";
}

const byte FONT_SPACE[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
const byte FONT_COLON[5] = {0x00, 0x00, 0x36, 0x36, 0x00};
const byte FONT_RIGHT_PAREN[5] = {0x00, 0x41, 0x22, 0x1C, 0x00};
const byte FONT_A[5] = {0x7C, 0x12, 0x11, 0x12, 0x7C};
const byte FONT_C[5] = {0x3E, 0x41, 0x41, 0x41, 0x22};
const byte FONT_E[5] = {0x7F, 0x49, 0x49, 0x49, 0x41};
const byte FONT_F[5] = {0x7F, 0x09, 0x09, 0x09, 0x01};
const byte FONT_G[5] = {0x3E, 0x41, 0x49, 0x49, 0x7A};
const byte FONT_H[5] = {0x7F, 0x08, 0x08, 0x08, 0x7F};
const byte FONT_I[5] = {0x00, 0x41, 0x7F, 0x41, 0x00};
const byte FONT_L[5] = {0x7F, 0x40, 0x40, 0x40, 0x40};
const byte FONT_M[5] = {0x7F, 0x02, 0x0C, 0x02, 0x7F};
const byte FONT_N[5] = {0x7F, 0x04, 0x08, 0x10, 0x7F};
const byte FONT_R[5] = {0x7F, 0x09, 0x19, 0x29, 0x46};
const byte FONT_V[5] = {0x1F, 0x20, 0x40, 0x20, 0x1F};
const byte FONT_W[5] = {0x7F, 0x20, 0x18, 0x20, 0x7F};

void redrawMatrixText();

const byte *getGlyph(char c) {
  switch (c) {
    case 'A': return FONT_A;
    case 'C': return FONT_C;
    case 'E': return FONT_E;
    case 'F': return FONT_F;
    case 'G': return FONT_G;
    case 'H': return FONT_H;
    case 'I': return FONT_I;
    case 'L': return FONT_L;
    case 'M': return FONT_M;
    case 'N': return FONT_N;
    case 'R': return FONT_R;
    case 'V': return FONT_V;
    case 'W': return FONT_W;
    case ':': return FONT_COLON;
    case ')': return FONT_RIGHT_PAREN;
  }

  return FONT_SPACE;
}

void max7219SendAll(byte reg, byte dataForDevice[MATRIX_MAX_DEVICES]) {
  digitalWrite(MATRIX_CS_PIN, LOW);

  for (int device = MATRIX_MAX_DEVICES - 1; device >= 0; device--) {
    shiftOut(MATRIX_DATA_PIN, MATRIX_CLK_PIN, MSBFIRST, reg);
    shiftOut(MATRIX_DATA_PIN, MATRIX_CLK_PIN, MSBFIRST, dataForDevice[device]);
  }

  digitalWrite(MATRIX_CS_PIN, HIGH);
}

void max7219SendSame(byte reg, byte data) {
  byte dataForDevice[MATRIX_MAX_DEVICES];

  for (byte i = 0; i < MATRIX_MAX_DEVICES; i++) {
    dataForDevice[i] = data;
  }

  max7219SendAll(reg, dataForDevice);
}

void matrixClearBuffer() {
  for (byte y = 0; y < 8; y++) {
    for (byte x = 0; x < 32; x++) {
      matrixBuffer[y][x] = false;
    }
  }
}

void matrixRefresh() {
  for (byte registerRow = 0; registerRow < 8; registerRow++) {
    byte dataForDevice[MATRIX_MAX_DEVICES] = {0, 0, 0, 0};

    for (byte y = 0; y < 8; y++) {
      for (byte x = 0; x < 32; x++) {
        if (!matrixBuffer[y][x]) continue;

        byte screenX = x;
        byte screenY = y;

        if (matrixReverseX) {
          screenX = 31 - screenX;
        }

        if (matrixReverseY) {
          screenY = 7 - screenY;
        }

        bool mapTranspose = bitRead(currentMatrixMap, 0);
        bool firstDeviceOnRight = bitRead(currentMatrixMap, 1);
        bool reverseModuleRow = bitRead(currentMatrixMap, 2);
        bool reverseModuleCol = bitRead(currentMatrixMap, 3);

        byte logicalDevice = screenX / 8;
        byte localX = screenX % 8;
        byte localY = screenY;

        byte physicalDevice = logicalDevice;
        if (firstDeviceOnRight) {
          physicalDevice = MATRIX_MAX_DEVICES - 1 - logicalDevice;
        }

        byte moduleRow = localY;
        byte moduleCol = localX;

        if (mapTranspose) {
          byte temp = moduleRow;
          moduleRow = moduleCol;
          moduleCol = temp;
        }

        if (reverseModuleRow) {
          moduleRow = 7 - moduleRow;
        }

        if (reverseModuleCol) {
          moduleCol = 7 - moduleCol;
        }

        if (moduleRow == registerRow) {
          dataForDevice[physicalDevice] |= (1 << moduleCol);
        }
      }
    }

    max7219SendAll(registerRow + 1, dataForDevice);
  }
}

void matrixBegin() {
  pinMode(MATRIX_DATA_PIN, OUTPUT);
  pinMode(MATRIX_CLK_PIN, OUTPUT);
  pinMode(MATRIX_CS_PIN, OUTPUT);

  digitalWrite(MATRIX_CS_PIN, HIGH);

  max7219SendSame(0x0F, 0x00); // display test off
  max7219SendSame(0x0C, 0x01); // normal operation
  max7219SendSame(0x0B, 0x07); // scan all 8 rows
  max7219SendSame(0x09, 0x00); // no decode
  max7219SendSame(0x0A, 0x03); // brightness 0-15

  matrixClearBuffer();
  matrixRefresh();
}

void matrixPixel(byte x, byte y, bool on) {
  if (x >= 32 || y >= 8) return;
  matrixBuffer[y][x] = on;
}

void testMatrixDisplay() {
  for (byte y = 0; y < 8; y++) {
    for (byte x = 0; x < 32; x++) {
      matrixBuffer[y][x] = true;
    }
  }

  matrixRefresh();
  delay(800);
  matrixClearBuffer();
  matrixRefresh();
  delay(200);
  redrawMatrixText();
}

void drawChar5x7(char c, byte startX) {
  const byte *glyph = getGlyph(c);

  for (byte col = 0; col < 5; col++) {
    for (byte row = 0; row < 7; row++) {
      bool pixelOn = bitRead(glyph[col], row);
      matrixPixel(startX + col, row, pixelOn);
    }
  }
}

void drawTextLeftToRight(const char *text) {
  byte len = strlen(text);
  byte textWidth = len * 5 + (len - 1);
  byte startX = 0;

  if (textWidth < 32) {
    startX = (32 - textWidth) / 2;
  }

  matrixClearBuffer();

  for (byte i = 0; i < len; i++) {
    drawChar5x7(text[i], startX + i * 6);
  }

  matrixRefresh();
}

byte modeIndex() {
  switch (currentMode) {
    case MODE_GAME: return 0;
    case MODE_WAVE: return 1;
    case MODE_RAIN: return 2;
    case MODE_CHILL: return 3;
    case MODE_FIRE: return 4;
  }

  return 0;
}

const RgbColor MODE_LEVEL_COLORS[5][STRIP_COLOR_LEVELS] = {
  // GAME: green, lime, cyan energy
  {{0, 22, 0}, {0, 70, 16}, {0, 130, 30}, {50, 205, 28}, {145, 245, 0}, {30, 255, 115}, {0, 255, 235}},

  // WAVE: deep blue to sea cyan
  {{0, 0, 38}, {0, 0, 95}, {0, 25, 160}, {0, 85, 225}, {0, 155, 255}, {0, 220, 255}, {120, 255, 255}},

  // RAIN: indigo, violet, pink
  {{22, 0, 55}, {55, 0, 115}, {95, 0, 175}, {145, 0, 230}, {205, 40, 255}, {255, 80, 220}, {255, 145, 190}},

  // CHILL: teal, mint, soft white
  {{0, 35, 32}, {0, 85, 75}, {0, 135, 120}, {45, 185, 165}, {120, 225, 205}, {190, 245, 230}, {245, 255, 250}},

  // FIRE: ember, red, orange, yellow
  {{45, 0, 0}, {105, 0, 0}, {175, 0, 0}, {235, 28, 0}, {255, 85, 0}, {255, 165, 0}, {255, 245, 20}}
};

RgbColor blendRgbColor(RgbColor fromColor, RgbColor toColor, byte amount) {
  RgbColor result;
  result.red = fromColor.red + (long)(toColor.red - fromColor.red) * amount / 255;
  result.green = fromColor.green + (long)(toColor.green - fromColor.green) * amount / 255;
  result.blue = fromColor.blue + (long)(toColor.blue - fromColor.blue) * amount / 255;
  return result;
}

void setRgbColor(byte red, byte green, byte blue) {
  if (rgbCommonAnode) {
    red = 255 - red;
    green = 255 - green;
    blue = 255 - blue;
  }

  analogWrite(RGB_RED_PIN, red);
  analogWrite(RGB_GREEN_PIN, green);
  analogWrite(RGB_BLUE_PIN, blue);
}

void setRgbOff() {
  if (rgbCommonAnode) {
    analogWrite(RGB_RED_PIN, 255);
    analogWrite(RGB_GREEN_PIN, 255);
    analogWrite(RGB_BLUE_PIN, 255);
  } else {
    analogWrite(RGB_RED_PIN, 0);
    analogWrite(RGB_GREEN_PIN, 0);
    analogWrite(RGB_BLUE_PIN, 0);
  }
}

void clearRgbStrip() {
  setRgbOff();
}

void resetAudioBeat() {
  smoothedVolume = 0;
  beatLevel = 0;
  currentVolumeLevel = 0;
}

void keepRgbStripOffUntilMode() {
  if (!displayEnabled || !modeSelected) {
    forceLedUntilMs = 0;
    resetAudioBeat();
    clearRgbStrip();
  }
}

void testRgbStrip() {
  setRgbColor(255, 0, 0);
  delay(700);
  setRgbColor(0, 255, 0);
  delay(700);
  setRgbColor(0, 0, 255);
  delay(700);
  setRgbColor(255, 255, 255);
  delay(700);

  clearRgbStrip();
}

void forceWhiteLed() {
  setRgbColor(255, 255, 255);
  forceLedUntilMs = millis() + 5000UL;
}

void calibrateSoundFloor() {
  unsigned long startMs = millis();
  long totalPeak = 0;
  int windows = 0;

  while (millis() - startMs < 900) {
    unsigned int signalMax = 0;
    unsigned int signalMin = 1023;
    unsigned long windowStart = millis();

    while (millis() - windowStart < 30) {
      int sample = analogRead(SOUND_PIN);
      if (sample > signalMax) signalMax = sample;
      if (sample < signalMin) signalMin = sample;
    }

    totalPeak += (signalMax - signalMin);
    windows++;
  }

  if (windows > 0) {
    ambientPeak = totalPeak / windows;
  }

  smoothedVolume = 0;
  beatLevel = 0;
  currentVolumeLevel = 0;
  clearRgbStrip();
}

RgbColor colorForModeVolume(byte volumeLevel) {
  if (volumeLevel >= STRIP_COLOR_LEVELS) volumeLevel = STRIP_COLOR_LEVELS - 1;
  return MODE_LEVEL_COLORS[modeIndex()][volumeLevel];
}

void showVolumeOnRgbStrip(int volumePercent) {
  if (millis() < forceLedUntilMs) return;

  if (!displayEnabled || !modeSelected) {
    resetAudioBeat();
    clearRgbStrip();
    return;
  }

  volumePercent = constrain(volumePercent, 0, 100);

  // Below this gate the strip must stay completely off.
  if (volumePercent < VOLUME_START_PERCENT) {
    resetAudioBeat();
    clearRgbStrip();
    return;
  }

  // Beat response: rise smoothly on loud sound and fall smoothly after spikes.
  int targetBeat = map(volumePercent, VOLUME_START_PERCENT, 100, 0, 255);
  if (targetBeat > beatLevel) {
    beatLevel += min(targetBeat - beatLevel, BEAT_RISE_STEP);
  } else {
    beatLevel -= min(beatLevel - targetBeat, BEAT_FALL_STEP);
  }

  int palettePosition = map(beatLevel, 0, 255, 0, (STRIP_COLOR_LEVELS - 1) * 255);
  byte colorIndex = palettePosition / 255;
  byte blendAmount = palettePosition % 255;
  RgbColor fromColor = colorForModeVolume(colorIndex);
  RgbColor toColor = colorForModeVolume(colorIndex + 1);
  RgbColor stripColor = blendRgbColor(fromColor, toColor, blendAmount);
  byte brightness = map(beatLevel, 0, 255, STRIP_MIN_BRIGHTNESS, 255);

  currentVolumeLevel = colorIndex + 1;

  setRgbColor(
    (byte)((unsigned int)stripColor.red * brightness / 255),
    (byte)((unsigned int)stripColor.green * brightness / 255),
    (byte)((unsigned int)stripColor.blue * brightness / 255)
  );
}

void drawModeOnMatrix() {
  if (displayEnabled && modeSelected) {
    drawTextLeftToRight(modeName(currentMode));
  }
}

void drawGreetingOnMatrix() {
  if (displayEnabled) {
    drawTextLeftToRight("HI :)");
  }
}

void redrawMatrixText() {
  if (!displayEnabled) return;

  if (modeSelected) {
    drawModeOnMatrix();
  } else {
    drawGreetingOnMatrix();
  }
}

void setMode(Mode mode) {
  currentMode = mode;
  modeSelected = true;
  modeSelectedMs = millis();
  lastMatrixAnimationMs = 0;
  matrixAnimationFrame = 0;
  resetAudioBeat();
  clearRgbStrip();
  drawModeOnMatrix();

  bluetooth.print("MODE:");
  bluetooth.println(modeName(currentMode));
}

void clearAllDisplays() {
  clearRgbStrip();
  matrixClearBuffer();
  matrixRefresh();
}

void drawGameAnimation(byte frame) {
  matrixClearBuffer();

  for (byte x = 0; x < 32; x++) {
    byte lane = (x + frame) % 8;
    matrixPixel(x, lane, true);
    if ((x + frame) % 6 == 0) {
      matrixPixel(x, 7 - lane, true);
    }
  }

  for (byte y = 0; y < 8; y++) {
    byte x = (frame * 2 + y * 4) % 32;
    matrixPixel(x, y, true);
  }

  matrixRefresh();
}

void drawWaveAnimation(byte frame) {
  const signed char wave[16] = {0, 1, 2, 3, 2, 1, 0, -1, -2, -3, -2, -1, 0, 1, 2, 1};
  matrixClearBuffer();

  for (byte x = 0; x < 32; x++) {
    byte index = (x + frame) % 16;
    byte y = 3 + wave[index];
    matrixPixel(x, y, true);
    if (y < 7) matrixPixel(x, y + 1, true);
  }

  matrixRefresh();
}

void drawRainAnimation(byte frame) {
  matrixClearBuffer();

  for (byte x = 0; x < 32; x += 3) {
    byte y = (frame + x * 2) % 12;
    if (y < 8) matrixPixel(x, y, true);
    if (y > 0 && y < 9) matrixPixel(x, y - 1, true);
  }

  matrixRefresh();
}

void drawChillAnimation(byte frame) {
  matrixClearBuffer();

  byte radius = 1 + ((frame / 3) % 4);
  byte centerX = 15;
  byte centerY = 3;

  for (byte x = 0; x < 32; x++) {
    for (byte y = 0; y < 8; y++) {
      byte dx = x > centerX ? x - centerX : centerX - x;
      byte dy = y > centerY ? y - centerY : centerY - y;
      if (dx + dy == radius || ((x + y + frame) % 17 == 0)) {
        matrixPixel(x, y, true);
      }
    }
  }

  matrixRefresh();
}

void drawFireAnimation(byte frame) {
  matrixClearBuffer();

  for (byte x = 0; x < 32; x++) {
    byte height = 1 + ((x * 5 + frame * 3 + (x % 3) * 7) % 7);
    for (byte y = 0; y < height; y++) {
      matrixPixel(x, 7 - y, true);
    }

    if ((x + frame) % 5 == 0) {
      matrixPixel(x, 7 - height, true);
    }
  }

  matrixRefresh();
}

void drawModeAnimation() {
  switch (currentMode) {
    case MODE_GAME: drawGameAnimation(matrixAnimationFrame); break;
    case MODE_WAVE: drawWaveAnimation(matrixAnimationFrame); break;
    case MODE_RAIN: drawRainAnimation(matrixAnimationFrame); break;
    case MODE_CHILL: drawChillAnimation(matrixAnimationFrame); break;
    case MODE_FIRE: drawFireAnimation(matrixAnimationFrame); break;
  }
}

void showModeOnMatrix() {
  if (!displayEnabled) {
    matrixClearBuffer();
    matrixRefresh();
    return;
  }

  if (!modeSelected) return;

  if (millis() - modeSelectedMs < MODE_NAME_HOLD_MS) {
    return;
  }

  if (millis() - lastMatrixAnimationMs < matrixAnimationIntervalMs) {
    return;
  }

  lastMatrixAnimationMs = millis();
  drawModeAnimation();
  matrixAnimationFrame++;
}

AudioData readAudioData() {
  const unsigned long sampleWindowMs = 40;
  unsigned int signalMax = 0;
  unsigned int signalMin = 1023;
  unsigned long startMs = millis();
  unsigned int sampleCount = 0;
  unsigned long sampleSum = 0;
  int lastRaw = analogRead(SOUND_PIN);
  bool lastAboveCenter = false;
  bool centerReady = false;
  unsigned int crossingCount = 0;

  while (millis() - startMs < sampleWindowMs) {
    int sample = analogRead(SOUND_PIN);
    lastRaw = sample;

    if (sample < 1024) {
      if (sample > signalMax) signalMax = sample;
      if (sample < signalMin) signalMin = sample;

      sampleSum += sample;
      sampleCount++;

      int center = sampleCount > 0 ? sampleSum / sampleCount : 512;
      bool aboveCenter = sample > center;

      if (centerReady && aboveCenter != lastAboveCenter && abs(sample - center) > 4) {
        crossingCount++;
      }

      lastAboveCenter = aboveCenter;
      centerReady = true;
    }
  }

  int peakToPeak = signalMax - signalMin;

  if (peakToPeak < ambientPeak + 8) {
    ambientPeak = (ambientPeak * 95 + peakToPeak * 5) / 100;
  }

  int adjustedPeak = peakToPeak - ambientPeak - SOUND_NOISE_FLOOR;
  if (adjustedPeak < 0) adjustedPeak = 0;

  int volume = map(adjustedPeak, 0, SOUND_FULL_SCALE, 0, 100);
  volume = constrain(volume, 0, 100);

  if (volume < VOLUME_START_PERCENT) {
    smoothedVolume = (smoothedVolume * 20) / 100;
  } else {
    smoothedVolume = (smoothedVolume * 25 + volume * 75) / 100;
  }

  int frequencyHz = 0;
  if (adjustedPeak > 0 && crossingCount >= 2) {
    frequencyHz = (int)((crossingCount * 1000UL) / (2UL * sampleWindowMs));
  }

  AudioData data;
  data.rawA0 = lastRaw;
  data.peakToPeak = adjustedPeak;
  data.volumePercent = smoothedVolume;
  data.frequencyHz = frequencyHz;
  return data;
}

const char *ledStateName() {
  if (!displayEnabled) return "OFF";
  if (!modeSelected) return "ON_NO_MODE";
  return modeName(currentMode);
}

const char *frequencyBandName(int frequencyHz) {
  if (frequencyHz <= 0) return "NO_FREQ";
  if (frequencyHz < 250) return "LOW_FREQ";
  if (frequencyHz < 1200) return "MID_FREQ";
  return "HIGH_FREQ";
}

const char *datasetNote(const AudioData &data) {
  if (!displayEnabled) return "System off";
  if (!modeSelected) return "Waiting for mode";
  if (data.volumePercent < 15) return "Low sound";
  if (data.volumePercent < 45) return "Medium sound";
  if (data.volumePercent < 75) return "High sound";
  return "Very high sound";
}

void printDatasetRow(Print &out, const AudioData &data) {
  out.print(datasetRowNumber);
  out.print(',');
  out.print(millis());
  out.print(',');
  out.print(lastBluetoothCommand);
  out.print(',');
  out.print(ledStateName());
  out.print(',');
  out.print(data.rawA0);
  out.print(',');
  out.print(data.peakToPeak);
  out.print(',');
  out.print(data.volumePercent);
  out.print(',');
  out.print(data.frequencyHz);
  out.print(',');
  out.print(frequencyBandName(data.frequencyHz));
  out.print(',');
  out.println(datasetNote(data));
}

void sendRealtimeDataset(const AudioData &data) {
  if (millis() - lastVolumeSendMs < DATASET_SEND_INTERVAL_MS) return;
  lastVolumeSendMs = millis();
  datasetRowNumber++;

  bluetooth.print("VOL:");
  bluetooth.println(data.volumePercent);

  printDatasetRow(Serial, data);
  printDatasetRow(bluetooth, data);
}

void normalizeCommand(char *text) {
  for (byte i = 0; text[i] != '\0'; i++) {
    if (text[i] >= 'a' && text[i] <= 'z') {
      text[i] = text[i] - 'a' + 'A';
    }
  }
}

bool commandIsNumber(const char *text) {
  if (text[0] == '\0') return false;

  for (byte i = 0; text[i] != '\0'; i++) {
    if (text[i] < '0' || text[i] > '9') {
      return false;
    }
  }

  return true;
}

void setMatrixAnimationSpeed(int speedValue) {
  speedValue = constrain(speedValue, MATRIX_SPEED_MIN, MATRIX_SPEED_MAX);

  matrixAnimationIntervalMs = map(
    speedValue,
    MATRIX_SPEED_MIN,
    MATRIX_SPEED_MAX,
    MATRIX_ANIMATION_SLOW_MS,
    MATRIX_ANIMATION_FAST_MS
  );

  bluetooth.print("MATRIX_SPEED:");
  bluetooth.print(speedValue);
  bluetooth.print(",INTERVAL_MS:");
  bluetooth.println(matrixAnimationIntervalMs);
}

void handleCommand(char *command) {
  normalizeCommand(command);
  strncpy(lastBluetoothCommand, command, sizeof(lastBluetoothCommand) - 1);
  lastBluetoothCommand[sizeof(lastBluetoothCommand) - 1] = '\0';

  if (commandIsNumber(command)) {
    setMatrixAnimationSpeed(atoi(command));
    return;
  }

  if (strcmp(command, "H") == 0) {
    displayEnabled = true;
    modeSelected = false;
    resetAudioBeat();
    forceLedUntilMs = 0;
    clearRgbStrip();
    drawGreetingOnMatrix();
    bluetooth.println("LED:ON");
    return;
  }

  if (strcmp(command, "L") == 0) {
    displayEnabled = false;
    modeSelected = false;
    resetAudioBeat();
    clearAllDisplays();
    bluetooth.println("LED:OFF");
    return;
  }

  if (strcmp(command, "TESTLED") == 0 || strcmp(command, "T") == 0) {
    bluetooth.println("TESTLED");
    testRgbStrip();
    return;
  }

  if (strcmp(command, "FORCELED") == 0) {
    bluetooth.println("FORCELED WHITE");
    forceWhiteLed();
    return;
  }

  if (strcmp(command, "RGBTYPE") == 0) {
    rgbCommonAnode = !rgbCommonAnode;
    clearRgbStrip();

    bluetooth.print("RGB_STRIP_COMMON_ANODE:");
    bluetooth.println(rgbCommonAnode ? "ON" : "OFF");
    return;
  }

  if (strcmp(command, "CAL") == 0) {
    bluetooth.println("CALIBRATING_SOUND");
    calibrateSoundFloor();
    bluetooth.print("AMBIENT_PEAK:");
    bluetooth.println(ambientPeak);
    return;
  }

  if (strcmp(command, "TESTMATRIX") == 0 || strcmp(command, "TM") == 0) {
    bluetooth.println("TESTMATRIX");
    testMatrixDisplay();
    return;
  }

  if (command[0] == 'M' && command[1] >= '0' && command[1] <= '9') {
    int mapNumber = atoi(command + 1);

    if (mapNumber >= 0 && mapNumber <= 15) {
      currentMatrixMap = mapNumber;
      redrawMatrixText();

      bluetooth.print("MATRIX_MAP:");
      bluetooth.println(currentMatrixMap);
    } else {
      bluetooth.println("MATRIX_MAP must be M0..M15");
    }

    return;
  }

  if (strcmp(command, "X") == 0 || strcmp(command, "MIRROR") == 0) {
    matrixReverseX = !matrixReverseX;
    redrawMatrixText();

    bluetooth.print("MATRIX_REVERSE_X:");
    bluetooth.println(matrixReverseX ? "ON" : "OFF");
    return;
  }

  if (strcmp(command, "Y") == 0) {
    matrixReverseY = !matrixReverseY;
    redrawMatrixText();

    bluetooth.print("MATRIX_REVERSE_Y:");
    bluetooth.println(matrixReverseY ? "ON" : "OFF");
    return;
  }

  if (strcmp(command, "GAME") == 0) {
    setMode(MODE_GAME);
  } else if (strcmp(command, "WAVE") == 0) {
    setMode(MODE_WAVE);
  } else if (strcmp(command, "RAIN") == 0) {
    setMode(MODE_RAIN);
  } else if (strcmp(command, "CHILL") == 0) {
    setMode(MODE_CHILL);
  } else if (strcmp(command, "FIRE") == 0) {
    setMode(MODE_FIRE);
  } else {
    bluetooth.print("UNKNOWN:");
    bluetooth.println(command);
  }
}

void readBluetoothCommands() {
  while (bluetooth.available()) {
    char c = bluetooth.read();

    if (c == '\n' || c == '\r' || c == ',' || c == ';') {
      if (commandIndex > 0) {
        commandBuffer[commandIndex] = '\0';
        handleCommand(commandBuffer);
        commandIndex = 0;
      }
      continue;
    }

    if (c == ' ') continue;

    if (commandIndex < sizeof(commandBuffer) - 1) {
      commandBuffer[commandIndex++] = c;
      lastCommandCharMs = millis();

      if (commandIndex == 1 && (c == 'H' || c == 'h' || c == 'L' || c == 'l')) {
        commandBuffer[commandIndex] = '\0';
        handleCommand(commandBuffer);
        commandIndex = 0;
      }
    }
  }
}

void flushBluetoothCommandAfterIdle() {
  if (commandIndex == 0) return;
  if (millis() - lastCommandCharMs < 80) return;

  commandBuffer[commandIndex] = '\0';
  handleCommand(commandBuffer);
  commandIndex = 0;
}

void setup() {
  Serial.begin(9600);
  bluetooth.begin(9600);

  pinMode(RGB_RED_PIN, OUTPUT);
  pinMode(RGB_GREEN_PIN, OUTPUT);
  pinMode(RGB_BLUE_PIN, OUTPUT);

  clearRgbStrip();

  matrixBegin();

  pinMode(SOUND_PIN, INPUT);

  Serial.println("STT,Time_ms,Bluetooth_Cmd,LED_Mode,Raw_A0,PeakToPeak,Volume_percent,Frequency_Hz,Frequency_Band,Note");
  bluetooth.println("READY");
  bluetooth.println("CMD:H,L,GAME,WAVE,RAIN,CHILL,FIRE");
  bluetooth.println("MATRIX SPEED: send 0..100");
  bluetooth.println("RGB STRIP TEST: send TESTLED, FORCELED, RGBTYPE, CAL");
  bluetooth.println("MATRIX TEST: send TESTMATRIX, M0..M15, X, Y");
  bluetooth.println("STT,Time_ms,Bluetooth_Cmd,LED_Mode,Raw_A0,PeakToPeak,Volume_percent,Frequency_Hz,Frequency_Band,Note");
}

void loop() {
  readBluetoothCommands();
  flushBluetoothCommandAfterIdle();
  keepRgbStripOffUntilMode();
  showModeOnMatrix();

  if (millis() - lastAudioReadMs >= 45) {
    lastAudioReadMs = millis();
    AudioData audio = readAudioData();
    showVolumeOnRgbStrip(audio.volumePercent);
    sendRealtimeDataset(audio);
  }
}
