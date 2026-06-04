#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_MCP23X17.h>
#include <BluetoothSerial.h>
#include <esp_timer.h>

// Bloco: Objetos principais usados pelo LCD, expansor MCP23017 e Bluetooth.
LiquidCrystal_I2C lcd(0x20, 16, 2);
Adafruit_MCP23X17 mcp;
BluetoothSerial SerialBT;

// Bloco: Pinos e enderecos de hardware usados no projeto.
#define LCD_ADDR 0x20
#define MCP_ADDR 0x21
#define ADC_NIVEL 34
#define MOTOR_PWM_PIN 25
#define BT_NAME "ESP32_G4_G9_WASHER"

// Bloco: Linhas e colunas do teclado 4x4 ligado ao MCP23017.
#define ROW1 0
#define ROW2 1
#define ROW3 2
#define ROW4 3
#define COL1 4
#define COL2 5
#define COL3 6
#define COL4 7

// Bloco: Saidas digitais do MCP23017 para os atuadores simulados.
#define MCP_TRAVA_PORTA 12
#define MCP_BOMBA 13
#define MCP_VALVULA 14
#define MCP_AUX_LED 15

// Bloco: Parametros do PWM real do motor no ESP32.
static const uint8_t MOTOR_PWM_CHANNEL = 0;
static const uint16_t MOTOR_PWM_FREQ = 2000;
static const uint8_t MOTOR_PWM_RESOLUTION = 8;
static const uint8_t MOTOR_PWM_WASH = 140;
static const uint8_t MOTOR_PWM_RINSE = 120;
static const uint8_t MOTOR_PWM_SPIN = 220;

// Bloco: Tempos do processo em milissegundos para a demonstracao da bancada.
static const uint32_t DOOR_LOCK_MS = 1500;
static const uint32_t FILL_TIMEOUT_MS = 30000;
static const uint32_t WASH_MS = 15000;
static const uint32_t DRAIN_TIMEOUT_MS = 12000;
static const uint32_t RINSE_BASE_MS = 9000;
static const uint32_t RINSE_ECO_MS = 5000;
static const uint32_t SPIN_MS = 10000;
static const uint32_t COMPLETE_MS = 4000;
static const uint32_t LCD_REFRESH_MS = 250;
static const uint32_t STATUS_PUSH_MS = 1000;

// Bloco: GRUPO 4 - Escala do atraso programavel.
// Para a demonstracao, 1 hora configurada equivale a 15 segundos reais.
static const uint32_t DEMO_DELAY_PER_HOUR_MS = 15000;

// Bloco: GRUPO 9 - Limites do ciclo economico adaptativo.
// Se o nivel inicial estiver baixo, o enxague fica mais curto para simular economia de agua.
static const int TARGET_WASH_LEVEL_PCT = 65;
static const int TARGET_RINSE_LEVEL_PCT = 50;
static const int DRAIN_EMPTY_LEVEL_PCT = 10;
static const int ECO_START_THRESHOLD_PCT = 35;

// Bloco: Estados principais da maquina de lavar exigida na avaliacao.
enum WashState {
  ST_IDLE,
  ST_WAIT_DELAY,
  ST_LOCK_DOOR,
  ST_FILL_WASH,
  ST_WASH,
  ST_DRAIN_WASH,
  ST_FILL_RINSE,
  ST_RINSE,
  ST_DRAIN_RINSE,
  ST_SPIN,
  ST_COMPLETE,
  ST_FAULT
};

// Bloco: Base de tempo do firmware usando esp_timer em vez de delay() ou millis().
volatile uint32_t gTicks10ms = 0;
esp_timer_handle_t gSystemTimer = nullptr;

// Bloco: Flags de disponibilidade dos perifericos para diagnostico e contingencia.
bool mcpOk = false;
bool btOk = false;

