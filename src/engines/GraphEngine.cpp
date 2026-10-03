#include "GraphEngine.h"
#include <esp_heap_caps.h>
#include "../services/DataFeed.h"
#include "../core/Logger.h"

GraphEngine::~GraphEngine() {
    if (m_data) heap_caps_free(m_data);
    m_data = nullptr;
}

EngineError GraphEngine::initialize(EngineContext* context, const EngineConfig* config) {
    if (!context || !context->getMatrix()) return EngineError::InitializationFailed;
    m_matrix = context->getMatrix();
    // About 5 KB; PSRAM keeps it out of the internal heap that TLS and the web server need.
    m_data = (graph::GraphData*)heap_caps_calloc(1, sizeof(graph::GraphData), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m_data) return EngineError::InitializationFailed;
    onConfigChanged(config);
    return EngineError::OK;
}

void GraphEngine::activate() {
    m_lastLookupMs = 0;   // look the topic up straight away
    requestRedraw();
}

void GraphEngine::onConfigChanged(const EngineConfig* config) {
    if (config) {
        String topic = config->getString("topic", "arcadematrix/graph/ac");
        topic.trim();
        strncpy(m_topic, topic.c_str(), sizeof(m_topic) - 1);
        m_topic[sizeof(m_topic) - 1] = '\0';
        m_showHeader = config->getBool("show_header", true);
    }
    m_slot = -1;
    m_seenSeq = 0;
    m_haveData = false;
    m_lastLookupMs = 0;
    requestRedraw();
}

void GraphEngine::pollFeed() {
    uint32_t now = millis();
    if (m_slot < 0 && (m_lastLookupMs == 0 || now - m_lastLookupMs >= 500)) {
        m_lastLookupMs = now;
        m_slot = dataFeed.findTopic(m_topic);
    }
    if (m_slot < 0) return;
    uint32_t seq = dataFeed.graphSequence(m_slot);
    if (seq == 0 || seq == m_seenSeq) return;
    uint32_t got = 0;
    if (dataFeed.copyGraph(m_slot, *m_data, got)) {   // false = mid-publication, retry next frame
        m_seenSeq = got;
        m_haveData = m_data->valid;
        requestRedraw();
    }
}

void GraphEngine::update(EngineContext* context) {
    m_presented = false;
    if (!m_matrix || !m_data) return;
    pollFeed();
    if (m_redrawFrames == 0) return;   // both DMA buffers already show this screen
    m_redrawFrames--;
    m_matrix->fillScreen(0);
    if (m_haveData) drawGraph();
    else drawNoData();
    m_presented = true;
}

void GraphEngine::drawNoData() {
    const char* msg = "NO DATA";
    m_matrix->setFont(nullptr);
    m_matrix->setTextSize(1);
    m_matrix->setTextWrap(false);
    m_matrix->setTextColor(m_matrix->color565(120, 120, 120));
    int16_t bx, by;
    uint16_t bw, bh;
    m_matrix->getTextBounds(msg, 0, 0, &bx, &by, &bw, &bh);
    m_matrix->setCursor((m_matrix->width() - (int)bw) / 2 - bx, (m_matrix->height() - (int)bh) / 2 - by);
    m_matrix->print(msg);
}

void GraphEngine::drawHeader(const graph::Layout& layout) {
    const graph::GraphData& g = *m_data;
    const int size = layout.textSize;
    const int charW = 6 * size;
    m_matrix->setFont(nullptr);
    m_matrix->setTextSize(size);
    m_matrix->setTextWrap(false);

    // Summary right-aligned in the first series' colour, then as much of the title as fits.
    int summaryW = 0;
    size_t summaryLen = strlen(g.summary);
    if (summaryLen) {
        summaryW = (int)summaryLen * charW - size;   // no trailing column after the last glyph
        uint16_t c = g.seriesCount ? color(g.series[0].color) : m_matrix->color565(255, 255, 255);
        m_matrix->setTextColor(c);
        m_matrix->setCursor(layout.title.w - summaryW, layout.title.y);
        m_matrix->print(g.summary);
    }
    int room = layout.title.w - (summaryW ? summaryW + charW : 0);
    int maxChars = room > 0 ? room / charW : 0;
    char title[sizeof(g.title)];
    strncpy(title, g.title, sizeof(title) - 1);
    title[sizeof(title) - 1] = '\0';
    if ((int)strlen(title) > maxChars) title[maxChars < 0 ? 0 : maxChars] = '\0';
    m_matrix->setTextColor(m_matrix->color565(255, 255, 255));
    m_matrix->setCursor(layout.title.x, layout.title.y);
    m_matrix->print(title);
}

