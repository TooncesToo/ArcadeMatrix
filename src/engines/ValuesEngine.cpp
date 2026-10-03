#include "ValuesEngine.h"
#include "../services/DataFeed.h"
#include "../core/Logger.h"
#include "graph/PanelText.h"

EngineError ValuesEngine::initialize(EngineContext* context, const EngineConfig* config) {
    if (!context || !context->getMatrix()) return EngineError::InitializationFailed;
    m_matrix = context->getMatrix();
    onConfigChanged(config);
    return EngineError::OK;
}

void ValuesEngine::activate() {
    m_lastLookupMs = 0;   // look the topic up straight away
    requestRedraw();
}

void ValuesEngine::onConfigChanged(const EngineConfig* config) {
    if (config) {
        String topic = config->getString("topic", "arcadematrix/values/energy");
        topic.trim();
        strlcpy(m_topic, topic.c_str(), sizeof(m_topic));
        m_showTitle = config->getBool("show_title", true);
    }
    m_slot = -1;
    m_seenSeq = 0;
    m_haveData = false;
    m_lastLookupMs = 0;
    requestRedraw();
}

void ValuesEngine::pollFeed() {
    uint32_t now = millis();
    if (m_slot < 0 && (m_lastLookupMs == 0 || now - m_lastLookupMs >= 500)) {
        m_lastLookupMs = now;
        m_slot = dataFeed.findTopic(m_topic);
    }
    if (m_slot < 0) return;
    uint32_t seq = dataFeed.sequence(m_slot);
    if (seq == 0 || seq == m_seenSeq) return;
    if (dataFeed.kindOf(m_slot) != feed::Kind::Values) {   // the topic carries something else
        m_seenSeq = seq;
        if (m_haveData) { m_haveData = false; requestRedraw(); }
        return;
    }
    uint32_t got = 0;
    if (dataFeed.copyValues(m_slot, m_data, got)) {   // false = mid-publication, retry next frame
        m_seenSeq = got;
        m_haveData = m_data.valid && m_data.count > 0;
        requestRedraw();
    }
}

void ValuesEngine::update(EngineContext* context) {
    m_presented = false;
    if (!m_matrix) return;
    pollFeed();
    if (m_redrawFrames == 0) return;   // both DMA buffers already show this screen
    m_redrawFrames--;
    m_matrix->fillScreen(0);
    if (m_haveData) drawValues();
    else drawNoData();
    m_presented = true;
}

void ValuesEngine::drawNoData() {
    panel_text::drawNoData(m_matrix);
}

namespace {

using panel_text::TINY;

// Stacked tiles (label above, value + unit under it, centred per cell), from layout-v2 §C, shared
// with the RPi firmware. Value font chains, largest first, per tile count.
struct Chain { uint8_t n; int8_t fonts[4]; };
const Chain kChainsSmall[4] = {{3, {3, 2, 1}}, {3, {3, 2, 1}}, {2, {2, 1}}, {1, {1}}};       // H < 64
const Chain kChainsLarge[4] = {{4, {5, 4, 3, 2}}, {3, {4, 3, 2}}, {3, {3, 2, 1}}, {3, {3, 2, 1}}};   // H >= 64

int unitFont(int valueFont) { return valueFont >= 2 ? 1 : TINY; }

// One column more than the value's scale, so "17.0h" does not read as one word.
int unitGap(int valueFont) { return valueFont + 1; }

int lineWidth(const char* value, const char* unit, int font, bool withUnit) {
    int w = panel_text::width(value, font);
    if (withUnit && unit[0]) w += unitGap(font) + panel_text::width(unit, unitFont(font));
    return w;
}

struct LabelChoice {
    bool present;
    int font;
    char text[sizeof(feed::Tile::label)];
};

// Full label at the intended font, then the short one, then (256x64) both in the tiny font, and only
// then the shortest text cut from the end. Values are never cut.
LabelChoice chooseLabel(const feed::Tile& t, bool large, int iw) {
    LabelChoice c{};
    c.present = t.label[0] || t.labelShort[0];
    if (!c.present) return c;
    const int intended = large ? 1 : TINY;
    const char* full = t.label[0] ? t.label : t.labelShort;
    const char* shortText = t.labelShort[0] ? t.labelShort : nullptr;
    const int fonts[2] = {intended, TINY};
    const int nFonts = large ? 2 : 1;
    for (int f = 0; f < nFonts; f++) {
        if (panel_text::width(full, fonts[f]) <= iw) { c.font = fonts[f]; strlcpy(c.text, full, sizeof(c.text)); return c; }
        if (shortText && panel_text::width(shortText, fonts[f]) <= iw) { c.font = fonts[f]; strlcpy(c.text, shortText, sizeof(c.text)); return c; }
    }
    c.font = TINY;
    strlcpy(c.text, shortText ? shortText : full, sizeof(c.text));
    size_t len = strlen(c.text);
    while (len > 0 && panel_text::width(c.text, c.font) > iw) c.text[--len] = '\0';
    return c;
}

struct ValueChoice { int font; bool unit; };

ValueChoice chooseValue(const feed::Tile& t, const Chain& chain, int iw, int vb) {
    for (int i = 0; i < chain.n; i++) {
        int f = chain.fonts[i];
        if (7 * f <= vb && lineWidth(t.value, t.unit, f, true) <= iw) return {f, t.unit[0] != '\0'};
    }
    for (int i = 0; i < chain.n; i++) {
        int f = chain.fonts[i];
        if (7 * f <= vb && panel_text::width(t.value, f) <= iw) return {f, false};
    }
    return {chain.fonts[chain.n - 1], false};   // drawn in full, centred, clipped only by the panel
}

}  // namespace

