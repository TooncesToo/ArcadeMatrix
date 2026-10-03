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

// Value font chains (largest first) per tile count, from layout-v2 (shared with the RPi firmware).
struct Chain { uint8_t n; int8_t fonts[4]; };
const Chain kChainsSmall[4] = {{3, {3, 2, 1}}, {3, {3, 2, 1}}, {2, {2, 1}}, {1, {1}}};       // H < 64
const Chain kChainsLarge[4] = {{4, {5, 4, 3, 2}}, {3, {4, 3, 2}}, {3, {3, 2, 1}}, {3, {3, 2, 1}}};   // H >= 64

struct Fit {
    int font;
    bool unit;
    char value[sizeof(feed::Tile::value)];
};

int unitFont(int valueFont) { return valueFont >= 2 ? 1 : TINY; }

// One column more than the value's scale, so "17.0h" does not read as one word.
int unitGap(int valueFont) { return valueFont + 1; }

int lineWidth(const char* value, const char* unit, int font, bool withUnit) {
    int w = panel_text::width(value, font);
    if (withUnit && unit[0]) w += unitGap(font) + panel_text::width(unit, unitFont(font));
    return w;
}

// Largest font that fits value + unit, else value alone, else the smallest font with the value
// cut from the end until it fits (no ellipsis).
Fit fitValue(const feed::Tile& t, const Chain& chain, int aw, int ah) {
    Fit f{};
    strlcpy(f.value, t.value, sizeof(f.value));
    for (int i = 0; i < chain.n; i++) {
        int font = chain.fonts[i];
        if (panel_text::height(font) <= ah && lineWidth(f.value, t.unit, font, true) <= aw) {
            f.font = font;
            f.unit = t.unit[0] != '\0';
            return f;
        }
    }
    for (int i = 0; i < chain.n; i++) {
        int font = chain.fonts[i];
        if (panel_text::height(font) <= ah && panel_text::width(f.value, font) <= aw) {
            f.font = font;
            return f;
        }
    }
    f.font = chain.fonts[chain.n - 1];
    size_t len = strlen(f.value);
    while (len > 0 && panel_text::width(f.value, f.font) > aw) f.value[--len] = '\0';
    return f;
}

void drawLine(MatrixPanel_I2S_DMA* m, int x, int y, const feed::Tile& t, const Fit& f) {
    panel_text::print(m, x, y, f.value, f.font, m->color565(t.color.r, t.color.g, t.color.b));
    if (!f.unit) return;
    const int uf = unitFont(f.font);
    const int ux = x + panel_text::width(f.value, f.font) + unitGap(f.font);
    const int uy = y + panel_text::height(f.font) - panel_text::height(uf);   // bottom-aligned
    panel_text::print(m, ux, uy, t.unit, uf, m->color565(t.labelColor.r, t.labelColor.g, t.labelColor.b));
}

}  // namespace

void ValuesEngine::drawValues() {
    const int W = m_matrix->width(), H = m_matrix->height();
    const bool large = H >= 64;
    const int labelFont = large ? 1 : TINY;
    const int labelH = panel_text::height(labelFont);
    const int n = m_data.count > feed::MAX_TILES ? feed::MAX_TILES : m_data.count;
    const Chain& chain = (large ? kChainsLarge : kChainsSmall)[n - 1];
    auto labelColor = [&](const feed::Tile& t) { return m_matrix->color565(t.labelColor.r, t.labelColor.g, t.labelColor.b); };

    if (n == 4) {
        // 2x2 grid, no title: label on the left of each cell, value right-aligned, both on one line.
        const int cw = W / 2, chh = H / 2;
        for (int i = 0; i < 4; i++) {
            const feed::Tile& t = m_data.tiles[i];
            const int c = i % 2, r = i / 2;
            const int x = c * cw, y = r * chh;
            const int ix = x + (c == 1 ? 2 : 0);
            const int rx = x + cw - (c == 0 ? 2 : 0);
            const int lw = panel_text::width(t.label, labelFont);
            const Fit f = fitValue(t, chain, rx - ix - lw - 2, chh);
            const int vh = panel_text::height(f.font);
            const int vy = y + (chh - vh) / 2;
            panel_text::print(m_matrix, ix, vy + vh - labelH, t.label, labelFont, labelColor(t), rx);
            drawLine(m_matrix, rx - lineWidth(f.value, t.unit, f.font, f.unit), vy, t, f);
        }
        return;
    }

    const bool title = m_showTitle && m_data.title[0] != '\0';
    const int ct = title ? (large ? 9 : 8) : 0;
    if (title) panel_text::print(m_matrix, 0, 0, m_data.title, 1, m_matrix->color565(255, 255, 255), W);

    for (int i = 0; i < n; i++) {
        const feed::Tile& t = m_data.tiles[i];
        const int cellX = i * W / n;
        // A 4 px gutter between columns: 2 px off each inner edge.
        const int cellR = (i + 1) * W / n - (i < n - 1 ? 2 : 0);
        const int ix = cellX + (i > 0 ? 2 : 0);
        int areaTop = ct;
        if (n == 1 && title) {
            // Single tile: its label shares the title row, right-aligned, bottom-aligned with the title.
            const int lw = panel_text::width(t.label, labelFont);
            panel_text::print(m_matrix, W - lw, labelFont == TINY ? 2 : 0, t.label, labelFont, labelColor(t), W);
        } else if (t.label[0]) {
            panel_text::print(m_matrix, ix, ct, t.label, labelFont, labelColor(t), cellR);
            areaTop = ct + labelH + 1;
        }
        const int aw = cellR - ix, ah = H - areaTop;
        const Fit f = fitValue(t, chain, aw, ah);
        const int total = lineWidth(f.value, t.unit, f.font, f.unit);
        const int vx = ix + (aw - total) / 2;
        const int vy = areaTop + (ah - panel_text::height(f.font)) / 2;
        drawLine(m_matrix, vx, vy, t, f);
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
