#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "gui_web_test";

static EventGroupHandle_t s_wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT = BIT1;
static const ledc_mode_t PWM_MODE = LEDC_LOW_SPEED_MODE;
static const ledc_timer_t PWM_TIMER = LEDC_TIMER_0;
static const ledc_timer_bit_t PWM_RESOLUTION = LEDC_TIMER_10_BIT;
static const uint32_t PWM_MAX_DUTY = (1U << 10) - 1U;
static const ledc_channel_t LIGHT_PWM_CHANNEL = LEDC_CHANNEL_0;
static const ledc_channel_t FAN_PWM_CHANNEL = LEDC_CHANNEL_1;
static const i2c_port_t SENSOR_I2C_PORT = I2C_NUM_0;
static const int SENSOR_I2C_FREQ_HZ = 100000;
static const uint8_t SCD40_ADDR = 0x62;
static const uint8_t AHT20_ADDR = 0x38;
static const uart_port_t PMS_UART_PORT = UART_NUM_1;
static const uart_port_t LD2410_UART_PORT = UART_NUM_2;
#define PMS_FRAME_LEN 32
#define INMP441_SAMPLE_COUNT 512
#define MAX_SCHEDULE_ITEMS 8
#define SCHEDULE_CHECK_INTERVAL_MS 30000
#define SCHEDULE_PRE_CLASS_MINUTES 5
#define SCHEDULE_POST_CLASS_MINUTES 5
#define SCHEDULE_POST_WINDOW_MINUTES 15
#define CLASSROOM_LIGHT_THRESHOLD_LUX 500
#define CLASSROOM_PRECLASS_BRIGHTNESS 80
static int s_retry_num;
static int s_manual_light;
static int s_manual_fan;
static int s_light_brightness = 40;
static int s_fan_speed = 0;
static int s_alarm_power;
static bool s_light_pwm_ready;
static bool s_fan_pwm_ready;
static bool s_i2c_ready;
static bool s_scd40_ready;
static bool s_aht20_ready;
static bool s_pms_ready;
static bool s_ld2410_ready;
#if CONFIG_GUI_WEB_TEST_INMP441_ENABLE
static i2s_chan_handle_t s_inmp441_rx_chan;
static bool s_inmp441_ready;
static bool s_inmp441_has_smoothed_dba;
static float s_inmp441_smoothed_dba = 42.0f;
static int32_t s_inmp441_samples[INMP441_SAMPLE_COUNT];
#endif
static int s_ai_runs;
static int s_last_http_status;
static esp_err_t s_last_http_err = ESP_OK;
static TaskHandle_t s_ai_task_handle;
static TaskHandle_t s_schedule_task_handle;
static char s_ip_addr[16] = "192.168.4.1";
static char s_ai_status[32] = "idle";
static char s_ai_result[1200] = "点击 AI 分析，让教室管家生成风扇和报警建议。";
static char s_ai_reason[256] = "尚未进行 AI 分析。";
static char s_ai_action[256] = "暂无执行动作。";
static char s_ai_summary[512] = "暂无 AI 环境总结。";
static char s_schedule_status[256] = "课程表调度等待时间同步。";
static char s_risk_level[24] = "Normal";
static char s_demo_scenario[24] = "Hardware";
static char s_data_source[64] = "hardware";
static bool s_light_is_fallback = true;
static bool s_time_sync_started;
static volatile bool s_time_synced;

typedef struct {
    int temperature_x10;
    int humidity;
    int co2_ppm;
    int pm25_ugm3;
    int ambient_light_lux;
    int noise_x10;
    int occupancy_count;
    int comfort_score;
    int air_quality_score;
    int safety_score;
} guardian_state_t;

typedef struct {
    int day;
    char start[6];
    char end[6];
    char name[32];
    char room[24];
    bool enabled;
} schedule_item_t;

static guardian_state_t s_state = {
    .temperature_x10 = 285,
    .humidity = 62,
    .co2_ppm = 1200,
    .pm25_ugm3 = 48,
    .ambient_light_lux = 180,
    .noise_x10 = 615,
    .occupancy_count = 18,
    .comfort_score = 74,
    .air_quality_score = 48,
    .safety_score = 92,
};

static schedule_item_t s_schedule[MAX_SCHEDULE_ITEMS] = {
    {.day = 1, .start = "08:00", .end = "09:40", .name = "智能物联网", .room = "A301", .enabled = true},
    {.day = 1, .start = "10:00", .end = "11:40", .name = "嵌入式系统", .room = "A301", .enabled = true},
    {.day = 2, .start = "14:00", .end = "15:40", .name = "传感器技术", .room = "A301", .enabled = true},
    {.day = 3, .start = "08:00", .end = "09:40", .name = "AIoT 实训", .room = "A301", .enabled = true},
    {.day = 4, .start = "10:00", .end = "11:40", .name = "数据采集", .room = "A301", .enabled = true},
    {.day = 5, .start = "14:00", .end = "15:40", .name = "项目展示", .room = "A301", .enabled = true},
    {.day = 0, .start = "00:00", .end = "00:00", .name = "", .room = "", .enabled = false},
    {.day = 0, .start = "00:00", .end = "00:00", .name = "", .room = "", .enabled = false},
};
static int s_schedule_pre_trigger_key[MAX_SCHEDULE_ITEMS];
static int s_schedule_post_trigger_key[MAX_SCHEDULE_ITEMS];

typedef struct {
    char *buf;
    int len;
    int max_len;
} http_capture_t;

static void read_real_sensors(void);
static void simulate_sensors(void);
static bool start_ai_analysis_task(const char *trigger);

