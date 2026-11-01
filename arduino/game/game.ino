#define DECODE_NEC
#define FASTLED_ALLOW_INTERRUPTS 0
// Lets IRremote's timer interrupt run a second receiver, so versus mode can
// listen to both players' targets at once
#define SUPPORT_MULTIPLE_RECEIVER_INSTANCES
void UserIRReceiveTimerInterruptHandler();

#include <FastLED.h>
#include <Arduino.h>
#include <IRremote.hpp>
#include <Servo.h>

// #define DEBUG  // Activate this for lots of lovely debug output from the IR decoders
// #define PRINT_IR_CODES  // Print the protocol and command of every IR signal, for finding the codes of a new remote
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

// A blaster shot is an IR frame of this many raw timings. Anything else, like
// the remote, isn't a hit.
#define SHOT_RAWLEN 18
// A destroyed target blinks for this long, then stays red for a random time in
// this range before its player gets a new one. Versus is quicker, so the two
// players race each other.
#define DESTROYED_BLINK_MS 800
#define MIN_RED_MS 1000
#define MAX_RED_MS 2500
#define VS_DESTROYED_BLINK_MS 400
#define VS_MIN_RED_MS 200
#define VS_MAX_RED_MS 500

// A working IR receiver's output sits HIGH between shots, and even heavy fire
// leaves gaps (kids spamming a target read up to ~60% LOW). One that reads LOW
// for at least this share of a window is unpowered or broken, so it's never made
// the live target. Each receiver's share is sent to the server every
// IR_HEALTH_WINDOW_MS, which shows it on the admin page.
#define IR_STUCK_LOW_PERCENT 98
#define IR_HEALTH_WINDOW_MS 1000
// A receiver stays marked broken this long after its last bad reading, so one
// that only works on and off isn't picked during a good moment
#define IR_RECOVERY_MS 10000

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

// Targets turned off from the admin page, e.g. with a broken sensor. They're
// never made a live target, but their LEDs still light up like the others.
bool targetEnabled[numTargets] = {true, true, true, true};

// IR receiver health: how much of the last window each one read LOW, 0-100
uint8_t irLowPercent[numTargets];
unsigned long irBrokenUntil[numTargets];
unsigned long irLowSamples[numTargets];
unsigned long irSamples = 0;
unsigned long irWindowStart = 0;

/*
 * Game state variables
 */
bool gameActive = false;
uint8_t gameMode = SHOWDOWN_GAME_MODE;
unsigned long gameStart;
unsigned long gameLength = 60000;

/*
 * Player state variables. Each player has a live target of their own: showdown
 * has one player, and versus has two, each scoring on the target lit in their
 * color.
 */
struct playerState
{
  uint16_t score = 0;
  uint8_t targetsDestroyed = 0;
  // LED points each shot knocks off their target, which is destroyed at NUM_LEDS
  uint8_t hitsPerSignal = NUM_LEDS;
  bool targetJumps = false;
  unsigned long lastJumpTime = 0;

  uint8_t targetIdx = 0;
  uint8_t hitsOnTarget = 0;
  // False while the target shows it was destroyed, until the player gets the next
  bool targetLive = false;
  // When the target went live, or was destroyed
  unsigned long targetSince = 0;
  // How long it shows it was destroyed for
  uint16_t destroyedMs = 0;
  // Set from the IR interrupt when the target's receiver hears a shot
  volatile bool shotReceived = false;
};

struct playerState playerStates[2];
const uint8_t numPlayers = ARRAY_SIZE(playerStates);

// Each player's target has its own IR receiver. Player 1's is IRremote's own,
// which also listens for the remote.
IRrecv secondIrReceiver;
IRrecv *const playerReceivers[numPlayers] = {&IrReceiver, &secondIrReceiver};

/*
 * ISR Variables for callback
 */
volatile bool gameActiveChangeSignalReceived = false;
volatile bool gameModeChangeSignalReceived = false;
volatile byte ledState = HIGH;

void firstReceiverCallback();
void secondReceiverCallback();

const int alertPin = 12;
const int buttonPin = 11;
const int servoPin = 10;

// Commands the server sends over serial, as an alternative to the IR remote.
// One byte each, so none get lost while FastLED has interrupts turned off.
#define CMD_START 's'
#define CMD_STOP 'x'
#define CMD_SHOWDOWN '0'
#define CMD_VERSUS '1'
// '@' plus a bit mask of the targets that are enabled, from '@' (none) to 'O'
// (all four). Bit 0 is target 1.
#define CMD_ENABLED_TARGETS '@'

