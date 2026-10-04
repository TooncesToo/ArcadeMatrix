#pragma once
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

/**
 * The weather screen's 24x24 vector icons (sun, cloud, rain, storm, snow), keyed by OpenWeatherMap
 * icon code ("01d", "10n", ...) and drawn at an integer `scale`. Shared by the Weather engine and the
 * Home Assistant engine's weather pages so both draw the same icons.
 */
namespace WeatherIcon {

void draw(MatrixPanel_I2S_DMA* matrix, const char* icon, int x, int y, int scale = 1);

}  // namespace WeatherIcon
