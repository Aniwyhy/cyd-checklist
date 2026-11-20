// Touch checklist for the 4" CYD (ESP32-3248S040: ST7796 320x480 portrait, XPT2046 resistive touch)
// - Tap a task to tick it, hold it to edit/delete, drag to scroll.
// - On-screen keyboard with English and Japanese (romaji -> hiragana/katakana) input.
// - Tasks persist in LittleFS. Touch calibration persists in NVS (hold BOOT to recalibrate).

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <LittleFS.h>
#include <Preferences.h>
#include <vector>

// ---------------------------------------------------------------- display
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_XPT2046 _touch;

 public:
  LGFX() {
    auto b = _bus.config();
    b.spi_host = HSPI_HOST;
    b.spi_mode = 0;
    b.freq_write = 40000000;
    b.freq_read = 16000000;
    b.pin_sclk = 14;
    b.pin_mosi = 13;
    b.pin_miso = 12;
    b.pin_dc = 2;
    _bus.config(b);
    _panel.setBus(&_bus);
    auto p = _panel.config();
    p.pin_cs = 15;
    p.pin_rst = -1;
    p.panel_width = 320;
    p.panel_height = 480;
    p.readable = true;
    p.bus_shared = true;
    _panel.config(p);
    auto l = _light.config();
    l.pin_bl = 27;
    l.freq = 44100;
    l.pwm_channel = 7;
    _light.config(l);
    _panel.setLight(&_light);
    auto t = _touch.config();
    t.x_min = 300;
    t.x_max = 3900;
    t.y_min = 200;
    t.y_max = 3900;
    t.pin_int = -1;
    t.bus_shared = true;
    t.offset_rotation = 0;
    t.spi_host = HSPI_HOST;
    t.freq = 1000000;
    t.pin_sclk = 14;
    t.pin_mosi = 13;
    t.pin_miso = 12;
    t.pin_cs = 33;
    _touch.config(t);
    _panel.setTouch(&_touch);
    setPanel(&_panel);
  }
};

static LGFX tft;
static LGFX_Sprite band(&tft);    // full-width x 80 off-screen strip for flicker-free redraws
static LGFX_Sprite keySpr(&tft);  // one keyboard key
static Preferences prefs;

static const int SCR_W = 320, SCR_H = 480;
static const int BAND_H = 80;

// ---------------------------------------------------------------- palette
static constexpr uint32_t C_BG = 0x151413;
static constexpr uint32_t C_CARD = 0x24221F;
static constexpr uint32_t C_CARD_DONE = 0x1C1B19;
static constexpr uint32_t C_CARD_HI = 0x34312D;
static constexpr uint32_t C_LINE = 0x4A4742;
static constexpr uint32_t C_FG = 0xF4F1EA;
static constexpr uint32_t C_DIM = 0x8F8B82;
static constexpr uint32_t C_FAINT = 0x5E5A54;
static constexpr uint32_t C_ACCENT = 0xE2795A;
static constexpr uint32_t C_ACCENT_HI = 0xF0947A;
static constexpr uint32_t C_ACCENT_DK = 0xB95E45;
static constexpr uint32_t C_DANGER = 0xE5615A;
static constexpr uint32_t C_KEY = 0x2C2A27;
static constexpr uint32_t C_KEY_SP = 0x3D3A36;
static constexpr uint32_t C_KEY_HI = 0x5C5852;

static bool gDim = false;  // draw everything darkened (behind the action sheet)
static uint32_t col(uint32_t c) {
  if (!gDim) return c;
  uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
  r = r * 2 / 5; g = g * 2 / 5; b = b * 2 / 5;
  return (r << 16) | (g << 8) | b;
}

static const lgfx::IFont* F_TEXT = &fonts::lgfxJapanGothicP_20;
static const lgfx::IFont* F_SMALLJ = &fonts::lgfxJapanGothicP_16;
static const lgfx::IFont* F_TITLE = &fonts::FreeSansBold18pt7b;
static const lgfx::IFont* F_UI = &fonts::FreeSans9pt7b;
static const lgfx::IFont* F_UIB = &fonts::FreeSansBold9pt7b;
static const lgfx::IFont* F_KEY = &fonts::FreeSans12pt7b;
static const lgfx::IFont* F_KEYB = &fonts::FreeSansBold12pt7b;

// ---------------------------------------------------------------- UTF-8 / text helpers
static int u8len(uint8_t c) { return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4; }

static int cpCount(const String& s) {
  int n = 0;
  for (size_t i = 0; i < s.length(); i += u8len(s[i])) n++;
  return n;
}

static void popCodepoint(String& s) {
  int i = s.length() - 1;
  while (i > 0 && (((uint8_t)s[i]) & 0xC0) == 0x80) i--;
  if (i >= 0) s.remove(i);
}

