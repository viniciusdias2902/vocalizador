/*
 * vocalizador - 3 slots de áudio para N16R8 Expansion Board
 *
 * Toque curto em SW1/SW2/SW3: toca o áudio gravado naquele slot
 * Segurar SW1 por >1s: grava áudio. Ao soltar, LED pisca.
 *   Então pressione SW1/SW2/SW3 para salvar naquele slot.
 *
 * ESP-IDF puro (sem ESP-ADF).
 * Áudio na PSRAM: 3 slots × 20 seg, 16kHz 16-bit mono.
 *
 * Persistência: cada slot é copiado para a partição SPIFFS "storage"
 * (4 MB) ao ser atribuído e recarregado para a PSRAM no boot. Se a
 * partição não montar, o aparelho funciona sem persistência.
 *
 * LED: apagado = ocioso | aceso = gravando, tocando ou salvando na flash
 *      piscando = aguardando o botão de destino (10 s, depois descarta)
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"

static const char *TAG = "vocalizador";

/* =====================================================================
 * Pinagem
 * ===================================================================== */
#define I2C_SDA_PIN         14
#define I2C_SCL_PIN         47

#define I2S_MCLK_PIN        8
#define I2S_BCLK_PIN        3
#define I2S_LRCLK_PIN       9
#define I2S_DOUT_PIN        46
#define I2S_DIN_PIN         10

#define LED_PIN             21

/* =====================================================================
 * Botões e slots
 * ===================================================================== */
#define NUM_SLOTS           3
static const int BTN_PIN[NUM_SLOTS] = { 12, 11, 13 };  /* SW1, SW2, SW3 */

#define LONG_PRESS_MS       1000    /* tempo para ativar gravação */

/* =====================================================================
 * Áudio
 * ===================================================================== */
#define SAMPLE_RATE         16000
#define RECORD_MAX_SEC      20
#define MAX_SLOT_BYTES      (SAMPLE_RATE * 2 * RECORD_MAX_SEC)  /* ~640 KB */
#define CHUNK_BYTES         4096

/* 3 slots de áudio (ponteiros para PSRAM) */
static uint8_t *slot_data[NUM_SLOTS] = { NULL, NULL, NULL };
static size_t   slot_len[NUM_SLOTS]  = { 0, 0, 0 };

/* Buffer temporário de gravação */
static uint8_t *rec_buf = NULL;
static size_t   rec_len = 0;

/* =====================================================================
 * Persistência (SPIFFS)
 * ===================================================================== */
#ifndef STORAGE_BASE
#define STORAGE_BASE        "/spiffs"
#endif
#define STORAGE_LABEL       "storage"           /* ver partitions.csv */
#define SLOT_MAGIC          0x31434F56u         /* "VOC1" */

typedef struct {
    uint32_t magic;
    uint32_t len;                               /* bytes de áudio após o cabeçalho */
} slot_header_t;

static bool storage_ok = false;

/* =====================================================================
 * ES8388 - Driver I2C mínimo
 * ===================================================================== */
#define ES8388_ADDR         0x10

static esp_err_t es8388_write_reg(uint8_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ES8388_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8388 write reg 0x%02X failed: %s", reg, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t es8388_read_reg(uint8_t reg, uint8_t *val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ES8388_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ES8388_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, val, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static void i2c_scan(void)
{
    ESP_LOGI(TAG, "Escaneando barramento I2C...");
    int found = 0;
    for (int addr = 0x08; addr <= 0x77; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "  Dispositivo encontrado em 0x%02X", addr);
            found++;
        }
    }
    ESP_LOGI(TAG, "Scan completo: %d dispositivo(s)", found);
}

static void i2c_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0));
}

static void es8388_init(void)
{
    uint8_t reg_val = 0;
    esp_err_t ret = es8388_read_reg(0x00, &reg_val);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8388 NAO encontrado em 0x%02X!", ES8388_ADDR);
        i2c_scan();
        return;
    }
    ESP_LOGI(TAG, "ES8388 detectado (reg0=0x%02X)", reg_val);

    es8388_write_reg(0x08, 0x00);   /* Slave */
    es8388_write_reg(0x02, 0xF3);   /* Power down */
    es8388_write_reg(0x2B, 0x80);   /* Same LRCK */
    es8388_write_reg(0x00, 0x36);   /* VMIDSEL */
    es8388_write_reg(0x01, 0x40);   /* Ref up */

    /* ADC */
    es8388_write_reg(0x03, 0x00);
    es8388_write_reg(0x09, 0x88);   /* Mic gain +24dB */
    es8388_write_reg(0x0A, 0x00);   /* LIN1/RIN1 */
    es8388_write_reg(0x0B, 0x02);   /* Mono L */
    es8388_write_reg(0x0C, 0x18);   /* I2S fmt */
    es8388_write_reg(0x0D, 0x02);   /* Fs=256 */
    es8388_write_reg(0x10, 0x00);
    es8388_write_reg(0x11, 0x00);

    /* DAC */
    es8388_write_reg(0x04, 0x3C);
    es8388_write_reg(0x17, 0x18);
    es8388_write_reg(0x18, 0x02);
    es8388_write_reg(0x1A, 0x00);
    es8388_write_reg(0x1B, 0x00);
    es8388_write_reg(0x19, 0x00);   /* Unmute */
    es8388_write_reg(0x26, 0x00);
    es8388_write_reg(0x27, 0xB8);
    es8388_write_reg(0x2A, 0xB8);
    es8388_write_reg(0x2E, 0x1E);
    es8388_write_reg(0x2F, 0x1E);
    es8388_write_reg(0x30, 0x1E);

    es8388_write_reg(0x02, 0x00);   /* Power up */
    ESP_LOGI(TAG, "ES8388 ok (ADC + DAC)");
}

