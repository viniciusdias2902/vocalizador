# Vocalizador - Relatório de Desenvolvimento

## Visão Geral do Projeto

Gravador e reprodutor de áudio com 3 slots para a placa **N16R8 Expansion Board** (ESP32-S3 + ES8388 codec + HT6872 amplificador). Cada botão toca um áudio diferente; segurar SW1 grava áudio e permite salvá-lo em qualquer slot.

**Stack final:** ESP-IDF 5.5.1 puro (sem ESP-ADF).

---

## 1. Dificuldade: Complexidade do ESP-ADF

**Problema:** O projeto original usava o ESP-ADF (Audio Development Framework), que exige:
- Configuração da variável `$ADF_PATH`
- Board definition correto (`CONFIG_ESP32_S3_KORVO2_V3_BOARD`)
- Dependência de múltiplos componentes (audio_pipeline, i2s_stream, raw_stream, board, iot_button, etc.)
- Configuração de Kconfig específica

Isso tornava o ambiente difícil de configurar e manter na máquina do desenvolvedor.

**Solução:** Criar um projeto **ESP-IDF puro** sem nenhuma dependência do ESP-ADF. Todo o driver do ES8388 foi implementado diretamente via registros I2C (~20 writes), e o I2S usa a API nativa `driver/i2s_std.h`. O projeto inteiro é um único arquivo `main.c` (~487 linhas).

---

## 2. Dificuldade: Pinos DOUT/DIN invertidos no código do fabricante

**Problema:** No exemplo fornecido pelo fabricante (`play_spiffs_mp3_example.c`), a função `i2s_init()` continha os pinos I2S invertidos:

```c
// Código do fabricante (ERRADO):
.dout = 10,  // deveria ser o DIN (ES8388 ASDOUT → ESP32)
.din  = 46,  // deveria ser o DOUT (ESP32 → ES8388 DSDIN)
```

O esquemático mostra claramente:
- **GPIO46** → ES8388 pino DSDIN (entrada de dados do DAC) → deve ser `dout` do ESP32
- **GPIO10** → ES8388 pino ASDOUT (saída de dados do ADC) → deve ser `din` do ESP32

A parte funcional do código do fabricante usava `audio_board_init()` do ESP-ADF, que configurava os pinos corretamente via board definition, mascarando o erro na função manual.

**Solução:** Corrigir o mapeamento:
```c
#define I2S_DOUT_PIN  46  // ESP32 TX → ES8388 DSDIN (DAC)
#define I2S_DIN_PIN   10  // ES8388 ASDOUT → ESP32 RX (ADC)
```

---

## 3. Dificuldade: ESP32-S3 entrando em modo Download após flash

**Problema:** Após gravar o firmware via `idf.py flash`, o ESP32-S3 reiniciava em modo download em vez de executar a aplicação:
```
rst:0x15 (USB_UART_CHIP_RESET), boot:0x21 (DOWNLOAD(USB/UART0))
waiting for download
```

Isso é um comportamento comum com ESP32-S3 usando USB-Serial/JTAG, onde o reset automático via RTS nem sempre funciona corretamente.

**Solução:** Pressionar manualmente o botão **RESET** na placa após o flash (sem segurar o botão BOOT/GPIO0). O monitor reconecta automaticamente.

---

## 4. Dificuldade: I2S RX retornando timeout (0 bytes lidos)

**Problema:** Ao tentar gravar áudio, `i2s_channel_read()` retornava `ESP_ERR_TIMEOUT` com 0 bytes:
```
i2s_read: ret=ESP_ERR_TIMEOUT, bytes=0
```

**Causa raiz:** No ESP-IDF 5.x, quando TX e RX são criados no mesmo port I2S (full-duplex), o **clock master (BCLK/WS/MCLK) é gerado pelo canal TX**. Com o TX desabilitado, não havia clock no barramento e o ES8388 não enviava dados.

**Solução:** Habilitar o canal TX junto com o RX durante a gravação:
```c
// do_record():
ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx));  // gera os clocks
ESP_ERROR_CHECK(i2s_channel_enable(i2s_rx));  // agora pode receber dados

// ao parar:
ESP_ERROR_CHECK(i2s_channel_disable(i2s_rx));
ESP_ERROR_CHECK(i2s_channel_disable(i2s_tx));
```

---

## 5. Dificuldade: Stack overflow na main task

**Problema:** Ao iniciar a gravação, ocorria crash imediato:
```
***ERROR*** A stack overflow in task main has been detected.
```

**Causa raiz:** A função de gravação declarava um buffer local de 4096 bytes na stack:
```c
uint8_t chunk[CHUNK_BYTES];  // 4096 bytes na stack!
```
A main task do FreeRTOS tem stack padrão de ~3584 bytes. Com 4096 bytes de buffer local + variáveis + overhead de chamadas, excedia o limite.