// Bloco: Variaveis globais de processo, entradas, saidas e interface.
WashState currentState = ST_IDLE;
uint32_t stateStartedMs = 0;
uint32_t lastLcdRefreshMs = 0;
uint32_t lastStatusPushMs = 0;
bool valveOn = false;
bool pumpOn = false;
bool doorLocked = false;
bool auxLedOn = false;
bool btConnected = false;

// Bloco: GRUPO 9 - Guarda se o ciclo economico adaptativo foi ativado no inicio do ciclo.
bool ecoModeActive = false;

// Bloco: GRUPO 4 - Variaveis do atraso configurado por teclado ou Bluetooth.
bool delayEditMode = false;
char delayDigits[3] = "";
uint8_t delayDigitCount = 0;
uint8_t configuredDelayHours = 0;
uint8_t scheduledDelayHours = 0;
uint32_t scheduledDelayMs = 0;
uint32_t delayDeadlineMs = 0;
char lastKey = '-';
int lastAdcRaw = 0;
int lastLevelPct = 0;

// Bloco: GRUPO 9 - Duracao do enxague muda conforme o nivel de agua inicial.
int rinseDurationMs = RINSE_BASE_MS;
char lcdLine1[17] = "";
char lcdLine2[17] = "";
char btBuffer[32] = "";
uint8_t btBufferLen = 0;
String faultReason = "";

// Bloco: Callback do timer periodico que gera a base de tempo usada pela logica.
void onSystemTick(void *arg) {
  (void)arg;
  gTicks10ms++;
}

// Bloco: Le o tempo atual em milissegundos a partir do timer configurado.
uint32_t nowMs() {
  return gTicks10ms * 10UL;
}

// Bloco: Informa se um intervalo de tempo ja passou desde uma marca inicial.
bool elapsedMs(uint32_t startedAt, uint32_t intervalMs) {
  return (nowMs() - startedAt) >= intervalMs;
}

// Bloco: Traduz o estado atual para texto curto usado no LCD e no Bluetooth.
const char *stateName(WashState state) {
  switch (state) {
    case ST_IDLE: return "IDLE";
    case ST_WAIT_DELAY: return "DELAY";
    case ST_LOCK_DOOR: return "LOCK";
    case ST_FILL_WASH: return "FILL";
    case ST_WASH: return "WASH";
    case ST_DRAIN_WASH: return "DRAIN1";
    case ST_FILL_RINSE: return "RFILL";
    case ST_RINSE: return "RINSE";
    case ST_DRAIN_RINSE: return "DRAIN2";
    case ST_SPIN: return "SPIN";
    case ST_COMPLETE: return "DONE";
    case ST_FAULT: return "FAULT";
    default: return "UNK";
  }
}

// Bloco: Atualiza as flags de saida digital ligadas ao MCP23017.
void writeMcpOutput(uint8_t pin, bool level) {
  if (mcpOk) {
    mcp.digitalWrite(pin, level ? HIGH : LOW);
  }
}

// Bloco: Controla o PWM real do motor no pino do ESP32.
void setMotorDuty(uint8_t duty) {
  ledcWrite(MOTOR_PWM_CHANNEL, duty);
}

// Bloco: Liga ou desliga a valvula de entrada de agua.
void setValve(bool on) {
  valveOn = on;
  writeMcpOutput(MCP_VALVULA, on);
}

// Bloco: Liga ou desliga a bomba de escoamento.
void setPump(bool on) {
  pumpOn = on;
  writeMcpOutput(MCP_BOMBA, on);
}

// Bloco: Liga ou desliga a trava de porta simulada.
void setDoorLock(bool on) {
  doorLocked = on;
  writeMcpOutput(MCP_TRAVA_PORTA, on);
}

// Bloco: Usa uma saida auxiliar como indicador visual de ciclo em andamento.
void setAuxLed(bool on) {
  auxLedOn = on;
  writeMcpOutput(MCP_AUX_LED, on);
}