/* =====================================================================
 * I2S
 * ===================================================================== */
static i2s_chan_handle_t i2s_tx = NULL;
static i2s_chan_handle_t i2s_rx = NULL;

static const i2s_std_slot_config_t slot_cfg = {
    .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
    .slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT,
    .slot_mode = I2S_SLOT_MODE_MONO,
    .slot_mask = I2S_STD_SLOT_LEFT,
    .ws_width = 16,
    .ws_pol = true,
    .bit_shift = true,
};

static void i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx, &i2s_rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = I2S_MCLK_PIN,
            .bclk = I2S_BCLK_PIN,
            .ws   = I2S_LRCLK_PIN,
            .dout = I2S_DOUT_PIN,
            .din  = I2S_DIN_PIN,
            .invert_flags = { false, false, false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx, &std_cfg));
    ESP_LOGI(TAG, "I2S ok (TX+RX)");
}

/* =====================================================================
 * Helpers de botão
 * ===================================================================== */

/* Retorna true se algum botão está pressionado */
static bool any_btn_pressed(void)
{
    for (int i = 0; i < NUM_SLOTS; i++) {
        if (gpio_get_level(BTN_PIN[i]) == 0) return true;
    }
    return false;
}

/* Espera todos os botões serem soltos */
static void wait_all_released(void)
{
    while (any_btn_pressed()) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* Espera qualquer botão ser pressionado. Retorna índice 0-2, ou -1 se timeout. */
static int wait_any_btn(int timeout_ms)
{
    int elapsed = 0;
    bool led_on = false;

    while (elapsed < timeout_ms) {
        /* Pisca LED para indicar que está esperando */
        if ((elapsed % 300) == 0) {
            led_on = !led_on;
            gpio_set_level(LED_PIN, led_on ? 1 : 0);
        }

        for (int i = 0; i < NUM_SLOTS; i++) {
            if (gpio_get_level(BTN_PIN[i]) == 0) {
                vTaskDelay(pdMS_TO_TICKS(50));  /* debounce */
                if (gpio_get_level(BTN_PIN[i]) == 0) {
                    gpio_set_level(LED_PIN, 0);
                    return i;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
        elapsed += 50;
    }

    gpio_set_level(LED_PIN, 0);
    return -1;
}

/* =====================================================================
 * Reprodução
 * ===================================================================== */
static void playback_slot(int slot)
{
    if (slot_len[slot] == 0 || slot_data[slot] == NULL) {
        ESP_LOGW(TAG, "Slot %d vazio!", slot + 1);
        return;
    }

    float sec = (float)slot_len[slot] / (float)(SAMPLE_RATE * 2);
    ESP_LOGI(TAG, "Tocando slot %d (%.1f seg)...", slot + 1, sec);

    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx));
    gpio_set_level(LED_PIN, 1);

    size_t offset = 0;
    while (offset < slot_len[slot]) {
        size_t to_write = CHUNK_BYTES;
        if (to_write > (slot_len[slot] - offset))
            to_write = slot_len[slot] - offset;

        size_t bw = 0;
        i2s_channel_write(i2s_tx, slot_data[slot] + offset, to_write,
                          &bw, portMAX_DELAY);
        offset += bw;
    }

    /* Flush DMA */
    static uint8_t silence[CHUNK_BYTES];
    memset(silence, 0, sizeof(silence));
    size_t bw;
    i2s_channel_write(i2s_tx, silence, sizeof(silence), &bw, portMAX_DELAY);

    ESP_ERROR_CHECK(i2s_channel_disable(i2s_tx));
    gpio_set_level(LED_PIN, 0);
    ESP_LOGI(TAG, "Slot %d finalizado.", slot + 1);
}

/* =====================================================================
 * Gravação
 * ===================================================================== */
static void do_record(void)
{
    /* Aloca buffer temporário se necessário */
    if (rec_buf == NULL) {
        rec_buf = heap_caps_malloc(MAX_SLOT_BYTES, MALLOC_CAP_SPIRAM);
        if (!rec_buf) {
            ESP_LOGE(TAG, "Falha ao alocar PSRAM para gravacao!");
            return;
        }
    }
    rec_len = 0;

    /* Habilita I2S */
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_rx));
    gpio_set_level(LED_PIN, 1);
    ESP_LOGI(TAG, "GRAVANDO... (solte SW1 para parar, max %d seg)", RECORD_MAX_SEC);

    /* Grava enquanto SW1 estiver pressionado */
    static uint8_t chunk[CHUNK_BYTES];
    size_t total = 0;

    while (gpio_get_level(BTN_PIN[0]) == 0) {
        if (total >= MAX_SLOT_BYTES) {
            ESP_LOGW(TAG, "Buffer cheio (%d seg)!", RECORD_MAX_SEC);
            break;
        }

        size_t to_read = CHUNK_BYTES;
        if (to_read > (MAX_SLOT_BYTES - total))
            to_read = MAX_SLOT_BYTES - total;

        size_t br = 0;
        esp_err_t ret = i2s_channel_read(i2s_rx, chunk, to_read,
                                         &br, pdMS_TO_TICKS(500));
        if (ret == ESP_OK && br > 0) {
            memcpy(rec_buf + total, chunk, br);
            total += br;
        }
    }

    rec_len = total;

    ESP_ERROR_CHECK(i2s_channel_disable(i2s_rx));
    ESP_ERROR_CHECK(i2s_channel_disable(i2s_tx));
    gpio_set_level(LED_PIN, 0);

    float sec = (float)rec_len / (float)(SAMPLE_RATE * 2);
    ESP_LOGI(TAG, "Gravacao: %zu bytes (%.1f seg)", rec_len, sec);
}

