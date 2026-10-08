// M5Stack PaperS3 — текущая погода и почасовой прогноз на сутки (альбомная ориентация).
// Цикл работы: включение кнопкой Power -> Wi-Fi -> загрузка данных ->
// отрисовка на e-ink -> выключение питания (картинка остаётся на экране).
//
// Данные: Open-Meteo (без API-ключа), местоположение — по IP (ip-api.com).
// Если хотите зафиксировать город, задайте FIXED_LAT/FIXED_LON/FIXED_CITY.

#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "fonts.h"

static const char* WIFI_SSID = "Akinava24";
static const char* WIFI_PASS = "kkn6thfvTqFj";
// Основной роутер сети. Используется, если шлюз/DNS из DHCP не работают.
static const IPAddress FALLBACK_GW(192, 168, 178, 1);

// Оставьте NAN, чтобы определять местоположение автоматически по IP.
static const float FIXED_LAT = NAN;
static const float FIXED_LON = NAN;
static const char* FIXED_CITY = "";

// Почасовой прогноз: 24 часа, начиная с ближайшего прошедшего часа, кратного 3.
// 25 точек — чтобы график заканчивался подписью через сутки.
static const int HOURS = 25;

// HTTPS + JSON не помещаются в стандартные 8 КБ стека loopTask.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

struct DayForecast {
  char date[11];       // YYYY-MM-DD
  int16_t code;
  float tmax, tmin;
  float precip;        // мм
  int16_t precipProb;  // %
  float windMax;       // м/с
};

struct HourForecast {
  char time[17];       // YYYY-MM-DDTHH:MM
  int16_t code;
  bool isDay;
  float temp;
  float precip;        // мм за предыдущий час
  int16_t precipProb;  // %, -1 — нет данных
};

struct WeatherData {
  uint32_t magic;
  char city[48];
  char updated[17];    // YYYY-MM-DDTHH:MM (локальное время)
  float lat, lon;
  float temp, feels;
  int16_t humidity;
  float pressure;      // гПа
  float wind;          // м/с
  int16_t windDir;
  int16_t code;
  bool isDay;
  char sunrise[6], sunset[6];
  DayForecast today;
  HourForecast hours[HOURS];
};

static const uint32_t DATA_MAGIC = 0x57545233;  // "WTR3"
static WeatherData wd;
static M5Canvas canvas(&M5.Display);

// ---------------------------------------------------------------- helpers

static const char* weatherText(int code) {
  switch (code) {
    case 0: return "Ясно";
    case 1: return "Преим. ясно";
    case 2: return "Переменная облачность";
    case 3: return "Пасмурно";
    case 45: case 48: return "Туман";
    case 51: case 53: case 55: return "Морось";
    case 56: case 57: return "Ледяная морось";
    case 61: return "Небольшой дождь";
    case 63: return "Дождь";
    case 65: return "Сильный дождь";
    case 66: case 67: return "Ледяной дождь";
    case 71: return "Небольшой снег";
    case 73: return "Снег";
    case 75: return "Сильный снег";
    case 77: return "Снежная крупа";
    case 80: case 81: return "Ливень";
    case 82: return "Сильный ливень";
    case 85: case 86: return "Снегопад";
    case 95: return "Гроза";
    case 96: case 99: return "Гроза с градом";
  }
  return "-";
}

static const char* windDirText(int deg) {
  static const char* d[] = {"С", "СВ", "В", "ЮВ", "Ю", "ЮЗ", "З", "СЗ"};
  return d[((deg + 22) % 360) / 45];
}

