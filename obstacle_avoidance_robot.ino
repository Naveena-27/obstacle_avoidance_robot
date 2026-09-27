/*
  Obstacle Avoidance Chassis Robot Pet
  ESP32 + L298N motor driver + HC-SR04 ultrasonic + SG90 scan servo + MAX98357A I2S amp (voice clips) + SSD1306 OLED (mouth expressions)

  The robot drives forward until it detects an obstacle, then backs up,
  scans left/right with the servo, and turns toward the more open side.
  Voice clips and OLED mouth expressions react to each stage of that
  decision (spotting an obstacle, scanning, turning, clearing a path).

  Full wiring diagram, pin table, and library list: see README.md
  Voice clips are loaded from LittleFS (/data) — 19 short WAV files,
  16-bit PCM mono, named for the event they play on (obstacle, scan,
  left, right, turn, blocked, easy, idle, bored, etc).
*/

#include <ESP32Servo.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <LittleFS.h>
#include <AudioFileSourceLittleFS.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>

//Motor driver pins
#define ENA 14
#define IN1 26
#define IN2 27
#define IN3 25
#define IN4 33
#define ENB 13

//Ultrasonic pins
#define TRIG_PIN 19
#define ECHO_PIN 18

//Servo 
#define SERVO_PIN 23
Servo scanServo;
const int SERVO_CENTER = 138;
const int SERVO_LEFT   = 78;
const int SERVO_RIGHT  = 180;

//Tuning 
const int OBSTACLE_DISTANCE = 20;
const int DANGER_DISTANCE   = 8;
const int MOTOR_SPEED       = 100;
const int TURN_SPEED        = 160;
const int TURN_TIME         = 450;
const int BACKUP_TIME       = 400;

//Motor polarity
const bool RIGHT_REVERSED = true;
const bool LEFT_REVERSED  = false;

//MAX98357A
#define I2S_BCLK 4
#define I2S_LRC  32
#define I2S_DOUT 15
AudioGeneratorWAV *wav;
AudioFileSourceLittleFS *audioFile;
AudioOutputI2S *audioOut;
bool audioReady = false;

//OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
bool displayReady = false;

//Personality state 
const unsigned long REPEAT_OBSTACLE_WINDOW = 15000;
const unsigned long IDLE_HUM_INTERVAL      = 10000;
const unsigned long BORED_THRESHOLD        = 30000;

unsigned long lastObstacleTime   = 0;
bool hadPreviousObstacle         = false;
bool wasBlocked                  = false;
unsigned long clearDrivingSince  = 0;
unsigned long lastIdleHumTime    = 0;
bool boredPlayedThisStreak       = false;

void setup() {
  Serial.begin(115200);

  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
  pinMode(ENA, OUTPUT); pinMode(ENB, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  scanServo.setPeriodHertz(50);
  scanServo.attach(SERVO_PIN, 500, 2400);
  scanServo.write(SERVO_CENTER);
  delay(500);

  if (LittleFS.begin(true)) {
    audioOut = new AudioOutputI2S();
    audioOut->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audioOut->SetGain(0.6);
    wav = new AudioGeneratorWAV();
    audioReady = true;
    Serial.println("LittleFS + I2S audio ready");
  } else {
    Serial.println("LittleFS mount failed — check partition scheme. Continuing without sound.");
  }

  Wire.begin(21, 22);
  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    displayReady = true;
    display.setRotation(2); // physically mounted upside down
    faceIdle();
    Serial.println("OLED ready");
  } else {
    Serial.println("OLED not found — check wiring/address (try 0x3D). Continuing without display.");
  }

  stopMotors();

  playSound("/hello.wav");
  waitForAudio();
  pumpDelay(150);
  playSound("/start.wav");
  waitForAudio();

  clearDrivingSince = millis();
}

