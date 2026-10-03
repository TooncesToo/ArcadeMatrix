#pragma once
#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "../../include/core/EngineContract.h"
#include "graph/GraphPayload.h"

/**
 * @class GraphEngine
 * @brief Bar/area graph of a payload received over the data feed MQTT link (see DataFeed), for
 *        example 24 hours of air-conditioning power published by Home Assistant.
 *
 * Static screen: it is painted into both DMA buffers when a new payload arrives or the slot starts,
 * and not presented again until the next change, like the weather screen. The payload is parsed on
 * Core 0 by the data feed; this engine only copies the finished GraphData (lock-free) and draws it.
 */
class GraphEngine : public IEngine {
public:
    GraphEngine() = default;
    ~GraphEngine() override;

    EngineError initialize(EngineContext* context, const EngineConfig* config) override;
    void activate() override;
    void update(EngineContext* context) override;
    void render(EngineContext* context) override {}
    void deactivate() override {}
    void onConfigChanged(const EngineConfig* config) override;
    void onDisplayGeometryChanged(const DisplayGeometry& geometry) override { requestRedraw(); }
    bool isRealtime() const override { return false; }
    bool needsClear() const override { return false; }
    bool hasNewFrame() const override { return m_presented; }

private:
    MatrixPanel_I2S_DMA* m_matrix = nullptr;
    graph::GraphData* m_data = nullptr;     ///< private copy (PSRAM), drawn from on Core 1
    char m_topic[96] = {0};
    bool m_showHeader = true;
    bool m_haveData = false;
    int m_slot = -1;                        ///< data feed slot of m_topic, once it has arrived
    uint32_t m_seenSeq = 0;
    uint32_t m_lastLookupMs = 0;
    uint8_t m_redrawFrames = 0;             ///< frames left to paint (2 = one per DMA buffer)
    bool m_presented = false;

    void requestRedraw() { m_redrawFrames = 2; }
    void pollFeed();
    void drawGraph();
    void drawHeader(const graph::Layout& layout);
    void drawNoData();
    uint16_t color(const graph::Rgb& c) const { return m_matrix->color565(c.r, c.g, c.b); }
};

class GraphEngineDescriptorHandler : public IEngineDescriptorHandler {
public:
    EngineDescriptor getDescriptor() const override;
};
