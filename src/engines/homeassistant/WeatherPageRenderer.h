#pragma once
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "FeedPayloads.h"

/**
 * Draws one page of a Home Assistant weather payload: page 0 is NOW (the live station reading)
 * when the payload has one, then one page per forecast day. Layouts per panel size follow
 * layout-v2 §B (shared with the RPi firmware): icon | day label + condition | temperatures
 * right-aligned on 128x32 and 256x64, high above low in °F, "90°F" with the degree glyph.
 * Text is built in stack buffers; nothing allocates.
 */
namespace weather_page {

void draw(MatrixPanel_I2S_DMA* matrix, const feed::WeatherFeed& wx, uint8_t page);

}  // namespace weather_page