void ValuesEngine::drawTile(const feed::Tile& t, int ix, int iw, int cellTop, int ch, bool large, const void* chainPtr) {
    const Chain& chain = *static_cast<const Chain*>(chainPtr);
    const int lg = large ? 2 : 1;
    const LabelChoice label = chooseLabel(t, large, iw);
    const int labelH = label.present ? panel_text::height(label.font) : 0;
    const int vb = ch - (label.present ? labelH + lg : 0);
    const ValueChoice v = chooseValue(t, chain, iw, vb);
    const int vh = 7 * v.font;
    const int blockH = (label.present ? labelH + lg : 0) + vh;
    const int top = cellTop + (ch - blockH) / 2;
    const uint16_t labelColor = m_matrix->color565(t.labelColor.r, t.labelColor.g, t.labelColor.b);

    if (label.present) {
        const int lw = panel_text::width(label.text, label.font);
        panel_text::print(m_matrix, ix + (iw - lw) / 2, top, label.text, label.font, labelColor);
    }
    const int vy = top + (label.present ? labelH + lg : 0);
    const int lineW = lineWidth(t.value, t.unit, v.font, v.unit);
    const int vx = ix + (iw - lineW) / 2;
    panel_text::print(m_matrix, vx, vy, t.value, v.font, m_matrix->color565(t.color.r, t.color.g, t.color.b));
    if (v.unit) {
        const int uf = unitFont(v.font);
        const int ux = vx + panel_text::width(t.value, v.font) + unitGap(v.font);
        panel_text::print(m_matrix, ux, vy + vh - panel_text::height(uf), t.unit, uf, labelColor);
    }
}

void ValuesEngine::drawValues() {
    const int W = m_matrix->width(), H = m_matrix->height();
    const bool large = H >= 64;
    const int n = m_data.count > feed::MAX_TILES ? feed::MAX_TILES : m_data.count;
    const Chain& chain = (large ? kChainsLarge : kChainsSmall)[n - 1];

    if (n == 4) {
        // 2x2 grid, no title; inner boxes 2 px off each inner edge (a 4 px gutter).
        const int half = W / 2, chh = H / 2;
        for (int i = 0; i < 4; i++) {
            const int c = i % 2, r = i / 2;
            const int x = c * half;
            const int ix = x + (c == 1 ? 2 : 0);
            const int cellEnd = x + half - (c == 0 ? 2 : 0);
            drawTile(m_data.tiles[i], ix, cellEnd - ix, r * chh, chh, large, &chain);
        }
        return;
    }

    const bool title = m_showTitle && m_data.title[0] != '\0';
    const int ct = title ? (large ? 9 : 8) : 0;
    if (title) panel_text::print(m_matrix, 0, 0, m_data.title, 1, m_matrix->color565(255, 255, 255), W);
    for (int i = 0; i < n; i++) {
        const int cellX = i * W / n;
        const int cellEnd = (i + 1) * W / n - (i < n - 1 ? 2 : 0);
        const int ix = cellX + (i > 0 ? 2 : 0);
        drawTile(m_data.tiles[i], ix, cellEnd - ix, ct, H - ct, large, &chain);
    }
}

EngineDescriptor ValuesEngineDescriptorHandler::getDescriptor() const {
    EngineDescriptor desc;
    desc.metadata = {"values", "Values", "info", FIRMWARE_VERSION};
    desc.capabilities.realtime = false;
    desc.requirements.needsNetwork = true;
    desc.requirements.needsPsram = true;   // the data feed keeps its payloads in PSRAM
    desc.schema.fields = {
        ConfigField("topic", ConfigType::STRING, "MQTT Topic", "Full topic the values are published on (Data Feed MQTT)", "arcadematrix/values/energy", true, "", "", "", "", "", false, "", ValidationPolicy::Accept),
        ConfigField("show_title", ConfigType::BOOLEAN, "Show Title", "Title row above the tiles, when the layout has room", "true", false, "", "", "", "", "", false, "", ValidationPolicy::FallbackDefault)
    };
    desc.factory = []() { return std::unique_ptr<IEngine>(new ValuesEngine()); };
    return desc;
}