// 0 = воскресенье
static int weekday(int y, int m, int d) {
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static const char* DOW_SHORT[] = {"Вс", "Пн", "Вт", "Ср", "Чт", "Пт", "Сб"};
static const char* MONTHS[] = {"янв", "фев", "мар", "апр", "мая", "июн",
                               "июл", "авг", "сен", "окт", "ноя", "дек"};

static void parseDate(const char* s, int& y, int& m, int& d) {
  y = atoi(s); m = atoi(s + 5); d = atoi(s + 8);
}

static void copyStr(char* dst, size_t n, const char* src) {
  strncpy(dst, src ? src : "", n - 1);
  dst[n - 1] = 0;
}

// ---------------------------------------------------------------- icons
// Рисуются примитивами, (cx, cy) — центр, s — размер (примерно ширина).

static void drawSun(LovyanGFX& g, int cx, int cy, int r, uint16_t col) {
  g.fillCircle(cx, cy, r, col);
  for (int i = 0; i < 8; ++i) {
    float a = i * PI / 4;
    int x0 = cx + cosf(a) * (r * 1.35f), y0 = cy + sinf(a) * (r * 1.35f);
    int x1 = cx + cosf(a) * (r * 1.85f), y1 = cy + sinf(a) * (r * 1.85f);
    g.drawWideLine(x0, y0, x1, y1, max(1, r / 6), col);
  }
}

static void drawMoon(LovyanGFX& g, int cx, int cy, int r, uint16_t col, uint16_t bg) {
  g.fillCircle(cx, cy, r, col);
  g.fillCircle(cx + r * 0.55f, cy - r * 0.35f, r * 0.85f, bg);
}

static void drawCloud(LovyanGFX& g, int cx, int cy, int s, uint16_t fill, uint16_t outline) {
  int r1 = s * 0.18f, r2 = s * 0.25f, r3 = s * 0.16f;
  int bx = cx - s * 0.38f, bw = s * 0.76f, by = cy, bh = r1 * 1.6f;
  // контур
  int o = max(2, s / 40);
  g.fillCircle(cx - s * 0.22f, cy, r1 + o, outline);
  g.fillCircle(cx + s * 0.02f, cy - s * 0.10f, r2 + o, outline);
  g.fillCircle(cx + s * 0.24f, cy + s * 0.02f, r3 + o, outline);
  g.fillRoundRect(bx - o, by - o, bw + 2 * o, bh + 2 * o, bh / 2 + o, outline);
  // заливка
  g.fillCircle(cx - s * 0.22f, cy, r1, fill);
  g.fillCircle(cx + s * 0.02f, cy - s * 0.10f, r2, fill);
  g.fillCircle(cx + s * 0.24f, cy + s * 0.02f, r3, fill);
  g.fillRoundRect(bx, by, bw, bh, bh / 2, fill);
}

static void drawRain(LovyanGFX& g, int cx, int cy, int s, int n, uint16_t col) {
  for (int i = 0; i < n; ++i) {
    int x = cx - s * 0.25f + i * (s * 0.5f / max(1, n - 1));
    g.drawWideLine(x, cy, x - s * 0.08f, cy + s * 0.2f, max(1, s / 30), col);
  }
}

static void drawSnow(LovyanGFX& g, int cx, int cy, int s, int n, uint16_t col) {
  for (int i = 0; i < n; ++i) {
    int x = cx - s * 0.25f + i * (s * 0.5f / max(1, n - 1));
    int y = cy + s * 0.1f + ((i & 1) ? s * 0.08f : 0);
    int r = s * 0.06f;
    for (int k = 0; k < 3; ++k) {
      float a = k * PI / 3;
      g.drawWideLine(x - cosf(a) * r, y - sinf(a) * r, x + cosf(a) * r, y + sinf(a) * r,
                     max(1, s / 50), col);
    }
  }
}

static void drawBolt(LovyanGFX& g, int cx, int cy, int s, uint16_t col) {
  int x = cx, y = cy;
  g.fillTriangle(x + s * 0.05f, y, x - s * 0.12f, y + s * 0.2f, x + s * 0.02f, y + s * 0.2f, col);
  g.fillTriangle(x + s * 0.02f, y + s * 0.15f, x + s * 0.12f, y + s * 0.15f, x - s * 0.08f, y + s * 0.4f, col);
}

static void drawWeatherIcon(LovyanGFX& g, int cx, int cy, int s, int code, bool day = true) {
  const uint16_t BLACK = TFT_BLACK, WHITE = TFT_WHITE;
  const uint16_t GREY = g.color565(150, 150, 150);
  const uint16_t LGREY = g.color565(215, 215, 215);
  const uint16_t DGREY = g.color565(90, 90, 90);

  auto sunOrMoon = [&](int x, int y, int r) {
    if (day) drawSun(g, x, y, r, BLACK);
    else drawMoon(g, x, y, r, BLACK, WHITE);
  };

  switch (code) {
    case 0:
      sunOrMoon(cx, cy, s * 0.22f);
      break;
    case 1:
    case 2:
      sunOrMoon(cx - s * 0.15f, cy - s * 0.15f, s * 0.17f);
      drawCloud(g, cx + s * 0.08f, cy + s * 0.08f, s * (code == 1 ? 0.7f : 0.85f), WHITE, BLACK);
      break;
    case 3:
      drawCloud(g, cx - s * 0.1f, cy - s * 0.1f, s * 0.7f, LGREY, BLACK);
      drawCloud(g, cx + s * 0.05f, cy + s * 0.05f, s * 0.9f, GREY, BLACK);
      break;
    case 45:
    case 48:
      drawCloud(g, cx, cy - s * 0.15f, s * 0.8f, LGREY, BLACK);
      for (int i = 0; i < 3; ++i)
        g.drawWideLine(cx - s * 0.35f + i * s * 0.05f, cy + s * 0.18f + i * s * 0.1f,
                       cx + s * 0.35f - i * s * 0.05f, cy + s * 0.18f + i * s * 0.1f,
                       max(1, s / 30), DGREY);
      break;
    case 51: case 53: case 55: case 56: case 57:
    case 61: case 63: case 65: case 66: case 67:
    case 80: case 81: case 82: {
      bool shower = code >= 80;
      if (shower) sunOrMoon(cx - s * 0.18f, cy - s * 0.22f, s * 0.14f);
      drawCloud(g, cx, cy - s * 0.12f, s * 0.85f, shower ? WHITE : GREY, BLACK);
      int n = (code == 51 || code == 61 || code == 80 || code == 56) ? 3
            : (code == 65 || code == 82 || code == 55 || code == 67) ? 5 : 4;
      drawRain(g, cx, cy + s * 0.18f, s, n, BLACK);
      break;
    }
    case 71: case 73: case 75: case 77: case 85: case 86: {
      drawCloud(g, cx, cy - s * 0.12f, s * 0.85f, LGREY, BLACK);
      int n = (code == 71 || code == 77) ? 3 : (code == 75 || code == 86) ? 5 : 4;
      drawSnow(g, cx, cy + s * 0.15f, s, n, BLACK);
      break;
    }
    case 95: case 96: case 99:
      drawCloud(g, cx, cy - s * 0.12f, s * 0.85f, DGREY, BLACK);
      drawBolt(g, cx, cy + s * 0.08f, s, BLACK);
      if (code != 95) drawSnow(g, cx + s * 0.05f, cy + s * 0.12f, s * 0.8f, 2, BLACK);
      break;
    default:
      drawCloud(g, cx, cy, s * 0.8f, LGREY, BLACK);
  }
}

// ---------------------------------------------------------------- drawing

static const uint8_t* currentFont = nullptr;

static void useFont(LovyanGFX& g, const uint8_t* font) {
  if (currentFont == font) return;
  g.unloadFont();
  g.loadFont(font);
  currentFont = font;
}

static void fmtTemp(char* buf, size_t n, float t) {
  int v = (int)lroundf(t);
  if (v > 0) snprintf(buf, n, "+%d°", v);
  else if (v < 0) snprintf(buf, n, "−%d°", -v);
  else snprintf(buf, n, "0°");
}

static void drawBattery(LovyanGFX& g, int x, int y, int level, bool charging) {
  int w = 44, h = 22;
  g.drawRect(x, y, w, h, TFT_BLACK);
  g.drawRect(x + 1, y + 1, w - 2, h - 2, TFT_BLACK);
  g.fillRect(x + w, y + 6, 4, h - 12, TFT_BLACK);
  int fw = (w - 6) * constrain(level, 0, 100) / 100;
  g.fillRect(x + 3, y + 3, fw, h - 6, TFT_BLACK);
  if (charging) {
    g.fillTriangle(x + w / 2 + 2, y + 2, x + w / 2 - 6, y + h / 2 + 1, x + w / 2, y + h / 2 + 1, TFT_WHITE);
    g.fillTriangle(x + w / 2 - 2, y + h - 2, x + w / 2 + 6, y + h / 2 - 1, x + w / 2, y + h / 2 - 1, TFT_WHITE);
  }
}

// Рисует строку, уменьшая шрифт, если она не влезает в maxW.
static void drawFit(LovyanGFX& g, const char* s, int x, int y, int maxW,
                    std::initializer_list<const uint8_t*> fonts) {
  for (auto f : fonts) {
    useFont(g, f);
    if (g.textWidth(s) <= maxW || f == *(fonts.end() - 1)) break;
  }
  g.drawString(s, x, y);
}

// Почасовой прогноз: время, иконки, график температуры, столбики осадков.
// Точки (время, иконка, температура) стоят на часах, кратных 3. Осадки у Open-Meteo —
// сумма за предыдущий час, поэтому столбик часа h рисуется между точками h-1 и h,
// а подписи под столбиками — суммы за интервалы между соседними подписанными точками.
static void drawHourly(LovyanGFX& g, int top) {
  const int W = g.width();
  const uint16_t GREY = g.color565(128, 128, 128);
  const uint16_t LGREY = g.color565(220, 220, 220);
  const uint16_t DGREY = g.color565(70, 70, 70);
  char buf[96], a[12], b[12];

  const int xl = 56, xr = W - 56;
  auto px = [&](float h) { return (int)lroundf(xl + (xr - xl) * h / (HOURS - 1)); };

  const int timeY = top + 36;                  // подписи времени
  const int iconY = top + 78;                  // центр иконок
  const int tTop = top + 132, tBot = top + 172;  // диапазон точек графика
  const int fillBot = tBot + 6;
  const int barBot = top + 220, barH = 36;     // столбики осадков
  const int mmY = barBot + 4, probY = barBot + 25;

  // заголовок: дата и итоги дня
  const DayForecast& d = wd.today;
  int y, m, dd;
  parseDate(d.date, y, m, dd);
  g.setTextDatum(textdatum_t::top_left);
  useFont(g, FontB28);
  snprintf(buf, sizeof(buf), "Сегодня, %s %d %s", DOW_SHORT[weekday(y, m, dd)], dd,
           MONTHS[constrain(m, 1, 12) - 1]);
  g.drawString(buf, 16, top);
  const int leftW = g.textWidth(buf);

  fmtTemp(a, sizeof(a), d.tmax);
  fmtTemp(b, sizeof(b), d.tmin);
  if (d.precipProb >= 0)
    snprintf(buf, sizeof(buf), "%s / %s   осадки %.1f мм, %d%%", a, b, d.precip, d.precipProb);
  else
    snprintf(buf, sizeof(buf), "%s / %s   осадки %.1f мм", a, b, d.precip);
  g.setTextDatum(textdatum_t::top_right);
  drawFit(g, buf, W - 16, top + 3, W - 32 - leftW - 24, {FontR24, FontR20, FontR18});

  // шкала температуры
  float lo = wd.hours[0].temp, hi = lo;
  for (int i = 1; i < HOURS; ++i) {
    lo = min(lo, wd.hours[i].temp);
    hi = max(hi, wd.hours[i].temp);
  }
  if (hi - lo < 4) {
    float mid = (hi + lo) / 2;
    lo = mid - 2;
    hi = mid + 2;
  }
  auto py = [&](float t) { return (int)lroundf(tBot - (t - lo) * (tBot - tTop) / (hi - lo)); };

  // граница суток — пунктир
  for (int i = 1; i < HOURS; ++i)
    if (atoi(wd.hours[i].time + 11) == 0)
      for (int yy = timeY + 26; yy < barBot; yy += 6) g.fillRect(px(i), yy, 1, 3, GREY);

  // заливка под графиком и сама линия
  for (int i = 0; i + 1 < HOURS; ++i) {
    int x1 = px(i), y1 = py(wd.hours[i].temp), x2 = px(i + 1), y2 = py(wd.hours[i + 1].temp);
    g.fillTriangle(x1, y1, x2, y2, x2, fillBot, LGREY);
    g.fillTriangle(x1, y1, x1, fillBot, x2, fillBot, LGREY);
  }
  for (int i = 0; i + 1 < HOURS; ++i)
    g.drawWideLine(px(i), py(wd.hours[i].temp), px(i + 1), py(wd.hours[i + 1].temp), 1.5f, TFT_BLACK);

  // столбики осадков по часам
  float maxP = 1.0f;  // шкала не мельче 1 мм/ч, чтобы морось не выглядела ливнем
  for (int i = 1; i < HOURS; ++i) maxP = max(maxP, wd.hours[i].precip);
  for (int i = 1; i < HOURS; ++i) {
    float p = wd.hours[i].precip;
    if (p <= 0) continue;
    int h = max(2, min(barH, (int)lroundf(p / maxP * barH)));
    g.fillRect(px(i - 1) + 3, barBot - h, px(i) - px(i - 1) - 5, h, DGREY);
  }
  g.fillRect(xl, barBot, xr - xl, 1, GREY);

  // подписанные точки: время, иконка, температура
  g.setTextDatum(textdatum_t::top_center);
  for (int i = 0; i < HOURS; i += 3) {
    const HourForecast& hf = wd.hours[i];
    const int x = px(i);
    const int hour = atoi(hf.time + 11);

    if (hour == 0) {
      // полночь — подпись днём недели, инверсией
      int hy, hm, hd;
      parseDate(hf.time, hy, hm, hd);
      snprintf(buf, sizeof(buf), "%s, %d", DOW_SHORT[weekday(hy, hm, hd)], hd);
      useFont(g, FontR20);
      int tw = g.textWidth(buf);
      g.fillRoundRect(x - tw / 2 - 8, timeY - 3, tw + 16, 27, 6, TFT_BLACK);
      g.setTextColor(TFT_WHITE, TFT_BLACK);
      g.drawString(buf, x, timeY);
      g.setTextColor(TFT_BLACK, TFT_WHITE);
    } else {
      snprintf(buf, sizeof(buf), "%02d:00", hour);
      useFont(g, FontR20);
      g.drawString(buf, x, timeY);
    }

    drawWeatherIcon(g, x, iconY, 44, hf.code, hf.isDay);

    const int y0 = py(hf.temp);
    g.fillCircle(x, y0, 5, TFT_BLACK);
    fmtTemp(buf, sizeof(buf), hf.temp);
    useFont(g, FontB28);
    g.setTextColor(TFT_BLACK);  // без фона, чтобы не стирать соседний участок графика
    g.drawString(buf, x, y0 - 34);
    g.setTextColor(TFT_BLACK, TFT_WHITE);
  }

  // суммы осадков и вероятность за 3-часовые интервалы
  useFont(g, FontR18);
  for (int i = 0; i + 3 < HOURS; i += 3) {
    float sum = 0;
    int prob = -1;
    for (int k = i + 1; k <= i + 3; ++k) {
      sum += wd.hours[k].precip;
      prob = max<int>(prob, wd.hours[k].precipProb);
    }
    const int x = px(i + 1.5f);
    if (sum >= 0.05f) snprintf(buf, sizeof(buf), "%.1f мм", sum);
    else snprintf(buf, sizeof(buf), "—");
    g.drawString(buf, x, mmY);
    if (prob >= 0) {
      snprintf(buf, sizeof(buf), "%d%%", prob);
      g.setTextColor(prob >= 50 ? TFT_BLACK : GREY, TFT_WHITE);
      g.drawString(buf, x, probY);
      g.setTextColor(TFT_BLACK, TFT_WHITE);
    }
  }
  g.setTextDatum(textdatum_t::top_left);
}

static void drawScreen(LovyanGFX& g, const char* errorMsg) {
  const int W = g.width(), H = g.height();
  char buf[96];

  g.fillScreen(TFT_WHITE);
  g.setTextColor(TFT_BLACK, TFT_WHITE);
  g.setTextDatum(textdatum_t::top_left);

  // ---------- верхняя панель: текущая погода
  const int topH = 222;

  drawWeatherIcon(g, 100, 100, 170, wd.code, wd.isDay);

  fmtTemp(buf, sizeof(buf), wd.temp);
  useFont(g, FontBig);
  g.drawString(buf, 200, 12);

  drawFit(g, weatherText(wd.code), 205, 150, 330, {FontR28, FontR24});

  // правая колонка
  const int dx = 560, vx = 728;
  drawFit(g, wd.city, dx, 14, W - 90 - dx, {FontB28, FontR24, FontR20});

  int bat = M5.Power.getBatteryLevel();
  bool chg = M5.Power.isCharging() == m5::Power_Class::is_charging;
  drawBattery(g, W - 62, 18, bat, chg);
  useFont(g, FontR18);
  g.setTextDatum(textdatum_t::top_right);
  snprintf(buf, sizeof(buf), "%d%%", bat);
  g.drawString(buf, W - 70, 20);
  g.setTextDatum(textdatum_t::top_left);

  useFont(g, FontR24);
  int ly = 60;
  const int lh = 31;
  auto row = [&](const char* label, const char* value) {
    g.drawString(label, dx, ly);
    g.drawString(value, vx, ly);
    ly += lh;
  };
  fmtTemp(buf, sizeof(buf), wd.feels);
  row("Ощущается", buf);
  snprintf(buf, sizeof(buf), "%.1f м/с, %s", wd.wind, windDirText(wd.windDir));
  row("Ветер", buf);
  snprintf(buf, sizeof(buf), "%d%%", wd.humidity);
  row("Влажность", buf);
  snprintf(buf, sizeof(buf), "%d мм рт. ст.", (int)lroundf(wd.pressure * 0.750062f));
  row("Давление", buf);
  snprintf(buf, sizeof(buf), "%s / %s", wd.sunrise, wd.sunset);
  row("Восх./закат", buf);

  g.fillRect(16, topH - 1, W - 32, 3, TFT_BLACK);

  // ---------- подробный прогноз на сутки
  const int footerY = H - 30;
  drawHourly(g, topH + 10);

  // ---------- нижняя строка
  g.fillRect(16, footerY - 6, W - 32, 1, TFT_BLACK);
  useFont(g, FontR18);
  g.setTextDatum(textdatum_t::top_left);
  {
    int y, m, dd;
    parseDate(wd.updated, y, m, dd);
    snprintf(buf, sizeof(buf), "Обновлено: %d %s, %s", dd, MONTHS[constrain(m, 1, 12) - 1], wd.updated + 11);
    g.drawString(buf, 16, footerY);
  }
  g.setTextDatum(textdatum_t::top_right);
  if (errorMsg) {
    snprintf(buf, sizeof(buf), "Ошибка: %s", errorMsg);
    int tw = g.textWidth(buf);
    g.fillRoundRect(W - 16 - tw - 16, footerY - 4, tw + 16, 26, 6, TFT_BLACK);
    g.setTextColor(TFT_WHITE, TFT_BLACK);
    g.drawString(buf, W - 24, footerY);
    g.setTextColor(TFT_BLACK, TFT_WHITE);
  } else {
    g.drawString("Обновить — кнопка питания", W - 16, footerY);
  }
  g.setTextDatum(textdatum_t::top_left);
}

// Отправляет содержимое экрана в USB-Serial (8 бит серого) — для отладки раскладки.
static void dumpCanvas() {
  if (!Serial) return;
  const int w = canvas.width(), h = canvas.height();
  Serial.printf("\nIMG_BEGIN %d %d\n", w, h);
  static uint8_t line[960];
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w && x < 960; ++x) {
      uint16_t c = canvas.readPixel(x, y);
      int r = (c >> 11) << 3, gg = ((c >> 5) & 0x3F) << 2, b = (c & 0x1F) << 3;
      line[x] = (r * 77 + gg * 150 + b * 29) >> 8;
    }
    Serial.write(line, w);
  }
  Serial.flush();
  Serial.println("\nIMG_END");
}

