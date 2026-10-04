#include "GraphEngine.h"
#include <esp_heap_caps.h>
#include <math.h>
#include "../services/DataFeed.h"
#include "../core/Logger.h"
#include "graph/PanelText.h"

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
    uint32_t seq = dataFeed.sequence(m_slot);
    if (seq == 0 || seq == m_seenSeq) return;
    if (dataFeed.kindOf(m_slot) != feed::Kind::Graph) {   // the topic now carries something else
        m_seenSeq = seq;
        if (m_haveData) { m_haveData = false; requestRedraw(); }
        return;
    }
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
    panel_text::drawNoData(m_matrix);
}

void GraphEngine::drawHeader(const graph::Layout& layout) {
    const graph::GraphData& g = *m_data;
    const int size = layout.textSize;
    // Title in white, clipped one scale unit before the summary, which is right-aligned with its
    // last pixel on the last column in the first series' colour (title first: the clip blanks overflow).
    const int summaryW = panel_text::width(g.summary, size);
    const int summaryX = layout.title.w - summaryW;
    const int clip = summaryW ? summaryX - size : layout.title.w;
    // The full title when it fits, else the short one, else the shorter of the two clipped at the edge.
    const int room = clip - layout.title.x;
    const char* title = g.title;
    if (panel_text::width(g.title, size) > room) {
        if (g.titleShort[0] && panel_text::width(g.titleShort, size) <= room) title = g.titleShort;
        else if (g.titleShort[0] && strlen(g.titleShort) < strlen(g.title)) title = g.titleShort;
    }
    panel_text::print(m_matrix, layout.title.x, layout.title.y, title, size, m_matrix->color565(255, 255, 255), clip);
    if (summaryW) {
        uint16_t c = g.hasSummaryColor ? color(g.summaryColor)
                   : g.seriesCount ? color(g.series[0].color) : m_matrix->color565(255, 255, 255);
        panel_text::print(m_matrix, summaryX, layout.title.y, g.summary, size, c);
    }
}

