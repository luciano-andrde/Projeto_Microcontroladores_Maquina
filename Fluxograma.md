# Fluxograma do Firmware

Este fluxograma representa o funcionamento completo do sketch `Microcontroladores.ino`.

## Fluxo Geral

```mermaid
flowchart TD
  A([Liga o ESP32]) --> B[setupSystemTimer]
  B --> C[setupHardware]
  C --> C1[Inicializa Serial, I2C, ADC, PWM, LCD, MCP23017 e Bluetooth]
  C1 --> D[readLevelAdc]
  D --> E[Estado inicial: ST_IDLE]
  E --> F[Atualiza LCD]
  F --> G{{loop}}

  G --> H[Atualiza btConnected]
  H --> I[readLevelAdc]
  I --> J[processBluetooth]
  J --> K[processKeypad]
  K --> L[runStateMachine]
  L --> M[pushStatusIfNeeded]
  M --> N[refreshLcdIfNeeded]
  N --> G
```

## Entradas do Usuario

```mermaid
flowchart TD
  A[processKeypad] --> B{Tecla pressionada?}
  B -- Nao --> Z[Retorna ao loop]
  B -- Sim --> C{Modo de editar atraso ativo?}

  C -- Sim --> D[handleDelayEditing]
  D --> D1{Tecla numerica?}
  D1 -- Sim --> D2[Adiciona digito do atraso]
  D1 -- Nao --> D3{Tecla #?}
  D3 -- Sim --> D4[Confirma atraso 0 a 24 horas]
  D3 -- Nao --> D5{Tecla *?}
  D5 -- Sim --> D6[Cancela edicao]
  D5 -- Nao --> Z
  D2 --> Z
  D4 --> Z
  D6 --> Z

  C -- Nao --> E{Tecla A?}
  E -- Sim --> F[GRUPO 4: entra na edicao do atraso]
  E -- Nao --> G{Tecla 1?}
  G -- Sim --> H[requestCycleStart]
  G -- Nao --> I{Tecla 2?}
  I -- Sim --> J[cancelCycle]
  I -- Nao --> K{Tecla 3?}
  K -- Sim --> L[sendBluetoothStatus]
  K -- Nao --> M{Tecla 0?}
  M -- Sim --> N[Zera atraso configurado]
  M -- Nao --> Z

  F --> Z
  H --> Z
  J --> Z
  L --> Z
  N --> Z
```

```mermaid
flowchart TD
  A[processBluetooth] --> B{Chegou comando completo?}
  B -- Nao --> Z[Retorna ao loop]
  B -- Sim --> C[handleBluetoothCommand]

  C --> D{PING?}
  D -- Sim --> D1[Responde PONG]
  D -- Nao --> E{STATUS?}
  E -- Sim --> E1[Envia estado, ADC, nivel, atraso e atuadores]
  E -- Nao --> F{START?}
  F -- Sim --> F1[requestCycleStart]
  F -- Nao --> G{STOP?}
  G -- Sim --> G1[cancelCycle]
  G -- Nao --> H{ADC?}
  H -- Sim --> H1[Envia leitura do ADC]
  H -- Nao --> I{DELAY:x?}
  I -- Sim --> I1[GRUPO 4: configura atraso remoto]
  I -- Nao --> J{ECO?}
  J -- Sim --> J1[GRUPO 9: informa se modo economico esta ativo]
  J -- Nao --> K{MOTOR:x?}
  K -- Sim --> K1[Testa PWM do motor]
  K -- Nao --> L[Responde ERR CMD]

  D1 --> Z
  E1 --> Z
  F1 --> Z
  G1 --> Z
  H1 --> Z
  I1 --> Z
  J1 --> Z
  K1 --> Z
  L --> Z
```

## Inicio do Ciclo

```mermaid
flowchart TD
  A[requestCycleStart] --> B{Estado e IDLE ou COMPLETE?}
  B -- Nao --> Z[Ignora comando]
  B -- Sim --> C[Le ADC do nivel de agua]

  C --> D{Nivel inicial <= 35%?}
  D -- Sim --> E[GRUPO 9: ativa modo economico]
  E --> E1[Enxague usa RINSE_ECO_MS]
  D -- Nao --> F[Modo normal]
  F --> F1[Enxague usa RINSE_BASE_MS]

  E1 --> G[GRUPO 4: calcula atraso configurado]
  F1 --> G
  G --> H{Atraso > 0?}
  H -- Sim --> I[Entra em ST_WAIT_DELAY]
  H -- Nao --> J[Entra em ST_LOCK_DOOR]
```

## Maquina de Estados

```mermaid
flowchart TD
  IDLE[ST_IDLE<br/>Atuadores desligados] -->|START sem atraso| LOCK[ST_LOCK_DOOR<br/>Trava porta]
  IDLE -->|START com atraso| DELAY[ST_WAIT_DELAY<br/>GRUPO 4: aguarda timer programado]
  DELAY -->|Tempo restante = 0| LOCK

  LOCK -->|DOOR_LOCK_MS passou| FILL[ST_FILL_WASH<br/>Liga valvula]
  FILL -->|Nivel >= 65%| WASH[ST_WASH<br/>Motor PWM lavagem]
  FILL -->|Timeout| FAULT[ST_FAULT<br/>Desliga atuadores]

  WASH -->|WASH_MS passou| DRAIN1[ST_DRAIN_WASH<br/>Liga bomba]
  DRAIN1 -->|Nivel <= 10%| RFILL[ST_FILL_RINSE<br/>Liga valvula]
  DRAIN1 -->|Timeout| FAULT

  RFILL -->|Nivel >= 50%| RINSE[ST_RINSE<br/>Motor PWM enxague]
  RFILL -->|Timeout| FAULT

  RINSE -->|Tempo de enxague passou| DRAIN2[ST_DRAIN_RINSE<br/>Liga bomba]
  RINSE -.->|GRUPO 9| ECO[Se ECO ativo, tempo de enxague e menor]

  DRAIN2 -->|Nivel <= 10%| SPIN[ST_SPIN<br/>Motor PWM centrifugacao]
  DRAIN2 -->|Timeout| FAULT

  SPIN -->|SPIN_MS passou| DONE[ST_COMPLETE<br/>Fim do ciclo]
  DONE -->|COMPLETE_MS passou| IDLE

  FAULT -->|Tecla 2 ou STOP| IDLE
```

## Onde Estao os Requisitos Especificos

- Grupo 4, Modo Atraso: constantes `DEMO_DELAY_PER_HOUR_MS`, variaveis `delayEditMode`, `configuredDelayHours`, `scheduledDelayMs`, funcao `handleDelayEditing()`, comando Bluetooth `DELAY:x`, estado `ST_WAIT_DELAY` e calculo em `remainingDelayMs()`.
- Grupo 9, Ciclo Economico Adaptativo: constantes `ECO_START_THRESHOLD_PCT`, `RINSE_BASE_MS`, `RINSE_ECO_MS`, variavel `ecoModeActive`, decisao em `requestCycleStart()` e uso de `rinseDurationMs` no estado `ST_RINSE`.