// Bloco: Coloca todos os atuadores em estado seguro.
void stopAllActuators() {
  setValve(false);
  setPump(false);
  setDoorLock(false);
  setAuxLed(false);
  setMotorDuty(0);
}

// Bloco: Faz a leitura do potenciometro que simula o nivel de agua.
void readLevelAdc() {
  lastAdcRaw = analogRead(ADC_NIVEL);
  lastLevelPct = map(lastAdcRaw, 0, 4095, 0, 100);
  if (lastLevelPct < 0) lastLevelPct = 0;
  if (lastLevelPct > 100) lastLevelPct = 100;
}

// Bloco: Prepara os pinos do teclado matricial no MCP23017.
void setupKeypad() {
  for (int i = 0; i <= 3; i++) {
    mcp.pinMode(i, OUTPUT);
    mcp.digitalWrite(i, HIGH);
  }
  for (int i = 4; i <= 7; i++) {
    mcp.pinMode(i, INPUT_PULLUP);
  }
}

// Bloco: Faz a varredura bruta do teclado 4x4.
char scanKeypadRaw() {
  const char mapKeys[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
  };
  uint8_t rows[4] = {ROW1, ROW2, ROW3, ROW4};
  uint8_t cols[4] = {COL1, COL2, COL3, COL4};

  for (int i = 0; i < 4; i++) {
    mcp.digitalWrite(rows[i], HIGH);
  }

  for (int r = 0; r < 4; r++) {
    mcp.digitalWrite(rows[r], LOW);
    for (int c = 0; c < 4; c++) {
      if (mcp.digitalRead(cols[c]) == LOW) {
        mcp.digitalWrite(rows[r], HIGH);
        return mapKeys[r][c];
      }
    }
    mcp.digitalWrite(rows[r], HIGH);
  }

  return 0;
}

// Bloco: Gera um evento unico por tecla, evitando repeticao por tecla segurada.
char getKeyEvent() {
  static bool locked = false;
  if (!mcpOk) return 0;

  char current = scanKeypadRaw();
  if (current != 0 && !locked) {
    locked = true;
    return current;
  }
  if (current == 0) {
    locked = false;
  }
  return 0;
}

// Bloco: GRUPO 4 - Reinicia o buffer usado para edicao do atraso no teclado.
void resetDelayEdit() {
  delayDigits[0] = '\0';
  delayDigitCount = 0;
}

// Bloco: Entra em um novo estado e atualiza a marca de tempo correspondente.
void enterState(WashState nextState) {
  currentState = nextState;
  stateStartedMs = nowMs();
}

// Bloco: Abre falha controlada para exibir erro e travar o ciclo com seguranca.
void enterFault(const String &reason) {
  faultReason = reason;
  stopAllActuators();
  enterState(ST_FAULT);
  Serial.println("FAULT: " + reason);
  if (btOk) {
    SerialBT.println("FAULT:" + reason);
  }
}

// Bloco: GRUPO 4 e GRUPO 9 - Inicia o ciclo aplicando atraso e modo economico.
void requestCycleStart() {
  if (currentState != ST_IDLE && currentState != ST_COMPLETE) {
    return;
  }

  // GRUPO 9: decide o ciclo economico pelo nivel de agua lido no ADC antes de iniciar.
  readLevelAdc();
  ecoModeActive = (lastLevelPct <= ECO_START_THRESHOLD_PCT);
  rinseDurationMs = ecoModeActive ? RINSE_ECO_MS : RINSE_BASE_MS;

  // GRUPO 4: converte o atraso em horas para tempo de demonstracao.
  scheduledDelayHours = configuredDelayHours;
  scheduledDelayMs = (uint32_t)scheduledDelayHours * DEMO_DELAY_PER_HOUR_MS;
  setAuxLed(true);
  faultReason = "";

  // GRUPO 4: se existir atraso configurado, entra primeiro no estado ST_WAIT_DELAY.
  if (scheduledDelayMs > 0) {
    delayDeadlineMs = nowMs() + scheduledDelayMs;
    enterState(ST_WAIT_DELAY);
  } else {
    enterState(ST_LOCK_DOOR);
  }
}