static void showCanvas() {
  dumpCanvas();
  M5.Display.setEpdMode(epd_mode_t::epd_quality);
  canvas.pushSprite(0, 0);
  M5.Display.display();
  M5.Display.waitDisplay();
}

static void drawMessage(const char* line1, const char* line2) {
  canvas.fillScreen(TFT_WHITE);
  canvas.setTextColor(TFT_BLACK, TFT_WHITE);
  canvas.setTextDatum(textdatum_t::middle_center);
  useFont(canvas, FontB36);
  canvas.drawString(line1, canvas.width() / 2, canvas.height() / 2 - 30);
  if (line2) drawFit(canvas, line2, canvas.width() / 2, canvas.height() / 2 + 30, canvas.width() - 40,
                     {FontR24, FontR20, FontR18});
  canvas.setTextDatum(textdatum_t::top_left);
  showCanvas();
}

// ---------------------------------------------------------------- network

static char wifiDiag[160] = "";

static void diagnoseWiFi(wl_status_t st) {
  WiFi.disconnect();
  delay(100);
  int n = WiFi.scanNetworks();
  int found = -1;
  for (int i = 0; i < n; ++i)
    if (WiFi.SSID(i) == WIFI_SSID) { found = i; break; }
  if (found < 0) {
    snprintf(wifiDiag, sizeof(wifiDiag),
             "сеть \"%s\" не найдена (видно сетей: %d). Нужен 2.4 ГГц", WIFI_SSID, n);
  } else {
    snprintf(wifiDiag, sizeof(wifiDiag), "сеть найдена: %d дБм, канал %d, статус %d%s",
             (int)WiFi.RSSI(found), (int)WiFi.channel(found), (int)st,
             st == WL_CONNECT_FAILED ? " (неверный пароль?)" : "");
  }
  Serial.println(wifiDiag);
  for (int i = 0; i < n; ++i)
    Serial.printf("  %s  %d dBm ch%d\n", WiFi.SSID(i).c_str(), (int)WiFi.RSSI(i), (int)WiFi.channel(i));
  WiFi.scanDelete();
}