static const char INDEX_HTML[] =
"<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>教室管家</title><style>"
":root{font-family:Arial,sans-serif;color:#18212f;background:#eef3f7}body{margin:0;padding:18px}.wrap{max-width:1080px;margin:auto}"
".top{display:flex;justify-content:space-between;gap:12px;align-items:flex-start}.brand h1{margin:0;font-size:30px}.brand .label{margin-top:5px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(172px,1fr));gap:12px;margin-top:12px}.columns{display:grid;grid-template-columns:1.1fr .9fr;gap:12px;margin-top:12px}"
".card,.hero{background:#fff;border:1px solid #d8e0e8;border-radius:8px;padding:14px;box-shadow:0 1px 2px #0001}.hero{margin-top:14px;display:grid;grid-template-columns:1.2fr 1fr;gap:16px;align-items:stretch}.hero h2{font-size:38px;margin:5px 0 8px}.hero-state{font-size:22px;font-weight:700;margin-top:6px}.hero-tip{line-height:1.55;margin:8px 0 0;color:#394557}"
".hero-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}.stat{border:1px solid #e1e7ee;border-radius:8px;padding:11px;background:#f8fafc}.label{font-size:13px;color:#5b6778}.value{font-size:26px;font-weight:700;margin-top:6px}.compact-value{font-size:16px;font-weight:700;margin-top:4px;line-height:1.35;word-break:break-word}.section-title{font-size:18px;font-weight:700;margin-top:16px}"
"button{height:40px;border:0;border-radius:6px;background:#1769aa;color:white;padding:0 14px;font-weight:700}button.secondary{background:#384250}button.danger{background:#a43f3f}.row{display:flex;gap:9px;flex-wrap:wrap}.ok{color:#0b7a45}.warn{color:#a15c00}.bad-text{color:#9f1d1d}.act-on{color:#0b7a45}.act-off{color:#6b7280}"
".pill{display:inline-flex;border-radius:999px;padding:5px 9px;font-size:12px;font-weight:700;background:#e5e7eb;color:#374151;white-space:nowrap}.pill.run{background:#fff3cd;color:#8a5a00}.pill.good{background:#dff6e8;color:#0b7a45}.pill.bad{background:#fde2e2;color:#9f1d1d}.pill.blue{background:#dbeafe;color:#1d4e89}"
".mini-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px;margin-top:12px}.section-head{display:flex;justify-content:space-between;gap:12px;align-items:flex-start}.ai-head{font-size:24px;font-weight:700;margin-top:5px}.reason{margin-top:6px;line-height:1.45}.timeline,.log{display:grid;gap:8px;margin-top:10px}.timeline-item,.log-item{border-left:4px solid #cbd5e1;background:#f8fafc;border-radius:6px;padding:9px}.timeline-item.active{border-left-color:#1769aa;background:#eef6ff}.chart-wrap{margin-top:10px}.chart-meta{display:flex;justify-content:space-between;gap:12px;align-items:center;flex-wrap:wrap}.chart-legend{display:flex;gap:12px;flex-wrap:wrap}.dot{width:10px;height:10px;border-radius:50%;display:inline-block;margin-right:5px}canvas{width:100%;height:180px;border:1px solid #e1e7ee;border-radius:8px;background:#f8fafc}.debug{margin-top:12px}.debug summary{cursor:pointer;font-weight:700;color:#384250}.debug pre{margin-top:10px}pre{white-space:pre-wrap;background:#111827;color:#e5e7eb;border-radius:8px;padding:12px;overflow:auto}@media(max-width:760px){.hero,.columns{grid-template-columns:1fr}.hero h2{font-size:32px}}"
"</style></head><body><div class=\"wrap\"><div class=\"top\"><div class=\"brand\"><h1>教室管家</h1><div class=\"label\">课前预判 · 课中守护 · 课后节能 - web v10</div></div><div id=\"link\" class=\"label\"></div></div>"
"<section class=\"hero\"><div><div class=\"label\">当前教室</div><h2 id=\"roomName\">A301</h2><div id=\"stewardState\" class=\"hero-state\">等待数据</div><p id=\"stewardTip\" class=\"hero-tip\">正在读取教室状态。</p></div><div class=\"hero-grid\"><div class=\"stat\"><div class=\"label\">当前课程</div><div id=\"currentClass\" class=\"compact-value\">--</div></div><div class=\"stat\"><div class=\"label\">下一节课</div><div id=\"nextClass\" class=\"compact-value\">--</div></div><div class=\"stat\"><div class=\"label\">北京时间</div><div id=\"clock\" class=\"compact-value\">--</div></div><div class=\"stat\"><div class=\"label\">自动调度</div><div id=\"classPhase\" class=\"compact-value\">--</div></div></div></section>"
"<div class=\"section-title\">教室状态</div><div class=\"grid\"><div class=\"card\"><div class=\"label\">联网</div><div id=\"conn\" class=\"value warn\">...</div></div><div class=\"card\"><div class=\"label\">在室人数</div><div id=\"occ\" class=\"value\">--</div></div><div class=\"card\"><div class=\"label\">教室健康指数</div><div id=\"health\" class=\"value\">--</div></div><div class=\"card\"><div class=\"label\">环境判断</div><div id=\"risk\" class=\"value\">--</div></div><div class=\"card\"><div class=\"label\">温度</div><div id=\"temp\" class=\"value\">--</div></div><div class=\"card\"><div class=\"label\">湿度</div><div id=\"hum\" class=\"value\">--</div></div><div class=\"card\"><div class=\"label\">CO2</div><div id=\"co2\" class=\"value\">--</div></div><div class=\"card\"><div class=\"label\">PM2.5</div><div id=\"pm25\" class=\"value\">--</div></div><div class=\"card\"><div class=\"label\">照度</div><div id=\"lux\" class=\"value\">--</div></div><div class=\"card\"><div class=\"label\">噪声</div><div id=\"noise\" class=\"value\">--</div></div></div>"
"<div class=\"card\" style=\"margin-top:12px\"><div class=\"section-head\"><div><div class=\"label\">最近环境曲线</div><div class=\"compact-value\">CO2、PM2.5 与温度趋势</div></div><span id=\"envNow\" class=\"pill blue\">等待数据</span></div><div class=\"chart-wrap\"><div class=\"chart-meta\"><div class=\"label\">最近 60 秒采样，自动随页面 1 秒刷新</div><div class=\"chart-legend\"><span class=\"label\"><i class=\"dot\" style=\"background:#1769aa\"></i>CO2</span><span class=\"label\"><i class=\"dot\" style=\"background:#a43f3f\"></i>PM2.5</span><span class=\"label\"><i class=\"dot\" style=\"background:#0b7a45\"></i>温度</span></div></div><canvas id=\"envChart\"></canvas></div></div>"
"<div class=\"section-title\">教室设备</div><div class=\"grid\"><div class=\"card\"><div class=\"label\">照明</div><div id=\"actLight\" class=\"value\">--</div></div><div class=\"card\"><div class=\"label\">通风</div><div id=\"actFan\" class=\"value\">--</div></div><div class=\"card\"><div class=\"label\">安防报警</div><div id=\"actAlarm\" class=\"value\">--</div></div><div class=\"card\"><div class=\"label\">蜂鸣器</div><div id=\"actBuzzer\" class=\"value\">--</div></div></div>"
"<div class=\"card\" style=\"margin-top:12px\"><div class=\"section-head\"><div><div class=\"label\">设备控制</div><div class=\"compact-value\">手动控制与 AI 分析</div></div><button class=\"secondary\" onclick=\"location.href='/schedule'\">课程表</button></div><div class=\"row\" style=\"margin-top:10px\"><button onclick=\"control('light')\">照明</button><button onclick=\"control('fan')\">通风</button><button class=\"danger\" onclick=\"control('alarm')\">报警</button><button class=\"secondary\" onclick=\"runAiTest()\">AI 分析</button></div></div>"
"<div class=\"columns\"><div class=\"card\"><div class=\"section-head\"><div><div class=\"label\">教室管家建议</div><div id=\"aiHeadline\" class=\"ai-head\">未判断</div></div><div id=\"aiBadge\" class=\"pill\">idle</div></div><div class=\"mini-grid\"><div><div class=\"label\">管家状态</div><div id=\"aiStatus\" class=\"compact-value\">--</div></div><div><div class=\"label\">HTTP 状态码</div><div id=\"aiHttp\" class=\"compact-value\">--</div></div><div><div class=\"label\">环境判断</div><div id=\"aiConditionText\" class=\"compact-value\">--</div></div><div><div class=\"label\">已执行动作</div><div id=\"aiAction\" class=\"compact-value\">--</div></div></div><div class=\"label\" style=\"margin-top:12px\">说明</div><div id=\"aiReason\" class=\"reason\">--</div><details class=\"debug\"><summary>Raw 返回</summary><pre id=\"aiRaw\">Idle</pre></details></div>"
"<div class=\"card\"><div class=\"section-head\"><div><div class=\"label\">今日课程</div><div id=\"todaySummary\" class=\"compact-value\">--</div></div><span class=\"pill blue\">自动策略</span></div><div id=\"todayList\" class=\"timeline\"></div></div></div>"
"<div class=\"card\" style=\"margin-top:12px\"><div class=\"section-head\"><div><div class=\"label\">管家记录</div><div class=\"compact-value\">最近状态变化</div></div><span id=\"sched\" class=\"pill\">--</span></div><div id=\"events\" class=\"log\"></div></div><div class=\"card\" style=\"margin-top:12px\"><div class=\"section-head\"><div><div class=\"label\">验收测试</div><div class=\"compact-value\">现场展示时切换教室场景</div></div><span class=\"pill blue\">演示模式</span></div><div class=\"row\" style=\"margin-top:10px\"><button class=\"secondary\" onclick=\"scenario('Hardware')\">真实教室</button><button class=\"secondary\" onclick=\"scenario('AirQuality')\">空气闷热</button><button class=\"secondary\" onclick=\"scenario('Security')\">异常闯入</button><button class=\"secondary\" onclick=\"scenario('EmptyWaste')\">课后无人</button></div></div><details class=\"card debug\"><summary>调试 JSON</summary><pre id=\"raw\">{}</pre></details></div>"
"<script>"
"let scheduleItems=[];let envHistory=[];function sleep(ms){return new Promise(r=>setTimeout(r,ms));}function esc(x){return String(x||'').replace(/[&<>']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;',\"'\":'&#39;'}[c]));}"
"function labelCondition(c){const m={normal:'正常',energy_save:'节能',air_quality:'空气质量',security:'安防',Normal:'正常',EnergySave:'节能',AirQuality:'空气质量',Security:'安防',connectivity:'联网测试',not_decided:'未判断'};return m[c]||c||'未判断';}"
"function getAiCondition(s){const reason=(s.ai.reason||'');const m=reason.match(/condition=([A-Za-z_]+)/);if(m)return m[1];if(s.ai.status==='llm_ok')return s.risk.level;if(s.ai.status==='internet_ok'||s.ai.status==='internet_fail')return 'connectivity';return 'not_decided';}"
"function statusClass(status){if(status==='running')return 'run';if(status==='llm_ok'||status==='internet_ok')return 'good';if(status==='llm_fail'||status==='internet_fail'||status==='task_fail')return 'bad';return '';}"
"function labelAiStatus(x){const m={idle:'待分析',running:'分析中',llm_ok:'AI分析完成',llm_fail:'AI分析失败',internet_ok:'联网正常',internet_fail:'联网失败',task_fail:'任务创建失败'};return m[x]||x||'待分析';}"
"function formatAction(a,s){if(!a)return '--';if(!a.startsWith('Applied '))return a;return `通风${s.actuators.fan_power?'开启':'关闭'}，报警${s.actuators.alarm_power?'开启':'关闭'}；照明${s.actuators.light_power?'开启':'关闭'}`;}"
"function deviceDate(s){const t=s.time&&s.time.local;if(t&&t.length>=16&&t[4]==='-'){return new Date(Number(t.slice(0,4)),Number(t.slice(5,7))-1,Number(t.slice(8,10)),Number(t.slice(11,13)),Number(t.slice(14,16)),Number(t.slice(17,19)||0));}return new Date();}"
"function dayNo(d){const n=d.getDay();return n===0?7:n;}function minOf(t){return Number(t.slice(0,2))*60+Number(t.slice(3,5));}"
"async function loadSchedule(){try{const r=await fetch('/api/schedule');const d=await r.json();scheduleItems=d.items||[];}catch(e){scheduleItems=[];}}"
"function courseInfo(s){const now=deviceDate(s);const nowMin=now.getHours()*60+now.getMinutes();const today=scheduleItems.filter(x=>x.enabled&&x.day===dayNo(now)).sort((a,b)=>minOf(a.start)-minOf(b.start));let current=null,next=null;for(const it of today){const st=minOf(it.start),en=minOf(it.end);if(nowMin>=st&&nowMin<=en)current=it;if(!next&&nowMin<st)next=it;}let phase='空闲巡检';if(current)phase='上课守护';else if(next&&minOf(next.start)-nowMin<=5)phase='课前准备';else if(next)phase='等待上课';return {today,current,next,phase,nowMin,room:(current||next||today[0]||{}).room||'A301'};}"
"function renderCourses(info){todaySummary.textContent=info.current?'正在上课':(info.next?'下一节 '+info.next.start:'今日无后续课程');if(!info.today.length){todayList.innerHTML='<div class=timeline-item>今日没有已启用课程</div>';return;}todayList.innerHTML=info.today.map(it=>{const active=info.current===it?' active':'';return `<div class='timeline-item${active}'><b>${it.start}-${it.end}</b> ${esc(it.name)}<div class='label'>${esc(it.room||'A301')} · 课前5分钟开灯并AI分析，课后5分钟无人关灯</div></div>`;}).join('');}"
"function stewardTitle(s,info){if(s.risk.level==='Security')return '安防告警';if(s.risk.level==='AirQuality')return '空气需通风';if(s.risk.level==='EnergySave')return '课后节能';if(info.phase==='课前准备')return '课前准备';if(info.phase==='上课守护')return '上课守护';return '空闲巡检';}"
"function stewardText(s,info){if(s.ai.status==='llm_ok'&&s.ai.summary)return s.ai.summary;if(info.current)return `${info.current.name}正在进行，教室管家持续监测空气、人数和噪声。`;if(info.next)return `${info.next.name}将在${info.next.start}开始，系统会在课前5分钟自动准备。`;return s.schedule&&s.schedule.status?s.schedule.status:'教室处于空闲巡检状态。';}"
"function renderEvents(s,info){const t=(s.time&&s.time.local)||'--';const list=[`${t} ${s.schedule&&s.schedule.status?s.schedule.status:'课程表调度待命'}`,`设备状态：照明${s.actuators.light_power?'开启':'关闭'}，通风${s.actuators.fan_power?'开启':'关闭'}，报警${s.actuators.alarm_power?'开启':'关闭'}`];if(s.ai.status==='llm_ok'&&s.ai.summary)list.unshift(`AI分析：${s.ai.summary}`);events.innerHTML=list.map(x=>`<div class='log-item'>${esc(x)}</div>`).join('');}"
"function pushEnvSample(s){envHistory.push({co2:s.sensors.co2_ppm,pm25:s.sensors.pm25_ugm3,temp:s.sensors.temperature_c,t:new Date()});while(envHistory.length>60)envHistory.shift();}"
"function drawEnvChart(){const c=document.getElementById('envChart');if(!c||!envHistory.length)return;const dpr=window.devicePixelRatio||1;const w=Math.max(320,Math.floor(c.getBoundingClientRect().width));const h=180;if(c.width!==w*dpr||c.height!==h*dpr){c.width=w*dpr;c.height=h*dpr;c.style.height=h+'px';}const ctx=c.getContext('2d');ctx.setTransform(dpr,0,0,dpr,0,0);ctx.clearRect(0,0,w,h);const pad={l:42,r:42,t:16,b:28};ctx.strokeStyle='#e1e7ee';ctx.lineWidth=1;ctx.font='12px Arial';ctx.fillStyle='#5b6778';for(let i=0;i<=4;i++){const y=pad.t+(h-pad.t-pad.b)*i/4;ctx.beginPath();ctx.moveTo(pad.l,y);ctx.lineTo(w-pad.r,y);ctx.stroke();}ctx.fillText('CO2',8,18);ctx.fillText('PM2.5 / C',w-58,18);function plot(key,color,min,max){ctx.strokeStyle=color;ctx.lineWidth=2;ctx.beginPath();envHistory.forEach((p,i)=>{const x=pad.l+(w-pad.l-pad.r)*(envHistory.length===1?1:i/(envHistory.length-1));const y=pad.t+(h-pad.t-pad.b)*(1-(Math.max(min,Math.min(max,p[key]))-min)/(max-min));if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);});ctx.stroke();}plot('co2','#1769aa',400,1800);plot('pm25','#a43f3f',0,120);plot('temp','#0b7a45',15,40);const latest=envHistory[envHistory.length-1];envNow.textContent=`CO2 ${latest.co2} ppm / PM2.5 ${latest.pm25} / 温度 ${latest.temp.toFixed(1)} C`;}"
"async function refresh(){try{const r=await fetch('/api/state');const s=await r.json();const info=courseInfo(s);roomName.textContent=info.room;stewardState.textContent=stewardTitle(s,info);stewardTip.textContent=stewardText(s,info);currentClass.textContent=info.current?`${info.current.name} ${info.current.start}-${info.current.end}`:'当前无课';nextClass.textContent=info.next?`${info.next.name} ${info.next.start}`:'今日无后续课程';classPhase.textContent=info.phase;renderCourses(info);"
"conn.textContent='正常';conn.className='value ok';link.textContent='http://'+location.host+'/';clock.textContent=s.time&&s.time.synced?s.time.local:'等待 SNTP';clock.className='compact-value '+(s.time&&s.time.synced?'ok':'warn');temp.textContent=s.sensors.temperature_c.toFixed(1)+' C';hum.textContent=s.sensors.humidity+'%';co2.textContent=s.sensors.co2_ppm+' ppm';pm25.textContent=s.sensors.pm25_ugm3+' ug/m3';lux.textContent=s.sensors.ambient_light_lux+' lux'+(s.sensors.ambient_light_fallback?' fallback':'');noise.textContent=s.sensors.noise_dba.toFixed(1)+' dBA';occ.textContent=s.sensors.occupancy_count;pushEnvSample(s);drawEnvChart();health.textContent=Math.round((s.risk.comfort_score+s.risk.air_quality_score+s.risk.safety_score)/3);risk.textContent=labelCondition(s.risk.level);sched.textContent=s.time&&s.time.synced?'调度运行':'等待时间';"
"actLight.textContent=s.actuators.light_power?'开启':'关闭';actLight.className='value '+(s.actuators.light_power?'act-on':'act-off');actFan.textContent=s.actuators.fan_power?'开启':'关闭';actFan.className='value '+(s.actuators.fan_power?'act-on':'act-off');actAlarm.textContent=s.actuators.alarm_power?'告警':'正常';actAlarm.className='value '+(s.actuators.alarm_power?'bad-text':'act-off');actBuzzer.textContent=s.actuators.buzzer_power?'响铃':'静音';actBuzzer.className='value '+(s.actuators.buzzer_power?'bad-text':'act-off');"
"const condition=getAiCondition(s);aiHeadline.textContent=labelCondition(condition);aiBadge.textContent=labelAiStatus(s.ai.status);aiBadge.className='pill '+statusClass(s.ai.status);aiStatus.textContent=labelAiStatus(s.ai.status);aiHttp.textContent=s.ai.http_status;aiConditionText.textContent=labelCondition(condition);aiAction.textContent=formatAction(s.ai.action,s);aiReason.textContent=s.ai.summary||s.ai.reason||'--';aiRaw.textContent=s.ai.result||'';renderEvents(s,info);raw.textContent=JSON.stringify(s,null,2);return s;}catch(e){conn.textContent='失败';conn.className='value warn';return null;}}"
"async function control(name){await fetch('/api/control',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({toggle:name})});refresh();}"
"async function scenario(name){await fetch('/api/control',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({scenario:name})});refresh();}"
"async function runAiTest(){aiHeadline.textContent='运行中';aiBadge.textContent='分析中';aiBadge.className='pill run';aiStatus.textContent='分析中';aiRaw.textContent='running...';try{await fetch('/api/ai/analyze',{method:'POST'});}catch(e){aiHeadline.textContent='请求失败';aiBadge.textContent='请求失败';aiBadge.className='pill bad';aiRaw.textContent='请求失败: '+e;return;}for(let i=0;i<45;i++){await sleep(1000);const s=await refresh();if(s&&s.ai.status!=='running'){if(s.ai.status==='llm_ok'&&s.ai.summary){alert(s.ai.summary);}break;}}}"
"loadSchedule().then(refresh);setInterval(refresh,1000);"
"</script></body></html>";