// Bloco: Cancela o ciclo e retorna ao estado ocioso.
void cancelCycle(const String &reason) {
  stopAllActuators();
  configuredDelayHours = 0;
  scheduledDelayHours = 0;
  scheduledDelayMs = 0;
  delayDeadlineMs = 0;
  delayEditMode = false;
  resetDelayEdit();
  faultReason = reason;
  enterState(ST_IDLE);
  Serial.println("CANCEL: " + reason);
  if (btOk) {
    SerialBT.println("CANCEL:" + reason);
  }
}

// Bloco: GRUPO 4 - Calcula o tempo restante do atraso programado para o LCD e Bluetooth.
uint32_t remainingDelayMs() {
  if (currentState != ST_WAIT_DELAY) return 0;
  uint32_t current = nowMs();
  if (current >= delayDeadlineMs) return 0;
  return delayDeadlineMs - current;
}

// Bloco: Monta a primeira linha do LCD de acordo com o estado do sistema.
void buildLcdLine1(char *out, size_t outSize) {
  if (delayEditMode) {
    snprintf(out, outSize, "ATRASO:%sh", delayDigitCount ? delayDigits : "_");
    return;
  }

  if (currentState == ST_WAIT_DELAY) {
    uint32_t remainSec = remainingDelayMs() / 1000UL;
    snprintf(out, outSize, "START %2lus ECO%c", remainSec, ecoModeActive ? '1' : '0');
    return;
  }

  if (currentState == ST_FAULT) {
    snprintf(out, outSize, "FAULT %-10s", "CHECK");
    return;
  }

  snprintf(out, outSize, "%-6s N:%3d%%", stateName(currentState), lastLevelPct);
}

// Bloco: Monta a segunda linha do LCD com atuadores e conectividade.
void buildLcdLine2(char *out, size_t outSize) {
  if (delayEditMode) {
    snprintf(out, outSize, "#OK *ESC A=%uh", configuredDelayHours);
    return;
  }

  if (currentState == ST_FAULT) {
    snprintf(out, outSize, "BT%c D:%uh", btConnected ? '+' : '-', configuredDelayHours);
    return;
  }

  snprintf(
    out,
    outSize,
    "V%c B%c T%c BT%c",
    valveOn ? '1' : '0',
    pumpOn ? '1' : '0',
    doorLocked ? '1' : '0',
    btConnected ? '+' : '-'
  );
}

// Bloco: Atualiza o LCD sem limpar a tela a cada loop, evitando flicker.
void refreshLcdIfNeeded() {
  if (!elapsedMs(lastLcdRefreshMs, LCD_REFRESH_MS)) {
    return;
  }
  lastLcdRefreshMs = nowMs();

  char newLine1[17];
  char newLine2[17];
  buildLcdLine1(newLine1, sizeof(newLine1));
  buildLcdLine2(newLine2, sizeof(newLine2));

  if (strcmp(newLine1, lcdLine1) != 0) {
    strncpy(lcdLine1, newLine1, sizeof(lcdLine1));
    lcdLine1[16] = '\0';
    lcd.setCursor(0, 0);
    lcd.print("                ");
    lcd.setCursor(0, 0);
    lcd.print(lcdLine1);
  }

  if (strcmp(newLine2, lcdLine2) != 0) {
    strncpy(lcdLine2, newLine2, sizeof(lcdLine2));
    lcdLine2[16] = '\0';
    lcd.setCursor(0, 1);
    lcd.print("                ");
    lcd.setCursor(0, 1);
    lcd.print(lcdLine2);
  }
}