static bool connectWiFi(uint32_t timeoutMs = 25000) {
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    Serial.printf("WiFi disconnected, reason %d\n", info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  // Роутер работает в смешанном режиме WPA/WPA2 — по умолчанию ESP32 требует WPA2.
  WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);
  wl_status_t st = WL_IDLE_STATUS;
  for (int attempt = 0; attempt < 2; ++attempt) {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t t0 = millis();
    while ((st = WiFi.status()) != WL_CONNECTED && millis() - t0 < timeoutMs) delay(100);
    if (st == WL_CONNECTED) {
      Serial.printf("WiFi OK, IP %s, GW %s, DNS %s\n", WiFi.localIP().toString().c_str(),
                    WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str());
      delay(500);  // дать сети (DHCP/DNS) устояться
      IPAddress ip;
      if (!WiFi.hostByName("api.open-meteo.com", ip) || ip == IPAddress(0, 0, 0, 0)) {
        // Точка доступа раздаёт по DHCP себя как шлюз/DNS, но не отвечает.
        // Оставляем выданный IP, шлюз — основной роутер, DNS — публичные.
        Serial.println("DNS failed, using router gateway + public DNS");
        WiFi.config(WiFi.localIP(), FALLBACK_GW, WiFi.subnetMask(),
                    IPAddress(1, 1, 1, 1), IPAddress(8, 8, 8, 8));
        delay(300);
        bool ok = WiFi.hostByName("api.open-meteo.com", ip);
        Serial.printf("DNS retry: %s %s\n", ok ? "ok" : "fail", ip.toString().c_str());
      } else {
        Serial.printf("DNS ok: %s\n", ip.toString().c_str());
      }
      return true;
    }
    Serial.printf("WiFi attempt %d failed, status %d\n", attempt + 1, (int)st);
    WiFi.disconnect();
    delay(500);
  }
  diagnoseWiFi(st);
  return false;
}