void GraphEngine::drawGraph() {
    const graph::GraphData& g = *m_data;
    const graph::Layout layout = graph::calculateLayout(m_matrix->width(), m_matrix->height(), m_showHeader);
    if (layout.header) drawHeader(layout);

    int px = layout.plot.x, pw = layout.plot.w;
    // Legend (tall panels only): one tiny-font row at the bottom; the plot ends 7 rows higher
    // (256x64: plot rows 15..56, legend 58..62, row 63 blank).
    const bool legend = g.legendCount > 0 && m_matrix->height() >= 64;
    const int py = layout.plot.y;
    const int ph = layout.plot.h - (legend ? 7 : 0);
    const int legendY = py + ph + 1;
    if (pw <= 0 || ph <= 0 || g.slots == 0) return;
    const int base = py + ph - 1;
    const graph::Range range = graph::yRange(g);
    auto ly = [&](float v) { return base - graph::lineOffsetFor(v, range, ph); };

    // 0. Y-axis labels (tall panels only): range ends as integers in the tiny font, right-aligned
    // against the plot, which then starts one column after them.
    if (g.yaxis && m_matrix->height() >= 64) {
        char lmax[12], lmin[12];
        snprintf(lmax, sizeof(lmax), "%ld", lroundf(range.hi));
        snprintf(lmin, sizeof(lmin), "%ld", lroundf(range.lo));
        const int wmax = panel_text::width(lmax, panel_text::TINY);
        const int wmin = panel_text::width(lmin, panel_text::TINY);
        const int lw = wmax > wmin ? wmax : wmin;
        const uint16_t grey = m_matrix->color565(0x70, 0x70, 0x70);
        panel_text::print(m_matrix, lw - wmax, py, lmax, panel_text::TINY, grey);
        panel_text::print(m_matrix, lw - wmin, py + ph - 5, lmin, panel_text::TINY, grey);   // plot's bottom rows
        px = lw + 1;
        pw = m_matrix->width() - px;
        if (pw <= 0) return;
    }
    if (legend) {
        // Items from the plot's left edge, 8 px apart; the first that would pass the right edge is
        // dropped with every item after it (words are never cut).
        int lx = px;
        for (uint8_t i = 0; i < g.legendCount; i++) {
            const int w = panel_text::width(g.legend[i].text, panel_text::TINY);
            if (lx + w > m_matrix->width()) break;
            panel_text::print(m_matrix, lx, legendY, g.legend[i].text, panel_text::TINY, color(g.legend[i].color));
            lx += w + 8;
        }
    }

    // 1. Bands: full-height background over their slot range.
    for (uint8_t i = 0; i < g.bandCount; i++) {
        int x0 = graph::columnForSlot(g.bands[i].from, pw, g.slots);
        int x1 = graph::columnForSlot(g.bands[i].to, pw, g.slots);
        if (x1 > x0) m_matrix->fillRect(px + x0, py, x1 - x0, ph, color(g.bands[i].color));
    }
    // 2. Zero line, when bars straddle it.
    if (graph::zeroLineVisible(g, range)) {
        m_matrix->drawFastHLine(px, ly(0.0f), pw, m_matrix->color565(0x30, 0x30, 0x30));
    }
    // 3. Mid-range gridline on the tall panels.
    if (layout.gridline) {
        m_matrix->drawFastHLine(px, ly((range.lo + range.hi) * 0.5f), pw, m_matrix->color565(0x20, 0x20, 0x20));
    }
    // 4. Marks: dotted vertical lines at the left edge of their slot.
    for (uint8_t i = 0; i < g.markCount; i++) {
        int x = px + graph::columnForSlot(g.marks[i].at, pw, g.slots);
        uint16_t c = color(g.marks[i].color);
        for (int y = py; y <= base; y += 2) m_matrix->drawPixel(x, y, c);
    }
    uint16_t seriesColor[graph::MAX_SERIES];
    for (uint8_t s = 0; s < g.seriesCount; s++) seriesColor[s] = color(g.series[s].color);

    // 5. Bars from the zero line: stacked (positive and negative stacks apart) or overlaid.
    for (int x = 0; x < pw; x++) {
        uint16_t slot = graph::slotForColumn(x, pw, g.slots);
        float pos = 0.0f, neg = 0.0f;
        for (uint8_t s = 0; s < g.seriesCount; s++) {
            if (g.series[s].line) continue;
            float v = graph::valueAt(g, s, slot);
            if (v == 0.0f) continue;
            float from = 0.0f;
            if (g.stack) {
                if (v > 0.0f) { from = pos; pos += v; } else { from = neg; neg += v; }
            }
            int h0 = graph::heightFor(from, range, ph);
            int h1 = graph::heightFor(from + v, range, ph);
            int lo = h0 < h1 ? h0 : h1, hi = h0 < h1 ? h1 : h0;
            if (hi > lo) m_matrix->drawFastVLine(px + x, base - hi + 1, hi - lo, seriesColor[s]);
        }
    }
    // 6. Lines: 1 px, in series order (later on top), vertical runs between columns (step look),
    // no pixel and no join at a gap.
    for (uint8_t s = 0; s < g.seriesCount; s++) {
        if (!g.series[s].line) continue;
        int prevRow = -1;
        for (int x = 0; x < pw; x++) {
            float v = graph::pointAt(g, s, graph::slotForColumn(x, pw, g.slots));
            if (isnan(v)) { prevRow = -1; continue; }
            int row = ly(v);
            if (prevRow < 0 || prevRow == row) {
                m_matrix->drawPixel(px + x, row, seriesColor[s]);
            } else {
                int y0 = prevRow < row ? prevRow : row, y1 = prevRow < row ? row : prevRow;
                m_matrix->drawFastVLine(px + x, y0, y1 - y0 + 1, seriesColor[s]);
            }
            prevRow = row;
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