**Solução:** Tornar os buffers grandes `static` para que fiquem no segmento BSS em vez da stack:
```c
static uint8_t chunk[CHUNK_BYTES];   // no do_record()
static uint8_t silence[CHUNK_BYTES]; // no playback_slot()
```

---

## 6. Dificuldade: Formato de registro do ES8388 (ADC vs DAC)

**Problema:** Os registros ADCCONTROL4 (0x0C) e DACCONTROL1 (0x17) controlam o formato I2S e bit width do codec. A documentação do ES8388 tem ambiguidades nos bit fields, e diferentes fontes online apresentam mapeamentos conflitantes.

O valor `0x18` para DACCONTROL1 funcionou na prática (playback do tom de 440Hz), mas ao decodificar os bits conforme o datasheet, não corresponde a "I2S 16-bit" de forma óbvia.

**Solução:** Abordagem pragmática:
- **DAC (reg 0x17):** Manter `0x18` - valor comprovadamente funcional
- **ADC (reg 0x0C):** Usar `0x18` também (mesmo formato) para consistência
- Se houver problemas, valores alternativos para testar: `0x0C` (derivado do ESP-ADF)

---

## 7. Dificuldade: Amplificador HT6872 em modo BTL

**Problema:** O HT6872 está conectado em configuração BTL (Bridge-Tied Load):
- IN+ = ROUT1 do ES8388
- IN- = LOUT1 do ES8388
- Saída do amp = ganho × (ROUT1 - LOUT1)

Se ambos os canais receberem o mesmo sinal, a saída seria zero (cancelamento).

**Solução:** Configurar I2S em modo **mono, canal esquerdo** (`I2S_STD_SLOT_LEFT`). Assim:
- LOUT1 = sinal de áudio
- ROUT1 = 0 (silêncio)
- Saída do amp = ganho × (0 - sinal) = sinal amplificado invertido (funciona normalmente)

Isso produz metade da potência BTL máxima, mas é suficiente e simples. Para potência total, seria necessário enviar sinais invertidos nos dois canais.

---

## 8. Evolução: Sistema de 3 slots de áudio

**Requisito:** Transformar o gravador de buffer único em um sistema com 3 slots independentes, onde cada botão toca um áudio diferente e gravações podem ser atribuídas a qualquer slot.

**Avaliação de viabilidade:** Cada slot armazena até 20 segundos (16kHz × 2 bytes × 20s = ~640 KB). Com 3 slots + buffer temporário de gravação = ~2.56 MB, muito abaixo dos 8 MB de PSRAM disponíveis.

**Implementação:**

1. **Detecção de long-press em SW1:** Loop de polling conta o tempo que SW1 fica pressionado. Se < 1 segundo, é toque curto (tocar slot 1). Se ≥ 1 segundo, inicia gravação.

2. **Gravação em buffer temporário:** O áudio é gravado em `rec_buf` (PSRAM). Ao soltar SW1, a gravação para.

3. **Modo de atribuição com LED piscante:** Após gravar, o LED pisca (300ms toggle) indicando que o sistema espera o usuário escolher um slot. O timeout é de 10 segundos.

4. **Pointer swap para evitar cópia:** Em vez de copiar ~640 KB do buffer temporário para o slot, a função `assign_recording()` transfere a propriedade do ponteiro:
```c
slot_data[slot] = rec_buf;
slot_len[slot] = rec_len;
rec_buf = NULL;  // será realocado na próxima gravação
```
Se o slot já tinha áudio, o buffer antigo é liberado com `heap_caps_free()`.

5. **Timeout e descarte:** Se nenhum botão for pressionado em 10 segundos, a gravação é descartada e o buffer temporário é liberado.

---

## Resumo da Arquitetura Final

```
Botões (GPIO12/11/13) = SW1/SW2/SW3
        │
        ▼
   app_main() loop
        │
        ├─ SW1 curto → playback_slot(0)       [toca slot 1]
        ├─ SW1 longo → do_record()            [grava em rec_buf]
        │               └─ wait_any_btn()     [LED pisca, espera escolha]
        │                    ├─ SW1 → assign_recording(0)  [salva no slot 1]
        │                    ├─ SW2 → assign_recording(1)  [salva no slot 2]
        │                    ├─ SW3 → assign_recording(2)  [salva no slot 3]
        │                    └─ timeout → descarta gravação
        │
        ├─ SW2 pressionado → playback_slot(1) [toca slot 2]
        └─ SW3 pressionado → playback_slot(2) [toca slot 3]
```

**Fluxo de dados - Gravação:**
```
Microfone → ES8388 ADC → I2S DIN (GPIO10) → ESP32 RX → rec_buf (PSRAM)
                                                           └─ assign → slot_data[N]
```

**Fluxo de dados - Reprodução:**
```
slot_data[N] (PSRAM) → ESP32 TX → I2S DOUT (GPIO46) → ES8388 DAC → HT6872 → Alto-falante
```