static bool httpGetJsonOnce(const String& url, JsonDocument& doc, JsonDocument* filter);

static bool httpGetJson(const String& url, JsonDocument& doc, JsonDocument* filter = nullptr) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (httpGetJsonOnce(url, doc, filter)) return true;
    delay(1000 + attempt * 1000);
  }
  return false;
}

static bool httpGetJsonOnce(const String& url, JsonDocument& doc, JsonDocument* filter) {
  HTTPClient http;
  WiFiClientSecure secure;
  WiFiClient plain;
  bool ok;
  if (url.startsWith("https")) {
    secure.setInsecure();
    ok = http.begin(secure, url);
  } else {
    ok = http.begin(plain, url);
  }
  if (!ok) return false;
  http.setTimeout(15000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("HTTP %d (%s) for %s\n", code, http.errorToString(code).c_str(), url.c_str());
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();
  DeserializationError err = filter
      ? deserializeJson(doc, body, DeserializationOption::Filter(*filter))
      : deserializeJson(doc, body);
  if (err) {
    Serial.printf("JSON error: %s\n", err.c_str());
    return false;
  }
  return true;
}

static bool locate() {
  if (!isnan(FIXED_LAT) && !isnan(FIXED_LON)) {
    wd.lat = FIXED_LAT;
    wd.lon = FIXED_LON;
    copyStr(wd.city, sizeof(wd.city), FIXED_CITY);
    return true;
  }
  JsonDocument doc;
  if (httpGetJson("http://ip-api.com/json/?fields=status,city,lat,lon&lang=ru", doc) &&
      strcmp(doc["status"] | "", "success") == 0) {
    wd.lat = doc["lat"];
    wd.lon = doc["lon"];
    copyStr(wd.city, sizeof(wd.city), doc["city"] | "");
    return true;
  }
  doc.clear();
  if (httpGetJson("https://ipwho.is/?fields=success,city,latitude,longitude&lang=ru", doc) &&
      (doc["success"] | false)) {
    wd.lat = doc["latitude"];
    wd.lon = doc["longitude"];
    copyStr(wd.city, sizeof(wd.city), doc["city"] | "");
    return true;
  }
  return false;
}

static bool fetchWeather() {
  char url[600];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,"
           "weather_code,surface_pressure,wind_speed_10m,wind_direction_10m"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum,"
           "precipitation_probability_max,wind_speed_10m_max,sunrise,sunset"
           "&hourly=temperature_2m,precipitation,precipitation_probability,weather_code,is_day"
           "&wind_speed_unit=ms&timezone=auto&forecast_days=2",
           wd.lat, wd.lon);

  JsonDocument doc;
  if (!httpGetJson(url, doc)) return false;

  JsonObject cur = doc["current"];
  JsonObject daily = doc["daily"];
  JsonObject hourly = doc["hourly"];
  if (cur.isNull() || daily.isNull() || hourly.isNull()) return false;

  copyStr(wd.updated, sizeof(wd.updated), cur["time"] | "");
  wd.temp = cur["temperature_2m"] | 0.0f;
  wd.feels = cur["apparent_temperature"] | 0.0f;
  wd.humidity = cur["relative_humidity_2m"] | 0;
  wd.pressure = cur["surface_pressure"] | 0.0f;
  wd.wind = cur["wind_speed_10m"] | 0.0f;
  wd.windDir = cur["wind_direction_10m"] | 0;
  wd.code = cur["weather_code"] | -1;
  wd.isDay = (cur["is_day"] | 1) != 0;

  const char* sr = daily["sunrise"][0] | "0000-00-00T--:--";
  const char* ss = daily["sunset"][0] | "0000-00-00T--:--";
  copyStr(wd.sunrise, sizeof(wd.sunrise), strlen(sr) >= 16 ? sr + 11 : "--:--");
  copyStr(wd.sunset, sizeof(wd.sunset), strlen(ss) >= 16 ? ss + 11 : "--:--");

  DayForecast& d = wd.today;
  copyStr(d.date, sizeof(d.date), daily["time"][0] | "0000-00-00");
  d.code = daily["weather_code"][0] | -1;
  d.tmax = daily["temperature_2m_max"][0] | 0.0f;
  d.tmin = daily["temperature_2m_min"][0] | 0.0f;
  d.precip = daily["precipitation_sum"][0] | 0.0f;
  d.precipProb = daily["precipitation_probability_max"][0].isNull()
                     ? -1 : (int)(daily["precipitation_probability_max"][0] | 0);
  d.windMax = daily["wind_speed_10m_max"][0] | 0.0f;

  // Первая точка — последний час, кратный 3, не позже текущего (например, 15:00 для 16:40).
  char from[14];
  snprintf(from, sizeof(from), "%.10sT%02d", wd.updated, atoi(wd.updated + 11) / 3 * 3);
  JsonArray ht = hourly["time"];
  int start = -1;
  for (int i = 0; i < (int)ht.size(); ++i)
    if (strncmp(ht[i] | "", from, 13) == 0) { start = i; break; }
  if (start < 0 || start + HOURS > (int)ht.size()) {
    Serial.printf("hourly: no data from %s\n", from);
    return false;
  }
  for (int i = 0; i < HOURS; ++i) {
    HourForecast& h = wd.hours[i];
    const int j = start + i;
    copyStr(h.time, sizeof(h.time), ht[j] | "0000-00-00T00:00");
    h.code = hourly["weather_code"][j] | -1;
    h.isDay = (hourly["is_day"][j] | 1) != 0;
    h.temp = hourly["temperature_2m"][j] | 0.0f;
    h.precip = hourly["precipitation"][j] | 0.0f;
    h.precipProb = hourly["precipitation_probability"][j].isNull()
                       ? -1 : (int)(hourly["precipitation_probability"][j] | 0);
  }
  if (wd.city[0] == 0) snprintf(wd.city, sizeof(wd.city), "%.2f, %.2f", wd.lat, wd.lon);
  wd.magic = DATA_MAGIC;
  return true;
}

