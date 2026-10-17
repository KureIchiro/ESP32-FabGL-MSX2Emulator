struct ControllerPins {
  uint8_t up;
  uint8_t down;
  uint8_t left;
  uint8_t right;
  uint8_t buttons[3];
  uint8_t turbo[3];
  uint8_t id;
};

const ControllerPins controllers[] = {
  {22, 23, 24, 25, {26, 27, 28}, {29, 30, 31}, 0},
  {32, 33, 34, 35, {36, 37, 38}, {39, 40, 41}, 1}
};

const uint32_t SERIAL_INTERVAL_MS = 12;
const uint32_t TURBO_INTERVAL_MS = 40;

uint32_t lastSerialTick = 0;
uint32_t lastTurboTick = 0;
bool turboState = false;

uint8_t readController(const ControllerPins &pins) {
  uint8_t data = 0xFF;

  if (digitalRead(pins.up) == LOW) {
    data &= ~(1U << 0);
  }
  if (digitalRead(pins.down) == LOW) {
    data &= ~(1U << 1);
  }
  if (digitalRead(pins.left) == LOW) {
    data &= ~(1U << 2);
  }
  if (digitalRead(pins.right) == LOW) {
    data &= ~(1U << 3);
  }

  for (uint8_t button = 0; button < 3; ++button) {
    const bool pressed = digitalRead(pins.buttons[button]) == LOW;
    const bool turboEnabled = digitalRead(pins.turbo[button]) == LOW;
    const bool pulseOn = (button == 1) ? !turboState : turboState;

    if (pressed && (!turboEnabled || pulseOn)) {
      data &= ~(1U << (button + 4));
    }
  }

  if (pins.id != 0) {
    data |= 1U << 7;
  } else {
    data &= ~(1U << 7);
  }

  return data;
}

void setup() {
  for (const ControllerPins &pins : controllers) {
    pinMode(pins.up, INPUT_PULLUP);
    pinMode(pins.down, INPUT_PULLUP);
    pinMode(pins.left, INPUT_PULLUP);
    pinMode(pins.right, INPUT_PULLUP);

    for (uint8_t button = 0; button < 3; ++button) {
      pinMode(pins.buttons[button], INPUT_PULLUP);
      pinMode(pins.turbo[button], INPUT_PULLUP);
    }
  }

  Serial1.begin(115200);
}

void loop() {
  const uint32_t now = millis();

  if (now - lastTurboTick >= TURBO_INTERVAL_MS) {
    turboState = !turboState;
    lastTurboTick = now;
  }

  if (now - lastSerialTick >= SERIAL_INTERVAL_MS) {
    const uint8_t frame[] = {
      readController(controllers[0]),
      readController(controllers[1])
    };
    Serial1.write(frame, sizeof(frame));
    lastSerialTick = now;
  }
}