static const char SCHEDULE_HTML[] =
"<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>教室管家课程表</title><style>"
":root{font-family:Arial,sans-serif;color:#18212f;background:#eef3f7}body{margin:0;padding:18px}.wrap{max-width:1080px;margin:auto}.top{display:flex;justify-content:space-between;gap:12px;align-items:flex-start}.top h1{margin:0;font-size:30px}.card,.hero{background:#fff;border:1px solid #d8e0e8;border-radius:8px;padding:14px;box-shadow:0 1px 2px #0001;margin-top:12px}.hero{display:grid;grid-template-columns:1fr 1fr;gap:12px}.label{font-size:13px;color:#5b6778}.big{font-size:26px;font-weight:700;margin-top:6px}.row{display:flex;gap:9px;flex-wrap:wrap}button{height:40px;border:0;border-radius:6px;background:#1769aa;color:#fff;padding:0 14px;font-weight:700}button.secondary{background:#384250}button.delete{background:#a43f3f}button.small{height:34px;padding:0 10px}button:disabled{background:#9ca3af}.status{font-weight:700;color:#0b7a45}.countdown{font-size:16px;font-weight:700;color:#1769aa;margin-top:8px}.tools{margin-top:10px;align-items:center}.rules{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:10px}.rule{border:1px solid #e1e7ee;background:#f8fafc;border-radius:8px;padding:10px}.timeline{display:grid;gap:8px;margin-top:10px}.timeline-item{border-left:4px solid #cbd5e1;background:#f8fafc;border-radius:6px;padding:9px}.timeline-item.active{border-left-color:#1769aa;background:#eef6ff}table{width:100%;border-collapse:collapse;margin-top:10px}tr.active-row{background:#eef6ff}tr.active-row td:first-child{border-left:4px solid #1769aa}th,td{border-bottom:1px solid #e5e7eb;padding:9px;text-align:left;font-size:14px}input,select{width:100%;box-sizing:border-box;height:34px;border:1px solid #cbd5e1;border-radius:6px;padding:0 8px;background:#fff}input:disabled,select:disabled{border:0;background:transparent;color:#18212f;padding:0}.timebox{display:grid;grid-template-columns:1fr 1fr;gap:6px}@media(max-width:760px){.hero{grid-template-columns:1fr}table,thead,tbody,tr,th,td{display:block}thead{display:none}td{border-bottom:0;padding:5px 0}tr{border-bottom:1px solid #e5e7eb;padding:8px 0}}</style></head>"
"<body><div class=\"wrap\"><div class=\"top\"><div><h1>教室管家课程表</h1><div class=\"label\">学生查看课程，管理员维护自动调度</div></div><div class=\"row\"><button class=\"secondary\" onclick=\"location.href='/'\">返回首页</button><button id=\"editBtn\" onclick=\"adminEdit()\">管理员编辑</button><button id=\"saveBtn\" onclick=\"saveSchedule()\" disabled>保存</button><button class=\"secondary\" onclick=\"loadSchedule()\">刷新</button></div></div>"
"<section class=\"hero\"><div><div class=\"label\">今日状态</div><div id=\"todayState\" class=\"big\">--</div><div class=\"label\" id=\"todayHint\" style=\"margin-top:8px\">--</div><div id=\"nextCountdown\" class=\"countdown\">--</div></div><div><div class=\"label\">自动策略</div><div class=\"rules\"><div class=\"rule\"><b>课前5分钟</b><div class=\"label\">本地判断开灯，并触发 AI 分析</div></div><div class=\"rule\"><b>课后5分钟</b><div class=\"label\">无人时自动关灯</div></div></div></div></section>"
"<div class=\"card\"><div class=\"row\"><div class=\"status\" id=\"mode\">只读查看</div><div class=\"label\" id=\"msg\"></div></div><div class=\"row tools\"><button id=\"addBtn\" class=\"secondary\" onclick=\"addCourse()\" disabled>新增课程</button><div class=\"label\">最多 8 条课程；管理员模式下可删除课程并套用节次模板。</div></div><div id=\"todayList\" class=\"timeline\"></div></div>"
"<div class=\"card\"><table><thead><tr><th>启用</th><th>星期</th><th>时间</th><th>课程</th><th>教室</th><th>快捷节次</th><th>自动策略</th><th>操作</th></tr></thead><tbody id=\"rows\"></tbody></table></div></div>"
"<script>"
"const MAX_ITEMS=8;const days=['未设置','周一','周二','周三','周四','周五','周六','周日'];const periods=[{label:'选择节次',start:'',end:''},{label:'第1节 08:00-09:40',start:'08:00',end:'09:40'},{label:'第2节 10:00-11:40',start:'10:00',end:'11:40'},{label:'第3节 14:00-15:40',start:'14:00',end:'15:40'},{label:'第4节 16:00-17:40',start:'16:00',end:'17:40'},{label:'晚课 19:00-20:40',start:'19:00',end:'20:40'}];let items=[];let editing=false;let admin={username:'',password:''};"
"function esc(s){return String(s||'').replace(/[&<>']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;',\"'\":'&#39;'}[c]));}function dayNo(){const d=new Date().getDay();return d===0?7:d;}function minOf(t){return Number(t.slice(0,2))*60+Number(t.slice(3,5));}function isBlank(it){return !it.enabled&&Number(it.day)===0&&it.start==='00:00'&&it.end==='00:00'&&!it.name&&!it.room;}function compact(list){return list.filter(it=>!isBlank(it)).slice(0,MAX_ITEMS);}function blankCourse(){return {day:dayNo(),start:'08:00',end:'09:40',name:'新课程',room:'A301',enabled:true};}function fmtSec(sec){sec=Math.max(0,Math.floor(sec));const h=Math.floor(sec/3600),m=Math.floor(sec%3600/60),s=sec%60;return h?`${h}小时${m}分`:`${m}分${String(s).padStart(2,'0')}秒`;}"
"function todayInfo(){const now=new Date();const nowSec=now.getHours()*3600+now.getMinutes()*60+now.getSeconds();const today=items.filter(x=>x.enabled&&x.day===dayNo()).sort((a,b)=>minOf(a.start)-minOf(b.start));let current=null,next=null;for(const it of today){const st=minOf(it.start)*60,en=minOf(it.end)*60;if(nowSec>=st&&nowSec<=en)current=it;if(!next&&nowSec<st)next=it;}return {now,nowSec,today,current,next};}"
"function daySelect(v,i){let h=`<select ${editing?'':'disabled'} onchange='items[${i}].day=Number(this.value)'>`;for(let d=0;d<days.length;d++){h+=`<option value='${d}' ${Number(v)===d?'selected':''}>${days[d]}</option>`;}return h+'</select>';}"
"function periodSelect(i){let h=`<select ${editing?'':'disabled'} onchange='applyPeriod(${i},this.value)'>`;periods.forEach((p,idx)=>h+=`<option value='${idx}'>${p.label}</option>`);return h+'</select>';}"
"function renderToday(){const info=todayInfo();const {today,current,next,nowSec}=info;todayState.textContent=current?'正在上课':(next?'等待下一节':'今日课程结束');todayHint.textContent=current?`${current.name} ${current.start}-${current.end} · ${current.room}`:(next?`${next.name} 将在 ${next.start} 开始`:'当前没有后续课程');if(current)nextCountdown.textContent=`距离下课 ${fmtSec(minOf(current.end)*60-nowSec)}`;else if(next)nextCountdown.textContent=`距离下一节 ${fmtSec(minOf(next.start)*60-nowSec)}`;else nextCountdown.textContent='今日自动调度已结束';todayList.innerHTML=today.length?today.map(it=>{const active=current===it?' active':'';return `<div class='timeline-item${active}'><b>${it.start}-${it.end}</b> ${esc(it.name)}<div class='label'>${esc(it.room||'A301')} · 课前准备 + 课后节能</div></div>`;}).join(''):'<div class=timeline-item>今日没有已启用课程</div>';return info;}"
"function render(){rows.innerHTML='';const info=renderToday();if(!items.length)rows.innerHTML='<tr><td colspan=8><span class=label>暂无课程，请进入管理员编辑后新增。</span></td></tr>';items.forEach((it,i)=>{const tr=document.createElement('tr');if(info.current===it)tr.className='active-row';tr.innerHTML=`<td><input type='checkbox' ${it.enabled?'checked':''} ${editing?'':'disabled'} onchange='items[${i}].enabled=this.checked'></td><td>${daySelect(it.day,i)}</td><td><div class='timebox'><input type='time' value='${esc(it.start)}' ${editing?'':'disabled'} onchange='items[${i}].start=this.value'><input type='time' value='${esc(it.end)}' ${editing?'':'disabled'} onchange='items[${i}].end=this.value'></div></td><td><input value='${esc(it.name)}' ${editing?'':'disabled'} onchange='items[${i}].name=this.value'></td><td><input value='${esc(it.room)}' ${editing?'':'disabled'} onchange='items[${i}].room=this.value'></td><td>${periodSelect(i)}</td><td><span class='label'>课前5分钟开灯+AI，课后无人关灯</span></td><td><button class='delete small' ${editing?'':'disabled'} onclick='deleteCourse(${i})'>删除</button></td>`;rows.appendChild(tr);});mode.textContent=editing?'管理员编辑':'只读查看';saveBtn.disabled=!editing;addBtn.disabled=!editing||items.length>=MAX_ITEMS;msg.textContent=editing?`已进入编辑模式，还可新增 ${MAX_ITEMS-items.length} 条`:'';}"
"async function loadSchedule(){try{const r=await fetch('/api/schedule');const data=await r.json();items=compact(data.items||[]);}catch(e){items=[];}editing=false;msg.textContent='';render();}"
"function adminEdit(){const u=prompt('管理员用户名');if(u===null)return;const p=prompt('管理员密码');if(p===null)return;if(u!=='admin'||p!=='admin'){alert('管理员身份验证失败');return;}admin={username:u,password:p};editing=true;msg.textContent='已进入编辑模式';render();}"
"function addCourse(){if(!editing)return;if(items.length>=MAX_ITEMS){alert('最多只能保存 8 条课程');return;}items.push(blankCourse());render();}function deleteCourse(i){if(!editing)return;if(!confirm('删除这节课程？'))return;items.splice(i,1);render();}function applyPeriod(i,v){if(!editing||Number(v)===0)return;const p=periods[Number(v)];items[i].start=p.start;items[i].end=p.end;render();}"
"async function saveSchedule(){const r=await fetch('/api/schedule',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:admin.username,password:admin.password,items:items.slice(0,MAX_ITEMS)})});const data=await r.json();if(!data.ok){alert(data.error||'保存失败');return;}alert('课程表已保存');editing=false;loadSchedule();}"
"loadSchedule();setInterval(()=>{if(editing)renderToday();else render();},1000);"
"</script></body></html>";