// Bloco: Envia um status detalhado pelo Bluetooth para a demonstracao e diagnostico.
void sendBluetoothStatus() {
  if (!btOk) return;

  SerialBT.print("STATE="); SerialBT.print(stateName(currentState));
  SerialBT.print(" ADC="); SerialBT.print(lastAdcRaw);
  SerialBT.print(" LEVEL="); SerialBT.print(lastLevelPct);
  SerialBT.print(" ECO="); SerialBT.print(ecoModeActive ? 1 : 0);
  SerialBT.print(" DELAY_H="); SerialBT.print(configuredDelayHours);
  SerialBT.print(" REMAIN_MS="); SerialBT.print(remainingDelayMs());
  SerialBT.print(" VALVE="); SerialBT.print(valveOn ? 1 : 0);
  SerialBT.print(" PUMP="); SerialBT.print(pumpOn ? 1 : 0);
  SerialBT.print(" LOCK="); SerialBT.print(doorLocked ? 1 : 0);
  SerialBT.print(" BT="); SerialBT.print(btConnected ? 1 : 0);
  SerialBT.println();
}

// Bloco: Publica periodicamente o status para cumprir o modo de diagnostico remoto.
void pushStatusIfNeeded() {
  if (!btOk || !btConnected) return;
  if (!elapsedMs(lastStatusPushMs, STATUS_PUSH_MS)) return;
  lastStatusPushMs = nowMs();
  sendBluetoothStatus();
}

// Bloco: Interpreta comandos completos recebidos pela serial Bluetooth.
void handleBluetoothCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();
  if (cmd.length() == 0) return;

  if (cmd == "PING") {
    SerialBT.println("PONG");
  } else if (cmd == "STATUS") {
    sendBluetoothStatus();
  } else if (cmd == "START") {
    requestCycleStart();
    SerialBT.println("START OK");
  } else if (cmd == "STOP") {
    cancelCycle("REMOTE STOP");
    SerialBT.println("STOP OK");
  } else if (cmd == "ADC") {
    SerialBT.print("ADC="); SerialBT.print(lastAdcRaw);
    SerialBT.print(" LEVEL="); SerialBT.print(lastLevelPct);
    SerialBT.println("%");
  } else if (cmd == "HELP") {
    SerialBT.println("CMD: START STOP STATUS ADC DELAY:x ECO? MOTOR:x");
  } else if (cmd == "ECO?") {
    // GRUPO 9: comando para verificar se o ciclo economico esta ativo.
    SerialBT.print("ECO="); SerialBT.println(ecoModeActive ? 1 : 0);
  } else if (cmd.startsWith("DELAY:")) {
    // GRUPO 4: comando remoto para configurar o atraso antes do START.
    int hours = cmd.substring(6).toInt();
    if (hours >= 0 && hours <= 24) {
      configuredDelayHours = (uint8_t)hours;
      SerialBT.print("DELAY SET "); SerialBT.print(configuredDelayHours); SerialBT.println("H");
    } else {
      SerialBT.println("ERR DELAY 0..24");
    }
  } else if (cmd.startsWith("MOTOR:")) {
    int duty = cmd.substring(6).toInt();
    duty = constrain(duty, 0, 255);
    setMotorDuty((uint8_t)duty);
    SerialBT.print("MOTOR DUTY "); SerialBT.println(duty);
  } else {
    SerialBT.println("ERR CMD");
  }
}

// Bloco: Processa o Bluetooth de forma nao bloqueante, byte a byte.
void processBluetooth() {
  if (!btOk) return;

  while (SerialBT.available()) {
    char ch = (char)SerialBT.read();
    if (ch == '\r') continue;

    if (ch == '\n') {
      btBuffer[btBufferLen] = '\0';
      handleBluetoothCommand(String(btBuffer));
      btBufferLen = 0;
      btBuffer[0] = '\0';
      continue;
    }

    if (btBufferLen < sizeof(btBuffer) - 1) {
      btBuffer[btBufferLen++] = ch;
      btBuffer[btBufferLen] = '\0';
    } else {
      btBufferLen = 0;
      btBuffer[0] = '\0';
      SerialBT.println("ERR BUF");
    }
  }
}