void loop() {
  pumpAudio();

  scanServo.write(SERVO_CENTER);
  pumpDelay(250);
  long frontDist = getDistanceCM();
  Serial.print("Front: ");
  Serial.println(frontDist);

  if (frontDist > OBSTACLE_DISTANCE) {
    if (wasBlocked) {
      faceHappy();
      playSound("/clear.wav");
      clearDrivingSince = millis();
      lastIdleHumTime = clearDrivingSince;
      boredPlayedThisStreak = false;
      wasBlocked = false;
      faceIdle();
    } else {
      unsigned long clearFor = millis() - clearDrivingSince;
      if (!boredPlayedThisStreak && clearFor >= BORED_THRESHOLD) {
        playSound("/bored.wav");
        boredPlayedThisStreak = true;
      } else if (millis() - lastIdleHumTime >= IDLE_HUM_INTERVAL) {
        playSound("/idle.wav");
        lastIdleHumTime = millis();
      }
    }
    moveForward();
  } else {
    wasBlocked = true;
    handleObstacle(frontDist);
  }
}

//Obstacle reactions 
void handleObstacle(long frontDist) {
  stopMotors();
  faceSurprised();

  bool isRepeat = hadPreviousObstacle && (millis() - lastObstacleTime < REPEAT_OBSTACLE_WINDOW);
  if (isRepeat) {
    playSound("/again.wav");
  } else if (frontDist <= DANGER_DISTANCE) {
    playSound("/danger.wav");
  } else {
    playSound("/obstacle.wav");
  }
  lastObstacleTime = millis();
  hadPreviousObstacle = true;
  waitForAudio();

  pumpDelay(200);
  moveBackward();
  pumpDelay(BACKUP_TIME);
  stopMotors();
  pumpDelay(200);

  faceLookLeft();
  scanServo.write(SERVO_LEFT);
  playSound("/scan.wav");
  pumpDelay(400);
  long leftDist = getDistanceCM();
  Serial.print("Left: "); Serial.println(leftDist);

  faceLookRight();
  scanServo.write(SERVO_RIGHT);
  playSound("/scan.wav");
  pumpDelay(600);
  long rightDist = getDistanceCM();
  Serial.print("Right: "); Serial.println(rightDist);

  scanServo.write(SERVO_CENTER);
  pumpDelay(250);
  faceIdle();

  bool bothBlocked = (leftDist <= OBSTACLE_DISTANCE && rightDist <= OBSTACLE_DISTANCE);

  if (bothBlocked) {
    faceSurprised();
    playSound("/blocked.wav");
    waitForAudio();
    turnRight();
    playSound("/turn.wav");
    pumpDelay(TURN_TIME + 200);
    stopMotors();
  } else if (leftDist > rightDist) {
    faceLookLeft();
    playSound("/left.wav");
    waitForAudio();
    turnLeft();
    playSound("/turn.wav");
    pumpDelay(TURN_TIME);
    stopMotors();
  } else {
    faceLookRight();
    playSound("/right.wav");
    waitForAudio();
    turnRight();
    playSound("/turn.wav");
    pumpDelay(TURN_TIME);
    stopMotors();
  }

  scanServo.write(SERVO_CENTER);
  pumpDelay(200);
  long recheck = getDistanceCM();
  if (recheck <= OBSTACLE_DISTANCE) {
    faceSurprised();
    playSound("/unexpected.wav");
    waitForAudio();
  } else {
    faceHappy();
    playSound(bothBlocked ? "/yay.wav" : "/easy.wav");
    waitForAudio();
    pumpDelay(200);
    faceIdle();
  }
}

//Audio helpers
void playSound(const char *filename) {
  if (!audioReady) return;
  if (wav->isRunning()) wav->stop();
  audioFile = new AudioFileSourceLittleFS(filename);
  wav->begin(audioFile, audioOut);
}

void pumpAudio() {
  if (audioReady && wav->isRunning()) {
    if (!wav->loop()) wav->stop();
  }
}

void waitForAudio() {
  if (!audioReady) return;
  while (wav->isRunning()) {
    if (!wav->loop()) wav->stop();
  }
}

void pumpDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    pumpAudio();
  }
}

// Not auto-triggered — call these once you wire up the matching event.
void greeting() { playSound("/greeting.wav"); }
void tired()    { playSound("/tired.wav"); }
void bye()      { playSound("/bye.wav"); }

