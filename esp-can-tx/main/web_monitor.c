/*
 * SoftAP web monitor for esp-can-tx.
 * Join the ESP32 hotspot and open http://192.168.4.1/
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_http_server.h"

#include "web_monitor.h"

static const char *TAG = "web_monitor";

#define FRAME_RING_SIZE  128
#define JSON_BUF_SIZE    4096

static httpd_handle_t s_server;
static SemaphoreHandle_t s_lock;
static can_monitor_stats_t s_stats;
static can_monitor_frame_t s_ring[FRAME_RING_SIZE];
static uint32_t s_total_frames;

static const char INDEX_HTML[] =
"<!DOCTYPE html>\n"
"<html lang=\"zh-CN\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">\n"
"<title>ESP-CAN 监视器</title>\n"
"<style>\n"
":root{--bg:#0e1410;--panel:#162019;--line:#2a3b30;--text:#d7e6d9;--muted:#7f9a86;--accent:#9acd32;--warn:#e0a106;--bad:#e35d5d;--mono:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;--sans:\"Segoe UI\",system-ui,-apple-system,sans-serif}\n"
"*{box-sizing:border-box}\n"
"body{margin:0;background:radial-gradient(1200px 600px at 10% -10%,#1b2a20 0%,var(--bg) 55%);color:var(--text);font-family:var(--sans);min-height:100vh}\n"
"header{padding:1.1rem 1.2rem .7rem;border-bottom:1px solid var(--line);background:linear-gradient(180deg,rgba(22,32,25,.95),rgba(14,20,16,.65))}\n"
"h1{margin:0;font-size:1.35rem;letter-spacing:.04em}\n"
".sub{margin:.35rem 0 0;color:var(--muted);font-size:.9rem}\n"
"main{padding:1rem;display:grid;gap:1rem}\n"
".stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(118px,1fr));gap:.75rem}\n"
".card{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:.85rem .9rem}\n"
".card .k{color:var(--muted);font-size:.72rem;letter-spacing:.08em;text-transform:uppercase}\n"
".card .v{margin-top:.25rem;font-family:var(--mono);font-size:1.15rem;color:var(--accent)}\n"
".toolbar{display:flex;flex-wrap:wrap;gap:.6rem;align-items:center}\n"
"button,.pill{appearance:none;border:1px solid var(--line);background:#1c2a21;color:var(--text);border-radius:999px;padding:.45rem .9rem;font:inherit;cursor:pointer}\n"
"button.active{border-color:var(--accent);color:var(--accent)}\n"
".status{font-family:var(--mono);font-size:.85rem}.ok{color:var(--accent)}.bad{color:var(--bad)}\n"
".table-wrap{overflow:auto;border:1px solid var(--line);border-radius:10px;background:var(--panel);max-height:70vh}\n"
"table{width:100%;border-collapse:collapse;font-family:var(--mono);font-size:.82rem}\n"
"th,td{padding:.55rem .65rem;border-bottom:1px solid var(--line);white-space:nowrap;text-align:left}\n"
"th{position:sticky;top:0;background:#1a2820;color:var(--muted);font-size:.72rem;letter-spacing:.06em;text-transform:uppercase}\n"
"tr:hover td{background:rgba(154,205,50,.06)}\n"
".id{color:var(--warn)}.data{color:var(--text)}.meta{color:var(--muted)}\n"
"@media (max-width:640px){th:nth-child(2),td:nth-child(2),th:nth-child(5),td:nth-child(5){display:none}}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<header>\n"
"  <h1>ESP-CAN 监视器</h1>\n"
"  <p class=\"sub\">手机/电脑连接本机热点后，打开 <b>http://192.168.4.1</b> 实时查看 CAN 帧</p>\n"
"</header>\n"
"<main>\n"
"  <section class=\"stats\">\n"
"    <div class=\"card\"><div class=\"k\">接收</div><div class=\"v\" id=\"rx\">0</div></div>\n"
"    <div class=\"card\"><div class=\"k\">转发成功</div><div class=\"v\" id=\"ok\">0</div></div>\n"
"    <div class=\"card\"><div class=\"k\">转发失败</div><div class=\"v\" id=\"fail\">0</div></div>\n"
"    <div class=\"card\"><div class=\"k\">丢弃</div><div class=\"v\" id=\"drop\">0</div></div>\n"
"    <div class=\"card\"><div class=\"k\">页面帧/秒</div><div class=\"v\" id=\"fps\">0</div></div>\n"
"  </section>\n"
"  <section class=\"toolbar\">\n"
"    <span class=\"status bad\" id=\"conn\">连接中…</span>\n"
"    <button id=\"pauseBtn\" type=\"button\">暂停</button>\n"
"    <button id=\"clearBtn\" type=\"button\">清空</button>\n"
"    <label class=\"pill\"><input id=\"filterExt\" type=\"checkbox\"> 只看扩展帧</label>\n"
"  </section>\n"
"  <section class=\"table-wrap\">\n"
"    <table>\n"
"      <thead><tr><th>时间ms</th><th>序号</th><th>ID</th><th>DLC</th><th>标志</th><th>数据</th></tr></thead>\n"
"      <tbody id=\"tbody\"></tbody>\n"
"    </table>\n"
"  </section>\n"
"</main>\n"
"<script>\n"
"const MAX_ROWS=200;\n"
"let paused=false, since=0, framesThisSec=0;\n"
"const tbody=document.getElementById('tbody');\n"
"const conn=document.getElementById('conn');\n"
"document.getElementById('pauseBtn').onclick=function(){paused=!paused;this.textContent=paused?'继续':'暂停';this.classList.toggle('active',paused)};\n"
"document.getElementById('clearBtn').onclick=function(){tbody.innerHTML=''};\n"
"setInterval(function(){document.getElementById('fps').textContent=framesThisSec;framesThisSec=0},1000);\n"
"function hex2(n){return ('0'+Number(n).toString(16).toUpperCase()).slice(-2)}\n"
"function fmtId(id,ext){var s=Number(id).toString(16).toUpperCase();return '0x'+s.padStart(ext?8:3,'0')}\n"
"function addRow(f){\n"
"  if(document.getElementById('filterExt').checked && !(f.flags&1)) return;\n"
"  framesThisSec++; if(paused) return;\n"
"  var flags=[]; if(f.flags&1) flags.push('EXT'); if(f.flags&2) flags.push('RTR');\n"
"  var data=(f.data||[]).slice(0,f.dlc).map(hex2).join(' ');\n"
"  var tr=document.createElement('tr');\n"
"  tr.innerHTML='<td class=\"meta\">'+((f.ts_us/1000)|0)+'</td><td class=\"meta\">'+f.seq+\n"
"    '</td><td class=\"id\">'+fmtId(f.id,f.flags&1)+'</td><td>'+f.dlc+\n"
"    '</td><td class=\"meta\">'+(flags.join('|')||'-')+'</td><td class=\"data\">'+data+'</td>';\n"
"  tbody.prepend(tr);\n"
"  while(tbody.children.length>MAX_ROWS) tbody.removeChild(tbody.lastChild);\n"
"}\n"
"async function tick(){\n"
"  try{\n"
"    var r=await fetch('/api/frames?since='+since,{cache:'no-store'});\n"
"    if(!r.ok) throw new Error('http');\n"
"    var j=await r.json();\n"
"    conn.textContent='已连接';conn.className='status ok';\n"
"    document.getElementById('rx').textContent=j.rx;\n"
"    document.getElementById('ok').textContent=j.ok;\n"
"    document.getElementById('fail').textContent=j.fail;\n"
"    document.getElementById('drop').textContent=j.drop;\n"
"    since=j.next;\n"
"    (j.frames||[]).forEach(addRow);\n"
"  }catch(e){conn.textContent='连接断开，重试中…';conn.className='status bad'}\n"
"}\n"
"setInterval(tick,200); tick();\n"
"</script>\n"
"</body>\n"
"</html>\n";

static void lock_mtx(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock_mtx(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static int append_frame_json(char *out, size_t out_sz, const can_monitor_frame_t *f, int first)
{
    char data_json[64] = {0};
    char *p = data_json;
    size_t left = sizeof(data_json);

    for (int i = 0; i < f->dlc && i < CAN_ESPNOW_MAX_DATA; i++) {
        int n = snprintf(p, left, "%s%u", (i ? "," : ""), (unsigned)f->data[i]);
        if (n < 0 || (size_t)n >= left) {
            break;
        }
        p += n;
        left -= (size_t)n;
    }

    return snprintf(out, out_sz,
                    "%s{\"seq\":%lu,\"id\":%lu,\"dlc\":%u,\"flags\":%u,\"ts_us\":%lld,\"data\":[%s]}",
                    first ? "" : ",",
                    (unsigned long)f->seq,
                    (unsigned long)f->id,
                    f->dlc,
                    f->flags,
                    (long long)f->ts_us,
                    data_json);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t frames_get_handler(httpd_req_t *req)
{
    uint32_t since = 0;
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK) {
            since = (uint32_t)strtoul(val, NULL, 10);
        }
    }

    char *json = malloc(JSON_BUF_SIZE);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    lock_mtx();
    uint32_t total = s_total_frames;
    uint32_t oldest = (total > FRAME_RING_SIZE) ? (total - FRAME_RING_SIZE) : 0;
    if (since < oldest) {
        since = oldest;
    }

    int off = snprintf(json, JSON_BUF_SIZE,
                       "{\"rx\":%lu,\"ok\":%lu,\"fail\":%lu,\"drop\":%lu,\"next\":%lu,\"frames\":[",
                       (unsigned long)s_stats.rx_count,
                       (unsigned long)s_stats.fwd_ok,
                       (unsigned long)s_stats.fwd_fail,
                       (unsigned long)s_stats.drop_count,
                       (unsigned long)total);

    int first = 1;
    for (uint32_t idx = since; idx < total; idx++) {
        const can_monitor_frame_t *f = &s_ring[idx % FRAME_RING_SIZE];
        int n = append_frame_json(json + off, JSON_BUF_SIZE - (size_t)off, f, first);
        if (n < 0 || (size_t)n >= (JSON_BUF_SIZE - (size_t)off)) {
            break;
        }
        off += n;
        first = 0;
    }
    unlock_mtx();

    if (off < (int)JSON_BUF_SIZE - 2) {
        json[off++] = ']';
        json[off++] = '}';
        json[off] = '\0';
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, json, off);
    free(json);
    return err;
}

void web_monitor_set_stats(const can_monitor_stats_t *stats)
{
    if (!stats) {
        return;
    }
    lock_mtx();
    s_stats = *stats;
    unlock_mtx();
}

void web_monitor_publish_frame(const can_monitor_frame_t *frame)
{
    if (!frame) {
        return;
    }
    lock_mtx();
    s_ring[s_total_frames % FRAME_RING_SIZE] = *frame;
    s_total_frames++;
    unlock_mtx();
}

esp_err_t web_monitor_start(void)
{
    if (s_server) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    config.max_open_sockets = 7;
    config.stack_size = 8192;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "httpd_start");

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    const httpd_uri_t frames = {
        .uri = "/api/frames",
        .method = HTTP_GET,
        .handler = frames_get_handler,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &root), TAG, "register /");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &frames), TAG, "register /api/frames");

    ESP_LOGI(TAG, "Web monitor ready: http://192.168.4.1/");
    return ESP_OK;
}