static void json_escape(char *dst, size_t dst_size, const char *src)
{
    size_t j = 0;
    if (dst_size == 0) {
        return;
    }
    for (size_t i = 0; src && src[i] && j + 1 < dst_size; i++) {
        char c = src[i];
        if ((c == '"' || c == '\\') && j + 2 < dst_size) {
            dst[j++] = '\\';
            dst[j++] = c;
        } else if ((c == '\n' || c == '\r' || c == '\t') && j + 2 < dst_size) {
            dst[j++] = ' ';
        } else if ((unsigned char)c >= 0x20) {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
}

static int clamp_int(int value, int min, int max)
{
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

static bool valid_time_text(const char *text)
{
    if (!text || strlen(text) != 5 ||
        text[0] < '0' || text[0] > '2' ||
        text[1] < '0' || text[1] > '9' ||
        text[2] != ':' ||
        text[3] < '0' || text[3] > '5' ||
        text[4] < '0' || text[4] > '9') {
        return false;
    }
    int hour = (text[0] - '0') * 10 + (text[1] - '0');
    return hour <= 23;
}

static int minutes_from_time_text(const char *text)
{
    if (!valid_time_text(text)) {
        return -1;
    }
    return ((text[0] - '0') * 10 + (text[1] - '0')) * 60 +
           ((text[3] - '0') * 10 + (text[4] - '0'));
}

static uint16_t be16(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8) | data[1];
}

static uint16_t le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint8_t sensirion_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xff;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static bool sensirion_word_crc_ok(const uint8_t *data)
{
    return sensirion_crc8(data, 2) == data[2];
}

static bool i2c_probe(uint8_t addr)
{
    if (!s_i2c_ready) {
        return false;
    }
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(SENSOR_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

static esp_err_t i2c_write_cmd16(uint8_t addr, uint16_t cmd)
{
    uint8_t data[2] = {(uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xff)};
    return i2c_master_write_to_device(SENSOR_I2C_PORT, addr, data, sizeof(data), pdMS_TO_TICKS(500));
}

static esp_err_t i2c_read_after_cmd16(uint8_t addr, uint16_t cmd, uint8_t *data, size_t len)
{
    uint8_t cmd_bytes[2] = {(uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xff)};
    return i2c_master_write_read_device(SENSOR_I2C_PORT, addr, cmd_bytes, sizeof(cmd_bytes),
                                        data, len, pdMS_TO_TICKS(500));
}

static void real_sensors_init(void)
{
#if CONFIG_GUI_WEB_TEST_REAL_SENSORS_ENABLE
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = CONFIG_GUI_WEB_TEST_SENSOR_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_GUI_WEB_TEST_SENSOR_I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = SENSOR_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(SENSOR_I2C_PORT, &conf);
    if (err == ESP_OK) {
        err = i2c_driver_install(SENSOR_I2C_PORT, conf.mode, 0, 0, 0);
    }
    s_i2c_ready = err == ESP_OK;
    ESP_LOGI(TAG, "Sensor I2C %s: SDA=GPIO%d SCL=GPIO%d", s_i2c_ready ? "ready" : "failed",
             CONFIG_GUI_WEB_TEST_SENSOR_I2C_SDA_GPIO, CONFIG_GUI_WEB_TEST_SENSOR_I2C_SCL_GPIO);

    if (i2c_probe(SCD40_ADDR)) {
        i2c_write_cmd16(SCD40_ADDR, 0x3F86);
        vTaskDelay(pdMS_TO_TICKS(600));
        s_scd40_ready = i2c_write_cmd16(SCD40_ADDR, 0x21B1) == ESP_OK;
    }
    ESP_LOGI(TAG, "SCD40 %s", s_scd40_ready ? "ready" : "not found");

    if (i2c_probe(AHT20_ADDR)) {
        uint8_t init_cmd[3] = {0xBE, 0x08, 0x00};
        i2c_master_write_to_device(SENSOR_I2C_PORT, AHT20_ADDR, init_cmd, sizeof(init_cmd), pdMS_TO_TICKS(500));
        vTaskDelay(pdMS_TO_TICKS(10));
        s_aht20_ready = true;
    }
    ESP_LOGI(TAG, "AHT20 %s", s_aht20_ready ? "ready" : "not found");

    uart_config_t pms_cfg = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    err = uart_driver_install(PMS_UART_PORT, 512, 0, 0, NULL, 0);
    if (err == ESP_OK) {
        err = uart_param_config(PMS_UART_PORT, &pms_cfg);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(PMS_UART_PORT, CONFIG_GUI_WEB_TEST_PMS_UART_TX_GPIO,
                           CONFIG_GUI_WEB_TEST_PMS_UART_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    s_pms_ready = err == ESP_OK;
    ESP_LOGI(TAG, "PMS5003 UART %s: RX=GPIO%d TX=GPIO%d", s_pms_ready ? "ready" : "failed",
             CONFIG_GUI_WEB_TEST_PMS_UART_RX_GPIO, CONFIG_GUI_WEB_TEST_PMS_UART_TX_GPIO);

    uart_config_t ld_cfg = {
        .baud_rate = 256000,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    err = uart_driver_install(LD2410_UART_PORT, 1024, 0, 0, NULL, 0);
    if (err == ESP_OK) {
        err = uart_param_config(LD2410_UART_PORT, &ld_cfg);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(LD2410_UART_PORT, CONFIG_GUI_WEB_TEST_LD2410_UART_TX_GPIO,
                           CONFIG_GUI_WEB_TEST_LD2410_UART_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (CONFIG_GUI_WEB_TEST_LD2410_OUT_GPIO >= 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << CONFIG_GUI_WEB_TEST_LD2410_OUT_GPIO,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
    }
    s_ld2410_ready = err == ESP_OK;
    ESP_LOGI(TAG, "LD2410 UART %s: RX=GPIO%d TX=GPIO%d OUT=GPIO%d", s_ld2410_ready ? "ready" : "failed",
             CONFIG_GUI_WEB_TEST_LD2410_UART_RX_GPIO, CONFIG_GUI_WEB_TEST_LD2410_UART_TX_GPIO,
             CONFIG_GUI_WEB_TEST_LD2410_OUT_GPIO);

#if CONFIG_GUI_WEB_TEST_INMP441_ENABLE
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 256;
    err = i2s_new_channel(&chan_cfg, NULL, &s_inmp441_rx_chan);
    if (err == ESP_OK) {
        i2s_std_config_t std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_GUI_WEB_TEST_INMP441_SAMPLE_RATE_HZ),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = CONFIG_GUI_WEB_TEST_INMP441_BCLK_GPIO,
                .ws = CONFIG_GUI_WEB_TEST_INMP441_WS_GPIO,
                .dout = I2S_GPIO_UNUSED,
                .din = CONFIG_GUI_WEB_TEST_INMP441_DIN_GPIO,
            },
        };
#if CONFIG_GUI_WEB_TEST_INMP441_RIGHT_CHANNEL
        std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;
#else
        std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
#endif
        err = i2s_channel_init_std_mode(s_inmp441_rx_chan, &std_cfg);
        if (err == ESP_OK) {
            err = i2s_channel_enable(s_inmp441_rx_chan);
        }
    }
    s_inmp441_ready = err == ESP_OK;
    ESP_LOGI(TAG, "INMP441 %s: BCLK=GPIO%d WS=GPIO%d DIN=GPIO%d", s_inmp441_ready ? "ready" : "failed",
             CONFIG_GUI_WEB_TEST_INMP441_BCLK_GPIO, CONFIG_GUI_WEB_TEST_INMP441_WS_GPIO,
             CONFIG_GUI_WEB_TEST_INMP441_DIN_GPIO);
#endif
#endif
}

static uint32_t pwm_duty_from_percent(int percent)
{
    percent = clamp_int(percent, 0, 100);
    return (PWM_MAX_DUTY * (uint32_t)percent) / 100U;
}

static void set_pwm_percent(bool ready, ledc_channel_t channel, int percent)
{
    if (!ready) {
        return;
    }
    esp_err_t err = ledc_set_duty(PWM_MODE, channel, pwm_duty_from_percent(percent));
    if (err == ESP_OK) {
        err = ledc_update_duty(PWM_MODE, channel);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PWM update failed for channel %d: %s", channel, esp_err_to_name(err));
    }
}

static void set_output_gpio_level(int gpio_num, bool on)
{
    if (gpio_num < 0) {
        return;
    }
    int active = CONFIG_GUI_WEB_TEST_OUTPUT_ACTIVE_LEVEL ? 1 : 0;
    gpio_set_level(gpio_num, on ? active : !active);
}

static void set_alarm_output(bool on)
{
    set_output_gpio_level(CONFIG_GUI_WEB_TEST_ALARM_GPIO, on);
}

static void set_buzzer_output(bool on)
{
    set_output_gpio_level(CONFIG_GUI_WEB_TEST_BUZZER_GPIO, on);
}

static void apply_hardware_outputs(void)
{
    int light_duty = s_manual_light ? s_light_brightness : 0;
    int fan_duty = s_manual_fan ? s_fan_speed : 0;
    set_pwm_percent(s_light_pwm_ready, LIGHT_PWM_CHANNEL, light_duty);
    set_pwm_percent(s_fan_pwm_ready, FAN_PWM_CHANNEL, fan_duty);
    set_alarm_output(s_alarm_power);
    set_buzzer_output(s_alarm_power);
    ESP_LOGI(TAG, "Hardware outputs: light=%d%% fan=%d%% alarm=%d buzzer=%d",
             light_duty, fan_duty, s_alarm_power, s_alarm_power);
}

static esp_err_t configure_pwm_output(int gpio_num, ledc_channel_t channel, int initial_percent)
{
    if (gpio_num < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    ledc_channel_config_t channel_cfg = {
        .gpio_num = gpio_num,
        .speed_mode = PWM_MODE,
        .channel = channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = PWM_TIMER,
        .duty = pwm_duty_from_percent(initial_percent),
        .hpoint = 0,
        .flags = {
            .output_invert = CONFIG_GUI_WEB_TEST_OUTPUT_ACTIVE_LEVEL ? 0 : 1,
        },
    };
    return ledc_channel_config(&channel_cfg);
}

static void configure_digital_output(int gpio_num, const char *name, bool initial_on)
{
    if (gpio_num < 0) {
        ESP_LOGI(TAG, "%s output disabled", name);
        return;
    }
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    set_output_gpio_level(gpio_num, initial_on);
    ESP_LOGI(TAG, "%s GPIO%d ready", name, gpio_num);
}

static void hardware_outputs_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = PWM_MODE,
        .timer_num = PWM_TIMER,
        .duty_resolution = PWM_RESOLUTION,
        .freq_hz = CONFIG_GUI_WEB_TEST_PWM_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PWM timer init failed: %s", esp_err_to_name(err));
        return;
    }

    err = configure_pwm_output(CONFIG_GUI_WEB_TEST_LIGHT_GPIO, LIGHT_PWM_CHANNEL,
                               s_manual_light ? s_light_brightness : 0);
    s_light_pwm_ready = (err == ESP_OK);
    ESP_LOGI(TAG, "Light PWM GPIO%d %s", CONFIG_GUI_WEB_TEST_LIGHT_GPIO,
             s_light_pwm_ready ? "ready" : "disabled");

    err = configure_pwm_output(CONFIG_GUI_WEB_TEST_FAN_GPIO, FAN_PWM_CHANNEL,
                               s_manual_fan ? s_fan_speed : 0);
    s_fan_pwm_ready = (err == ESP_OK);
    ESP_LOGI(TAG, "Fan PWM GPIO%d %s", CONFIG_GUI_WEB_TEST_FAN_GPIO,
             s_fan_pwm_ready ? "ready" : "disabled");

    configure_digital_output(CONFIG_GUI_WEB_TEST_ALARM_GPIO, "Alarm", s_alarm_power);
    configure_digital_output(CONFIG_GUI_WEB_TEST_BUZZER_GPIO, "Buzzer", s_alarm_power);
}

static const char *risk_from_sensors(void)
{
    if (s_state.occupancy_count > 35 || s_state.noise_x10 > 800 || s_alarm_power) {
        return "Security";
    }
    if (s_state.co2_ppm > 1000 || s_state.pm25_ugm3 > 35 || s_state.temperature_x10 > 300) {
        return "AirQuality";
    }
    if (s_state.occupancy_count == 0 && (s_manual_light || s_manual_fan)) {
        return "EnergySave";
    }
    return "Normal";
}

static void update_scores(void)
{
    strlcpy(s_risk_level, risk_from_sensors(), sizeof(s_risk_level));
    s_state.air_quality_score = clamp_int(100 - (s_state.co2_ppm - 500) / 18 - s_state.pm25_ugm3 / 3, 0, 100);
    s_state.comfort_score = clamp_int(100 - abs(s_state.temperature_x10 - 260) / 3 - abs(s_state.humidity - 55), 0, 100);
    s_state.safety_score = clamp_int(100 - s_state.occupancy_count - (s_state.noise_x10 > 700 ? 20 : 0), 0, 100);
}

static void simulate_sensors(void)
{
    int uptime = (int)(esp_timer_get_time() / 1000000);
    snprintf(s_data_source, sizeof(s_data_source), "simulated:%s", s_demo_scenario);
    s_light_is_fallback = false;
    if (strcmp(s_demo_scenario, "AirQuality") == 0) {
        s_state.temperature_x10 = 305 + (uptime % 10);
        s_state.humidity = 70;
        s_state.co2_ppm = 1350 + ((uptime * 13) % 300);
        s_state.pm25_ugm3 = 55 + (uptime % 20);
        s_state.ambient_light_lux = 160;
        s_state.noise_x10 = 620;
        s_state.occupancy_count = 22;
    } else if (strcmp(s_demo_scenario, "Security") == 0) {
        s_state.temperature_x10 = 282;
        s_state.humidity = 60;
        s_state.co2_ppm = 900;
        s_state.pm25_ugm3 = 25;
        s_state.ambient_light_lux = 80;
        s_state.noise_x10 = 860 + (uptime % 10);
        s_state.occupancy_count = 42;
    } else if (strcmp(s_demo_scenario, "EmptyWaste") == 0) {
        s_state.temperature_x10 = 265;
        s_state.humidity = 56;
        s_state.co2_ppm = 620;
        s_state.pm25_ugm3 = 12;
        s_state.ambient_light_lux = 420;
        s_state.noise_x10 = 410;
        s_state.occupancy_count = 0;
        s_manual_light = 1;
    } else {
        s_state.temperature_x10 = 260 + (uptime % 35);
        s_state.humidity = 48 + (uptime % 18);
        s_state.co2_ppm = 650 + ((uptime * 17) % 260);
        s_state.pm25_ugm3 = 10 + (uptime % 16);
        s_state.ambient_light_lux = 220 + (uptime % 260);
        s_state.noise_x10 = 480 + (uptime % 90);
        s_state.occupancy_count = 8 + (uptime % 12);
    }
    update_scores();
}

static bool scd40_read(float *temperature_c, int *humidity, int *co2_ppm)
{
    if (!s_scd40_ready) {
        return false;
    }
    uint8_t ready[3] = {0};
    if (i2c_read_after_cmd16(SCD40_ADDR, 0xE4B8, ready, sizeof(ready)) != ESP_OK ||
        !sensirion_word_crc_ok(ready) || ((be16(ready) & 0x07ff) == 0)) {
        return false;
    }
    uint8_t data[9] = {0};
    if (i2c_read_after_cmd16(SCD40_ADDR, 0xEC05, data, sizeof(data)) != ESP_OK ||
        !sensirion_word_crc_ok(&data[0]) ||
        !sensirion_word_crc_ok(&data[3]) ||
        !sensirion_word_crc_ok(&data[6])) {
        return false;
    }
    uint16_t temp_raw = be16(&data[3]);
    uint16_t rh_raw = be16(&data[6]);
    *co2_ppm = be16(&data[0]);
    *temperature_c = -45.0f + 175.0f * ((float)temp_raw / 65535.0f);
    *humidity = clamp_int((int)((100.0f * ((float)rh_raw / 65535.0f)) + 0.5f), 0, 100);
    return true;
}

static bool aht20_read(float *temperature_c, int *humidity)
{
    if (!s_aht20_ready) {
        return false;
    }
    uint8_t cmd[3] = {0xAC, 0x33, 0x00};
    if (i2c_master_write_to_device(SENSOR_I2C_PORT, AHT20_ADDR, cmd, sizeof(cmd), pdMS_TO_TICKS(500)) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(90));
    uint8_t data[6] = {0};
    if (i2c_master_read_from_device(SENSOR_I2C_PORT, AHT20_ADDR, data, sizeof(data), pdMS_TO_TICKS(500)) != ESP_OK ||
        (data[0] & 0x80)) {
        return false;
    }
    uint32_t raw_h = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
    uint32_t raw_t = (((uint32_t)data[3] & 0x0f) << 16) | ((uint32_t)data[4] << 8) | data[5];
    *humidity = clamp_int((int)((((float)raw_h * 100.0f) / 1048576.0f) + 0.5f), 0, 100);
    *temperature_c = (((float)raw_t * 200.0f) / 1048576.0f) - 50.0f;
    return true;
}

static bool pms_checksum_ok(const uint8_t *frame)
{
    uint16_t sum = 0;
    for (int i = 0; i < PMS_FRAME_LEN - 2; i++) {
        sum += frame[i];
    }
    return sum == be16(&frame[PMS_FRAME_LEN - 2]);
}

static bool pms5003_read(int *pm25_ugm3)
{
    if (!s_pms_ready) {
        return false;
    }
    uint8_t byte = 0;
    uint8_t frame[PMS_FRAME_LEN] = {0};
    int pos = 0;
    for (int attempts = 0; attempts < 128; attempts++) {
        if (uart_read_bytes(PMS_UART_PORT, &byte, 1, pdMS_TO_TICKS(10)) <= 0) {
            continue;
        }
        if (pos == 0 && byte != 0x42) {
            continue;
        }
        if (pos == 1 && byte != 0x4D) {
            pos = 0;
            continue;
        }
        frame[pos++] = byte;
        if (pos == PMS_FRAME_LEN) {
            if (be16(&frame[2]) == 28 && pms_checksum_ok(frame)) {
                *pm25_ugm3 = be16(&frame[12]);
                return true;
            }
            pos = 0;
        }
    }
    return false;
}

static bool ld2410_read(int *occupancy_count)
{
    bool detected = false;
    if (CONFIG_GUI_WEB_TEST_LD2410_OUT_GPIO >= 0 && gpio_get_level(CONFIG_GUI_WEB_TEST_LD2410_OUT_GPIO)) {
        detected = true;
    }
    if (s_ld2410_ready) {
        uint8_t rx[160] = {0};
        int len = uart_read_bytes(LD2410_UART_PORT, rx, sizeof(rx), pdMS_TO_TICKS(60));
        for (int i = 0; i + 19 < len; i++) {
            if (rx[i] == 0xF4 && rx[i + 1] == 0xF3 && rx[i + 2] == 0xF2 && rx[i + 3] == 0xF1) {
                uint16_t data_len = le16(&rx[i + 4]);
                int total_len = (int)data_len + 10;
                if (i + total_len <= len && rx[i + total_len - 4] == 0xF8 &&
                    rx[i + total_len - 3] == 0xF7 && rx[i + total_len - 2] == 0xF6 &&
                    rx[i + total_len - 1] == 0xF5) {
                    const uint8_t *payload = &rx[i + 6];
                    if (payload[0] == 0x02 && payload[1] == 0xAA) {
                        detected = payload[2] != 0;
                        break;
                    }
                }
            }
        }
    }
    *occupancy_count = detected ? 1 : 0;
    return true;
}

static bool inmp441_read_noise(int *noise_x10)
{
#if CONFIG_GUI_WEB_TEST_INMP441_ENABLE
    if (!s_inmp441_ready || !s_inmp441_rx_chan) {
        return false;
    }
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(s_inmp441_rx_chan, s_inmp441_samples,
                                     sizeof(s_inmp441_samples), &bytes_read, 200);
    if (err != ESP_OK || bytes_read < sizeof(int32_t) * 32) {
        return false;
    }
    size_t count = bytes_read / sizeof(int32_t);
    int64_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum += (int32_t)(s_inmp441_samples[i] >> 8);
    }
    int32_t mean = (int32_t)(sum / (int64_t)count);
    int64_t sum_squares = 0;
    for (size_t i = 0; i < count; i++) {
        int64_t sample = (int32_t)(s_inmp441_samples[i] >> 8);
        int64_t centered = sample - mean;
        sum_squares += centered * centered;
    }
    double rms = sqrt((double)sum_squares / (double)count);
    if (rms < 8.0) {
        return false;
    }
    double dbfs = 20.0 * log10(rms / 8388608.0);
    double dba = dbfs + (double)CONFIG_GUI_WEB_TEST_INMP441_DBFS_TO_DBA_OFFSET;
    if (!isfinite(dba)) {
        return false;
    }
    dba = (double)clamp_int((int)(dba + 0.5), 20, 120);
    if (s_inmp441_has_smoothed_dba) {
        dba = ((double)s_inmp441_smoothed_dba * 0.75) + (dba * 0.25);
    } else {
        s_inmp441_has_smoothed_dba = true;
    }
    s_inmp441_smoothed_dba = (float)dba;
    *noise_x10 = (int)(s_inmp441_smoothed_dba * 10.0f);
    return true;
#else
    return false;
#endif
}

static void read_real_sensors(void)
{
    bool got_temp_humidity = false;
    bool got_co2 = false;
    bool got_pm25 = false;
    bool got_occupancy = false;
    bool got_noise = false;
    float temp_c = s_state.temperature_x10 / 10.0f;
    int humidity = s_state.humidity;
    int co2 = s_state.co2_ppm;
    bool got_scd40 = scd40_read(&temp_c, &humidity, &co2);
    if (got_scd40) {
        s_state.temperature_x10 = (int)(temp_c * 10.0f);
        s_state.humidity = humidity;
        s_state.co2_ppm = co2;
        got_temp_humidity = true;
        got_co2 = true;
    } else if (aht20_read(&temp_c, &humidity)) {
        s_state.temperature_x10 = (int)(temp_c * 10.0f);
        s_state.humidity = humidity;
        got_temp_humidity = true;
    }
    int pm25 = s_state.pm25_ugm3;
    if (pms5003_read(&pm25)) {
        s_state.pm25_ugm3 = pm25;
        got_pm25 = true;
    }
    int occ = s_state.occupancy_count;
    if (ld2410_read(&occ)) {
        s_state.occupancy_count = occ;
        got_occupancy = true;
    }
    int noise = s_state.noise_x10;
    if (inmp441_read_noise(&noise)) {
        s_state.noise_x10 = noise;
        got_noise = true;
    }
    s_state.ambient_light_lux = 460;
    s_light_is_fallback = true;
    snprintf(s_data_source, sizeof(s_data_source), "hardware T/RH=%d CO2=%d PM25=%d OCC=%d Noise=%d Light=fallback",
             got_temp_humidity, got_co2, got_pm25, got_occupancy, got_noise);
    update_scores();
}

static bool json_bool_value(const char *json, const char *key, bool *out)
{
    char pattern[32];
    char escaped_pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    snprintf(escaped_pattern, sizeof(escaped_pattern), "\\\"%s\\\"", key);
    const char *p = strstr(json, pattern);
    if (!p) {
        p = strstr(json, escaped_pattern);
    }
    if (!p) {
        return false;
    }
    p = strchr(p, ':');
    if (!p) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\\' || *p == '"') {
        p++;
    }
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool json_int_value(const char *json, const char *key, int *out)
{
    char pattern[32];
    char escaped_pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    snprintf(escaped_pattern, sizeof(escaped_pattern), "\\\"%s\\\"", key);
    const char *p = strstr(json, pattern);
    if (!p) {
        p = strstr(json, escaped_pattern);
    }
    if (!p) {
        return false;
    }
    p = strchr(p, ':');
    if (!p) {
        return false;
    }
    *out = atoi(p + 1);
    return true;
}

static void apply_action_json(cJSON *actions, const char *fallback_text, int *changed)
{
    bool bval;
    int ival;
    bool got_fan_power = false;
    bool got_fan_speed = false;

    cJSON *item = NULL;
    item = actions ? cJSON_GetObjectItem(actions, "fan_power") : NULL;
    if (cJSON_IsBool(item)) {
        s_manual_fan = cJSON_IsTrue(item);
        got_fan_power = true;
        (*changed)++;
    } else if (fallback_text && json_bool_value(fallback_text, "fan_power", &bval)) {
        s_manual_fan = bval;
        got_fan_power = true;
        (*changed)++;
    }
    item = actions ? cJSON_GetObjectItem(actions, "fan_speed") : NULL;
    if (cJSON_IsNumber(item)) {
        s_fan_speed = clamp_int(item->valueint, 0, 100);
        got_fan_speed = true;
        (*changed)++;
    } else if (fallback_text && json_int_value(fallback_text, "fan_speed", &ival)) {
        s_fan_speed = clamp_int(ival, 0, 100);
        got_fan_speed = true;
        (*changed)++;
    }
    item = actions ? cJSON_GetObjectItem(actions, "alarm_power") : NULL;
    if (cJSON_IsBool(item)) {
        s_alarm_power = cJSON_IsTrue(item);
        (*changed)++;
    } else if (fallback_text && json_bool_value(fallback_text, "alarm_power", &bval)) {
        s_alarm_power = bval;
        (*changed)++;
    }

    if (*changed == 0 && fallback_text) {
        if (strstr(fallback_text, "ventilate") || strstr(fallback_text, "fan") || strstr(fallback_text, "AirQuality")) {
            s_manual_fan = 1;
            s_fan_speed = 85;
            (*changed)++;
        }
        if (strstr(fallback_text, "alarm") || strstr(fallback_text, "Security")) {
            s_alarm_power = 1;
            (*changed)++;
        }
    }

    if (strcmp(s_risk_level, "Security") == 0) {
        s_alarm_power = 1;
    }
    if (got_fan_speed && s_fan_speed > 0) {
        s_manual_fan = 1;
    }
    if (got_fan_power && !s_manual_fan) {
        s_fan_speed = 0;
    }
    if (strcmp(s_risk_level, "AirQuality") == 0 && (!s_manual_fan || s_fan_speed == 0)) {
        s_manual_fan = 1;
        s_fan_speed = 80;
    }
    if (strcmp(s_risk_level, "EnergySave") == 0) {
        s_manual_fan = 0;
        s_fan_speed = 0;
    }
}

static void apply_ai_downlink(const char *response)
{
    int changed = 0;
    char content[1536] = {0};
    const char *fallback_text = response;

    cJSON *root = cJSON_Parse(response);
    if (root) {
        cJSON *choices = cJSON_GetObjectItem(root, "choices");
        cJSON *choice0 = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
        cJSON *message = choice0 ? cJSON_GetObjectItem(choice0, "message") : NULL;
        cJSON *content_item = message ? cJSON_GetObjectItem(message, "content") : NULL;
        if (cJSON_IsString(content_item) && content_item->valuestring) {
            snprintf(content, sizeof(content), "%s", content_item->valuestring);
            fallback_text = content;
            cJSON *inner = cJSON_Parse(content_item->valuestring);
            if (inner) {
                cJSON *condition = cJSON_GetObjectItem(inner, "condition");
                cJSON *reason = cJSON_GetObjectItem(inner, "reason");
                cJSON *recommendation = cJSON_GetObjectItem(inner, "recommendation");
                cJSON *summary = cJSON_GetObjectItem(inner, "summary");
                cJSON *actions = cJSON_GetObjectItem(inner, "actions");
                if (cJSON_IsString(summary) && summary->valuestring) {
                    snprintf(s_ai_summary, sizeof(s_ai_summary), "%s", summary->valuestring);
                } else if (cJSON_IsString(recommendation) && recommendation->valuestring) {
                    snprintf(s_ai_summary, sizeof(s_ai_summary), "%s", recommendation->valuestring);
                }
                if (cJSON_IsString(condition) && condition->valuestring) {
                    snprintf(s_ai_reason, sizeof(s_ai_reason), "AI condition=%s. %s",
                             condition->valuestring,
                             cJSON_IsString(reason) && reason->valuestring ? reason->valuestring :
                             (cJSON_IsString(recommendation) && recommendation->valuestring ? recommendation->valuestring : ""));
                }
                apply_action_json(cJSON_IsObject(actions) ? actions : NULL, content_item->valuestring, &changed);
                cJSON_Delete(inner);
            }
        }
        cJSON_Delete(root);
    }

    if (changed == 0) {
        apply_action_json(NULL, fallback_text, &changed);
    }
    snprintf(s_ai_action, sizeof(s_ai_action), "AI执行%d项：通风%s，报警%s；照明由本地策略%s",
             changed,
             s_manual_fan ? "开启" : "关闭",
             s_alarm_power ? "开启" : "关闭",
             s_manual_light ? "开启" : "关闭");
    apply_hardware_outputs();
}

static esp_err_t outbound_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        http_capture_t *capture = (http_capture_t *)evt->user_data;
        if (capture && capture->buf && capture->len < capture->max_len - 1) {
            int copy_len = evt->data_len;
            int space = capture->max_len - capture->len - 1;
            if (copy_len > space) {
                copy_len = space;
            }
            memcpy(capture->buf + capture->len, evt->data, copy_len);
            capture->len += copy_len;
            capture->buf[capture->len] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t run_connectivity_get(void)
{
    char response[320] = {0};
    http_capture_t capture = {
        .buf = response,
        .max_len = sizeof(response),
    };
    esp_http_client_config_t config = {
        .url = CONFIG_GUI_WEB_TEST_CONNECTIVITY_URL,
        .event_handler = outbound_http_event_handler,
        .user_data = &capture,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Testing HTTPS connectivity: %s", CONFIG_GUI_WEB_TEST_CONNECTIVITY_URL);
    esp_err_t err = esp_http_client_perform(client);
    s_last_http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK) {
        snprintf(s_ai_status, sizeof(s_ai_status), "internet_ok");
        snprintf(s_ai_result, sizeof(s_ai_result), "GET %s succeeded. Response: %.260s",
                 CONFIG_GUI_WEB_TEST_CONNECTIVITY_URL, response);
    } else {
        snprintf(s_ai_status, sizeof(s_ai_status), "internet_fail");
        snprintf(s_ai_result, sizeof(s_ai_result), "GET %s failed: %s",
                 CONFIG_GUI_WEB_TEST_CONNECTIVITY_URL, esp_err_to_name(err));
    }
    s_last_http_err = err;
    return err;
}

static esp_err_t run_llm_post(void)
{
    char response[4096] = {0};
    char auth_header[192];
    char post_data[2800];
    http_capture_t capture = {
        .buf = response,
        .max_len = sizeof(response),
    };
    esp_http_client_config_t config = {
        .url = CONFIG_GUI_WEB_TEST_LLM_API_URL,
        .event_handler = outbound_http_event_handler,
        .user_data = &capture,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    if (strcmp(s_demo_scenario, "Hardware") == 0) {
        read_real_sensors();
    } else {
        simulate_sensors();
    }

    snprintf(post_data, sizeof(post_data),
             "{\"model\":\"%s\",\"messages\":["
             "{\"role\":\"system\",\"content\":\"You are an AIoT device controller. Return only compact JSON. No markdown. Decide only fan_power, fan_speed and alarm_power from the provided measurements. The buzzer follows alarm_power. Light is controlled by local classroom schedule logic and must not be changed by AI. Use Simplified Chinese for summary, reason and recommendation. The summary must be a natural Chinese environment status sentence for a browser alert, within 90 Chinese characters. For poor air, enable fan_power and use fan_speed at least 80. For possible intrusion or human presence alert, enable alarm_power. For low-need energy saving, turn fan off. Always include every action field.\"},"
             "{\"role\":\"user\",\"content\":\"Data source=%s. Measurements: temp %.1f C, humidity %d%%, CO2 %d ppm, PM2.5 %d ug/m3, ambient_light %d lux (%s), noise %.1f dBA, occupancy %d. Current actuators: light=%s brightness=%d fan=%s speed=%d alarm=%s. Return JSON: {\\\"condition\\\":\\\"normal|energy_save|air_quality|security\\\",\\\"confidence\\\":0-100,\\\"summary\\\":\\\"中文环境状态弹窗提示\\\",\\\"reason\\\":\\\"中文原因\\\",\\\"recommendation\\\":\\\"中文建议\\\",\\\"actions\\\":{\\\"fan_power\\\":true|false,\\\"fan_speed\\\":0-100,\\\"alarm_power\\\":true|false}}.\"}"
             "],\"stream\":false,\"temperature\":0.2}",
             CONFIG_GUI_WEB_TEST_LLM_MODEL,
             s_data_source,
             s_state.temperature_x10 / 10.0, s_state.humidity, s_state.co2_ppm, s_state.pm25_ugm3,
             s_state.ambient_light_lux, s_light_is_fallback ? "fallback-no-real-light-sensor" : "real",
             s_state.noise_x10 / 10.0, s_state.occupancy_count,
             s_manual_light ? "on" : "off", s_light_brightness,
             s_manual_fan ? "on" : "off", s_fan_speed,
             s_alarm_power ? "on" : "off");
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", CONFIG_GUI_WEB_TEST_LLM_API_KEY);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    ESP_LOGI(TAG, "Testing LLM API: %s", CONFIG_GUI_WEB_TEST_LLM_API_URL);
    esp_err_t err = esp_http_client_perform(client);
    s_last_http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && s_last_http_status >= 200 && s_last_http_status < 300) {
        snprintf(s_ai_status, sizeof(s_ai_status), "llm_ok");
        snprintf(s_ai_result, sizeof(s_ai_result), "%.1100s", response);
        snprintf(s_ai_reason, sizeof(s_ai_reason), "DeepSeek returned HTTP 200; parsing structured downlink.");
        snprintf(s_ai_summary, sizeof(s_ai_summary), "AI 正在解析当前环境状态。");
        apply_ai_downlink(response);
    } else {
        snprintf(s_ai_status, sizeof(s_ai_status), "llm_fail");
        snprintf(s_ai_result, sizeof(s_ai_result), "POST LLM failed: err=%s status=%d response=%.650s",
                 esp_err_to_name(err), s_last_http_status, response);
        snprintf(s_ai_reason, sizeof(s_ai_reason), "LLM request failed; keeping local actuator state.");
        snprintf(s_ai_summary, sizeof(s_ai_summary), "AI 分析失败，请检查网络或 API 配置。");
    }
    s_last_http_err = err;
    return err;
}

static esp_err_t run_network_or_ai_test(void)
{
    if (strlen(CONFIG_GUI_WEB_TEST_LLM_API_URL) > 0 && strlen(CONFIG_GUI_WEB_TEST_LLM_API_KEY) > 0) {
        return run_llm_post();
    }
    return run_connectivity_get();
}

static void ai_test_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "AI/network test task started");
    run_network_or_ai_test();
    ESP_LOGI(TAG, "AI/network test task finished: status=%s http=%d err=%s",
             s_ai_status, s_last_http_status, esp_err_to_name(s_last_http_err));
    s_ai_task_handle = NULL;
    vTaskDelete(NULL);
}

static bool start_ai_analysis_task(const char *trigger)
{
    if (s_ai_task_handle) {
        ESP_LOGW(TAG, "AI analysis skipped, already running. trigger=%s", trigger ? trigger : "unknown");
        return false;
    }

    s_ai_runs++;
    s_last_http_status = 0;
    s_last_http_err = ESP_OK;
    snprintf(s_ai_status, sizeof(s_ai_status), "running");
    snprintf(s_ai_result, sizeof(s_ai_result), "Running HTTPS request in background task. trigger=%s",
             trigger ? trigger : "manual");
    snprintf(s_ai_reason, sizeof(s_ai_reason), "AI analysis requested by %s.", trigger ? trigger : "manual");
    snprintf(s_ai_summary, sizeof(s_ai_summary), "AI 正在分析当前环境，请稍候。");

    BaseType_t ret = xTaskCreate(ai_test_task, "ai_test", 12288, NULL, 5, &s_ai_task_handle);
    if (ret != pdPASS) {
        s_ai_task_handle = NULL;
        snprintf(s_ai_status, sizeof(s_ai_status), "task_fail");
        snprintf(s_ai_result, sizeof(s_ai_result), "Failed to create AI test task");
        snprintf(s_ai_summary, sizeof(s_ai_summary), "AI 分析任务创建失败。");
        return false;
    }
    return true;
}

static void schedule_reset_trigger_keys(void)
{
    memset(s_schedule_pre_trigger_key, 0, sizeof(s_schedule_pre_trigger_key));
    memset(s_schedule_post_trigger_key, 0, sizeof(s_schedule_post_trigger_key));
}

static bool get_local_time(struct tm *tm_now)
{
    time_t now = 0;
    time(&now);
    if (now < 1704067200) {
        return false;
    }
    localtime_r(&now, tm_now);
    return true;
}

static int schedule_day_from_tm(const struct tm *tm_now)
{
    return tm_now->tm_wday == 0 ? 7 : tm_now->tm_wday;
}

static int schedule_trigger_key(const struct tm *tm_now, int index)
{
    return (((tm_now->tm_year + 1900) * 1000) + tm_now->tm_yday) * 100 + index + 1;
}

static void time_sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    struct tm tm_now;
    char text[32] = {0};
    s_time_synced = true;
    if (get_local_time(&tm_now)) {
        strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &tm_now);
        ESP_LOGI(TAG, "SNTP time synced, Beijing time: %s", text);
    } else {
        ESP_LOGI(TAG, "SNTP time synced");
    }
}

static void initialize_time_sync(void)
{
    if (s_time_sync_started) {
        return;
    }

    setenv("TZ", "CST-8", 1);
    tzset();
    s_time_sync_started = true;

    if (!esp_sntp_enabled()) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
        esp_sntp_init();
    }
    ESP_LOGI(TAG, "SNTP started, timezone=Asia/Shanghai (UTC+8), server=ntp.aliyun.com");
}