/* =====================================================================
 * Persistência em flash (SPIFFS)
 * ===================================================================== */
static void slot_path(int slot, char *buf, size_t n, bool tmp)
{
    snprintf(buf, n, STORAGE_BASE "/slot%d.%s", slot + 1, tmp ? "tmp" : "pcm");
}

/* Monta a partição. Em caso de falha o aparelho segue sem persistência. */
static void storage_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = STORAGE_BASE,
        .partition_label = STORAGE_LABEL,
        .max_files = 2,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS nao montou (%s): sem persistencia", esp_err_to_name(ret));
        storage_ok = false;
        return;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info(STORAGE_LABEL, &total, &used);
    ESP_LOGI(TAG, "SPIFFS ok: %zu de %zu bytes em uso", used, total);
    storage_ok = true;
}

/* Copia o slot da PSRAM para a flash. Grava em .tmp e renomeia,
 * para que uma queda de energia no meio não corrompa o arquivo anterior. */
static void storage_save(int slot)
{
    if (!storage_ok || slot_data[slot] == NULL || slot_len[slot] == 0) return;

    char path[64], tmp[64];
    slot_path(slot, path, sizeof(path), false);
    slot_path(slot, tmp, sizeof(tmp), true);

    gpio_set_level(LED_PIN, 1);                 /* LED aceso durante a escrita */
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Nao abriu %s para escrita", tmp);
        gpio_set_level(LED_PIN, 0);
        return;
    }
    slot_header_t hdr = { .magic = SLOT_MAGIC, .len = (uint32_t)slot_len[slot] };
    bool ok = fwrite(&hdr, sizeof(hdr), 1, f) == 1 &&
              fwrite(slot_data[slot], 1, slot_len[slot], f) == slot_len[slot];
    ok = (fclose(f) == 0) && ok;

    if (ok) {
        remove(path);                           /* SPIFFS não sobrescreve no rename */
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        ESP_LOGE(TAG, "Falha ao salvar slot %d na flash (%s)", slot + 1, strerror(errno));
        remove(tmp);
    } else {
        ESP_LOGI(TAG, "Slot %d salvo na flash (%zu bytes)", slot + 1, slot_len[slot]);
    }
    gpio_set_level(LED_PIN, 0);
}

