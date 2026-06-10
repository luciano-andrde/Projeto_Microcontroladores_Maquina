# Fluxograma do Firmware

Este documento representa o funcionamento completo do sketch `Microcontroladores.ino`, incluindo a especialização do Grupo 1.

## Fluxo Geral

```mermaid
flowchart TD
  A([Liga o ESP32]) --> B[Configura esp_timer]
  B --> C[Inicializa Serial, I2C, ADC, PWM, LCD, MCP23017 e Bluetooth]
  C --> D[Le o nivel pelo ADC]
  D --> E[Estado inicial ST_IDLE]
  E --> F{{loop}}

  F --> G[Atualiza conexao Bluetooth]
  G --> H[Le ADC]
  H --> I[Processa Bluetooth]
  I --> J[Processa teclado]
  J --> K[Executa maquina de estados]
  K --> L[Envia status e grafico]
  L --> M[Atualiza LCD]
  M --> F
```

## Teclado

```mermaid
flowchart TD
  A[Le teclado] --> B{Tecla pressionada?}
  B -- Nao --> Z[Retorna ao loop]
  B -- Sim --> C{Tecla 1?}
  C -- Sim --> D[Inicia ciclo]
  C -- Nao --> E{Tecla 2?}
  E -- Sim --> F[Cancela ciclo]
  E -- Nao --> G{Tecla 3?}
  G -- Sim --> H[Envia status e grafico]
  G -- Nao --> I{Tecla A?}
  I -- Sim --> J[Alterna grafico automatico]
  I -- Nao --> Z
  D --> Z
  F --> Z
  H --> Z
  J --> Z
```

## Bluetooth

```mermaid
flowchart TD
  A[Recebe comando] --> B{PING?}
  B -- Sim --> B1[Responde PONG]
  B -- Nao --> C{STATUS?}
  C -- Sim --> C1[Envia estado, nivel e atuadores]
  C -- Nao --> D{START?}
  D -- Sim --> D1[Inicia ciclo]
  D -- Nao --> E{STOP?}
  E -- Sim --> E1[Cancela ciclo]
  E -- Nao --> F{ADC?}
  F -- Sim --> F1[Envia ADC e percentual]
  F -- Nao --> G{GRAPH?}
  G -- Sim --> G1[GRUPO 1: envia grafico do nivel]
  G -- Nao --> H{GRAPH ON ou OFF?}
  H -- Sim --> H1[Ativa ou desativa streaming]
  H -- Nao --> I{MOTOR x?}
  I -- Sim --> I1[Testa PWM]
  I -- Nao --> J[Responde ERR CMD]
```

## Controle Inteligente de Nivel

```mermaid
flowchart TD
  A[Inicio de um enchimento] --> B[Le nivel atual pelo ADC]
  B --> C[Calcula deficit: alvo menos nivel atual]
  C --> D[Calcula tempo: deficit vezes 400 ms]
  D --> E{Tempo menor que 3 s?}
  E -- Sim --> F[Usa 3 s]
  E -- Nao --> G{Tempo maior que 30 s?}
  G -- Sim --> H[Usa 30 s]
  G -- Nao --> I[Usa tempo calculado]
  F --> J[Liga valvula]
  H --> J
  I --> J
  J --> K{Nivel atingiu o alvo?}
  K -- Sim --> L[Desliga valvula e avanca estado]
  K -- Nao --> M{Tempo calculado terminou?}
  M -- Nao --> K
  M -- Sim --> N[ST_FAULT por timeout]
```

## Grafico de Nivel

```mermaid
flowchart TD
  A[Le percentual do ADC] --> B[Converte 0 a 100 em 0 a 20 barras]
  B --> C[Monta LEVEL com # e -]
  C --> D[Envia percentual pelo Bluetooth]
  D --> E{Streaming ativo?}
  E -- Sim --> F[Repete a cada 1 segundo]
  E -- Nao --> G[Aguarda comando GRAPH]
```

## Maquina de Estados

```mermaid
flowchart TD
  IDLE[ST_IDLE<br/>Atuadores desligados] -->|START| LOCK[ST_LOCK_DOOR<br/>Trava a porta]

  LOCK -->|1,5 s| CALC1[GRUPO 1<br/>Calcula tempo de enchimento]
  CALC1 --> FILL[ST_FILL_WASH<br/>Liga valvula]
  FILL -->|Nivel maior ou igual a 65%| WASH[ST_WASH<br/>Motor PWM]
  FILL -->|Timeout adaptativo| FAULT[ST_FAULT<br/>Atuadores desligados]

  WASH -->|15 s| DRAIN1[ST_DRAIN_WASH<br/>Liga bomba]
  DRAIN1 -->|Nivel menor ou igual a 10%| CALC2[GRUPO 1<br/>Recalcula enchimento]
  DRAIN1 -->|Timeout| FAULT

  CALC2 --> RFILL[ST_FILL_RINSE<br/>Liga valvula]
  RFILL -->|Nivel maior ou igual a 50%| RINSE[ST_RINSE<br/>Motor PWM]
  RFILL -->|Timeout adaptativo| FAULT

  RINSE -->|9 s| DRAIN2[ST_DRAIN_RINSE<br/>Liga bomba]
  DRAIN2 -->|Nivel menor ou igual a 10%| SPIN[ST_SPIN<br/>PWM alto]
  DRAIN2 -->|Timeout| FAULT

  SPIN -->|10 s| DONE[ST_COMPLETE<br/>Fim do ciclo]
  DONE -->|4 s| IDLE
  FAULT -->|Tecla 2 ou STOP| IDLE
```

## Especializacao do Grupo 1

- `calculateAdaptiveFillTimeout()` calcula o limite de enchimento a partir do nivel atual.
- `prepareAdaptiveFill()` aplica o calculo antes da lavagem e antes do enxague.
- `sendLevelGraph()` monta e envia o grafico textual.
- `GRAPH`, `GRAPH:ON` e `GRAPH:OFF` controlam o grafico pelo Bluetooth.
- `pushStatusIfNeeded()` transmite o nivel e o grafico a cada segundo.