static void refresh_environment_state_for_schedule(void)
{
    if (strcmp(s_demo_scenario, "Hardware") == 0) {
        read_real_sensors();
    } else {
        simulate_sensors();
    }
    update_scores();
}

static void schedule_preclass_action(const schedule_item_t *item, int index)
{
    bool light_needed;

    refresh_environment_state_for_schedule();
    light_needed = s_light_is_fallback || s_state.ambient_light_lux < CLASSROOM_LIGHT_THRESHOLD_LUX;
    if (light_needed) {
        s_manual_light = 1;
        if (s_light_brightness < CLASSROOM_PRECLASS_BRIGHTNESS) {
            s_light_brightness = CLASSROOM_PRECLASS_BRIGHTNESS;
        }
    }
    update_scores();
    apply_hardware_outputs();

    bool ai_started = start_ai_analysis_task("schedule_preclass");
    snprintf(s_schedule_status, sizeof(s_schedule_status),
             "课前触发: %s %s-%s %s, 灯光%s, AI%s。",
             item->room, item->start, item->end, item->name,
             light_needed ? "已按本地策略开启" : "环境亮度足够未开启",
             ai_started ? "已启动" : "未启动/已有任务运行");
    ESP_LOGI(TAG, "Schedule pre-class action #%d: %s", index, s_schedule_status);
}