// Bloco: GRUPO 4 - Trata a edicao do atraso programado pelo teclado.
void handleDelayEditing(char key) {
  if (key >= '0' && key <= '9' && delayDigitCount < 2) {
    delayDigits[delayDigitCount++] = key;
    delayDigits[delayDigitCount] = '\0';
    return;
  }

  if (key == '#') {
    int hours = atoi(delayDigits);
    if (hours >= 0 && hours <= 24) {
      configuredDelayHours = (uint8_t)hours;
    }
    delayEditMode = false;
    resetDelayEdit();
    return;
  }

  if (key == '*') {
    delayEditMode = false;
    resetDelayEdit();
  }
}

// Bloco: Interpreta as teclas do painel frontal conforme o modo atual.
void processKeypad() {
  char key = getKeyEvent();
  if (!key) return;

  lastKey = key;
  Serial.print("KEY: "); Serial.println(key);
  if (btOk) {
    SerialBT.print("KEY:");
    SerialBT.println(key);
  }

  if (delayEditMode) {
    handleDelayEditing(key);
    return;
  }

  if (key == 'A') {
    // GRUPO 4: tecla A entra no modo de configuracao do atraso.
    delayEditMode = true;
    resetDelayEdit();
    return;
  }

  if (key == '1') {
    requestCycleStart();
    return;
  }

  if (key == '2') {
    cancelCycle("PANEL STOP");
    return;
  }

  if (key == '3' && btOk) {
    sendBluetoothStatus();
    return;
  }

  if (key == '0') {
    configuredDelayHours = 0;
    return;
  }
}

// Bloco: Implementa a maquina de estados principal do processo de lavagem.
void runStateMachine() {
  switch (currentState) {
    case ST_IDLE:
      stopAllActuators();
      break;

    case ST_WAIT_DELAY:
      // GRUPO 4: estado que segura o inicio do ciclo ate o timer programado terminar.
      stopAllActuators();
      setAuxLed(true);
      if (remainingDelayMs() == 0) {
        enterState(ST_LOCK_DOOR);
      }
      break;

    case ST_LOCK_DOOR:
      setDoorLock(true);
      setAuxLed(true);
      if (elapsedMs(stateStartedMs, DOOR_LOCK_MS)) {
        enterState(ST_FILL_WASH);
      }
      break;

    case ST_FILL_WASH:
      setValve(true);
      setPump(false);
      setMotorDuty(0);
      if (lastLevelPct >= TARGET_WASH_LEVEL_PCT) {
        setValve(false);
        enterState(ST_WASH);
      } else if (elapsedMs(stateStartedMs, FILL_TIMEOUT_MS)) {
        enterFault("FILL TIMEOUT");
      }
      break;

    case ST_WASH:
      setValve(false);
      setPump(false);
      setDoorLock(true);
      setMotorDuty(MOTOR_PWM_WASH);
      if (elapsedMs(stateStartedMs, WASH_MS)) {
        setMotorDuty(0);
        enterState(ST_DRAIN_WASH);
      }
      break;

    case ST_DRAIN_WASH:
      setPump(true);
      setValve(false);
      setMotorDuty(0);
      if (lastLevelPct <= DRAIN_EMPTY_LEVEL_PCT) {
        setPump(false);
        enterState(ST_FILL_RINSE);
      } else if (elapsedMs(stateStartedMs, DRAIN_TIMEOUT_MS)) {
        enterFault("DRAIN1 TIMEOUT");
      }
      break;

    case ST_FILL_RINSE:
      setValve(true);
      setPump(false);
      setMotorDuty(0);
      if (lastLevelPct >= TARGET_RINSE_LEVEL_PCT) {
        setValve(false);
        enterState(ST_RINSE);
      } else if (elapsedMs(stateStartedMs, FILL_TIMEOUT_MS)) {
        enterFault("RINSE FILL TO");
      }
      break;

    case ST_RINSE:
      // GRUPO 9: usa RINSE_ECO_MS ou RINSE_BASE_MS conforme o nivel inicial do ADC.
      setValve(false);
      setPump(false);
      setDoorLock(true);
      setMotorDuty(MOTOR_PWM_RINSE);
      if (elapsedMs(stateStartedMs, (uint32_t)rinseDurationMs)) {
        setMotorDuty(0);
        enterState(ST_DRAIN_RINSE);
      }
      break;

    case ST_DRAIN_RINSE:
      setPump(true);
      setValve(false);
      setMotorDuty(0);
      if (lastLevelPct <= DRAIN_EMPTY_LEVEL_PCT) {
        setPump(false);
        enterState(ST_SPIN);
      } else if (elapsedMs(stateStartedMs, DRAIN_TIMEOUT_MS)) {
        enterFault("DRAIN2 TIMEOUT");
      }
      break;

    case ST_SPIN:
      setDoorLock(true);
      setValve(false);
      setPump(false);
      setMotorDuty(MOTOR_PWM_SPIN);
      if (elapsedMs(stateStartedMs, SPIN_MS)) {
        setMotorDuty(0);
        enterState(ST_COMPLETE);
      }
      break;

    case ST_COMPLETE:
      stopAllActuators();
      configuredDelayHours = 0;
      if (elapsedMs(stateStartedMs, COMPLETE_MS)) {
        enterState(ST_IDLE);
      }
      break;

    case ST_FAULT:
      stopAllActuators();
      break;
  }
}

