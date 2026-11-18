// The mods' notices over the game (modnotice.h).
#include "modnotice.h"

#include "display.h"
#include "hud.h"
#include "screenrt.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <mutex>
#include <vector>

float DxTextUi(const FONTBRX& fontBase, const FONTBRX* pfontOther, const std::string& text, float k88);   // splayer.cpp

namespace
{
struct NOTICE
{
    std::string text;
    double tStart = -1.0;   // (the first frame it is drawn)
};
std::mutex s_mutex;
std::deque<NOTICE> s_anotice;
int s_cposted = 0;
constexpr double c_dtShow = 10.0;
constexpr std::size_t c_cMax = 3;
constexpr float c_depth = 16760000.0f;

// (the HUD's own 640 x 448 canvas: x from the picture's left / right)
float XLeft() { return ((-1.0f - g_display.uiOffset.x) / g_display.uiScale.x + 1.0f) * 320.0f; }
float XRight() { return ((1.0f - g_display.uiOffset.x) / g_display.uiScale.x + 1.0f) * 320.0f; }
void Line(const FONTBRX& font, const std::string& text, float x, float y, float size, glm::vec4 off)
{
    g_hud.DrawTextBox(font, nullptr, text, x - 320.0f, (y - 224.0f) * 1.1f, size, 1.0f, 1.0f, 0.0f, 0.0f, 0, 0, off, glm::vec4(1.0f),
                      glm::vec4(0.0f), c_depth + 100.0f, false);
}
}  // namespace

void PostModNotice(const std::string& text)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    ++s_cposted;
    for (const NOTICE& n : s_anotice)
        if (n.text == text) return;
    s_anotice.push_back({text, -1.0});
    while (s_anotice.size() > 8) s_anotice.pop_front();
}

int CModNoticesPosted()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_cposted;
}

void DrawModNotices(int width, int height)
{
    std::vector<std::string> a;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_anotice.empty()) return;
        const double t = glfwGetTime();
        for (NOTICE& n : s_anotice)
            if (n.tStart < 0.0) n.tStart = t;
        while (!s_anotice.empty() && t - s_anotice.front().tStart > c_dtShow) s_anotice.pop_front();
        for (std::size_t i = 0; i < s_anotice.size() && a.size() < c_cMax; ++i) a.push_back(s_anotice[i].text);
    }
    const FONTBRX* pfont = PfontHud();
    if (!pfont || a.empty()) return;
    g_hud.Begin();
    const float size = 10.0f, dy = 13.0f;
    const float x0 = std::max(XLeft(), -1000.0f) + 10.0f, y0 = 10.0f;
    const float wMax = std::max(120.0f, std::min(XRight(), 2000.0f) - x0 - 30.0f);
    auto dx = [&](const std::string& s) { return DxTextUi(*pfont, nullptr, s, size / 51.2f); };
    // the notices' lines: each wrapped at words to the picture's width
    std::vector<std::string> aline;
    for (const std::string& s : a)
    {
        std::string cur;
        std::size_t i = 0;
        while (i < s.size() && aline.size() < 8)
        {
            const std::size_t e = std::min(s.find(' ', i + 1), s.size());
            const std::string word = s.substr(i, e - i);
            if (!cur.empty() && dx(cur + word) > wMax) { aline.push_back(cur); cur = word.substr(word[0] == ' ' ? 1 : 0); }
            else cur += word;
            i = e;
        }
        if (!cur.empty() && aline.size() < 8) aline.push_back(cur);
    }
    float w = 0.0f;
    for (const std::string& s : aline) w = std::max(w, dx(s));
    g_hud.FillScreenRect(x0, y0, x0 + w + 16.0f, y0 + dy * float(aline.size()) + 8.0f, glm::vec4(40, 0, 0, 110), c_depth / 16777216.0f - 1e-4f);
    for (std::size_t i = 0; i < aline.size(); ++i) Line(*pfont, aline[i], x0 + 8.0f, y0 + 5.0f + dy * float(i), size, glm::vec4(128, 90, 40, 0));
    g_hud.Flush(width, height);
}