// ---------------------------------------------------------------- cache

static void saveCache() {
  Preferences p;
  p.begin("weather", false);
  p.putBytes("data", &wd, sizeof(wd));
  p.end();
}

static bool loadCache() {
  Preferences p;
  p.begin("weather", true);
  size_t n = p.getBytes("data", &wd, sizeof(wd));
  p.end();
  return n == sizeof(wd) && wd.magic == DATA_MAGIC;
}

// ---------------------------------------------------------------- main

static void goToSleep() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  M5.Display.waitDisplay();
  delay(200);
  // На батарее — полное отключение питания, включение кнопкой Power.
  // При питании от USB выключиться нельзя — уйдёт в deep sleep.
  M5.Power.powerOff();
}

void setup() {
  auto cfg = M5.config();
  cfg.clear_display = false;
  M5.begin(cfg);
  Serial.begin(115200);
  delay(1500);  // успеть открыть монитор порта

  // альбомная ориентация
  for (int r = 0; r < 4; ++r) {
    M5.Display.setRotation(r);
    if (M5.Display.width() > M5.Display.height()) break;
  }

  canvas.setColorDepth(16);
  canvas.setPsram(true);
  if (!canvas.createSprite(M5.Display.width(), M5.Display.height())) {
    Serial.println("canvas alloc failed");
  }

  const char* err = nullptr;
  bool haveCache = loadCache();

  if (!connectWiFi()) {
    err = "нет Wi-Fi";
    if (!haveCache) drawMessage("Нет Wi-Fi", wifiDiag);
    else err = wifiDiag;
  } else if (!locate()) {
    if (haveCache) {
      // используем последние известные координаты
    } else {
      err = "не удалось определить место";
    }
    if (!err && !fetchWeather()) err = "нет данных погоды";
  } else if (!fetchWeather()) {
    err = "нет данных погоды";
  }

  if (!err) saveCache();
  else loadCache();  // восстановить последние хорошие данные

  if (err && !haveCache) {
    if (WiFi.status() == WL_CONNECTED) drawMessage("Нет данных", err);
  } else {
    drawScreen(canvas, err);
    showCanvas();
  }

  goToSleep();
}

void loop() {
  delay(1000);
}
