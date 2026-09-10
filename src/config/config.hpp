// MIT License
//
// Copyright (c) 2024-2026 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <string>
#include <vector>
#ifdef RCV_BUILD_GUI
#include <QColor>
#endif
#include <filesystem>
#include <map>
#include <string>
#ifdef RCV_BUILD_GUI
#include "util/highlight.h"
#endif

#ifdef RCV_BUILD_GUI
namespace WindowColors
{
const QColor& Background();
const QColor& TraceBackground();
const QColor& LineSlowHighlight();
const QColor& LineRefHighlight(bool bClick);
const QColor& GraphBkg();
const QColor& HotspotBkg();
const QColor& HotspotOutline();
const QColor& MeasureTool();
const QColor& ShaderDataColor();
const QColor& MarkerFunctionColor();
const QColor& MarkerKernelColor();
const QColor& MarkerScopeColor();
const QColor& MarkerPointColor();
const QColor& MarkerUnknownColor();
const QColor& DecoderEventCsPartialFlushColor();
const QColor& DecoderEventDefaultColor();
const QColor& DecoderEventPacketLossColor();
const QColor& DecoderEventDispatchEndColor();
const QColor& DecoderEventStallColor();
const QColor& DecoderEventCodeObjectColor();
const QColor& DecoderEventColor(int type);
const QColor& DecoderDispatchEventColor();
const QColor& LatencyTextColor();
const QColor& UtilizationBarColor();
const QColor& UtilizationBarColorBg();

const Color& StripeBackground();

QColor textColor();
QColor reverseTextColor();
void setDark(bool bDark);
bool isDark();
}; // namespace WindowColors
#endif


struct StyleColor
{
#ifdef RCV_BUILD_GUI
    StyleColor(const std::string& _name, const std::string& _style) :
    name(_name), style(_style), qcolor(ToColor(_style)){};
#else
    // Headless builds need the name/style pair only; nothing renders.
    StyleColor(const std::string& _name, const std::string& _style) : name(_name), style(_style){};
#endif
    std::string name;
    std::string style;
#ifdef RCV_BUILD_GUI
    QColor qcolor;

    static QColor ToColor(const std::string& style);
#endif
};

namespace Config
{
const std::vector<StyleColor>& StateColors();
const std::vector<StyleColor>& TokenColors();
const std::vector<StyleColor>& StallReasonColors();
const std::vector<std::pair<std::string, int>> CustomTokens();
#ifdef RCV_BUILD_GUI
const QColor& PlotColors(int index);

const QColor& StallColor();
const QColor& IssueColor();
const QColor& HiddenLatencyColor();
#endif
}; // namespace Config