static void schedule_postclass_action(const schedule_item_t *item, int index)
{
    refresh_environment_state_for_schedule();
    if (s_state.occupancy_count == 0) {
        s_manual_light = 0;
        apply_hardware_outputs();
        snprintf(s_schedule_status, sizeof(s_schedule_status),
                 "课后触发: %s %s 已下课 5 分钟, occupancy=0, 已自动关灯。",
                 item->name, item->end);
    } else {
        snprintf(s_schedule_status, sizeof(s_schedule_status),
                 "课后触发: %s %s 已下课 5 分钟, occupancy=%d, 保持灯光状态。",
                 item->name, item->end, s_state.occupancy_count);
    }
    ESP_LOGI(TAG, "Schedule post-class action #%d: %s", index, s_schedule_status);
}

static void schedule_task(void *arg)
{
    (void)arg;
    while (true) {
        struct tm tm_now;
        if (!get_local_time(&tm_now)) {
            snprintf(s_schedule_status, sizeof(s_schedule_status), "课程表调度等待 SNTP 北京时间同步。");
            vTaskDelay(pdMS_TO_TICKS(SCHEDULE_CHECK_INTERVAL_MS));
            continue;
        }

        s_time_synced = true;
        int today = schedule_day_from_tm(&tm_now);
        int now_min = tm_now.tm_hour * 60 + tm_now.tm_min;
        for (int i = 0; i < MAX_SCHEDULE_ITEMS; i++) {
            schedule_item_t *item = &s_schedule[i];
            if (!item->enabled || item->day != today) {
                continue;
            }

            int start_min = minutes_from_time_text(item->start);
            int end_min = minutes_from_time_text(item->end);
            if (start_min < 0 || end_min < 0 || end_min <= start_min) {
                continue;
            }

            int key = schedule_trigger_key(&tm_now, i);
            int pre_min = start_min - SCHEDULE_PRE_CLASS_MINUTES;
            if (pre_min < 0) {
                pre_min = 0;
            }
            if (now_min >= pre_min && now_min <= start_min && s_schedule_pre_trigger_key[i] != key) {
                s_schedule_pre_trigger_key[i] = key;
                schedule_preclass_action(item, i);
            }

            int post_min = end_min + SCHEDULE_POST_CLASS_MINUTES;
            int post_until = post_min + SCHEDULE_POST_WINDOW_MINUTES;
            if (post_min < 24 * 60 && now_min >= post_min && now_min <= post_until &&
                s_schedule_post_trigger_key[i] != key) {
                s_schedule_post_trigger_key[i] = key;
                schedule_postclass_action(item, i);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(SCHEDULE_CHECK_INTERVAL_MS));
    }
}

static void start_schedule_task(void)
{
    if (s_schedule_task_handle) {
        return;
    }
    BaseType_t ret = xTaskCreate(schedule_task, "schedule", 4096, NULL, 4, &s_schedule_task_handle);
    if (ret != pdPASS) {
        s_schedule_task_handle = NULL;
        snprintf(s_schedule_status, sizeof(s_schedule_status), "课程表调度任务创建失败。");
        ESP_LOGE(TAG, "Failed to create schedule task");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < 8) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "Retrying Wi-Fi connection");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_addr, sizeof(s_ip_addr), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip_addr);
        ESP_LOGI(TAG, "Open GUI: http://%s/", s_ip_addr);
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "Phone/client connected, AID=%d", event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "Phone/client disconnected, AID=%d", event->aid);
    }
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t schedule_page_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    return httpd_resp_send(req, SCHEDULE_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t schedule_get_handler(httpd_req_t *req)
{
    char chunk[256];
    char escaped_name[80];
    char escaped_room[64];

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, "{\"items\":["), TAG, "schedule chunk failed");

    for (int i = 0; i < MAX_SCHEDULE_ITEMS; i++) {
        json_escape(escaped_name, sizeof(escaped_name), s_schedule[i].name);
        json_escape(escaped_room, sizeof(escaped_room), s_schedule[i].room);
        snprintf(chunk, sizeof(chunk),
                 "%s{\"day\":%d,\"start\":\"%s\",\"end\":\"%s\",\"name\":\"%s\",\"room\":\"%s\",\"enabled\":%s}",
                 i == 0 ? "" : ",", s_schedule[i].day, s_schedule[i].start, s_schedule[i].end,
                 escaped_name, escaped_room, s_schedule[i].enabled ? "true" : "false");
        ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, chunk), TAG, "schedule chunk failed");
    }

    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, "]}"), TAG, "schedule chunk failed");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t schedule_post_handler(httpd_req_t *req)
{
    char body[4096] = {0};
    int remaining = req->content_len;
    int received = 0;

    if (remaining <= 0 || remaining >= (int)sizeof(body)) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"payload_too_large\"}");
    }
    while (remaining > 0) {
        int ret = httpd_req_recv(req, body + received, remaining);
        if (ret <= 0) {
            return ret;
        }
        received += ret;
        remaining -= ret;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid_json\"}");
    }

    cJSON *username = cJSON_GetObjectItem(root, "username");
    cJSON *password = cJSON_GetObjectItem(root, "password");
    if (!cJSON_IsString(username) || !cJSON_IsString(password) ||
        strcmp(username->valuestring, "admin") != 0 || strcmp(password->valuestring, "admin") != 0) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"admin_auth_failed\"}");
    }

    cJSON *items = cJSON_GetObjectItem(root, "items");
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"items_required\"}");
    }

    schedule_item_t next[MAX_SCHEDULE_ITEMS] = {0};
    for (int i = 0; i < MAX_SCHEDULE_ITEMS; i++) {
        snprintf(next[i].start, sizeof(next[i].start), "00:00");
        snprintf(next[i].end, sizeof(next[i].end), "00:00");
    }

    int count = cJSON_GetArraySize(items);
    if (count > MAX_SCHEDULE_ITEMS) {
        count = MAX_SCHEDULE_ITEMS;
    }
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(items, i);
        cJSON *day = cJSON_GetObjectItem(item, "day");
        cJSON *start = cJSON_GetObjectItem(item, "start");
        cJSON *end = cJSON_GetObjectItem(item, "end");
        cJSON *name = cJSON_GetObjectItem(item, "name");
        cJSON *room = cJSON_GetObjectItem(item, "room");
        cJSON *enabled = cJSON_GetObjectItem(item, "enabled");

        next[i].day = cJSON_IsNumber(day) ? clamp_int(day->valueint, 0, 7) : 0;
        if (cJSON_IsString(start) && valid_time_text(start->valuestring)) {
            snprintf(next[i].start, sizeof(next[i].start), "%s", start->valuestring);
        }
        if (cJSON_IsString(end) && valid_time_text(end->valuestring)) {
            snprintf(next[i].end, sizeof(next[i].end), "%s", end->valuestring);
        }
        if (cJSON_IsString(name) && name->valuestring) {
            snprintf(next[i].name, sizeof(next[i].name), "%.31s", name->valuestring);
        }
        if (cJSON_IsString(room) && room->valuestring) {
            snprintf(next[i].room, sizeof(next[i].room), "%.23s", room->valuestring);
        }
        next[i].enabled = cJSON_IsBool(enabled) ? cJSON_IsTrue(enabled) : false;
    }

    memcpy(s_schedule, next, sizeof(s_schedule));
    schedule_reset_trigger_keys();
    snprintf(s_schedule_status, sizeof(s_schedule_status), "课程表已更新，今日触发记录已重置。");
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t state_get_handler(httpd_req_t *req)
{
    int uptime = (int)(esp_timer_get_time() / 1000000);
    char escaped_ai_result[512];
    char escaped_ai_reason[320];
    char escaped_ai_action[320];
    char escaped_ai_summary[640];
    char escaped_schedule_status[320];
    char local_time_text[32] = "not_synced";
    char json[3600];
    struct tm tm_now;
    bool time_valid = get_local_time(&tm_now);

    if (strcmp(s_demo_scenario, "Hardware") == 0) {
        read_real_sensors();
    } else {
        simulate_sensors();
    }
    if (time_valid) {
        strftime(local_time_text, sizeof(local_time_text), "%Y-%m-%d %H:%M:%S", &tm_now);
    }
    json_escape(escaped_ai_result, sizeof(escaped_ai_result), s_ai_result);
    json_escape(escaped_ai_reason, sizeof(escaped_ai_reason), s_ai_reason);
    json_escape(escaped_ai_action, sizeof(escaped_ai_action), s_ai_action);
    json_escape(escaped_ai_summary, sizeof(escaped_ai_summary), s_ai_summary);
    json_escape(escaped_schedule_status, sizeof(escaped_schedule_status), s_schedule_status);

    snprintf(json, sizeof(json),
             "{"
             "\"device\":{\"name\":\"教室管家\",\"ip\":\"%s\",\"uptime_s\":%d},"
             "\"time\":{\"synced\":%s,\"sntp_started\":%s,\"timezone\":\"Asia/Shanghai\",\"local\":\"%s\"},"
             "\"sensors\":{\"temperature_c\":%d.%d,\"humidity\":%d,\"co2_ppm\":%d,"
             "\"pm25_ugm3\":%d,\"ambient_light_lux\":%d,\"ambient_light_fallback\":%s,"
             "\"noise_dba\":%d.%d,\"occupancy_count\":%d},"
             "\"risk\":{\"level\":\"%s\",\"comfort_score\":%d,\"air_quality_score\":%d,\"safety_score\":%d,"
             "\"recommendation\":\"Local fusion plus DeepSeek downlink control test\"},"
             "\"actuators\":{\"light_power\":%s,\"light_brightness\":%d,\"fan_power\":%s,\"fan_speed\":%d,\"alarm_power\":%s,\"buzzer_power\":%s},"
             "\"policy\":{\"scenario\":\"%s\",\"data_source\":\"%s\"},"
             "\"schedule\":{\"status\":\"%s\"},"
             "\"ai\":{\"status\":\"%s\",\"runs\":%d,\"http_status\":%d,\"esp_err\":\"%s\","
             "\"action\":\"%s\",\"reason\":\"%s\",\"summary\":\"%s\",\"result\":\"%s\"}"
             "}",
             s_ip_addr, uptime,
             (time_valid || s_time_synced) ? "true" : "false", s_time_sync_started ? "true" : "false", local_time_text,
             s_state.temperature_x10 / 10, s_state.temperature_x10 % 10, s_state.humidity, s_state.co2_ppm,
             s_state.pm25_ugm3, s_state.ambient_light_lux, s_light_is_fallback ? "true" : "false",
             s_state.noise_x10 / 10, s_state.noise_x10 % 10,
             s_state.occupancy_count, s_risk_level,
             s_state.comfort_score, s_state.air_quality_score, s_state.safety_score,
             s_manual_light ? "true" : "false", s_light_brightness,
             s_manual_fan ? "true" : "false", s_fan_speed,
             s_alarm_power ? "true" : "false", s_alarm_power ? "true" : "false", s_demo_scenario, s_data_source,
             escaped_schedule_status,
             s_ai_status, s_ai_runs, s_last_http_status, esp_err_to_name(s_last_http_err),
             escaped_ai_action, escaped_ai_reason, escaped_ai_summary, escaped_ai_result);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t control_post_handler(httpd_req_t *req)
{
    char body[192] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len < 0) {
        return len;
    }
    if (strstr(body, "light")) {
        s_manual_light = !s_manual_light;
    }
    if (strstr(body, "fan")) {
        s_manual_fan = !s_manual_fan;
        s_fan_speed = s_manual_fan ? 70 : 0;
    }
    if (strstr(body, "alarm")) {
        s_alarm_power = !s_alarm_power;
    }
    if (strstr(body, "AirQuality")) {
        strlcpy(s_demo_scenario, "AirQuality", sizeof(s_demo_scenario));
    } else if (strstr(body, "Security")) {
        strlcpy(s_demo_scenario, "Security", sizeof(s_demo_scenario));
    } else if (strstr(body, "EmptyWaste")) {
        strlcpy(s_demo_scenario, "EmptyWaste", sizeof(s_demo_scenario));
    } else if (strstr(body, "Hardware")) {
        strlcpy(s_demo_scenario, "Hardware", sizeof(s_demo_scenario));
    }
    update_scores();
    apply_hardware_outputs();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t ai_post_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    if (s_ai_task_handle) {
        return httpd_resp_sendstr(req, "{\"ok\":true,\"status\":\"already_running\"}");
    }

    if (!start_ai_analysis_task("manual_web")) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"status\":\"task_fail\"}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":true,\"status\":\"started\"}");
}

