/*
 * UART console for esp-can-rx.
 *
 * Commands:
 *   help
 *   mac
 *   peer [aa:bb:cc:dd:ee:ff]
 *   can stats
 *   can last [n]
 *   can watch on|off
 *   can clear
 *   can filter <id|off>
 *   can send [-e] <id> <hex-bytes...>
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_console.h"
#include "linenoise/linenoise.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "can_cli.h"

static const char *TAG = "can_cli";

#define FRAME_RING_SIZE     128
#define DEFAULT_LAST_N      20

typedef struct {
    can_espnow_frame_t frame;
    int64_t ts_us;
} stored_frame_t;

static stored_frame_t s_ring[FRAME_RING_SIZE];
static uint32_t s_total;
static SemaphoreHandle_t s_lock;

static uint32_t s_rx_ok;
static uint32_t s_rx_drop;
static bool s_watch;
static bool s_filter_en;
static uint32_t s_filter_id;

static uint8_t s_peer_mac[ESP_NOW_ETH_ALEN];
static bool s_peer_valid;
static uint32_t s_tx_seq;
static uint32_t s_tx_ok;
static uint32_t s_tx_fail;

static void lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void print_frame(const can_espnow_frame_t *f, int64_t ts_us)
{
    char flags[16] = {0};
    if (f->flags & CAN_ESPNOW_FLAG_EXT) {
        strcat(flags, "EXT ");
    }
    if (f->flags & CAN_ESPNOW_FLAG_RTR) {
        strcat(flags, "RTR ");
    }
    if (flags[0] == '\0') {
        strcpy(flags, "-");
    }

    printf("t=%llu seq=%lu id=0x%lX dlc=%u %s data=",
           (unsigned long long)(ts_us / 1000),
           (unsigned long)f->seq,
           (unsigned long)f->id,
           f->dlc,
           flags);
    for (int i = 0; i < f->dlc && i < CAN_ESPNOW_MAX_DATA; i++) {
        printf("%02X%s", f->data[i], (i + 1 < f->dlc) ? " " : "");
    }
    printf("\n");
}

void can_cli_on_frame(const can_espnow_frame_t *frame, const uint8_t src_mac[6])
{
    if (!frame) {
        return;
    }

    bool learn = false;
    uint8_t learned_mac[ESP_NOW_ETH_ALEN] = {0};

    lock();
    if (src_mac && !s_peer_valid) {
        memcpy(s_peer_mac, src_mac, ESP_NOW_ETH_ALEN);
        s_peer_valid = true;
        memcpy(learned_mac, src_mac, ESP_NOW_ETH_ALEN);
        learn = true;
    }

    s_ring[s_total % FRAME_RING_SIZE].frame = *frame;
    s_ring[s_total % FRAME_RING_SIZE].ts_us = esp_timer_get_time();
    s_total++;
    s_rx_ok++;

    bool watch = s_watch;
    bool pass = !s_filter_en || (frame->id == s_filter_id);
    can_espnow_frame_t copy = *frame;
    int64_t ts = s_ring[(s_total - 1) % FRAME_RING_SIZE].ts_us;
    unlock();

    if (learn && !esp_now_is_peer_exist(learned_mac)) {
        esp_now_peer_info_t peer = {0};
        memcpy(peer.peer_addr, learned_mac, ESP_NOW_ETH_ALEN);
        peer.channel = CONFIG_EXAMPLE_ESPNOW_CHANNEL;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;
        esp_err_t err = esp_now_add_peer(&peer);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Learned TX peer %02X:%02X:%02X:%02X:%02X:%02X",
                     learned_mac[0], learned_mac[1], learned_mac[2],
                     learned_mac[3], learned_mac[4], learned_mac[5]);
        } else {
            ESP_LOGW(TAG, "add peer failed: %s", esp_err_to_name(err));
        }
    }

    if (watch && pass) {
        print_frame(&copy, ts);
    }
}

static int cmd_mac(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    printf("本机 STA MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

static int parse_mac(const char *str, uint8_t mac[6])
{
    unsigned int b[6];
    if (sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return -1;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)b[i];
    }
    return 0;
}

static int cmd_peer(int argc, char **argv)
{
    if (argc == 1) {
        lock();
        if (!s_peer_valid) {
            unlock();
            printf("尚未设置对端 MAC（可手动 peer aa:bb:...，或等收到 TX 帧后自动学习）\n");
            return 0;
        }
        uint8_t mac[6];
        memcpy(mac, s_peer_mac, 6);
        unlock();
        printf("对端 TX MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return 0;
    }

    uint8_t mac[6];
    if (parse_mac(argv[1], mac) != 0) {
        printf("MAC 格式错误，示例: peer AA:BB:CC:DD:EE:FF\n");
        return 1;
    }

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = CONFIG_EXAMPLE_ESPNOW_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    if (esp_now_is_peer_exist(mac)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_now_del_peer(mac));
    }
    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        printf("添加 peer 失败: %s\n", esp_err_to_name(err));
        return 1;
    }

    lock();
    memcpy(s_peer_mac, mac, 6);
    s_peer_valid = true;
    unlock();
    printf("已设置对端 TX MAC: %s\n", argv[1]);
    return 0;
}

static int cmd_can_stats(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    lock();
    uint32_t rx = s_rx_ok, drop = s_rx_drop, total = s_total;
    uint32_t tx_ok = s_tx_ok, tx_fail = s_tx_fail;
    bool watch = s_watch;
    bool fen = s_filter_en;
    uint32_t fid = s_filter_id;
    unlock();

    printf("接收成功=%lu  本地丢弃=%lu  缓冲总数=%lu\n",
           (unsigned long)rx, (unsigned long)drop, (unsigned long)total);
    printf("回传成功=%lu  回传失败=%lu\n",
           (unsigned long)tx_ok, (unsigned long)tx_fail);
    printf("watch=%s  filter=%s",
           watch ? "on" : "off",
           fen ? "" : "off");
    if (fen) {
        printf("0x%lX", (unsigned long)fid);
    }
    printf("\n");
    return 0;
}

static int cmd_can_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    lock();
    s_total = 0;
    memset(s_ring, 0, sizeof(s_ring));
    unlock();
    printf("已清空本地帧缓冲\n");
    return 0;
}

static int cmd_can_watch(int argc, char **argv)
{
    if (argc < 2) {
        printf("用法: can watch on|off\n");
        return 1;
    }
    if (!strcmp(argv[1], "on")) {
        s_watch = true;
        printf("已开启实时打印\n");
    } else if (!strcmp(argv[1], "off")) {
        s_watch = false;
        printf("已关闭实时打印\n");
    } else {
        printf("用法: can watch on|off\n");
        return 1;
    }
    return 0;
}

static int cmd_can_filter(int argc, char **argv)
{
    if (argc < 2) {
        printf("用法: can filter <id|off>   例如: can filter 0x123\n");
        return 1;
    }
    if (!strcmp(argv[1], "off") || !strcmp(argv[1], "clear")) {
        s_filter_en = false;
        printf("已清除 ID 过滤\n");
        return 0;
    }
    char *end = NULL;
    unsigned long id = strtoul(argv[1], &end, 0);
    if (end == argv[1]) {
        printf("无效 ID\n");
        return 1;
    }
    s_filter_id = (uint32_t)id;
    s_filter_en = true;
    printf("已设置过滤 ID=0x%lX\n", id);
    return 0;
}

static int cmd_can_last(int argc, char **argv)
{
    int n = DEFAULT_LAST_N;
    if (argc >= 2) {
        n = atoi(argv[1]);
        if (n <= 0) {
            n = DEFAULT_LAST_N;
        }
    }
    if (n > FRAME_RING_SIZE) {
        n = FRAME_RING_SIZE;
    }

    lock();
    uint32_t total = s_total;
    uint32_t available = (total > FRAME_RING_SIZE) ? FRAME_RING_SIZE : total;
    if ((uint32_t)n > available) {
        n = (int)available;
    }

    printf("最近 %d 帧（共缓冲 %lu）:\n", n, (unsigned long)available);
    for (int i = n; i > 0; i--) {
        uint32_t idx = total - (uint32_t)i;
        stored_frame_t item = s_ring[idx % FRAME_RING_SIZE];
        if (s_filter_en && item.frame.id != s_filter_id) {
            continue;
        }
        print_frame(&item.frame, item.ts_us);
    }
    unlock();
    return 0;
}

static int parse_hex_byte(const char *s, uint8_t *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 16);
    if (end == s || v > 0xFF) {
        return -1;
    }
    *out = (uint8_t)v;
    return 0;
}

static int cmd_can_send(int argc, char **argv)
{
    if (argc < 3) {
        printf("用法: can send [-e] <id> <b0> [b1] ... [b7]\n");
        printf("示例: can send 0x123 01 02 03 04\n");
        printf("      can send -e 0x18DAF110 02 10 01\n");
        return 1;
    }

    int argi = 1;
    uint8_t flags = 0;
    if (!strcmp(argv[argi], "-e") || !strcmp(argv[argi], "--ext")) {
        flags |= CAN_ESPNOW_FLAG_EXT;
        argi++;
    }
    if (argi >= argc) {
        printf("缺少 ID\n");
        return 1;
    }

    char *end = NULL;
    unsigned long id = strtoul(argv[argi], &end, 0);
    if (end == argv[argi]) {
        printf("无效 ID\n");
        return 1;
    }
    argi++;

    can_espnow_frame_t pkt = {
        .magic = CAN_ESPNOW_MAGIC,
        .version = CAN_ESPNOW_VERSION,
        .flags = flags,
        .id = (uint32_t)id,
    };

    while (argi < argc && pkt.dlc < CAN_ESPNOW_MAX_DATA) {
        if (parse_hex_byte(argv[argi], &pkt.data[pkt.dlc]) != 0) {
            printf("无效数据字节: %s\n", argv[argi]);
            return 1;
        }
        pkt.dlc++;
        argi++;
    }
    if (argi < argc) {
        printf("数据最多 8 字节\n");
        return 1;
    }

    lock();
    if (!s_peer_valid) {
        unlock();
        printf("未设置对端 MAC，请先: peer AA:BB:CC:DD:EE:FF\n");
        return 1;
    }
    uint8_t peer[6];
    memcpy(peer, s_peer_mac, 6);
    pkt.seq = s_tx_seq++;
    unlock();

    if (!esp_now_is_peer_exist(peer)) {
        esp_now_peer_info_t info = {0};
        memcpy(info.peer_addr, peer, 6);
        info.channel = CONFIG_EXAMPLE_ESPNOW_CHANNEL;
        info.ifidx = WIFI_IF_STA;
        info.encrypt = false;
        esp_err_t add_err = esp_now_add_peer(&info);
        if (add_err != ESP_OK) {
            printf("添加 peer 失败: %s\n", esp_err_to_name(add_err));
            return 1;
        }
    }

    esp_err_t err = esp_now_send(peer, (const uint8_t *)&pkt, sizeof(pkt));
    if (err != ESP_OK) {
        lock();
        s_tx_fail++;
        unlock();
        printf("发送失败: %s\n", esp_err_to_name(err));
        return 1;
    }

    lock();
    s_tx_ok++;
    unlock();
    printf("已回传至 TX: id=0x%lX dlc=%u\n", (unsigned long)pkt.id, pkt.dlc);
    return 0;
}

static int cmd_can(int argc, char **argv)
{
    if (argc < 2) {
        printf("用法:\n");
        printf("  can stats\n");
        printf("  can last [n]\n");
        printf("  can watch on|off\n");
        printf("  can clear\n");
        printf("  can filter <id|off>\n");
        printf("  can send [-e] <id> <hex...>\n");
        return 0;
    }
    if (!strcmp(argv[1], "stats")) {
        return cmd_can_stats(argc - 1, argv + 1);
    }
    if (!strcmp(argv[1], "last")) {
        return cmd_can_last(argc - 1, argv + 1);
    }
    if (!strcmp(argv[1], "watch")) {
        return cmd_can_watch(argc - 1, argv + 1);
    }
    if (!strcmp(argv[1], "clear")) {
        return cmd_can_clear(argc - 1, argv + 1);
    }
    if (!strcmp(argv[1], "filter")) {
        return cmd_can_filter(argc - 1, argv + 1);
    }
    if (!strcmp(argv[1], "send")) {
        return cmd_can_send(argc - 1, argv + 1);
    }
    printf("未知子命令: %s\n", argv[1]);
    return 1;
}

static void register_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "mac", .help = "显示本机 STA MAC", .hint = NULL, .func = &cmd_mac },
        { .command = "peer", .help = "查看/设置 TX 对端 MAC: peer [aa:bb:cc:dd:ee:ff]", .hint = NULL, .func = &cmd_peer },
        { .command = "can", .help = "CAN 操作: stats|last|watch|clear|filter|send", .hint = NULL, .func = &cmd_can },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}


esp_err_t can_cli_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    /* Optional default peer from menuconfig. */
    uint8_t def_mac[6];
    unsigned int b[6];
    if (sscanf(CONFIG_EXAMPLE_ESPNOW_PEER_MAC, "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
        for (int i = 0; i < 6; i++) def_mac[i] = (uint8_t)b[i];
        bool nonzero = false;
        for (int i = 0; i < 6; i++) if (def_mac[i]) nonzero = true;
        if (nonzero) {
            memcpy(s_peer_mac, def_mac, 6);
            s_peer_valid = true;
            esp_now_peer_info_t peer = {0};
            memcpy(peer.peer_addr, def_mac, 6);
            peer.channel = CONFIG_EXAMPLE_ESPNOW_CHANNEL;
            peer.ifidx = WIFI_IF_STA;
            peer.encrypt = false;
            if (!esp_now_is_peer_exist(def_mac)) {
                esp_now_add_peer(&peer);
            }
            ESP_LOGI(TAG, "Default TX peer from Kconfig");
        }
    }

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "esp-can-rx> ";
    repl_config.max_cmdline_length = 256;

    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_uart(&uart_config, &repl_config, &repl), TAG, "repl uart");

    esp_console_register_help_command();
    register_commands();

    /* Use REPL task provided by esp_console. */
    ESP_RETURN_ON_ERROR(esp_console_start_repl(repl), TAG, "start repl");
    ESP_LOGI(TAG, "Console ready");
    return ESP_OK;
}
