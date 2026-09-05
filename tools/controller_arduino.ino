#define DEBUG_TEXT_MODE 0

const int joy1XPin = A0;   // movement stick X
const int joy1YPin = A1;   // movement stick Y
const int joy1ButtonPin = 7;   // movement stick click to JUMP

const int joy2XPin = A4;   // camera stick X
const int joy2YPin = A5;   // camera stick Y
const int joy2ButtonPin = 6;   // camera stick click to SHOOT

const unsigned long SEND_INTERVAL_MS = 10;  // ~100Hz
unsigned long lastSendMs = 0;

void setup() {
  Serial.begin(115200);
  pinMode(joy1ButtonPin, INPUT_PULLUP);
  pinMode(joy2ButtonPin, INPUT_PULLUP);
}

void loop() {
  unsigned long now = millis();
  if (now - lastSendMs < SEND_INTERVAL_MS) return;
  lastSendMs = now;

  uint8_t x1 = map(analogRead(joy1XPin), 0, 1023, 0, 254);
  uint8_t y1 = map(analogRead(joy1YPin), 0, 1023, 0, 254);
  uint8_t x2 = map(analogRead(joy2XPin), 0, 1023, 0, 254);
  uint8_t y2 = map(analogRead(joy2YPin), 0, 1023, 0, 254);

  uint8_t buttons = 0;
  if (digitalRead(joy1ButtonPin) == LOW) buttons |= 0x01;  // jump
  if (digitalRead(joy2ButtonPin) == LOW) buttons |= 0x02;  // shoot

#if DEBUG_TEXT_MODE
  Serial.print("X1: "); Serial.print(x1);
  Serial.print("  Y1: "); Serial.print(y1);
  Serial.print("  X2: "); Serial.print(x2);
  Serial.print("  Y2: "); Serial.print(y2);
  Serial.print("  Jump: "); Serial.print((buttons & 0x01) ? "PRESSED" : "released");
  Serial.print("  Shoot: "); Serial.println((buttons & 0x02) ? "PRESSED" : "released");
#else
  uint8_t checksum = x1 ^ y1 ^ x2 ^ y2 ^ buttons;
  uint8_t frame[7] = {0xFF, x1, y1, x2, y2, buttons, checksum};
  Serial.write(frame, sizeof(frame));
#endif
}