//OLED — mouth only; HC-SR04 = "eyes"
void drawMouthCurve(int16_t x0, int16_t y0, int16_t cx, int16_t cy,
                     int16_t x1, int16_t y1, uint8_t thickness) {
  const int STEPS = 28;
  float prevX = x0, prevY = y0;
  for (int i = 1; i <= STEPS; i++) {
    float t = (float)i / STEPS;
    float xt = (1 - t) * (1 - t) * x0 + 2 * (1 - t) * t * cx + t * t * x1;
    float yt = (1 - t) * (1 - t) * y0 + 2 * (1 - t) * t * cy + t * t * y1;
    for (uint8_t w = 0; w < thickness; w++) {
      display.drawLine((int16_t)prevX, (int16_t)prevY + w,
                        (int16_t)xt, (int16_t)yt + w, SSD1306_WHITE);
    }
    prevX = xt;
    prevY = yt;
  }
}

void faceIdle() {
  if (!displayReady) return;
  display.clearDisplay();
  // relaxed, almost-flat closed mouth spanning nearly the full width
  drawMouthCurve(4, 30, 64, 35, 124, 30, 7);
  display.display();
}

void faceSurprised() {
  if (!displayReady) return;
  display.clearDisplay();
  // big open "O" ring, nearly filling the 64px screen height
  display.fillCircle(64, 32, 31, SSD1306_WHITE);
  display.fillCircle(64, 32, 18, SSD1306_BLACK);
  display.display();
}

void faceLookLeft() {
  if (!displayReady) return;
  display.clearDisplay();
  // large pursed "thinking" mouth, shifted toward the left side
  display.fillCircle(46, 32, 27, SSD1306_WHITE);
  display.fillCircle(46, 32, 15, SSD1306_BLACK);
  display.display();
}

void faceLookRight() {
  if (!displayReady) return;
  display.clearDisplay();
  // large pursed "thinking" mouth, shifted toward the right side
  display.fillCircle(82, 32, 27, SSD1306_WHITE);
  display.fillCircle(82, 32, 15, SSD1306_BLACK);
  display.display();
}

void faceHappy() {
  if (!displayReady) return;
  display.clearDisplay();
  // big smooth smile stretching edge-to-edge and near top-to-bottom
  drawMouthCurve(2, 8, 64, 63, 126, 8, 8);
  display.display();
}

//Ultrasonic sensor
long getDistanceCM() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return 400;
  return duration * 0.0343 / 2;
}

//Motor control
void rightForward()  { digitalWrite(IN1, RIGHT_REVERSED ? LOW : HIGH); digitalWrite(IN2, RIGHT_REVERSED ? HIGH : LOW); }
void rightBackward() { digitalWrite(IN1, RIGHT_REVERSED ? HIGH : LOW); digitalWrite(IN2, RIGHT_REVERSED ? LOW : HIGH); }
void leftForward()   { digitalWrite(IN3, LEFT_REVERSED ? LOW : HIGH);  digitalWrite(IN4, LEFT_REVERSED ? HIGH : LOW); }
void leftBackward()  { digitalWrite(IN3, LEFT_REVERSED ? HIGH : LOW);  digitalWrite(IN4, LEFT_REVERSED ? LOW : HIGH); }

void moveForward() {
  rightForward(); leftForward();
  analogWrite(ENA, MOTOR_SPEED);
  analogWrite(ENB, MOTOR_SPEED);
}

void moveBackward() {
  rightBackward(); leftBackward();
  analogWrite(ENA, MOTOR_SPEED);
  analogWrite(ENB, MOTOR_SPEED);
}

void turnLeft() {
  rightForward(); leftBackward();
  analogWrite(ENA, TURN_SPEED);
  analogWrite(ENB, TURN_SPEED);
}

void turnRight() {
  rightBackward(); leftForward();
  analogWrite(ENA, TURN_SPEED);
  analogWrite(ENB, TURN_SPEED);
}

void stopMotors() {
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
  analogWrite(ENA, 0);
  analogWrite(ENB, 0);
}