uint8_t analogPinValue = 0;

const unsigned long debounceDelay = 100;
unsigned long lastDebounceTime = 0;
unsigned long lastActiveStateChangeTime = 0;
byte lastButtonState = LOW;
byte buttonState = LOW;

uint8_t numPlayersInGame()
{
  return gameMode == VS_GAME_MODE ? numPlayers : 1;
}

uint16_t destroyedBlinkMs()
{
  return gameMode == VS_GAME_MODE ? VS_DESTROYED_BLINK_MS : DESTROYED_BLINK_MS;
}

CRGB playerColor(uint8_t player)
{
  if (gameMode == SHOWDOWN_GAME_MODE)
  {
    return CRGB::Green;
  }
  return player ? CRGB::Blue : CRGB::Orange;
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

// Each player's live target, numbered from 1, or 0 while their last one shows
// it was destroyed
void emitLiveTargets()
{
  Serial.print(F("EMIT LIVE_TARGETS"));
  for (uint8_t p = 0; p < numPlayersInGame(); p++)
  {
    Serial.print(' ');
    Serial.print(playerStates[p].targetLive ? playerStates[p].targetIdx + 1 : 0);
  }
  Serial.println();
}

// "EMIT <event> <target> <player>", both numbered from 1
void emitTargetEvent(const __FlashStringHelper *event, uint8_t player)
{
  Serial.print(F("EMIT "));
  Serial.print(event);
  Serial.print(' ');
  Serial.print(playerStates[player].targetIdx + 1);
  Serial.print(' ');
  Serial.println(player + 1);
}

void sampleIrReceivers()
{
  irSamples++;
  for (uint8_t i = 0; i < numTargets; i++)
  {
    if (digitalRead(irTargets[i]->irPin) == LOW)
    {
      irLowSamples[i]++;
    }
  }
}

void reportIrReceivers()
{
  Serial.print(F("EMIT IR_RECEIVERS"));
  for (uint8_t i = 0; i < numTargets; i++)
  {
    irLowPercent[i] = irSamples ? irLowSamples[i] * 100 / irSamples : 0;
    irLowSamples[i] = 0;
    if (irLowPercent[i] >= IR_STUCK_LOW_PERCENT)
    {
      irBrokenUntil[i] = millis() + IR_RECOVERY_MS;
    }
    Serial.print(' ');
    Serial.print(irLowPercent[i]);
  }
  Serial.println();
  irSamples = 0;
  irWindowStart = millis();
}

void resetGameState()
{
  for (int i = 0; i < numPlayers; i++)
  {
    playerStates[i].score = 0;
    playerStates[i].targetsDestroyed = 0;
    playerStates[i].hitsPerSignal = NUM_LEDS;
    playerStates[i].targetJumps = false;
  }

  emitScore();
  emitGameMode();
}

// How good a pick a target is for a player's next one: 2 if it's enabled and
// its receiver works, 1 if it's only enabled, 0 if it's disabled, and -1 if
// another player has it
int8_t targetSuitability(uint8_t targetIdx, uint8_t player)
{
  for (uint8_t p = 0; p < numPlayersInGame(); p++)
  {
    if (p != player && playerStates[p].targetIdx == targetIdx)
    {
      return -1;
    }
  }
  if (!targetEnabled[targetIdx])
  {
    return 0;
  }
  return millis() >= irBrokenUntil[targetIdx] ? 2 : 1;
}

// Move a player to a random one of the best targets for them, and point their
// IR receiver at it. Their hits carry over, so a jumping target isn't easier.
void moveTarget(uint8_t player)
{
  uint8_t candidates[numTargets];
  uint8_t numCandidates = 0;
  int8_t best = -1;
  for (uint8_t i = 0; i < numTargets; i++)
  {
    int8_t suitability = targetSuitability(i, player);
    if (suitability > best)
    {
      best = suitability;
      numCandidates = 0;
    }
    if (suitability == best)
    {
      candidates[numCandidates++] = i;
    }
  }
  playerState &p = playerStates[player];
  p.targetIdx = candidates[random8(numCandidates)];
  playerReceivers[player]->setReceivePin(irTargets[p.targetIdx]->irPin);
  playerReceivers[player]->resume();
  // Drop shots that landed on the previous target (e.g. during the destroyed
  // animation) so they aren't credited to this one
  p.shotReceived = false;
  emitLiveTargets();
}

// Give a player a fresh live target
void activateNextTarget(uint8_t player)
{
  playerState &p = playerStates[player];
  p.hitsOnTarget = 0;
  p.targetLive = true;
  p.targetSince = millis();
  moveTarget(player);
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

void setEnabledTargets(uint8_t mask)
{
  for (uint8_t i = 0; i < numTargets; i++)
  {
    targetEnabled[i] = mask & (1 << i);
  }
  // Move anyone off a target that was just disabled
  for (uint8_t p = 0; p < numPlayersInGame(); p++)
  {
    if (!targetEnabled[playerStates[p].targetIdx])
    {
      activateNextTarget(p);
    }
  }
}

void readServerCommands()
{
  while (Serial.available())
  {
    char command = Serial.read();
    if ((command == CMD_START && !gameActive) || (command == CMD_STOP && gameActive))
    {
      gameActiveChangeSignalReceived = true;
    }
    else if ((command == CMD_SHOWDOWN || command == CMD_VERSUS) && !gameActive)
    {
      gameMode = command == CMD_VERSUS ? VS_GAME_MODE : SHOWDOWN_GAME_MODE;
      gameModeChangeSignalReceived = true;
    }
    else if (command >= CMD_ENABLED_TARGETS && command < CMD_ENABLED_TARGETS + (1 << numTargets))
    {
      setEnabledTargets(command - CMD_ENABLED_TARGETS);
    }
  }
}

// FastLED turns interrupts off while it sends, which would garble an IR frame
// being received, so only update the LEDs while every receiver is between frames
void safeFastLedShow()
{
  for (uint8_t p = 0; p < numPlayersInGame(); p++)
  {
    if (!playerReceivers[p]->isIdle())
    {
      return;
    }
  }
  FastLED.show();
}

void fillLeds()
{
  // Targets nobody has are red
  for (int i = 0; i < numTargets; i++)
  {
    fill_solid(irTargets[i]->leds, NUM_LEDS, CRGB::Red);
  }

  for (uint8_t p = 0; p < numPlayersInGame(); p++)
  {
    playerState &player = playerStates[p];
    CRGB *leds = irTargets[player.targetIdx]->leds;
    unsigned long sinceMs = millis() - player.targetSince;
    if (player.targetLive)
    {
      // Each hit knocks LEDs out, from the end
      fill_solid(leds, NUM_LEDS - player.hitsOnTarget, playerColor(p));
      fill_solid(leds + NUM_LEDS - player.hitsOnTarget, player.hitsOnTarget, CRGB::Black);
    }
    else if (sinceMs < destroyedBlinkMs())
    {
      // Destroyed: blink rainbow in showdown, or the player's color in versus,
      // then stay red until the next target
      bool on = sinceMs / 100 % 2 == 0;
      if (!on)
      {
        fill_solid(leds, NUM_LEDS, CRGB::Black);
      }
      else if (gameMode == SHOWDOWN_GAME_MODE)
      {
        fill_rainbow(leds, NUM_LEDS, 0, 16);
      }
      else
      {
        fill_solid(leds, NUM_LEDS, playerColor(p));
      }
    }
  }
}

// Points for destroying a target
uint16_t destroyPoints(playerState &player)
{
  if (gameMode == SHOWDOWN_GAME_MODE)
  {
    // The faster the better: 200 within 0.1s, dropping to 100 at 5s
    unsigned long timeToDestroyTarget = millis() - player.targetSince;
    if (timeToDestroyTarget <= 100)
    {
      return 200;
    }
    else if (timeToDestroyTarget > 5000)
    {
      return 100;
    }
    return 100 + (uint16_t)((5000 - timeToDestroyTarget) * 100 / 5000);
  }
  return player.targetsDestroyed < LEVEL_1 ? 100 : player.targetsDestroyed < LEVEL_2 ? 200
                                               : player.targetsDestroyed < LEVEL_3   ? 600
                                                                                     : 1000;
}

// A player's targets get harder as they destroy more: they start jumping
// around, and from LEVEL_2 take 2 hits each
void levelUp(playerState &player)
{
  if (player.targetsDestroyed == (gameMode == SHOWDOWN_GAME_MODE ? LEVEL_1 : LEVEL_3))
  {
    player.targetJumps = true;
  }
  if (player.targetsDestroyed == LEVEL_2)
  {
    player.hitsPerSignal = NUM_LEDS / 2;
  }
}

void targetDestroyed(uint8_t player)
{
  playerState &p = playerStates[player];
  p.targetsDestroyed += 1;
  p.score += destroyPoints(p);
  levelUp(p);
  p.targetLive = false;
  p.targetSince = millis();
  p.destroyedMs = destroyedBlinkMs() + (gameMode == VS_GAME_MODE ? random16(VS_MIN_RED_MS, VS_MAX_RED_MS)
                                                                 : random16(MIN_RED_MS, MAX_RED_MS));

  emitTargetEvent(F("TARGET_DESTROYED"), player);
  emitScore();
  emitLiveTargets();
}

void updatePlayer(uint8_t player)
{
  playerState &p = playerStates[player];
  if (!p.targetLive)
  {
    if (millis() - p.targetSince >= p.destroyedMs)
    {
      activateNextTarget(player);
    }
    return;
  }

  if (p.shotReceived)
  {
    p.shotReceived = false;
    emitTargetEvent(F("TARGET_HIT"), player);
    p.hitsOnTarget += p.hitsPerSignal;
    if (p.hitsOnTarget >= NUM_LEDS)
    {
      targetDestroyed(player);
      return;
    }
  }

  // About a 1 in 100 chance each time round the loop
  if (p.targetJumps && millis() > p.lastJumpTime + MIN_MS_BETWEEN_JUMP && random8(100) == 0)
  {
    p.lastJumpTime = millis();
    moveTarget(player);
  }
}

void setup()
{
  Serial.begin(115200);
  // The server identifies this board by this line
  Serial.println(F("HELLO GAME"));

  Serial.println(F("START " __FILE__ " from " __DATE__ "\r\nUsing library version " VERSION_IRREMOTE));
  Serial.print("INFO: number of targets initialized: ");
  Serial.println(numTargets);

  for (uint8_t i = 0; i < numTargets; i++)
  {
    pinMode(irTargets[i]->irPin, INPUT);
  }
  // Check the receivers so the first target is one that works
  irWindowStart = millis();
  while (millis() - irWindowStart < 100)
  {
    sampleIrReceivers();
  }
  reportIrReceivers();
  // Starts the IR timer interrupt, which runs both receivers
  IrReceiver.begin(irTargets[0]->irPin, ENABLE_LED_FEEDBACK);
  IrReceiver.registerReceiveCompleteCallback(firstReceiverCallback);
  secondIrReceiver.registerReceiveCompleteCallback(secondReceiverCallback);
  activateNextTarget(0);

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

  sampleIrReceivers();
  if (millis() - irWindowStart >= IR_HEALTH_WINDOW_MS)
  {
    reportIrReceivers();
  }

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

  readServerCommands();

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
        // Start everyone on a random working target, which also points their
        // receivers at them. Doing it after the pause means shots fired during
        // it aren't credited.
        for (uint8_t p = 0; p < numPlayersInGame(); p++)
        {
          activateNextTarget(p);
        }
      }
    }

    gameActiveChangeSignalReceived = false;
  }

  if (gameModeChangeSignalReceived)
  {
    emitGameMode();
    // Versus has a second player, who needs a target of their own
    for (uint8_t p = 0; p < numPlayersInGame(); p++)
    {
      activateNextTarget(p);
    }
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

  for (uint8_t p = 0; p < numPlayersInGame(); p++)
  {
    updatePlayer(p);
  }

  fillLeds();
  safeFastLedShow();
}

// Called from the IR timer interrupt when a receiver has a complete frame
void handleIrFrame(uint8_t player)
{
  if (playerReceivers[player]->irparams.rawlen == SHOT_RAWLEN)
  {
    playerStates[player].shotReceived = true;
    ledState = !ledState;
  }
  playerReceivers[player]->resume();
}

void firstReceiverCallback()
{
  IrReceiver.decode();
#ifdef PRINT_IR_CODES
  Serial.print("protocol ");
  Serial.println(IrReceiver.decodedIRData.protocol);
  Serial.print("command ");
  Serial.println(IrReceiver.decodedIRData.command);
#endif

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

  handleIrFrame(0);
}

void secondReceiverCallback()
{
  handleIrFrame(1);
}

// IRremote calls this from its timer interrupt after running IrReceiver
void UserIRReceiveTimerInterruptHandler()
{
  if (numPlayersInGame() > 1)
  {
    secondIrReceiver.ReceiveInterruptHandler();
  }
}
