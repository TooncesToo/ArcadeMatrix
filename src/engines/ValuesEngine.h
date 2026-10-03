#pragma once
#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "../../include/core/EngineContract.h"
#include "graph/FeedPayloads.h"

/**
 * @class ValuesEngine
 * @brief One to four labelled values ("tiles") received over the data feed MQTT link, e.g. house
 *        power, today's energy and the outdoor temperature published by Home Assistant. The values
 *        arrive preformatted; the sign never does arithmetic on them.
 *
 * Static screen, same rules as the graph: painted into both DMA buffers on new data, on activate()
 * and on resume() (preemption or slot transition), otherwise not presented.
 */
class ValuesEngine : public IEngine {
public:
    ValuesEngine() = default;
    ~ValuesEngine() override = default;

    EngineError initialize(EngineContext* context, const EngineConfig* config) override;
    void activate() override;
    void update(EngineContext* context) override;
    void render(EngineContext* context) override {}
    void deactivate() override {}
    void resume() override { requestRedraw(); }   // back from a preemption or a slot transition
    void onConfigChanged(const EngineConfig* config) override;
    void onDisplayGeometryChanged(const DisplayGeometry& geometry) override { requestRedraw(); }
    bool isRealtime() const override { return false; }
    bool needsClear() const override { return false; }
    bool hasNewFrame() const override { return m_presented; }

private:
    MatrixPanel_I2S_DMA* m_matrix = nullptr;
    feed::ValuesData m_data{};          ///< private copy, drawn from on Core 1 (~200 bytes)
    char m_topic[96] = {0};
    bool m_showTitle = true;
    bool m_haveData = false;
    int m_slot = -1;
    uint32_t m_seenSeq = 0;
    uint32_t m_lastLookupMs = 0;
    uint8_t m_redrawFrames = 0;
    bool m_presented = false;

    void requestRedraw() { m_redrawFrames = 2; }
    void pollFeed();
    void drawValues();
    void drawNoData();
};

class ValuesEngineDescriptorHandler : public IEngineDescriptorHandler {
public:
    EngineDescriptor getDescriptor() const override;
};
