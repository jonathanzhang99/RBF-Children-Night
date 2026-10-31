#define DECODE_NEC
#define FASTLED_ALLOW_INTERRUPTS 0

#include <FastLED.h>
#include <Arduino.h>
#include <IRremote.hpp>
#include <assert.h>
#include <Servo.h>

// #define DEBUG  // Activate this for lots of lovely debug output from the IR decoders
#define ARRAY_SIZE(A) (sizeof(A) / sizeof((A)[0]))

FASTLED_USING_NAMESPACE

// Fast LED Macros
#define LED_TYPE WS2812
#define COLOR_ORDER GRB
#define NUM_LEDS 16
#define LED_DATA_PIN_1 6
#define LED_DATA_PIN_2 7
#define LED_DATA_PIN_3 8
#define LED_DATA_PIN_4 9
#define BRIGHTNESS 96
#define FRAMES_PER_SECOND 120

#define MS_OFFSET 1000
#define MIN_MS_BETWEEN_JUMP 1000
#define LEVEL_1 8
#define LEVEL_2 14
#define LEVEL_3 20
#define SHOWDOWN_GAME_MODE 0
#define VS_GAME_MODE 1

/*
 * struct for the servno
 */
Servo servo;
const unsigned int servoMoveMs = 20;
unsigned long lastServoMove = millis();
uint8_t servoPos = 0;
bool servoMoving = true;
bool servoIncreasing = true;

/*
 * struct to manage ir targets (ir + light)
 */
struct irTarget
{
  uint_fast8_t irPin;
  CRGB leds[NUM_LEDS];
};

irTarget target1 = {2};
irTarget target2 = {3};
irTarget target3 = {4};
irTarget target4 = {5};

// irTarget *irTargets[] = { &target1, &target2, &target3, &target4 };
irTarget *irTargets[] = {&target1, &target2, &target3, &target4};
const uint8_t numTargets = ARRAY_SIZE(irTargets);

/*
 * Game state variables
 */
uint8_t activeTargetIdx = 0;
bool gameActive = false;
uint8_t gameMode = SHOWDOWN_GAME_MODE;
unsigned long gameStart;
unsigned long targetStart;
unsigned long timeToDestroyTarget;
unsigned long gameLength = 60000;
bool randomTargetJumpEnabled = false;
unsigned long lastJumpTime = 0;

/*
 * Player state variables
 */
struct playerState
{
  uint16_t score = 0;
  uint8_t targetsDestroyed = 0;
  uint8_t hitsPerSignal = 16;
  uint8_t hitsOnTarget = 0;
};

struct playerState playerStates[2];
const uint8_t numPlayers = ARRAY_SIZE(playerStates);
uint8_t lastPlayerHit;
uint8_t totalHitsOnTarget = 0;
uint8_t totalTargetsDestroyed = 0;

/*
 * ISR Variables for callback
 */
volatile bool gameActiveChangeSignalReceived = false;
volatile bool gameModeChangeSignalReceived = false;
volatile bool sIRDataJustReceived = false;
volatile byte ledState = HIGH;

void ReceiveCompleteCallbackHandler();

const int alertPin = 12;
const int buttonPin = 11;
const int servoPin = 10;

uint8_t analogPinValue = 0;

const unsigned long debounceDelay = 100;
unsigned long lastDebounceTime = 0;
unsigned long lastActiveStateChangeTime = 0;
byte lastButtonState = LOW;
byte buttonState = LOW;

void printActivePin()
{
  Serial.print(F("INFO: Active pin: "));
  Serial.println(IrReceiver.irparams.IRReceivePin);
}

void blinkRainbow(int tDelay = 100)
{
  for (int i = 0; i < numTargets; i++)
  {
    fill_rainbow(irTargets[i]->leds, NUM_LEDS, 0, 16);
  }
  FastLED.show();
  delay(tDelay);

  for (int i = 0; i < numTargets; i++)
  {
    fill_solid(irTargets[i]->leds, NUM_LEDS, CRGB::Black);
  }
  FastLED.show();
  delay(tDelay);
}