/* Recarrega um slot da flash para a PSRAM. Arquivo truncado ou inválido é descartado. */
static void storage_load(int slot)
{
    char path[64];
    slot_path(slot, path, sizeof(path), false);

    struct stat st;
    if (stat(path, &st) != 0) return;           /* slot nunca gravado */

    FILE *f = fopen(path, "rb");
    if (f == NULL) return;

    slot_header_t hdr;
    bool ok = fread(&hdr, sizeof(hdr), 1, f) == 1 &&
              hdr.magic == SLOT_MAGIC &&
              hdr.len > 0 && hdr.len <= MAX_SLOT_BYTES && (hdr.len % 2) == 0 &&
              (size_t)st.st_size == sizeof(hdr) + hdr.len;

    uint8_t *buf = NULL;
    if (ok) {
        buf = heap_caps_malloc(hdr.len, MALLOC_CAP_SPIRAM);
        ok = buf != NULL && fread(buf, 1, hdr.len, f) == hdr.len;
    }
    fclose(f);

    if (!ok) {
        ESP_LOGW(TAG, "Slot %d na flash invalido ou truncado: descartado", slot + 1);
        if (buf) heap_caps_free(buf);
        remove(path);
        return;
    }
    slot_data[slot] = buf;
    slot_len[slot] = hdr.len;
    ESP_LOGI(TAG, "Slot %d carregado da flash (%.1f seg)", slot + 1,
             (float)hdr.len / (float)(SAMPLE_RATE * 2));
}

static void storage_load_all(void)
{
    if (!storage_ok) return;
    for (int i = 0; i < NUM_SLOTS; i++) {
        storage_load(i);
    }
}

/* Salva a gravação temporária no slot escolhido */
static void assign_recording(int slot)
{
    if (rec_len == 0 || rec_buf == NULL) {
        ESP_LOGW(TAG, "Nada para salvar!");
        return;
    }

    /* Libera slot antigo se existir */
    if (slot_data[slot] != NULL) {
        heap_caps_free(slot_data[slot]);
    }

    /* Transfere ponteiro (evita copiar ~640KB) */
    slot_data[slot] = rec_buf;
    slot_len[slot] = rec_len;
    rec_buf = NULL;   /* será realocado na próxima gravação */
    rec_len = 0;

    float sec = (float)slot_len[slot] / (float)(SAMPLE_RATE * 2);
    ESP_LOGI(TAG, "Audio salvo no slot %d (%.1f seg)", slot + 1, sec);

    /* PSRAM é volátil: copia para a flash */
    storage_save(slot);
}

/* =====================================================================
 * Main
 * ===================================================================== */
void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Vocalizador - 3 Slots de Audio");
    ESP_LOGI(TAG, "========================================");

    /* GPIOs */
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out_cfg);
    gpio_set_level(LED_PIN, 0);

    uint64_t btn_mask = 0;
    for (int i = 0; i < NUM_SLOTS; i++)
        btn_mask |= (1ULL << BTN_PIN[i]);

    gpio_config_t btn_cfg = {
        .pin_bit_mask = btn_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn_cfg);

    /* Inicializa hardware */
    i2c_init();
    es8388_init();
    i2s_init();

    /* Persistência: monta a flash e recupera as mensagens gravadas */
    storage_init();
    storage_load_all();

    ESP_LOGI(TAG, "Pronto!");
    ESP_LOGI(TAG, "  Toque curto SW1/SW2/SW3: toca o audio do slot");
    ESP_LOGI(TAG, "  Segure SW1 por >1s: grava, depois escolha o slot");

    /* Loop principal */
    while (1) {

        /* --- SW1 pressionado: detecta curto vs longo --- */
        if (gpio_get_level(BTN_PIN[0]) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));  /* debounce */
            if (gpio_get_level(BTN_PIN[0]) == 0) {

                /* Espera até 1s para decidir: curto ou longo? */
                int held = 50;  /* já esperamos 50ms no debounce */
                while (gpio_get_level(BTN_PIN[0]) == 0 && held < LONG_PRESS_MS) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                    held += 50;
                }

                if (held >= LONG_PRESS_MS) {
                    /* === LONG PRESS: GRAVAR === */
                    do_record();
                    wait_all_released();

                    if (rec_len > 0) {
                        ESP_LOGI(TAG, "Pressione SW1/SW2/SW3 para salvar no slot...");
                        int slot = wait_any_btn(10000);  /* 10s timeout */
                        if (slot >= 0) {
                            assign_recording(slot);
                        } else {
                            ESP_LOGW(TAG, "Timeout! Gravacao descartada.");
                            /* Libera buffer temporário */
                            if (rec_buf) {
                                heap_caps_free(rec_buf);
                                rec_buf = NULL;
                            }
                            rec_len = 0;
                        }
                        wait_all_released();
                    }
                } else {
                    /* === SHORT PRESS: TOCAR SLOT 1 === */
                    wait_all_released();
                    playback_slot(0);
                }
            }
        }

        /* --- SW2 pressionado: tocar slot 2 --- */
        if (gpio_get_level(BTN_PIN[1]) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(BTN_PIN[1]) == 0) {
                wait_all_released();
                playback_slot(1);
            }
        }

        /* --- SW3 pressionado: tocar slot 3 --- */
        if (gpio_get_level(BTN_PIN[2]) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(BTN_PIN[2]) == 0) {
                wait_all_released();
                playback_slot(2);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
