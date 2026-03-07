# Vocalizador - Guia de Manutenção para LLMs

Este documento descreve o projeto para que uma LLM possa dar manutenção, corrigir bugs e adicionar funcionalidades.

---

## Estrutura do Projeto

```
vocalizador/
├── play_tone/                      ← Projeto ESP-IDF
│   ├── CMakeLists.txt              ← Build config (ESP-IDF puro, sem ADF)
│   ├── sdkconfig.defaults          ← Target ESP32-S3, flash 16MB, PSRAM 8MB octal
│   └── main/
│       ├── CMakeLists.txt
│       └── main.c                  ← Código completo (~435 linhas, arquivo único)
├── RELATORIO.md                    ← Histórico de problemas e soluções
├── LLM_GUIDE.md                    ← Este arquivo
├── ESP32-S3 Dev/                   ← Docs do ESP32-S3 (datasheets, pinout, firmware)
└── S3 MIC SPK Expansion Module/    ← Docs da placa de áudio (esquemático, specs)
```

---

## Hardware

| Componente | Descrição |
|---|---|
| **MCU** | ESP32-S3 N16R8 (16MB flash, 8MB PSRAM octal) |
| **Codec** | ES8388 (I2C para controle, I2S para áudio) |
| **Amplificador** | HT6872 (BTL, alimentado por 5V, CTRL habilitado via divisor de tensão) |
| **Microfones** | 2x MEMS GMI6027P-56DB (conectados a LIN1/RIN1 do ES8388) |
| **Alto-falante** | Via conector J1, alimentado pelo HT6872 |

### Mapeamento de Pinos

```
I2C:
  SDA = GPIO14
  SCL = GPIO47

I2S:
  MCLK  = GPIO8
  BCLK  = GPIO3
  LRCLK = GPIO9
  DOUT  = GPIO46  (ESP32 → ES8388 DSDIN, dados para o DAC)
  DIN   = GPIO10  (ES8388 ASDOUT → ESP32, dados do ADC)

Botões (ativos em LOW, pull-up interno):
  SW1 = GPIO12  (gravar)
  SW2 = GPIO11  (reproduzir)
  SW3 = GPIO13  (LED toggle)

LED = GPIO21
```

### Diagrama do HT6872 (Amplificador BTL)

```
ES8388 ROUT1 (pino 11) ──► HT6872 IN+
ES8388 LOUT1 (pino 12) ──► HT6872 IN-
Saída = ganho × (IN+ - IN-)

CTRL habilitado por divisor de tensão (R7=100K para 3.3V, R11=100K para GND → ~1.65V > threshold 1.4V)
Alimentação: 5V via VDD
```

Em modo mono left, LOUT1 = sinal, ROUT1 = 0 → saída = -ganho × sinal (funciona normalmente).

---

## ES8388 - Registros Críticos

Endereço I2C: **0x10** (7-bit). Se não responder, tente **0x11** (depende do nível do pino CE).

### Sequência de inicialização (DAC + ADC)

```
Reg   Valor   Função
0x08  0x00    Modo escravo (ESP32 é master I2S)
0x02  0xF3    Power down DEM+STM para configurar
0x2B  0x80    Mesmo LRCK para ADC e DAC
0x00  0x36    VMIDSEL=500k, referência ativa
0x01  0x40    Power up referência de tensão

--- ADC ---
0x03  0x00    ADC power on (L+R). Use 0xFF para desligar
0x09  0x88    Ganho mic +24dB. Use 0x44 para +12dB se distorcer
0x0A  0x00    Entrada = LIN1/RIN1
0x0B  0x02    Mono (L=R=canal esquerdo)
0x0C  0x18    Formato I2S (consistente com DAC)
0x0D  0x02    ADC Fs ratio = 256
0x10  0x00    LADCVOL 0dB
0x11  0x00    RADCVOL 0dB

--- DAC ---
0x04  0x3C    DAC on, LOUT1/ROUT1 habilitados
0x17  0x18    Formato I2S, 16-bit (valor testado e funcional)
0x18  0x02    DACFsRatio = 256
0x1A  0x00    LDACVOL 0dB
0x1B  0x00    RDACVOL 0dB
0x19  0x00    Unmute DAC

--- Mixer ---
0x26  0x00    LMIXSEL = LIN1
0x27  0xB8    LD2LO habilitado
0x2A  0xB8    RD2RO habilitado

--- Volume saída ---
0x2E  0x1E    LOUT1 volume
0x2F  0x1E    ROUT1 volume
0x30  0x1E    LOUT2 volume

--- Power up ---
0x02  0x00    Liga DEM+STM
```

### Notas sobre formatos

O valor `0x18` para os registros de formato (0x0C e 0x17) funciona na prática, embora a decodificação dos bit fields conforme o datasheet não seja totalmente clara. Se houver problemas com formato de áudio, valores alternativos a testar:
- `0x0C` - derivado do ESP-ADF para I2S normal + 16-bit
- `0x03` - outra interpretação do datasheet para I2S + 16-bit

