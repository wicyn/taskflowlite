/// @file  d2_render.hpp
/// @brief D2 可视化渲染器 D2Renderer —— Work / Graph 的图形导出。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>

#include "work.hpp"
#include "graph.hpp"
#include "semaphore.hpp"
#include "enums.hpp"

namespace tfl {

/// @brief Work / Graph 的 D2 描述语言渲染器 —— 单一职责，聚合内部友元。
///
/// 核心入口: render_work() 渲染单节点，render_graph() 渲染嵌套 Flow 容器。
/// 信号量配色用 HSL 黄金比例哈希 —— 同一 sem 在所有节点上颜色一致，不同 sem 尽量分散。
/// 全 static 无状态设计；定义为类的唯一理由是 `friend class D2Renderer` 一句搞定访问授权。
class D2Renderer {
public:
    /// @brief 信号量请求只读视图。
    using SemReqs = std::span<const Work::SemaphoreReq>;

    //=========================================================================
    //  Palette —— HSL -> hex, hue 由 semaphore 地址黄金比例旋转得到
    //=========================================================================

    /// @brief D2 渲染时使用的信号量配色。
    struct Palette {
        char bg[8];   ///< pill 暗底色
        char fg[8];   ///< pill 亮字色
        char md[8];   ///< |md 块内文字色
    };

    /// @brief 计算 HSL 转 RGB 过程中的单个颜色通道。
    /// @param p 二次色参数 (2*L - Q)。
    /// @param q 主色参数 (L <= 0.5? L*(1+S) : L+S-L*S)。
    /// @param t 色调偏移 (h +/- 1/3 for R/G/B)。
    /// @return 0..255 的颜色通道值。
    [[nodiscard]] static std::uint8_t format_hsl_component(float p, float q, float t) noexcept;

    /// @brief 将 HSL 颜色格式化为 #rrggbb 十六进制字符串。
    /// @param h hue (0..1)。
    /// @param s saturation (0..1)。
    /// @param l lightness (0..1)。
    /// @param out [out] 至少 8 字节的缓冲区。
    static void format_hsl_hex(float h, float s, float l, char* out) noexcept;

    /// @brief 根据 Semaphore 地址生成稳定配色 —— 同一 sem 在所有节点上颜色一致。
    /// @param sem 信号量指针, 其地址用于确定性哈希。
    /// @return Palette{ bg, fg, md } 三色。
    ///
    /// 哈希: 指针 -> splitmix64 -> hi:16 位映射到 hue 空间。
    /// 黄金比例旋转让相邻地址的 sem 颜色尽量分散。
    [[nodiscard]] static Palette format_palette(const Semaphore* sem) noexcept;

    //=========================================================================
    //  底层写入器
    //=========================================================================

    /// @brief 写入 HTML 转义后的字符串, 用于 D2 markdown 块。
    /// @param os 输出流。
    /// @param s  原始字符串 (可能含 <, >, &, " 等)。
    ///
    /// 这一层小工具看似无聊, 实则是渲染 robust 的底座 ——
    /// 用户随便取个含 < 的节点名也不会破坏输出。
    static void write_html_escaped(std::ostream& os, std::string_view s);

    /// @brief 写入双引号字符串转义后的内容, 用于 D2 字符串字面量。
    /// @param os 输出流。
    /// @param s  原始字符串 (可能含 ", \)。
    static void write_quoted_escaped(std::ostream& os, std::string_view s);

    /// @brief 将指针地址格式化为合法的 D2 节点 ID (以字母开头)。
    /// @param os 输出流。
    /// @param p  指针。
    ///
    /// D2 标识符需以字母开头, 直接用 %p 会以数字开头导致语法错误。
    /// 这里前缀 p 字符, 后跟十六进制地址。
    static void format_id(std::ostream& os, const void* p);

    //=========================================================================
    //  信号量药丸写入器
    //=========================================================================