// Bloco: Inicializa o esp_timer periodico exigido para a temporizacao do firmware.
void setupSystemTimer() {
  esp_timer_create_args_t timerArgs = {};
  timerArgs.callback = &onSystemTick;
  timerArgs.name = "washer_tick";
  esp_err_t createErr = esp_timer_create(&timerArgs, &gSystemTimer);
  if (createErr == ESP_OK) {
    esp_timer_start_periodic(gSystemTimer, 10000);
  }
}

// Bloco: Configura o hardware usado pelo sketch logo no boot.
void setupHardware() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  pinMode(ADC_NIVEL, INPUT);

  ledcSetup(MOTOR_PWM_CHANNEL, MOTOR_PWM_FREQ, MOTOR_PWM_RESOLUTION);
  ledcAttachPin(MOTOR_PWM_PIN, MOTOR_PWM_CHANNEL);
  setMotorDuty(0);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Init washer...");

  mcpOk = mcp.begin_I2C(MCP_ADDR);
  if (mcpOk) {
    setupKeypad();
    mcp.pinMode(MCP_TRAVA_PORTA, OUTPUT);
    mcp.pinMode(MCP_BOMBA, OUTPUT);
    mcp.pinMode(MCP_VALVULA, OUTPUT);
    mcp.pinMode(MCP_AUX_LED, OUTPUT);
    stopAllActuators();
  } else {
    Serial.println("MCP FAIL");
  }

  btOk = SerialBT.begin(BT_NAME);
  if (!btOk) {
    Serial.println("BT FAIL");
  }
}

// Bloco: Executa as rotinas de inicializacao do firmware.
void setup() {
  setupSystemTimer();
  setupHardware();
  readLevelAdc();
  enterState(ST_IDLE);
  lastLcdRefreshMs = nowMs() - LCD_REFRESH_MS;
  refreshLcdIfNeeded();
  Serial.println("WASHER READY");
  Serial.println(BT_NAME);
}

// Bloco: Mantem o sistema reagindo a entradas, estados e atualizacao da interface.
void loop() {
  btConnected = btOk && SerialBT.hasClient();
  readLevelAdc();
  processBluetooth();
  processKeypad();
  runStateMachine();
  pushStatusIfNeeded();
  refreshLcdIfNeeded();
}