---

## I2S - Detalhes Importantes

### Configuração

```c
Modo: Standard (Philips-like)
Sample rate: 16000 Hz
Bit width: 16-bit
Slot mode: MONO
Slot mask: LEFT
ws_pol: true
bit_shift: true
MCLK multiple: 256 (padrão)
```

### Full-Duplex (TX + RX no mesmo port)

TX e RX são criados juntos via `i2s_new_channel(&cfg, &tx, &rx)`. Regra crítica:

**O canal TX DEVE estar habilitado para gerar os clocks (BCLK/WS/MCLK).** Sem TX habilitado, RX retorna `ESP_ERR_TIMEOUT` porque não há clock no barramento.

- **Gravação:** habilitar TX + RX. TX gera clocks, RX recebe dados.
- **Reprodução:** habilitar só TX.
- **Desligar:** desabilitar RX primeiro, depois TX.

### DMA

```c
dma_desc_num = 6      // número de descritores DMA
dma_frame_num = 240   // frames por descritor
```

Tamanho total do buffer DMA = 6 × 240 × 2 bytes = 2880 bytes. O chunk de leitura/escrita é 4096 bytes.

---

## Memória

- **PSRAM (8MB):** Usada para o buffer de áudio. Alocada via `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`.
- **Buffer de áudio:** 960.000 bytes (30 seg × 16kHz × 2 bytes/amostra)
- **Stack da main task:** ~3.5KB. Buffers grandes (>256 bytes) DEVEM ser `static` ou alocados no heap. Nunca declarar arrays grandes como variáveis locais.

---

## Fluxo de Execução

```
app_main()
  ├── GPIO config (LED out, botões in com pull-up)
  ├── i2c_init()           → I2C master, 100kHz
  ├── es8388_init()        → Configura ADC+DAC via I2C
  ├── i2s_init()           → Cria canais TX+RX, configura std mode
  └── loop:
       ├── SW1 baixo? → record_start() → record_loop() → record_stop()
       ├── SW2 baixo? → playback()
       └── SW3 baixo? → toggle LED
```

### Gravação (record)

```
record_start():  aloca PSRAM, habilita TX+RX
record_loop():   i2s_channel_read() em loop até soltar botão ou buffer cheio
record_stop():   desabilita RX+TX, loga duração
```

### Reprodução (playback)

```
playback():  habilita TX, i2s_channel_write() em loop, flush com silêncio, desabilita TX
```

---

## Compilação e Flash

```bash
# Pré-requisito: ESP-IDF 5.1+ instalado
. $HOME/esp/esp-idf/export.sh

cd play_tone

# Primeira vez (ou após mudar sdkconfig.defaults):
idf.py fullclean
idf.py set-target esp32s3
idf.py build

# Flash (ajustar porta serial):
idf.py -p /dev/tty.usbmodem313301 flash

# Se entrar em download mode, pressionar RESET manual na placa

# Monitor:
idf.py -p /dev/tty.usbmodem313301 monitor
# Ctrl+] para sair
```

Porta serial no macOS: `/dev/tty.usbmodem*` (USB-Serial/JTAG do ESP32-S3).

---

## Problemas Conhecidos e Debugging

| Sintoma | Causa provável | Solução |
|---|---|---|
| `ES8388 NAO encontrado` | Endereço I2C errado | Alterar `ES8388_ADDR` para `0x11`. O código faz scan automático. |
| `ESP_ERR_TIMEOUT` no `i2s_channel_read` | TX não habilitado durante gravação | Garantir que `i2s_channel_enable(i2s_tx)` é chamado ANTES de `i2s_channel_enable(i2s_rx)` |
| Stack overflow | Buffer grande na stack | Usar `static` para arrays > 256 bytes |
| Download mode após flash | Reset automático falhou | Pressionar RESET manualmente |
| Áudio distorcido na gravação | Ganho do mic muito alto | Alterar reg 0x09 de `0x88` (+24dB) para `0x44` (+12dB) |
| Sem som no playback | Pinos DOUT/DIN invertidos | Verificar: DOUT=GPIO46, DIN=GPIO10 |
| Playback com volume baixo | Modo mono BTL (metade potência) | Para potência total: enviar sinal invertido no canal R (requer mudar slot para stereo) |

---

## Possíveis Extensões Futuras

1. **Múltiplas gravações:** Usar índices no buffer PSRAM para armazenar N gravações
2. **Salvar em flash (SPIFFS/LittleFS):** Persistir áudio entre resets
3. **Wi-Fi + HTTP:** Enviar áudio gravado para um servidor
4. **Decodificação MP3:** Adicionar decoder para tocar arquivos MP3 do flash
5. **Wake word / reconhecimento de voz:** Integrar com ESP-SR (Speech Recognition)
6. **Ajuste de volume por botão:** Usar long-press em SW3 para controlar volume (reg 0x2E/0x2F)
7. **Modo stereo completo:** Configurar I2S stereo com sinal invertido em L para BTL full power