    /// @brief 写入 grid 布局的信号量 pill bar, 供 rectangle 外壳节点使用。
    /// @param os   输出流。
    /// @param reqs acquire 或 release 请求列表。
    /// @param tag  标签前缀 (如 "acq", "rel")。
    static void write_sem_pill_grid(std::ostream& os, SemReqs reqs, const char* tag);

    /// @brief 写入 markdown 内联信号量 pill 行, 供 diamond / hexagon 等节点使用。
    /// @param os   输出流。
    /// @param reqs acquire 或 release 请求列表。
    /// @param tag  标签前缀 (如 "acq", "rel")。
    static void write_sem_pill_row(std::ostream& os, SemReqs reqs, const char* tag);

    /// @brief 渲染单个 Work 节点。
    ///
    /// 依据 shape 与信号量存在性走两条路径:
    ///   - 无信号量 / 非 rectangle -> |md ... | 块
    ///   - rectangle + 信号量      -> grid pill bar
    ///
    /// @param os            输出流。
    /// @param w             待渲染的 Work 节点。
    /// @param shape         D2 形状 (rectangle / diamond / hexagon / circle)。
    /// @param fill          填充颜色。
    /// @param stroke        描边颜色。
    /// @param font_color    字体颜色。
    /// @param border_radius 圆角半径。
    /// @param stroke_dash   虚线样式 (空字符串 = 实线)。
    static void render_work(std::ostream& os, const Work* w,
                            const char* shape,
                            const char* fill, const char* stroke,
                            const char* font_color, const char* border_radius,
                            const char* stroke_dash = "");