// Hiragana -> katakana (shift U+3041..U+3096 by 0x60)
static String toKatakana(const String& s) {
  String out;
  for (size_t i = 0; i < s.length();) {
    int n = u8len(s[i]);
    if (n == 3) {
      uint32_t cp = ((s[i] & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
      if (cp >= 0x3041 && cp <= 0x3096) cp += 0x60;
      out += (char)(0xE0 | (cp >> 12));
      out += (char)(0x80 | ((cp >> 6) & 0x3F));
      out += (char)(0x80 | (cp & 0x3F));
    } else {
      out += s.substring(i, i + n);
    }
    i += n;
  }
  return out;
}

// Truncate to fit maxW pixels with the current font, adding an ellipsis.
template <typename G>
static String fitText(G& g, const String& s, int maxW) {
  if (g.textWidth(s) <= maxW) return s;
  String t = s;
  int ell = g.textWidth("…");
  while (t.length() && g.textWidth(t) + ell > maxW) popCodepoint(t);
  return t + "…";
}

// Byte offsets where each wrapped line starts. Breaks after spaces when it can, else mid-word
// (Japanese has no spaces, so it wraps per character).
template <typename G>
static std::vector<int> wrapBreaks(G& g, const String& s, int maxW) {
  std::vector<int> starts{0};
  int lineStart = 0, lastSpaceEnd = -1;
  int i = 0, len = s.length();
  while (i < len) {
    int n = u8len(s[i]);
    if (i > lineStart && g.textWidth(s.substring(lineStart, i + n)) > maxW) {
      if (lastSpaceEnd > lineStart && lastSpaceEnd <= i) {
        lineStart = i = lastSpaceEnd;
      } else {
        lineStart = i;
      }
      lastSpaceEnd = -1;
      starts.push_back(lineStart);
      continue;
    }
    if (s[i] == ' ') lastSpaceEnd = i + 1;
    i += n;
  }
  return starts;
}

// Wrap into at most two lines; the second gets an ellipsis if it still overflows.
template <typename G>
static void wrap2(G& g, const String& s, int maxW, String& l1, String& l2) {
  auto b = wrapBreaks(g, s, maxW);
  if (b.size() == 1) { l1 = s; l2 = ""; return; }
  l1 = s.substring(0, b[1]);
  l1.trim();
  l2 = fitText(g, s.substring(b[1]), maxW);
}

// ---------------------------------------------------------------- romaji -> kana
struct Kana { const char* r; const char* k; };
static const Kana ROMAJI[] = {
  {"a","あ"},{"i","い"},{"u","う"},{"e","え"},{"o","お"},
  {"ka","か"},{"ki","き"},{"ku","く"},{"ke","け"},{"ko","こ"},
  {"ca","か"},{"cu","く"},{"co","こ"},{"ci","し"},{"ce","せ"},
  {"sa","さ"},{"si","し"},{"shi","し"},{"su","す"},{"se","せ"},{"so","そ"},
  {"ta","た"},{"ti","ち"},{"chi","ち"},{"tu","つ"},{"tsu","つ"},{"te","て"},{"to","と"},
  {"na","な"},{"ni","に"},{"nu","ぬ"},{"ne","ね"},{"no","の"},
  {"ha","は"},{"hi","ひ"},{"hu","ふ"},{"fu","ふ"},{"he","へ"},{"ho","ほ"},
  {"ma","ま"},{"mi","み"},{"mu","む"},{"me","め"},{"mo","も"},
  {"ya","や"},{"yu","ゆ"},{"yo","よ"},{"ye","いぇ"},
  {"ra","ら"},{"ri","り"},{"ru","る"},{"re","れ"},{"ro","ろ"},
  {"wa","わ"},{"wi","うぃ"},{"we","うぇ"},{"wo","を"},
  {"nn","ん"},{"xn","ん"},
  {"ga","が"},{"gi","ぎ"},{"gu","ぐ"},{"ge","げ"},{"go","ご"},
  {"za","ざ"},{"zi","じ"},{"ji","じ"},{"zu","ず"},{"ze","ぜ"},{"zo","ぞ"},
  {"da","だ"},{"di","ぢ"},{"du","づ"},{"de","で"},{"do","ど"},
  {"ba","ば"},{"bi","び"},{"bu","ぶ"},{"be","べ"},{"bo","ぼ"},
  {"pa","ぱ"},{"pi","ぴ"},{"pu","ぷ"},{"pe","ぺ"},{"po","ぽ"},
  {"kya","きゃ"},{"kyi","きぃ"},{"kyu","きゅ"},{"kye","きぇ"},{"kyo","きょ"},
  {"sya","しゃ"},{"syu","しゅ"},{"sye","しぇ"},{"syo","しょ"},
  {"sha","しゃ"},{"shu","しゅ"},{"she","しぇ"},{"sho","しょ"},
  {"tya","ちゃ"},{"tyu","ちゅ"},{"tye","ちぇ"},{"tyo","ちょ"},
  {"cha","ちゃ"},{"chu","ちゅ"},{"che","ちぇ"},{"cho","ちょ"},
  {"cya","ちゃ"},{"cyu","ちゅ"},{"cyo","ちょ"},
  {"nya","にゃ"},{"nyi","にぃ"},{"nyu","にゅ"},{"nye","にぇ"},{"nyo","にょ"},
  {"hya","ひゃ"},{"hyu","ひゅ"},{"hye","ひぇ"},{"hyo","ひょ"},
  {"mya","みゃ"},{"myu","みゅ"},{"mye","みぇ"},{"myo","みょ"},
  {"rya","りゃ"},{"ryu","りゅ"},{"rye","りぇ"},{"ryo","りょ"},
  {"gya","ぎゃ"},{"gyu","ぎゅ"},{"gye","ぎぇ"},{"gyo","ぎょ"},
  {"ja","じゃ"},{"ju","じゅ"},{"je","じぇ"},{"jo","じょ"},
  {"jya","じゃ"},{"jyu","じゅ"},{"jye","じぇ"},{"jyo","じょ"},
  {"zya","じゃ"},{"zyu","じゅ"},{"zye","じぇ"},{"zyo","じょ"},
  {"dya","ぢゃ"},{"dyu","ぢゅ"},{"dyo","ぢょ"},
  {"bya","びゃ"},{"byu","びゅ"},{"bye","びぇ"},{"byo","びょ"},
  {"pya","ぴゃ"},{"pyu","ぴゅ"},{"pye","ぴぇ"},{"pyo","ぴょ"},
  {"fa","ふぁ"},{"fi","ふぃ"},{"fe","ふぇ"},{"fo","ふぉ"},{"fyu","ふゅ"},
  {"va","ヴぁ"},{"vi","ヴぃ"},{"vu","ヴ"},{"ve","ヴぇ"},{"vo","ヴぉ"},
  {"tsa","つぁ"},{"tsi","つぃ"},{"tse","つぇ"},{"tso","つぉ"},
  {"thi","てぃ"},{"thu","てゅ"},{"dhi","でぃ"},{"dhu","でゅ"},{"twu","とぅ"},{"dwu","どぅ"},
  {"xa","ぁ"},{"xi","ぃ"},{"xu","ぅ"},{"xe","ぇ"},{"xo","ぉ"},
  {"la","ぁ"},{"li","ぃ"},{"lu","ぅ"},{"le","ぇ"},{"lo","ぉ"},
  {"xya","ゃ"},{"xyu","ゅ"},{"xyo","ょ"},{"lya","ゃ"},{"lyu","ゅ"},{"lyo","ょ"},
  {"xtu","っ"},{"xtsu","っ"},{"ltu","っ"},{"ltsu","っ"},{"xwa","ゎ"},{"lwa","ゎ"},
  {"-","ー"},{".","。"},{",","、"},{"?","？"},{"!","！"},{"~","〜"},
  {"[","「"},{"]","」"},{"/","・"},
};

static bool isVowel(char c) { return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o'; }
static bool isConsonant(char c) { return c >= 'a' && c <= 'z' && !isVowel(c) && c != 'n'; }

static const char* romajiExact(const String& s) {
  for (auto& e : ROMAJI) if (s == e.r) return e.k;
  return nullptr;
}
static bool romajiPrefix(const String& s) {
  for (auto& e : ROMAJI) {
    size_t n = strlen(e.r);
    if (n > s.length() && strncmp(e.r, s.c_str(), s.length()) == 0) return true;
  }
  return false;
}

// ---------------------------------------------------------------- tasks
struct Task { String text; bool done; };
static std::vector<Task> tasks;
static const char* TASK_FILE = "/tasks.txt";
static const int MAX_TASKS = 60;
static const int MAX_CHARS = 80;

static void saveTasks() {
  File f = LittleFS.open("/tasks.tmp", "w");
  if (!f) return;
  for (auto& t : tasks) {
    f.print(t.done ? '1' : '0');
    f.print('\t');
    f.print(t.text);
    f.print('\n');
  }
  f.close();
  LittleFS.remove(TASK_FILE);
  LittleFS.rename("/tasks.tmp", TASK_FILE);
}

static void loadTasks() {
  tasks.clear();
  File f = LittleFS.open(TASK_FILE, "r");
  if (!f) {
    tasks.push_back({"Tap a task to tick it off", false});
    tasks.push_back({"Hold a task to edit or delete it", false});
    tasks.push_back({"Tap  + Add  for a new task", false});
    tasks.push_back({"にほんごもかけます (romaji)", false});
    tasks.push_back({"Drag the list to scroll", true});
    saveTasks();
    return;
  }
  while (f.available() && (int)tasks.size() < MAX_TASKS) {
    String line = f.readStringUntil('\n');
    if (line.length() < 2 || line[1] != '\t') continue;
    tasks.push_back({line.substring(2), line[0] == '1'});
  }
  f.close();
}

static int doneCount() {
  int n = 0;
  for (auto& t : tasks) n += t.done;
  return n;
}

// ---------------------------------------------------------------- UI state
enum Screen { S_LIST, S_SHEET, S_KB };
static Screen screen = S_LIST;

static const int HEADER_H = 80;
static const int LIST_Y = HEADER_H, LIST_HT = SCR_H - HEADER_H;  // 400 = 5 bands
static const int ROW_X = 10, ROW_W = SCR_W - 20;
static const int ROW_H1 = 56, ROW_H2 = 80, ROW_GAP = 8, LIST_PAD = 8;
static const int TEXT_X = 52, TEXT_W = ROW_W - TEXT_X - 14;
static const int FOOTER_H = 44, FOOTER_W = 170;
static const int ROW_FOOTER = -2;  // pseudo row index for the "Clear done" button

struct Rect {
  int x, y, w, h;
  bool hit(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};
static const Rect BTN_ADD = {SCR_W - 16 - 96, 12, 96, 40};

struct RowLayout { int y, h; String l1, l2; };
static std::vector<RowLayout> layout;
static int footerY = -1;  // content y of the Clear button, -1 if hidden
static int contentHt = 0;

static float scrollY = 0, scrollVel = 0;
static int pressedRow = -1;
static bool pressedAdd = false;
static int sheetRow = -1, sheetPressed = 0;

static void relayout() {
  band.setFont(F_TEXT);
  layout.clear();
  int y = LIST_PAD;
  for (auto& t : tasks) {
    RowLayout r;
    wrap2(band, t.text, TEXT_W, r.l1, r.l2);
    r.h = r.l2.length() ? ROW_H2 : ROW_H1;
    r.y = y;
    y += r.h + ROW_GAP;
    layout.push_back(r);
  }
  footerY = -1;
  if (doneCount()) {
    footerY = y + 4;
    y = footerY + FOOTER_H + ROW_GAP;
  }
  contentHt = y - ROW_GAP + LIST_PAD;
}

static int maxScroll() { return max(0, contentHt - LIST_HT); }
static void clampScroll() { scrollY = constrain(scrollY, 0.0f, (float)maxScroll()); }

// ---------------------------------------------------------------- drawing: list
static void drawCheck(LGFX_Sprite& s, int cx, int cy, bool done, uint32_t cardCol) {
  if (done) {
    s.fillSmoothCircle(cx, cy, 13, col(C_ACCENT));
    s.drawWideLine(cx - 6, cy + 1, cx - 2, cy + 5, 1.5f, col(C_BG));
    s.drawWideLine(cx - 2, cy + 5, cx + 6, cy - 4, 1.5f, col(C_BG));
  } else {
    s.fillSmoothCircle(cx, cy, 13, col(C_LINE));
    s.fillSmoothCircle(cx, cy, 11, col(cardCol));
  }
}

static void drawHeader() {
  auto& s = band;
  s.fillScreen(col(C_BG));
  int n = tasks.size(), d = doneCount();

  s.setTextDatum(textdatum_t::middle_left);
  s.setFont(F_TITLE);
  s.setTextColor(col(C_FG));
  s.drawString("Checklist", 16, BTN_ADD.y + BTN_ADD.h / 2);

  // subtitle + progress bar on the second line
  s.setFont(F_UI);
  s.setTextColor(col(C_DIM));
  String sub = n == 0 ? String("Nothing here yet")
             : d == n ? String("All done!")
                      : String(d) + " of " + n + " done";
  int sy = 66;
  s.drawString(sub, 17, sy);
  int bx = 17 + s.textWidth(sub) + 14, bw = SCR_W - 16 - bx;
  s.fillSmoothRoundRect(bx, sy - 2, bw, 5, 2, col(C_CARD_HI));
  if (n && d) s.fillSmoothRoundRect(bx, sy - 2, max(5, bw * d / n), 5, 2, col(C_ACCENT));

  // + Add
  s.fillSmoothRoundRect(BTN_ADD.x, BTN_ADD.y, BTN_ADD.w, BTN_ADD.h, 20, col(pressedAdd ? C_ACCENT_DK : C_ACCENT));
  int px = BTN_ADD.x + 24, py = BTN_ADD.y + BTN_ADD.h / 2;
  s.fillRect(px - 7, py - 1, 15, 3, col(C_FG));
  s.fillRect(px - 1, py - 7, 3, 15, col(C_FG));
  s.setFont(F_UIB);
  s.setTextColor(col(C_FG));
  s.drawString("Add", px + 15, py + 1);
  s.pushSprite(0, 0);
}

static void drawRow(LGFX_Sprite& s, int i, int y) {
  const Task& t = tasks[i];
  const RowLayout& L = layout[i];
  uint32_t card = i == pressedRow ? C_CARD_HI : t.done ? C_CARD_DONE : C_CARD;
  s.fillSmoothRoundRect(ROW_X, y, ROW_W, L.h, 14, col(card));
  drawCheck(s, ROW_X + 27, y + L.h / 2, t.done, card);

  s.setFont(F_TEXT);
  s.setTextDatum(textdatum_t::middle_left);
  s.setTextColor(col(t.done ? C_FAINT : C_FG));
  int tx = ROW_X + TEXT_X;
  int ly[2] = {y + L.h / 2 + 1, 0};
  int nl = 1;
  if (L.l2.length()) { ly[0] = y + 27; ly[1] = y + 53; nl = 2; }
  for (int k = 0; k < nl; k++) {
    const String& line = k ? L.l2 : L.l1;
    s.drawString(line, tx, ly[k]);
    if (t.done) s.drawFastHLine(tx - 2, ly[k], s.textWidth(line) + 4, col(C_FAINT));
  }
}

static void drawFooter(LGFX_Sprite& s, int y) {
  int x = (SCR_W - FOOTER_W) / 2;
  bool p = pressedRow == ROW_FOOTER;
  s.fillSmoothRoundRect(x, y, FOOTER_W, FOOTER_H, FOOTER_H / 2, col(p ? C_KEY_HI : C_LINE));
  s.fillSmoothRoundRect(x + 1, y + 1, FOOTER_W - 2, FOOTER_H - 2, FOOTER_H / 2 - 1, col(p ? C_KEY_HI : C_BG));
  s.setFont(F_UI);
  s.setTextColor(col(C_DIM));
  s.setTextDatum(textdatum_t::middle_center);
  s.drawString(String("Clear ") + doneCount() + " done", SCR_W / 2, y + FOOTER_H / 2 + 1);
}

static void drawEmpty(LGFX_Sprite& s, int y0) {
  int cy = LIST_HT / 2 - 40 - y0, cx = SCR_W / 2;
  s.fillSmoothCircle(cx, cy, 30, col(C_CARD));
  s.drawWideLine(cx - 11, cy + 1, cx - 3, cy + 9, 2.4f, col(C_ACCENT));
  s.drawWideLine(cx - 3, cy + 9, cx + 13, cy - 8, 2.4f, col(C_ACCENT));
  s.setTextDatum(textdatum_t::middle_center);
  s.setFont(F_UIB);
  s.setTextColor(col(C_FG));
  s.drawString("All clear", cx, cy + 56);
  s.setFont(F_UI);
  s.setTextColor(col(C_DIM));
  s.drawString("Tap + Add to write a new task", cx, cy + 80);
}

static void drawListBand(int k) {
  auto& s = band;
  int y0 = (int)scrollY + k * BAND_H;  // content y at top of this band
  s.fillScreen(col(C_BG));
  if (tasks.empty()) {
    drawEmpty(s, k * BAND_H);
  } else {
    for (int i = 0; i < (int)tasks.size(); i++) {
      int ry = layout[i].y - y0;
      if (ry >= BAND_H) break;
      if (ry + layout[i].h > 0) drawRow(s, i, ry);
    }
    if (footerY >= 0 && footerY - y0 < BAND_H && footerY + FOOTER_H - y0 > 0) drawFooter(s, footerY - y0);
    int ms = maxScroll();
    if (ms > 0) {  // scroll indicator
      int th = max(28, LIST_HT * LIST_HT / contentHt);
      int ty = (LIST_HT - th) * scrollY / ms - k * BAND_H;
      s.fillSmoothRoundRect(SCR_W - 5, ty, 3, th, 1, col(C_FAINT));
    }
  }
  s.pushSprite(0, LIST_Y + k * BAND_H);
}

static void drawList() {
  for (int k = 0; k < LIST_HT / BAND_H; k++) drawListBand(k);
}

static void drawRowOnly(int i) {  // redraw just the bands a row touches
  int top, h;
  if (i == ROW_FOOTER) { if (footerY < 0) return; top = footerY; h = FOOTER_H; }
  else if (i >= 0 && i < (int)tasks.size()) { top = layout[i].y; h = layout[i].h; }
  else return;
  top -= (int)scrollY;
  for (int k = 0; k < LIST_HT / BAND_H; k++)
    if (top + h > k * BAND_H && top < (k + 1) * BAND_H) drawListBand(k);
}

// ---------------------------------------------------------------- drawing: action sheet
static const int SHEET_Y = 290;
static const Rect SB_EDIT = {14, 388, 92, 54};
static const Rect SB_DEL = {114, 388, 92, 54};
static const Rect SB_CANCEL = {214, 388, 92, 54};

static void sheetButton(LGFX_Sprite& s, const Rect& r, int off, const char* label, uint32_t fg, bool pressed) {
  s.fillSmoothRoundRect(r.x, r.y - off, r.w, r.h, 16, pressed ? C_KEY_HI : C_KEY_SP);
  s.setFont(F_UIB);
  s.setTextColor(fg);
  s.setTextDatum(textdatum_t::middle_center);
  s.drawString(label, r.x + r.w / 2, r.y - off + r.h / 2 + 1);
}

static void drawSheet() {
  String l1, l2;
  band.setFont(F_TEXT);
  if (sheetRow >= 0 && sheetRow < (int)tasks.size()) wrap2(band, tasks[sheetRow].text, SCR_W - 48, l1, l2);
  for (int off = SHEET_Y; off < SCR_H; off += BAND_H) {
    auto& s = band;
    s.fillScreen(C_BG);
    s.fillSmoothRoundRect(0, SHEET_Y - off, SCR_W, SCR_H - SHEET_Y + 30, 22, C_CARD);
    s.fillSmoothRoundRect(SCR_W / 2 - 20, SHEET_Y + 10 - off, 40, 5, 2, C_LINE);
    s.setFont(F_TEXT);
    s.setTextColor(C_FG);
    s.setTextDatum(textdatum_t::middle_left);
    s.drawString(l1, 24, SHEET_Y + 42 - off);
    if (l2.length()) s.drawString(l2, 24, SHEET_Y + 68 - off);
    sheetButton(s, SB_EDIT, off, "Edit", C_FG, sheetPressed == 1);
    sheetButton(s, SB_DEL, off, "Delete", C_DANGER, sheetPressed == 2);
    sheetButton(s, SB_CANCEL, off, "Cancel", C_DIM, sheetPressed == 3);
    s.pushSprite(0, off);
  }
}

static void openSheet(int row) {
  sheetRow = row;
  sheetPressed = 0;
  pressedRow = -1;
  screen = S_SHEET;
  gDim = true;
  drawHeader();
  for (int k = 0; k * BAND_H + LIST_Y < SHEET_Y; k++) drawListBand(k);
  gDim = false;
  drawSheet();
}

static void showList() {
  screen = S_LIST;
  relayout();
  clampScroll();
  drawHeader();
  drawList();
}

// ---------------------------------------------------------------- keyboard
enum KeyType : uint8_t { K_CHAR, K_SHIFT, K_BACK, K_LANG, K_SYM, K_SPACE, K_DONE, K_CANCEL };
struct Key { int16_t x, y, w, h; KeyType type; char ch; };
static std::vector<Key> keys;

static String kbText, comp;
static bool kbJP = false, kbKata = false, kbShift = false, kbCaps = false, kbSym = false;
static int kbEdit = -1;  // index of task being edited, -1 = new
static int pressedKey = -1;
static uint32_t lastShiftTap = 0, cursorBlinkAt = 0, backRepeatAt = 0;
static bool cursorOn = true;
static int cursorBand = 0, cursorBand2 = 0;

static const int FIELD_H = 240;  // text area: 3 bands
static const int KB_Y = 244, KEY_H = 54, KEY_ROW = 59, KEY_GAP = 4, KB_M = 3;
static const float UNIT = (SCR_W - 2 * KB_M - 9.0f * KEY_GAP) / 10.0f;

static Key mk(KeyType t, char c = 0) { return Key{0, 0, 0, 0, t, c}; }

static void addRow(int row, std::initializer_list<std::pair<float, Key>> items) {
  float total = 0;
  for (auto& it : items) total += it.first;
  float x = KB_M + (10 - total) * (UNIT + KEY_GAP) / 2;  // center short rows
  for (auto& it : items) {
    Key k = it.second;
    float w = it.first * UNIT + (it.first - 1) * KEY_GAP;
    k.x = (int16_t)roundf(x);
    k.w = (int16_t)roundf(x + w) - k.x;
    k.y = KB_Y + row * KEY_ROW;
    k.h = KEY_H;
    keys.push_back(k);
    x += w + KEY_GAP;
  }
}

static void addCharRow(int row, const char* chars) {
  float x = KB_M;
  for (int i = 0; chars[i]; i++) {
    Key k = mk(K_CHAR, chars[i]);
    k.x = (int16_t)roundf(x);
    k.w = (int16_t)roundf(x + UNIT) - k.x;
    k.y = KB_Y + row * KEY_ROW;
    k.h = KEY_H;
    keys.push_back(k);
    x += UNIT + KEY_GAP;
  }
}

static void buildKeys() {
  keys.clear();
  addCharRow(0, kbSym ? "1234567890" : "qwertyuiop");
  addCharRow(1, kbSym ? "-/:;()&@\"'" : "asdfghjkl-");
  if (kbSym) {
    addRow(2, {{1, mk(K_CHAR, '.')}, {1, mk(K_CHAR, ',')}, {1, mk(K_CHAR, '?')}, {1, mk(K_CHAR, '!')},
               {1, mk(K_CHAR, '#')}, {1, mk(K_CHAR, '%')}, {1, mk(K_CHAR, '+')}, {1, mk(K_CHAR, '=')},
               {2, mk(K_BACK)}});
  } else {
    addRow(2, {{1.5f, mk(K_SHIFT)}, {1, mk(K_CHAR, 'z')}, {1, mk(K_CHAR, 'x')}, {1, mk(K_CHAR, 'c')},
               {1, mk(K_CHAR, 'v')}, {1, mk(K_CHAR, 'b')}, {1, mk(K_CHAR, 'n')}, {1, mk(K_CHAR, 'm')},
               {1.5f, mk(K_BACK)}});
  }
  addRow(3, {{1, mk(K_CANCEL)}, {1.25f, mk(K_SYM)}, {1.75f, mk(K_LANG)}, {3.5f, mk(K_SPACE)},
             {2.5f, mk(K_DONE)}});
}

static bool upperNow() { return !kbJP && (kbShift || kbCaps); }

static void drawKey(int i) {
  const Key& k = keys[i];
  bool pressed = i == pressedKey;
  if (keySpr.width() != k.w || keySpr.height() != k.h) {
    keySpr.deleteSprite();
    keySpr.createSprite(k.w, k.h);
  }
  auto& s = keySpr;
  uint32_t bg;
  switch (k.type) {
    case K_CHAR: case K_SPACE: bg = C_KEY; break;
    case K_DONE: bg = C_ACCENT; break;
    default: bg = C_KEY_SP;
  }
  if (k.type == K_SHIFT && !kbJP && (kbShift || kbCaps)) bg = C_FG;
  if (pressed) bg = k.type == K_DONE ? C_ACCENT_DK : C_KEY_HI;
  s.fillScreen(C_BG);
  s.fillSmoothRoundRect(0, 0, k.w, k.h, 8, bg);
  int cx = k.w / 2, cy = k.h / 2;
  s.setTextDatum(textdatum_t::middle_center);
  s.setTextColor(C_FG);

  switch (k.type) {
    case K_CHAR: {
      char c = k.ch;
      if (upperNow() && c >= 'a' && c <= 'z') c -= 32;
      char str[2] = {c, 0};
      s.setFont(F_KEY);
      s.drawString(str, cx, cy + (c >= 'a' && c <= 'z' ? -2 : 1));
      break;
    }
    case K_SHIFT:
      if (kbJP) {
        s.setFont(F_SMALLJ);
        s.setTextColor(kbKata ? C_ACCENT_HI : C_FG);
        s.drawString(kbKata ? "カナ" : "かな", cx, cy + 1);
      } else {
        uint32_t fc = (kbShift || kbCaps) ? C_BG : C_FG;
        s.fillTriangle(cx, cy - 11, cx - 10, cy, cx + 10, cy, fc);
        s.fillRect(cx - 4, cy, 9, 8, fc);
        if (kbCaps) s.fillRect(cx - 6, cy + 11, 13, 3, fc);
      }
      break;
    case K_BACK: {
      int l = cx - 13, r = cx + 11, t = cy - 9, b = cy + 9;
      s.drawWideLine(l, cy, l + 8, t, 1.0f, C_FG);
      s.drawWideLine(l, cy, l + 8, b, 1.0f, C_FG);
      s.drawWideLine(l + 8, t, r, t, 1.0f, C_FG);
      s.drawWideLine(l + 8, b, r, b, 1.0f, C_FG);
      s.drawWideLine(r, t, r, b, 1.0f, C_FG);
      s.drawWideLine(cx - 1, cy - 4, cx + 6, cy + 4, 1.0f, C_FG);
      s.drawWideLine(cx - 1, cy + 4, cx + 6, cy - 4, 1.0f, C_FG);
      break;
    }
    case K_LANG:
      s.setFont(F_SMALLJ);
      s.setTextColor(kbJP ? C_FAINT : C_ACCENT_HI);
      s.drawString("EN", cx - 13, cy + 1);
      s.setTextColor(C_FAINT);
      s.drawString("/", cx + 2, cy + 1);
      s.setTextColor(kbJP ? C_ACCENT_HI : C_FAINT);
      s.drawString("あ", cx + 15, cy + 1);
      break;
    case K_SYM:
      s.setFont(F_UIB);
      s.drawString(kbSym ? "ABC" : "123", cx, cy + 1);
      break;
    case K_SPACE:
      if (kbJP && comp.length()) {
        s.setFont(F_TEXT);
        s.setTextColor(C_ACCENT_HI);
        s.drawString("確定", cx, cy + 1);
      } else {
        s.setFont(F_UI);
        s.setTextColor(C_DIM);
        s.drawString("space", cx, cy);
      }
      break;
    case K_DONE:
      s.setFont(F_KEYB);
      s.drawString(kbEdit >= 0 ? "Save" : "Add", cx, cy);
      break;
    case K_CANCEL:
      s.drawWideLine(cx - 6, cy - 6, cx + 6, cy + 6, 1.2f, C_DIM);
      s.drawWideLine(cx - 6, cy + 6, cx + 6, cy - 6, 1.2f, C_DIM);
      break;
  }
  s.pushSprite(k.x, k.y);
}

static void drawKeysOfType(KeyType t) {
  for (int i = 0; i < (int)keys.size(); i++) if (keys[i].type == t) drawKey(i);
}

static void drawKeyboard() {
  tft.fillRect(0, FIELD_H, SCR_W, SCR_H - FIELD_H, C_BG);
  for (int i = 0; i < (int)keys.size(); i++) drawKey(i);
}

// Text area: title row, then a card with the wrapped text (last lines if it overflows).
static const int BOX_X = 10, BOX_Y = 44, BOX_W = SCR_W - 20, BOX_H = FIELD_H - BOX_Y - 6;
static const int LINE_H = 30, TXT_PAD = 16, MAX_LINES = 6;

static void drawFieldBand(int k) {
  auto& s = band;
  int off = k * BAND_H;
  s.fillScreen(C_BG);

  // title row
  s.setTextDatum(textdatum_t::middle_left);
  s.setFont(F_UIB);
  s.setTextColor(C_DIM);
  s.drawString(kbEdit >= 0 ? "EDIT TASK" : "NEW TASK", 16, 24 - off);
  const char* chip = kbJP ? (kbKata ? "カタカナ" : "ひらがな") : "English";
  s.setFont(F_SMALLJ);
  int cw = s.textWidth(chip) + 18;
  s.fillSmoothRoundRect(SCR_W - 12 - cw, 11 - off, cw, 26, 13, C_KEY_SP);
  s.setTextColor(kbJP ? C_ACCENT_HI : C_DIM);
  s.setTextDatum(textdatum_t::middle_center);
  s.drawString(chip, SCR_W - 12 - cw / 2, 25 - off);

  // text card
  s.fillSmoothRoundRect(BOX_X, BOX_Y - off, BOX_W, BOX_H, 16, C_ACCENT);
  s.fillSmoothRoundRect(BOX_X + 2, BOX_Y + 2 - off, BOX_W - 4, BOX_H - 4, 14, C_CARD);

  s.setFont(F_TEXT);
  s.setTextDatum(textdatum_t::middle_left);
  int tx = BOX_X + TXT_PAD, aw = BOX_W - 2 * TXT_PAD - 4;
  int firstY = BOX_Y + 8 + LINE_H / 2;
  String cd = comp == "nn" ? String("ん") : comp;
  int curX, curY;
  if (!kbText.length() && !cd.length()) {
    s.setTextColor(C_FAINT);
    s.drawString(kbJP ? "あたらしいタスク…" : "What needs doing?", tx + 6, firstY - off);
    curX = tx; curY = firstY;
  } else {
    String all = kbText + cd;
    int compStart = kbText.length();
    auto br = wrapBreaks(s, all, aw);
    int nl = br.size(), first = max(0, nl - MAX_LINES);
    curX = tx; curY = firstY;
    for (int li = first; li < nl; li++) {
      int a = br[li], b = li + 1 < nl ? br[li + 1] : all.length();
      int y = firstY + (li - first) * LINE_H;
      int x = tx;
      if (a < compStart) {  // committed part
        String part = all.substring(a, min(b, compStart));
        s.setTextColor(C_FG);
        s.drawString(part, x, y - off);
        x += s.textWidth(part);
      }
      if (b > compStart) {  // romaji still being typed: accent + underline
        String part = all.substring(max(a, compStart), b);
        int pw = s.textWidth(part);
        s.setTextColor(C_ACCENT_HI);
        s.drawString(part, x, y - off);
        s.fillRect(x, y + 13 - off, pw, 2, C_ACCENT_HI);
        x += pw;
      }
      curX = x; curY = y;
    }
    // counter
    s.setFont(F_UI);
    s.setTextColor(C_FAINT);
    s.setTextDatum(textdatum_t::bottom_right);
    s.drawString(String(cpCount(kbText) + (int)comp.length()) + "/" + MAX_CHARS, BOX_X + BOX_W - 14,
                 BOX_Y + BOX_H - 10 - off);
  }
  cursorBand = max(0, curY - 13) / BAND_H;
  cursorBand2 = min(FIELD_H - 1, curY + 13) / BAND_H;
  if (cursorOn) s.fillRect(curX + 2, curY - 13 - off, 2, 26, C_ACCENT);
  s.pushSprite(0, off);
}

static void drawField() {
  for (int k = 0; k < FIELD_H / BAND_H; k++) drawFieldBand(k);
}

static void kbRefresh() {
  cursorOn = true;
  cursorBlinkAt = millis() + 530;
  drawField();
}

static void emitKana(const char* k) {
  kbText += kbKata ? toKatakana(k) : String(k);
}

// Run the romaji state machine over `comp`, moving finished kana into kbText.
static void processComp() {
  while (comp.length()) {
    char c0 = comp[0];
    if (comp.length() >= 2 && c0 == 'n' && comp[1] == 'n') {
      if (comp.length() == 2) break;  // "nn": wait, next char decides (konnichiha / konnnichiha)
      emitKana("ん");
      char c2 = comp[2];
      comp.remove(0, (isVowel(c2) || c2 == 'y') ? 1 : 2);
      continue;
    }
    if (comp.length() >= 2 && c0 == 'n' && !isVowel(comp[1]) && comp[1] != 'y' && comp[1] != 'n') {
      emitKana("ん");
      comp.remove(0, comp[1] == '\'' ? 2 : 1);
      continue;
    }
    if (comp.length() >= 2 && isConsonant(c0) && (comp[1] == c0 || (c0 == 't' && comp[1] == 'c'))) {
      emitKana("っ");
      comp.remove(0, 1);
      continue;
    }
    if (const char* k = romajiExact(comp)) {
      emitKana(k);
      comp = "";
      break;
    }
    if (romajiPrefix(comp)) break;
    kbText += c0;  // no kana for this, keep the character as typed
    comp.remove(0, 1);
  }
}

static void flushComp() {
  if (!comp.length()) return;
  if (comp == "n" || comp == "nn") emitKana("ん");
  else kbText += comp;
  comp = "";
}

static void autoShift() {
  bool want = !kbJP && !kbSym && kbText.length() == 0;
  if (want != kbShift && !kbCaps) {
    kbShift = want;
    drawKeysOfType(K_SHIFT);
    drawKeysOfType(K_CHAR);
  }
}

static void openKeyboard(int editIndex) {
  kbEdit = editIndex;
  kbText = editIndex >= 0 ? tasks[editIndex].text : String("");
  comp = "";
  kbSym = false;
  kbCaps = false;
  kbShift = !kbJP && kbText.length() == 0;
  pressedKey = -1;
  screen = S_KB;
  buildKeys();
  drawKeyboard();
  kbRefresh();
}

static void closeKeyboard(bool commit) {
  bool added = false;
  if (commit) {
    flushComp();
    kbText.trim();
    if (kbText.length()) {
      if (kbEdit >= 0 && kbEdit < (int)tasks.size()) {
        tasks[kbEdit].text = kbText;
      } else if ((int)tasks.size() < MAX_TASKS) {
        tasks.push_back({kbText, false});
        added = true;
      }
      saveTasks();
    }
  }
  pressedKey = -1;
  relayout();
  if (added && !tasks.empty()) {  // scroll so the new task is visible
    const RowLayout& r = layout.back();
    scrollY = max<float>(scrollY, r.y + r.h + LIST_PAD - LIST_HT);
  }
  showList();
}

static void typeChar(char c) {
  if (cpCount(kbText) + (int)comp.length() >= MAX_CHARS) return;
  if (kbJP) {
    if (c >= 'A' && c <= 'Z') c += 32;
    comp += c;
    processComp();
    drawKeysOfType(K_SPACE);
  } else {
    if (upperNow() && c >= 'a' && c <= 'z') c -= 32;
    kbText += c;
    if (kbShift && !kbCaps) {
      kbShift = false;
      drawKeysOfType(K_SHIFT);
      drawKeysOfType(K_CHAR);
    }
  }
  kbRefresh();
}

static void backspace() {
  if (comp.length()) comp.remove(comp.length() - 1);
  else if (kbText.length()) popCodepoint(kbText);
  else return;
  if (kbJP) drawKeysOfType(K_SPACE);
  autoShift();
  kbRefresh();
}

static void keyAction(const Key& k) {
  switch (k.type) {
    case K_CHAR: typeChar(k.ch); break;
    case K_BACK: break;  // handled on press (with auto-repeat)
    case K_SHIFT:
      if (kbJP) {
        kbKata = !kbKata;
        prefs.putBool("kata", kbKata);
      } else {
        uint32_t now = millis();
        if (kbCaps) { kbCaps = false; kbShift = false; }
        else if (kbShift && now - lastShiftTap < 400) kbCaps = true;
        else kbShift = !kbShift;
        lastShiftTap = now;
        drawKeysOfType(K_CHAR);
      }
      drawKeysOfType(K_SHIFT);
      kbRefresh();
      break;
    case K_LANG:
      flushComp();
      kbJP = !kbJP;
      prefs.putBool("jp", kbJP);
      kbCaps = false;
      kbShift = !kbJP && !kbSym && kbText.length() == 0;
      for (int i = 0; i < (int)keys.size(); i++) drawKey(i);
      kbRefresh();
      break;
    case K_SYM:
      kbSym = !kbSym;
      if (kbSym) kbShift = false;
      buildKeys();
      drawKeyboard();
      autoShift();
      break;
    case K_SPACE:
      if (kbJP && comp.length()) {
        flushComp();
        drawKeysOfType(K_SPACE);
      } else if (kbText.length() && cpCount(kbText) < MAX_CHARS) {
        kbText += ' ';
      }
      kbRefresh();
      break;
    case K_DONE: closeKeyboard(true); break;
    case K_CANCEL: closeKeyboard(false); break;
  }
}

static int keyAt(int x, int y) {
  // nearest key, so taps in the gaps between keys still register
  int best = -1, bestD = 1 << 30;
  for (int i = 0; i < (int)keys.size(); i++) {
    const Key& k = keys[i];
    if (y < k.y - KEY_GAP || y >= k.y + k.h + KEY_GAP) continue;
    int dx = x < k.x ? k.x - x : x >= k.x + k.w ? x - (k.x + k.w) + 1 : 0;
    int dy = y < k.y ? k.y - y : y >= k.y + k.h ? y - (k.y + k.h) + 1 : 0;
    int d = dx * dx + dy * dy;
    if (d < bestD) { bestD = d; best = i; }
  }
  return bestD <= 64 ? best : -1;
}

// ---------------------------------------------------------------- touch handling
static bool tDown = false, dragging = false, consumed = false;
static int sx, sy, lx, ly;
static float startScroll;
static uint32_t tStart, lastMoveT;
static uint32_t lastActivity = 0;
static bool dimmed = false;

static int rowAt(int y) {
  if (y < LIST_Y) return -1;
  int cy = y - LIST_Y + (int)scrollY;
  for (int i = 0; i < (int)layout.size(); i++)
    if (cy >= layout[i].y - 3 && cy < layout[i].y + layout[i].h + 3) return i;
  if (footerY >= 0 && cy >= footerY - 4 && cy < footerY + FOOTER_H + 4) return ROW_FOOTER;
  return -1;
}

static void onDown(int x, int y) {
  sx = lx = x; sy = ly = y;
  tStart = lastMoveT = millis();
  dragging = consumed = false;
  if (screen == S_LIST) {
    scrollVel = 0;
    startScroll = scrollY;
    if (y < HEADER_H) {
      pressedAdd = BTN_ADD.hit(x, y) || (x > BTN_ADD.x - 10 && y < BTN_ADD.y + BTN_ADD.h + 10);
      if (pressedAdd) drawHeader();
    } else {
      pressedRow = rowAt(y);
      if (pressedRow == ROW_FOOTER && abs(x - SCR_W / 2) > FOOTER_W / 2 + 10) pressedRow = -1;
      if (pressedRow != -1) drawRowOnly(pressedRow);
    }
  } else if (screen == S_SHEET) {
    sheetPressed = SB_EDIT.hit(x, y) ? 1 : SB_DEL.hit(x, y) ? 2 : SB_CANCEL.hit(x, y) ? 3 : 0;
    if (sheetPressed) drawSheet();
  } else if (screen == S_KB) {
    pressedKey = keyAt(x, y);
    if (pressedKey >= 0) {
      drawKey(pressedKey);
      if (keys[pressedKey].type == K_BACK) {
        backspace();
        backRepeatAt = millis() + 450;
      }
    }
  }
}

static void onMove(int x, int y) {
  uint32_t now = millis();
  if (screen == S_LIST && !pressedAdd && !consumed && sy >= HEADER_H) {
    if (!dragging && abs(y - sy) > 10) {
      dragging = true;
      if (pressedRow != -1) { int r = pressedRow; pressedRow = -1; drawRowOnly(r); }
    }
    if (dragging) {
      float ns = constrain(startScroll - (y - sy), 0.0f, (float)maxScroll());
      uint32_t dt = max<uint32_t>(1, now - lastMoveT);
      scrollVel = 0.6f * scrollVel + 0.4f * (-(float)(y - ly) * 16.0f / dt);  // px per 16 ms frame
      if ((int)ns != (int)scrollY) { scrollY = ns; drawList(); }
    }
  }
  lx = x; ly = y; lastMoveT = now;
}

static void onHold() {  // called each frame while pressed
  uint32_t held = millis() - tStart;
  if (screen == S_LIST && !dragging && !consumed && pressedRow >= 0 && held > 550) {
    consumed = true;
    openSheet(pressedRow);
  }
  if (screen == S_KB && pressedKey >= 0 && keys[pressedKey].type == K_BACK && millis() >= backRepeatAt) {
    backspace();
    backRepeatAt = millis() + 70;
  }
}

static void onUp() {
  if (screen == S_LIST) {
    if (pressedAdd) {
      pressedAdd = false;
      if (ly < HEADER_H + 10 && lx > BTN_ADD.x - 20) openKeyboard(-1);
      else drawHeader();
      return;
    }
    if (dragging) {
      if (millis() - lastMoveT > 80) scrollVel = 0;
      dragging = false;
      return;
    }
    int r = pressedRow;
    pressedRow = -1;
    if (consumed || r == -1) return;
    if (r == ROW_FOOTER) {
      std::vector<Task> keep;
      for (auto& t : tasks) if (!t.done) keep.push_back(t);
      tasks.swap(keep);
      saveTasks();
      showList();
      return;
    }
    tasks[r].done = !tasks[r].done;
    saveTasks();
    bool footerWas = footerY >= 0;
    relayout();
    clampScroll();
    if (footerWas != (footerY >= 0)) drawList();  // Clear button appeared/disappeared
    else { drawRowOnly(r); drawRowOnly(ROW_FOOTER); }
    drawHeader();
  } else if (screen == S_SHEET) {
    if (consumed) {  // finger still down from the long-press that opened the sheet
      consumed = false;
      return;
    }
    int b = sheetPressed;
    sheetPressed = 0;
    bool close = false;
    if (b == 1 && SB_EDIT.hit(lx, ly)) { openKeyboard(sheetRow); return; }
    if (b == 2 && SB_DEL.hit(lx, ly)) {
      if (sheetRow >= 0 && sheetRow < (int)tasks.size()) tasks.erase(tasks.begin() + sheetRow);
      saveTasks();
      close = true;
    }
    if (b == 3 && SB_CANCEL.hit(lx, ly)) close = true;
    if (b == 0 && sy < SHEET_Y) close = true;  // tapped the dimmed area above the sheet
    if (!close) {
      if (b) drawSheet();  // slid off a button: just un-highlight it
      return;
    }
    showList();
  } else if (screen == S_KB) {
    int k = pressedKey;
    pressedKey = -1;
    if (k >= 0) {
      drawKey(k);
      keyAction(keys[k]);
    }
  }
}

static int dnCount = 0, upCount = 0;

static void pollTouch() {
  int32_t x, y;
  bool raw = tft.getTouch(&x, &y);
  if (raw) {
    upCount = 0;
    lastActivity = millis();
    if (dimmed) {  // first touch only wakes the screen
      dimmed = false;
      tft.setBrightness(210);
      consumed = true;
      tDown = true;
      sx = lx = x; sy = ly = y;
      tStart = millis();
      return;
    }
    if (!tDown) {
      if (++dnCount >= 2) { tDown = true; onDown(x, y); }
    } else {
      int fx = (lx * 2 + x) / 3, fy = (ly * 2 + y) / 3;  // light smoothing for resistive jitter
      onMove(fx, fy);
      onHold();
    }
  } else {
    dnCount = 0;
    if (tDown && ++upCount >= 3) {
      tDown = false;
      if (consumed && screen == S_LIST) { consumed = false; return; }
      onUp();
    }
  }
}

// ---------------------------------------------------------------- calibration
static const char* CAL_KEY = "cal_p";  // portrait calibration

static void runCalibration() {
  tft.fillScreen(C_BG);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setFont(F_UIB);
  tft.setTextColor(C_FG);
  tft.drawString("Touch calibration", SCR_W / 2, SCR_H / 2 - 16);
  tft.setFont(F_UI);
  tft.setTextColor(C_DIM);
  tft.drawString("Tap the tip of each arrow", SCR_W / 2, SCR_H / 2 + 12);
  uint16_t cal[8];
  tft.calibrateTouch(cal, C_ACCENT, C_BG, 18);
  prefs.putBytes(CAL_KEY, cal, sizeof(cal));
  tft.fillScreen(C_BG);
}

// ---------------------------------------------------------------- main
void setup() {
  Serial.begin(115200);
  pinMode(0, INPUT_PULLUP);
  tft.init();
  tft.setRotation(0);  // portrait, USB at the bottom
  tft.setBrightness(210);
  tft.fillScreen(C_BG);
  band.setColorDepth(16);
  band.createSprite(SCR_W, BAND_H);
  keySpr.setColorDepth(16);

  prefs.begin("checklist", false);
  kbJP = prefs.getBool("jp", false);
  kbKata = prefs.getBool("kata", false);
  uint16_t cal[8];
  if (prefs.getBytes(CAL_KEY, cal, sizeof(cal)) == sizeof(cal) && digitalRead(0) == HIGH) tft.setTouchCalibrate(cal);
  else runCalibration();

  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
  loadTasks();
  showList();
  lastActivity = millis();
}

void loop() {
  static uint32_t nextFrame = 0, bootHeld = 0;
  uint32_t now = millis();
  if (now < nextFrame) { delay(1); return; }
  nextFrame = now + 14;

  pollTouch();

  // fling
  if (screen == S_LIST && !tDown && fabsf(scrollVel) > 0.4f) {
    float ns = constrain(scrollY + scrollVel, 0.0f, (float)maxScroll());
    scrollVel *= 0.92f;
    if (ns == 0 || ns == maxScroll()) scrollVel = 0;
    if ((int)ns != (int)scrollY) { scrollY = ns; drawList(); }
  }

  // cursor blink: only the strip the cursor is in
  if (screen == S_KB && now >= cursorBlinkAt) {
    cursorOn = !cursorOn;
    cursorBlinkAt = now + 530;
    drawFieldBand(cursorBand);
    if (cursorBand2 != cursorBand) drawFieldBand(cursorBand2);
  }

  // dim after 2 minutes idle
  if (!dimmed && now - lastActivity > 120000) {
    dimmed = true;
    tft.setBrightness(25);
  }

  // hold BOOT for 2 s to recalibrate touch
  if (digitalRead(0) == LOW) {
    if (!bootHeld) bootHeld = now;
    else if (now - bootHeld > 2000) {
      bootHeld = 0;
      runCalibration();
      showList();
      lastActivity = millis();
    }
  } else {
    bootHeld = 0;
  }
}