void GraphEngine::drawGraph() {
    const graph::GraphData& g = *m_data;
    const graph::Layout layout = graph::calculateLayout(m_matrix->width(), m_matrix->height(), m_showHeader);
    if (layout.header) drawHeader(layout);

    const int px = layout.plot.x, py = layout.plot.y, pw = layout.plot.w, ph = layout.plot.h;
    if (pw <= 0 || ph <= 0 || g.slots == 0) return;
    const int base = py + ph - 1;
    const float top = graph::scaleMax(g);

    // 1. Bands: full-height background over their slot range.
    for (uint8_t i = 0; i < g.bandCount; i++) {
        int x0 = graph::columnForSlot(g.bands[i].from, pw, g.slots);
        int x1 = graph::columnForSlot(g.bands[i].to, pw, g.slots);
        if (x1 > x0) m_matrix->fillRect(px + x0, py, x1 - x0, ph, color(g.bands[i].color));
    }
    // 2. Half-scale gridline on the tall panels.
    if (layout.gridline) {
        int y = base - graph::barHeight(top * 0.5f, top, ph) + 1;
        m_matrix->drawFastHLine(px, y, pw, m_matrix->color565(0x20, 0x20, 0x20));
    }
    // 3. Marks: dotted vertical lines at the left edge of their slot.
    for (uint8_t i = 0; i < g.markCount; i++) {
        int x = px + graph::columnForSlot(g.marks[i].at, pw, g.slots);
        uint16_t c = color(g.marks[i].color);
        for (int y = py; y <= base; y += 2) m_matrix->drawPixel(x, y, c);
    }
    // 4. Bars: one column per x, stacked bottom-up in series order (or overlaid, later on top).
    uint16_t seriesColor[graph::MAX_SERIES];
    for (uint8_t s = 0; s < g.seriesCount; s++) seriesColor[s] = color(g.series[s].color);
    for (int x = 0; x < pw; x++) {
        uint16_t slot = graph::slotForColumn(x, pw, g.slots);
        float cum = 0.0f;
        for (uint8_t s = 0; s < g.seriesCount; s++) {
            float v = graph::valueAt(g, s, slot);
            if (v <= 0.0f) continue;
            int lo = g.stack ? graph::barHeight(cum, top, ph) : 0;
            int hi = graph::barHeight(g.stack ? cum + v : v, top, ph);
            if (g.stack) cum += v;
            if (hi > lo) m_matrix->drawFastVLine(px + x, base - hi + 1, hi - lo, seriesColor[s]);
        }
    }
}

EngineDescriptor GraphEngineDescriptorHandler::getDescriptor() const {
    EngineDescriptor desc;
    desc.metadata = {"graph", "Graph", "info", FIRMWARE_VERSION};
    desc.capabilities.realtime = false;
    desc.requirements.needsNetwork = true;
    desc.requirements.needsPsram = true;   // the data feed keeps its payloads in PSRAM
    desc.schema.fields = {
        ConfigField("topic", ConfigType::STRING, "MQTT Topic", "Full topic the graph is published on (Data Feed MQTT)", "arcadematrix/graph/ac", true, "", "", "", "", "", false, "", ValidationPolicy::Accept),
        ConfigField("show_header", ConfigType::BOOLEAN, "Show Header", "Title and summary line above the graph", "true", false, "", "", "", "", "", false, "", ValidationPolicy::FallbackDefault)
    };
    desc.factory = []() { return std::unique_ptr<IEngine>(new GraphEngine()); };
    return desc;
}