    /// @brief 渲染内嵌 Flow 的容器节点, Subflow / DepFlow 通用。
    /// @param os        输出流。
    /// @param w         待渲染的容器 Work 节点。
    /// @param type_name 节点类型字符串 (如 "Subflow", "DepFlow")。
    /// @param graph     内嵌的子图。
    static void render_graph(std::ostream& os, const Work* w,
                             const char* type_name,
                             const Graph& graph);
};
//=============================================================================
//  实现
//=============================================================================

// ---- 调色板 ---------------------------------------------------------------

inline std::uint8_t D2Renderer::format_hsl_component(float p, float q, float t) noexcept {
    if (t < 0.f) t += 1.f;
    if (t > 1.f) t -= 1.f;
    float v;
    if      (t < 1.f/6.f) v = p + (q - p) * 6.f * t;
    else if (t < 1.f/2.f) v = q;
    else if (t < 2.f/3.f) v = p + (q - p) * (2.f/3.f - t) * 6.f;
    else                  v = p;
    return static_cast<std::uint8_t>(v * 255.f + 0.5f);
}

inline void D2Renderer::format_hsl_hex(float h, float s, float l, char* out) noexcept {
    const float q = (l < 0.5f) ? l * (1.f + s) : l + s - l * s;
    const float p = 2.f * l - q;
    std::snprintf(out, 8, "#%02x%02x%02x",
                  format_hsl_component(p, q, h + 1.f/3.f),
                  format_hsl_component(p, q, h),
                  format_hsl_component(p, q, h - 1.f/3.f));
}

inline D2Renderer::Palette D2Renderer::format_palette(const Semaphore* sem) noexcept {
    // 黄金比例逆元旋转 hue, 让相邻 sem 地址的颜色尽量分散
    constexpr float PHI_INV = 0.6180339887498949f;
    // splitmix64 位混合: 指针 -> 64 位 -> xor-shift -> multiply -> xor-shift
    auto v = reinterpret_cast<std::uintptr_t>(sem);
    v ^= v >> 16; v *= 0x45d9f3bu; v ^= v >> 16;
    // 取 hi 16 位, 归一化到 [0,1), 再乘 PHI_INV 旋转
    const float hue = std::fmod(static_cast<float>(v & 0xFFFFu) / 65536.f * PHI_INV, 1.f);

    Palette p{};
    format_hsl_hex(hue, 0.45f, 0.17f, p.bg);
    format_hsl_hex(hue, 0.80f, 0.72f, p.fg);
    format_hsl_hex(hue, 0.70f, 0.38f, p.md);
    return p;
}

// ---- 底层写入器 ------------------------------------------------------

inline void D2Renderer::write_html_escaped(std::ostream& os, std::string_view s) {
    for (char c : s) {
        switch (c) {
        case '"':  os << "&quot;"; break;
        case '&':  os << "&amp;";  break;
        case '<':  os << "&lt;";   break;
        case '>':  os << "&gt;";   break;
        case '\\': os << "\\\\";   break;
        default:   os << c;
        }
    }
}

inline void D2Renderer::write_quoted_escaped(std::ostream& os, std::string_view s) {
    for (char c : s) {
        switch (c) {
        case '"':  os << "\\\""; break;
        case '\\': os << "\\\\"; break;
        default:   os << c;
        }
    }
}

inline void D2Renderer::format_id(std::ostream& os, const void* p) {
    char buf[24];
    const int n = std::snprintf(buf, sizeof(buf), "p%zx",
                                reinterpret_cast<std::uintptr_t>(p));
    os.write(buf, n);
}

// ---- 信号量药丸写入器 -------------------------------------------------
inline void D2Renderer::write_sem_pill_grid(std::ostream& os, SemReqs reqs, const char* tag) {
    os << "  " << tag[0] << "_bar: \"\" {\n"
                            "    style.fill: transparent\n"
                            "    style.stroke: transparent\n"
                            "    grid-rows: 1\n"
                            "    grid-gap: 6\n\n";

    for (std::size_t i = 0; i < reqs.size(); ++i) {
        const auto& req = reqs[i];
        const auto pal = format_palette(req.sem);

        os << "    " << tag[0] << i << ": \"" << tag << ":[";
        write_quoted_escaped(os, req.sem->name());
        os << "][" << req.count << '/' << req.sem->max_value()
           << "]\" {\n"
              "      shape: rectangle\n"
              "      style.border-radius: 12\n"
              "      style.fill: \""       << pal.bg << "\"\n"
                        "      style.font-color: \"" << pal.fg << "\"\n"
                        "      style.stroke: transparent\n"
                        "      style.font-size: 10\n"
                        "      height: 20\n"
                        "    }\n";
    }
    os << "  }\n\n";
}

inline void D2Renderer::write_sem_pill_row(std::ostream& os, SemReqs reqs, const char* tag) {
    for (const auto& req : reqs) {
        const auto pal = format_palette(req.sem);

        os << "  <span style=\"background-color:" << pal.bg
           << "; color:"           << pal.fg
           << "; border-radius:8px"
              "; padding:1px 6px"
              "; font-size:9px;\">"
           << tag << ":[";
        write_html_escaped(os, req.sem->name());
        os << "][" << req.count << '/' << req.sem->max_value()
           << "]</span> ";
    }
    os << "<br/>\n";
}

// ---- 公开渲染器 -------------------------------------------------------

inline void D2Renderer::render_work(std::ostream& os, const Work* w,
                                    const char* shape,
                                    const char* fill, const char* stroke,
                                    const char* font_color, const char* border_radius,
                                    const char* stroke_dash)
{
    const char* type_name  = to_string(w->m_type);
    const auto* sd         = w->m_semaphores.get();
    const bool  has_acq    = sd && !sd->acquires.empty();
    const bool  has_rel    = sd && !sd->releases.empty();
    const bool  is_rect    = (std::strcmp(shape, "rectangle") == 0);
    const std::string& raw = w->m_name;

    auto write_name = [&]{
        if (raw.empty()) format_id(os, w);
        else             write_html_escaped(os, raw);
    };

    // Path A — |md 块 (无信号量 或 非矩形)
    if (!(has_acq || has_rel) || !is_rect) [[likely]] {
        format_id(os, w);
        os << ": |md\n  <center>\n";

        if (has_acq) write_sem_pill_row(os, sd->acquires, "acq");

        os << "  <span style=\"color:" << font_color << ";\"><b>";
        write_name();
        os << "</b></span><br/>\n"
              "  <span style=\"color: #6b7280;\">[ " << type_name << " ]</span>\n";

        if (has_rel) {
            os << "  <br/>\n";
            write_sem_pill_row(os, sd->releases, "rel");
        }

        os << "  </center>\n| {\n"
              "  shape: "               << shape         << "\n"
                       "  style.fill: \""        << fill          << "\"\n"
                      "  style.stroke: \""      << stroke        << "\"\n"
                        "  style.font-color: \""  << font_color    << "\"\n"
                            "  style.border-radius: " << border_radius << "\n";
        if (stroke_dash && stroke_dash[0])
            os << "  style.stroke-dash: " << stroke_dash << "\n";
        os << "  style.font-size: 14\n}";
        return;
    }

    // Path B — rectangle + 信号量: grid pill bar
    format_id(os, w);
    os << ": \"\" {\n"
          "  style.fill: \""        << fill          << "\"\n"
                  "  style.stroke: \""      << stroke        << "\"\n"
                    "  style.border-radius: " << border_radius << "\n";
    if (stroke_dash && stroke_dash[0])
        os << "  style.stroke-dash: " << stroke_dash << "\n";
    os << "  grid-columns: 1\n  grid-gap: 6\n\n";

    if (has_acq) write_sem_pill_grid(os, sd->acquires, "acq");

    os << "  mid: |md\n    <center>\n"
          "    <span style=\"color:" << font_color << "; font-size:14px;\"><b>";
    write_name();
    os << "</b></span><br/>\n"
          "    <span style=\"color:#6b7280; font-size:11px;\">[ " << type_name << " ]</span>\n"
                       "    </center>\n  | {\n    shape: text\n  }\n\n";

    if (has_rel) write_sem_pill_grid(os, sd->releases, "rel");

    os << "}";
}

inline void D2Renderer::render_graph(std::ostream& os, const Work* w,
                                     const char* type_name,
                                     const Graph& graph)
{
    if (graph.empty()) {
        render_work(os, w, "rectangle", "#e8f5e9", "#10b981", "#065f46", "8");
        return;
    }

    const auto* sd      = w->m_semaphores.get();
    const bool  has_acq = sd && !sd->acquires.empty();
    const bool  has_rel = sd && !sd->releases.empty();
    const std::string& raw = w->m_name;

    auto write_name = [&]{
        if (raw.empty()) format_id(os, w);
        else             write_html_escaped(os, raw);
    };

    // Path A — 无信号量: |md 块内嵌子图
    if (!has_acq && !has_rel) [[likely]] {
        format_id(os, w);
        os << ": |md\n  <center>";
        write_name();
        os << "<br/><span style=\"color: #6b7280;\">[ " << type_name
           << " ]</span></center>\n| {\n"
              "  shape: rectangle\n"
              "  label.near: top-center\n"
              "  style.fill: \"#e8f5e9\"\n"
              "  style.stroke: \"#10b981\"\n"
              "  style.stroke-width: 2\n"
              "  style.border-radius: 14\n\n";
        graph.dump(os);
        os << "}";
        return;
    }

    // Path B — 有信号量: grid(title + content)
    format_id(os, w);
    os << ": \"\" {\n"
          "  style.fill: \"#e8f5e9\"\n"
          "  style.stroke: \"#10b981\"\n"
          "  style.stroke-width: 2\n"
          "  style.border-radius: 14\n"
          "  grid-columns: 1\n"
          "  grid-gap: 4\n\n";

    if (has_acq) write_sem_pill_grid(os, sd->acquires, "acq");

    os << "  title: |md\n    <center>";
    write_name();
    os << "<br/><span style=\"color: #6b7280;\">[ " << type_name
       << " ]</span></center>\n  | { shape: text }\n\n"
          "  content: \"\" {\n"
          "    style.fill: transparent\n"
          "    style.stroke: transparent\n\n";
    graph.dump(os);
    os << "  }\n\n";

    if (has_rel) write_sem_pill_grid(os, sd->releases, "rel");

    os << "}";
}

}  // namespace tfl