static esp_err_t start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "Failed to start HTTP server");

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    const httpd_uri_t schedule_page = {
        .uri = "/schedule",
        .method = HTTP_GET,
        .handler = schedule_page_get_handler,
    };
    const httpd_uri_t state = {
        .uri = "/api/state",
        .method = HTTP_GET,
        .handler = state_get_handler,
    };
    const httpd_uri_t schedule_get = {
        .uri = "/api/schedule",
        .method = HTTP_GET,
        .handler = schedule_get_handler,
    };
    const httpd_uri_t schedule_post = {
        .uri = "/api/schedule",
        .method = HTTP_POST,
        .handler = schedule_post_handler,
    };
    const httpd_uri_t control = {
        .uri = "/api/control",
        .method = HTTP_POST,
        .handler = control_post_handler,
    };
    const httpd_uri_t ai = {
        .uri = "/api/ai/analyze",
        .method = HTTP_POST,
        .handler = ai_post_handler,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &schedule_page));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &state));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &schedule_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &schedule_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &control));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ai));
    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

#if CONFIG_GUI_WEB_TEST_WIFI_MODE_SOFTAP
static esp_err_t wifi_init_softap(void)
{
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.ap.ssid, CONFIG_GUI_WEB_TEST_AP_SSID, sizeof(wifi_config.ap.ssid));
    strlcpy((char *)wifi_config.ap.password, CONFIG_GUI_WEB_TEST_AP_PASSWORD, sizeof(wifi_config.ap.password));
    wifi_config.ap.ssid_len = strlen(CONFIG_GUI_WEB_TEST_AP_SSID);
    wifi_config.ap.channel = 6;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    if (strlen(CONFIG_GUI_WEB_TEST_AP_PASSWORD) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started");
    ESP_LOGI(TAG, "SSID: %s", CONFIG_GUI_WEB_TEST_AP_SSID);
    ESP_LOGI(TAG, "Password: %s", CONFIG_GUI_WEB_TEST_AP_PASSWORD);
    ESP_LOGI(TAG, "Open GUI: http://192.168.4.1/");
    return ESP_OK;
}
#endif

#if CONFIG_GUI_WEB_TEST_WIFI_MODE_STA
static esp_err_t wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, CONFIG_GUI_WEB_TEST_STA_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_GUI_WEB_TEST_STA_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Connecting to SSID: %s", CONFIG_GUI_WEB_TEST_STA_SSID);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(30000));
    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Failed to connect to Wi-Fi");
    return ESP_FAIL;
}
#endif

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    hardware_outputs_init();
    real_sensors_init();

#if CONFIG_GUI_WEB_TEST_WIFI_MODE_STA
    ESP_ERROR_CHECK(wifi_init_sta());
    initialize_time_sync();
    start_schedule_task();
#else
    ESP_ERROR_CHECK(wifi_init_softap());
    snprintf(s_schedule_status, sizeof(s_schedule_status),
             "SoftAP 模式未启动 SNTP；课程表自动调度需要 Station 联网。");
#endif

    ESP_ERROR_CHECK(start_web_server());
}