void blinkRainbow(irTarget *ir, int tDelay = 100)
{
  fill_rainbow(ir->leds, NUM_LEDS, 0, 16);
  FastLED.show();
  delay(tDelay);
  fill_solid(ir->leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
  delay(tDelay);
}

void blinkSolid(irTarget *ir, CRGB color, int tDelay = 100)
{
  fill_solid(ir->leds, NUM_LEDS, color);
  FastLED.show();
  delay(tDelay);
  fill_solid(ir->leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
  delay(tDelay);
}

void emitScore()
{
  Serial.print(F("EMIT SCORE "));
  for (int i = 0; i < numPlayers; i++)
  {
    Serial.print(playerStates[i].score);
    Serial.print(" ");
  }
  Serial.println();
}

void emitGameActiveState()
{
  if (gameActive)
  {
    Serial.print(F("EMIT GAME_ACTIVE "));
    Serial.println(gameLength);
  }
  else
  {
    Serial.println(F("EMIT GAME_INACTIVE"));
  }
}

void emitGameMode()
{
  Serial.print(F("EMIT GAME_MODE "));
  Serial.println(gameMode);
}

void emitTargetHit()
{
  Serial.print(F("EMIT TARGET_HIT "));
  Serial.println(irTargets[activeTargetIdx]->irPin);
}

void emitTargetDestroyed()
{
  Serial.print(F("EMIT TARGET_DESTROYED "));
  Serial.println(irTargets[activeTargetIdx]->irPin);
}

void resetGameState()
{
  for (int i = 0; i < numPlayers; i++)
  {
    playerStates[i].hitsOnTarget = 0;
    playerStates[i].targetsDestroyed = 0;
    playerStates[i].hitsPerSignal = (gameMode == SHOWDOWN_GAME_MODE) ? 16 : 8;
    playerStates[i].score = 0;
  }

  totalHitsOnTarget = 0;
  totalTargetsDestroyed = 0;
  randomTargetJumpEnabled = false;

  emitScore();
  emitGameMode();
}

// Activate the next target keeping the number of hits set
void activateRandomNextTarget()
{
  activeTargetIdx = random8(numTargets);
  IrReceiver.begin(irTargets[activeTargetIdx]->irPin, ENABLE_LED_FEEDBACK);
  printActivePin();
}

void debounceButton()
{
  byte buttonReading = digitalRead(buttonPin);

  if (buttonReading != lastButtonState)
  {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > debounceDelay && buttonReading != buttonState)
  {
    buttonState = buttonReading;

    if (buttonState == HIGH)
    {
      servoMoving = !servoMoving;
    }
  }

  lastButtonState = buttonReading;
}

void safeFastLedShow()
{
  if (IrReceiver.isIdle())
  {
    FastLED.show();
  }
}

void fillShowdownLeds()
{
  fill_solid(irTargets[activeTargetIdx]->leds, NUM_LEDS, CRGB::Green);
  for (int i = NUM_LEDS - 1; i > NUM_LEDS - 1 - totalHitsOnTarget; i--)
  {
    irTargets[activeTargetIdx]->leds[i] = CRGB::Black;
  }
  for (int i = 0; i < numTargets; i++)
  {
    if (i != activeTargetIdx)
    {
      fill_solid(irTargets[i]->leds, NUM_LEDS, CRGB::Red);
    }
  }
}

void fillVSLeds()
{
  fill_solid(irTargets[activeTargetIdx]->leds, NUM_LEDS, CRGB::Green);
  for (int i = 0; i < playerStates[0].hitsOnTarget; i++)
  {
    irTargets[activeTargetIdx]->leds[i] = CRGB::Orange;
  }
  for (int i = NUM_LEDS - 1; i > NUM_LEDS - 1 - playerStates[1].hitsOnTarget; i--)
  {
    irTargets[activeTargetIdx]->leds[i] = CRGB::Blue;
  }

  for (int i = 0; i < numTargets; i++)
  {
    if (i != activeTargetIdx)
    {
      fill_solid(irTargets[i]->leds, NUM_LEDS, CRGB::Red);
    }
  }
}

void fillLeds()
{
  if (gameMode == SHOWDOWN_GAME_MODE)
  {
    fillShowdownLeds();
  }
  else if (gameMode == VS_GAME_MODE)
  {
    fillVSLeds();
  }
}

void targetDestroyedShowdown()
{
  totalHitsOnTarget = 0;
  totalTargetsDestroyed += 1;
  timeToDestroyTarget = millis() - targetStart;
  if (timeToDestroyTarget <= 500) {
    playerStates[0].score += 200;
  }
  else if (timeToDestroyTarget > 5000) {
    playerStates[0].score += 100;
  }
  else {
    playerStates[0].score += 100 + (uint16_t)((timeToDestroyTarget - 500) * 100 / 4500);
  }
  // playerStates[0].score += totalTargetsDestroyed < LEVEL_1 ? 100 : totalTargetsDestroyed < LEVEL_2 ? 200
  //                                                              : totalTargetsDestroyed < LEVEL_3   ? 600
  //                                                                                                  : 1000;
  emitTargetDestroyed();
  emitScore();

  uint8_t newHitsPerSignal = playerStates[0].hitsPerSignal;
  if (totalTargetsDestroyed == LEVEL_1)
  {
    randomTargetJumpEnabled = true
  }
  else if (totalTargetsDestroyed == LEVEL_2)
  {
    newHitsPerSignal = 8;
  }
  // else if (totalTargetsDestroyed == LEVEL_3)
  // {
  //   randomTargetJumpEnabled = true;
  // }

  for (int i = 0; i < numPlayers; i++)
  {
    playerStates[i].hitsPerSignal = newHitsPerSignal;
    playerStates[i].hitsOnTarget = 0;
  }

  for (int i = 0; i < 4; i++)
  {
    blinkRainbow(irTargets[activeTargetIdx]);
  }
  fill_solid(irTargets[activeTargetIdx]->leds, NUM_LEDS, CRGB::Red);
  FastLED.show();
  delay(random16(1000, 2500));

  activateRandomNextTarget();
}

void targetDestroyedVS()
{
  totalHitsOnTarget = 0;
  totalTargetsDestroyed += 1;
  playerStates[lastPlayerHit].score += totalTargetsDestroyed < LEVEL_1 ? 100 : totalTargetsDestroyed < LEVEL_2 ? 200
                                                                           : totalTargetsDestroyed < LEVEL_3   ? 600
                                                                                                               : 1000;
  playerStates[lastPlayerHit].targetsDestroyed += 1;

  emitTargetDestroyed();
  emitScore();

  uint8_t newHitsPerSignal = playerStates[0].hitsPerSignal;
  if (totalTargetsDestroyed == LEVEL_1)
  {
    newHitsPerSignal = 8;
  }
  else if (totalTargetsDestroyed == LEVEL_2)
  {
    newHitsPerSignal = 4;
  }
  else if (totalTargetsDestroyed == LEVEL_3)
  {
    randomTargetJumpEnabled = true;
  }
  for (int i = 0; i < numPlayers; i++)
  {
    playerStates[i].hitsPerSignal = newHitsPerSignal;
    playerStates[i].hitsOnTarget = 0;
  }

  CRGB blinkColor = lastPlayerHit ? CRGB::Blue : CRGB::Orange;
  for (int i = 0; i < 4; i++)
  {
    blinkSolid(irTargets[activeTargetIdx], blinkColor);
  }
  fill_solid(irTargets[activeTargetIdx]->leds, NUM_LEDS, CRGB::Red);
  FastLED.show();
  delay(random16(1000, 2500));
  activateRandomNextTarget();
}

void targetDestroyedUpdate()
{
  if (gameMode == SHOWDOWN_GAME_MODE)
  {
    targetDestroyedShowdown();
  }
  else if (gameMode == VS_GAME_MODE)
  {
    targetDestroyedVS();
  }
}

void setup()
{
  Serial.begin(115200);

  Serial.println(F("START " __FILE__ " from " __DATE__ "\r\nUsing library version " VERSION_IRREMOTE));
  Serial.print("INFO: number of targets initialized: ");
  Serial.println(numTargets);

  irTarget *activeTarget = irTargets[activeTargetIdx];
  IrReceiver.begin(activeTarget->irPin, ENABLE_LED_FEEDBACK);
  IrReceiver.registerReceiveCompleteCallback(ReceiveCompleteCallbackHandler);
  printActivePin();

  FastLED.addLeds<LED_TYPE, LED_DATA_PIN_1, COLOR_ORDER>(irTargets[0]->leds, NUM_LEDS);
  FastLED.addLeds<LED_TYPE, LED_DATA_PIN_2, COLOR_ORDER>(irTargets[1]->leds, NUM_LEDS);
  FastLED.addLeds<LED_TYPE, LED_DATA_PIN_3, COLOR_ORDER>(irTargets[2]->leds, NUM_LEDS);
  FastLED.addLeds<LED_TYPE, LED_DATA_PIN_4, COLOR_ORDER>(irTargets[3]->leds, NUM_LEDS);
  // FastLED.addLeds<LED_TYPE, LED_DATA_PIN_4, COLOR_ORDER>(irTargets[3]->leds, NUM_LEDS);

  for (int i = 0; i < numTargets; i++)
  {
    fill_solid(irTargets[i]->leds, NUM_LEDS, CRGB::Red);
  }
  FastLED.setBrightness(BRIGHTNESS);

  pinMode(alertPin, OUTPUT);
  pinMode(buttonPin, INPUT);
  pinMode(10, OUTPUT);
  servo.attach(servoPin);

  resetGameState();
}

void loop()
{

  digitalWrite(alertPin, ledState);

  if (servoMoving && (millis() > (lastServoMove + servoMoveMs)))
  {
    servo.write(servoPos);
    lastServoMove = millis();
    if (servoIncreasing)
    {
      servoPos += 1;
    }
    else
    {
      servoPos -= 1;
    }

    if (servoPos == 180)
    {
      servoIncreasing = false;
    }

    if (servoPos == 0)
    {
      servoIncreasing = true;
    }
  }

  debounceButton();

  if (sIRDataJustReceived)
  {
    if (gameActive)
    {
      emitTargetHit();
    }
    assert(IrReceiver.irparams.IRReceivePin == irTargets[activeTargetIdx]->irPin);

    uint16_t sumIrBuffer = 0;
    for (uint8_t i = 0; i < IrReceiver.irparams.rawlen; i++)
    {
      sumIrBuffer = IrReceiver.irparams.rawbuf[i];
    }

    lastPlayerHit = sumIrBuffer < 80 ? 0 : 1;

    playerStates[lastPlayerHit].hitsOnTarget += playerStates[lastPlayerHit].hitsPerSignal;
    totalHitsOnTarget += playerStates[lastPlayerHit].hitsPerSignal;

    sIRDataJustReceived = false;
  }

  if (gameActiveChangeSignalReceived)
  {
    // simple debounce logic to avoid repeated hits
    if (millis() - lastActiveStateChangeTime > debounceDelay) {    
      lastActiveStateChangeTime = millis();
      gameActive = !gameActive;
      emitGameActiveState();
      if (gameActive)
      {
        resetGameState();
        gameStart = millis();
        delay(1000);
        targetStart = millis();
      } else {
        activeTargetIdx = 0;
      }
    }

    gameActiveChangeSignalReceived = false;
  }

  if (gameModeChangeSignalReceived)
  {
    emitGameMode();
    gameModeChangeSignalReceived = false;
  }

  if (gameActive && millis() > (gameStart + gameLength + MS_OFFSET))
  {
    Serial.println(F("time expired"));
    gameActive = false;
    for (int i = 0; i < 6; i++)
    {
      blinkRainbow();
    }
    emitGameActiveState();
  }

  // If game is not active, just display a holding color until ready
  if (!gameActive)
  {
    for (int i = 0; i < numTargets; i++)
    {
      fill_rainbow(irTargets[i]->leds, NUM_LEDS, 0, 16);
    }
    safeFastLedShow();
    return;
  }

  if (
      (gameMode == SHOWDOWN_GAME_MODE && totalHitsOnTarget >= NUM_LEDS) ||
      (gameMode == VS_GAME_MODE && playerStates[lastPlayerHit].hitsOnTarget >= NUM_LEDS / 2))
  {
    targetDestroyedUpdate();
    targetStart = millis();
  }

  if (randomTargetJumpEnabled && millis() > lastJumpTime + MIN_MS_BETWEEN_JUMP && random8(100) < 0.05)
  {
    lastJumpTime = millis();
    activateRandomNextTarget();
  }

  fillLeds();
  safeFastLedShow();
}

void ReceiveCompleteCallbackHandler()
{
  IrReceiver.decode();
  Serial.print("protocol ");
  Serial.println(IrReceiver.decodedIRData.protocol);
  Serial.print("command ");
  Serial.println(IrReceiver.decodedIRData.command);

  if (IrReceiver.decodedIRData.protocol == 8 && IrReceiver.decodedIRData.command == 10) {
    gameActiveChangeSignalReceived = true;
  }

  if (IrReceiver.decodedIRData.protocol == NEC)
  {
    if (IrReceiver.decodedIRData.command == 0x43)
    {
      gameActiveChangeSignalReceived = true;
    }
    else if (!gameActive)
    {
      if (IrReceiver.decodedIRData.command == 0xC)
      {
        gameModeChangeSignalReceived = true;
        gameMode = SHOWDOWN_GAME_MODE;
      }
      else if (IrReceiver.decodedIRData.command == 0X18)
      {
        gameModeChangeSignalReceived = true;
        gameMode = VS_GAME_MODE;
      }
    }
  }

  if (IrReceiver.irparams.rawlen == 18)
  {
    sIRDataJustReceived = true;
    ledState = !ledState;
  }

  IrReceiver.resume();
}