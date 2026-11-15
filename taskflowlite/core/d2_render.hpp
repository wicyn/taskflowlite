/// @file d2_render.hpp
/// @brief D2 可视化渲染器 D2Renderer —— Work / Graph 的图形导出。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <iterator>
#include <memory>
#include <ostream>
#include <span>
#include <string_view>

#include "enums.hpp"
#include "graph.hpp"
#include "semaphore.hpp"
#include "work.hpp"

namespace tfl {

/// @brief 提供将任务节点和子图写成 D2 描述文本的无状态渲染工具。
///
/// 全部接口均为静态操作，只读取传入的 `Work`、`Graph` 和 `Semaphore` 元数据并
/// 写入调用方提供的流；渲染器不拥有这些对象，也不缓存渲染状态。
///
/// 普通 Work 通过单层 Grid 使标题、类型及 Semaphore 标签共用列宽。
/// 无信号量的非矩形节点和 Graph 使用 D2 原生多行标签，保证文本居中。
/// 所有路径均不生成 HTML，保留 Work 形状、颜色及 Graph 子节点路径。
class D2Renderer {
public:
    /// @brief 信号量请求只读视图。
    using SemReqs = std::span<const Work::SemaphoreReq>;

    // ============================================================================
    // Palette 颜色转换
    // ============================================================================

    static constexpr std::size_t COLOR_HEX_LEN = 8;

    /// @brief 保存同一信号量在 D2 标签中使用的三组固定长度十六进制颜色。
    struct Palette {
        char bg[COLOR_HEX_LEN]; ///< pill 暗底色。
        char fg[COLOR_HEX_LEN]; ///< pill 亮字色。
        char md[COLOR_HEX_LEN]; ///< 保留供外部使用的 Markdown 配色。
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

    /// @brief 根据 Semaphore 地址生成稳定配色。
    /// @param sem 用于计算颜色的信号量地址。
    /// @return 背景色、前景色和 Markdown 文字色。
    [[nodiscard]] static Palette format_palette(const Semaphore* sem) noexcept;

    // ============================================================================
    // 底层写入器
    // ============================================================================

    /// @brief HTML/XML 文本转义（兼容原有外部调用；D2 渲染不再使用）。
    static void write_html_escaped(std::ostream& os, std::string_view s);

    /// @brief 写入经过 Markdown 转义的节点文本，禁止原始 HTML 被解析。
    /// @note 将名称中的换行和制表符折叠为空格，避免破坏 D2 块结构。
    static void write_markdown_escaped(std::ostream& os, std::string_view s);

    /// @brief 写入经过转义的 D2 双引号字符串内容。
    static void write_quoted_escaped(std::ostream& os, std::string_view s);

    /// @brief 将指针地址写成以字母开头的 D2 节点 ID。
    static void format_id(std::ostream& os, const void* p);

    // ============================================================================
    // 信号量标签与节点标题
    // ============================================================================

    /// @brief 用单行 D2 网格节点写入彩色信号量 pill，保留原有深色背景和亮字配色。
    /// @param os 目标输出流。
    /// @param reqs 信号量请求列表。
    /// @param tag 标签前缀（"acq" 或 "rel"）。
    static void write_sem_pill_grid(std::ostream& os, SemReqs reqs, const char* tag);

    /// @brief 保留原有接口，改为写入原生 D2 水平 pill 网格（不再输出 HTML）。
    /// @note 此函数现在应在 D2 容器内调用，不应插入 |md 文本块。
    static void write_sem_pill_row(std::ostream& os, SemReqs reqs, const char* tag);

    /// @brief 向当前 Grid 直接添加名称、类型两个透明矩形（禁止额外嵌套 Grid）。
    /// @note 两个节点与 Acquire/Release 同为外层网格的直接子节点，自动等宽居中。
    /// @param key 两个文本节点的 ID 前缀（"mid" 或 "title"）。
    static void write_title_grid(std::ostream& os, const Work& w, std::string_view type_name,
                                 const char* font_color, const char* key,
                                 int type_font_size = 11);

    // ============================================================================
    // 公开渲染器
    // ============================================================================

    /// @brief 渲染单个 Work 节点。
    /// @param os 目标输出流。
    /// @param w 要渲染的 Work。
    /// @param shape D2 节点形状。
    /// @param fill 填充颜色。
    /// @param stroke 描边颜色。
    /// @param font_color 文字颜色。
    /// @param border_radius 圆角半径。
    /// @param stroke_dash 描边虚线样式；空字符串表示实线。
    static void render_work(std::ostream& os, const Work& w, const char* shape, const char* fill,
                            const char* stroke, const char* font_color, const char* border_radius,
                            const char* stroke_dash = "");

    /// @brief 渲染包含子图的 Work 节点。
    /// @param os 目标输出流。
    /// @param w 要渲染的容器 Work。
    /// @param type_name 节点类型名称。
    /// @param graph 内嵌子图。
    static void render_graph(std::ostream& os, const Work& w, const char* type_name,
                             const Graph& graph);

private:
    /// @brief 原生 D2 pill 共用实现；radius/font_size 用于贴近原来的两种样式。
    static void write_sem_pill_native(std::ostream& os, SemReqs reqs, const char* tag,
                                      int radius, int font_size);
};

// ============================================================================
// 实现
// ============================================================================

// ---- 调色板 ---------------------------------------------------------------

inline std::uint8_t D2Renderer::format_hsl_component(float p, float q, float t) noexcept {
    if (t < 0.f) t += 1.f;
    if (t > 1.f) t -= 1.f;

    float v;
    if (t < 1.f / 6.f)
        v = p + (q - p) * 6.f * t;
    else if (t < 1.f / 2.f)
        v = q;
    else if (t < 2.f / 3.f)
        v = p + (q - p) * (2.f / 3.f - t) * 6.f;
    else
        v = p;

    return static_cast<std::uint8_t>(v * 255.f + 0.5f);
}

inline void D2Renderer::format_hsl_hex(float h, float s, float l, char* out) noexcept {
    const float q = (l < 0.5f) ? l * (1.f + s) : l + s - l * s;
    const float p = 2.f * l - q;

    std::snprintf(out, COLOR_HEX_LEN, "#%02x%02x%02x", format_hsl_component(p, q, h + 1.f / 3.f),
                  format_hsl_component(p, q, h), format_hsl_component(p, q, h - 1.f / 3.f));
}

inline D2Renderer::Palette D2Renderer::format_palette(const Semaphore* sem) noexcept {
    constexpr float PHI_INV = 0.6180339887498949f;

    auto v = reinterpret_cast<std::uintptr_t>(sem);
    v ^= v >> 16;
    v *= 0x45d9f3bu;
    v ^= v >> 16;

    const float hue = std::fmod(static_cast<float>(v & 0xFFFFu) / 65536.f * PHI_INV, 1.f);

    Palette p{};
    format_hsl_hex(hue, 0.45f, 0.17f, p.bg);
    format_hsl_hex(hue, 0.80f, 0.72f, p.fg);
    format_hsl_hex(hue, 0.70f, 0.38f, p.md);
    return p;
}

// ---- 底层写入器 -----------------------------------------------------------

inline void D2Renderer::write_html_escaped(std::ostream& os, std::string_view s) {
    for (char c : s) {
        switch (c) {
        case '"':
            os << "&quot;";
            break;
        case '&':
            os << "&amp;";
            break;
        case '<':
            os << "&lt;";
            break;
        case '>':
            os << "&gt;";
            break;
        case '\\':
            os << "\\\\";
            break;
        default:
            os << c;
            break;
        }
    }
}

inline void D2Renderer::write_markdown_escaped(std::ostream& os, std::string_view s) {
    for (char c : s) {
        switch (c) {
        // 不让名称注入 Markdown 强调、链接、标题、HTML 等语法。
        case '\\':
        case '`':
        case '*':
        case '_':
        case '{':
        case '}':
        case '[':
        case ']':
        case '(':
        case ')':
        case '#':
        case '+':
        case '-':
        case '.':
        case '!':
        case '|':
        case '<':
        case '>':
        case '~':
            os << '\\' << c;
            break;
        case '&':
            os << "&amp;";
            break;
        case '\r':
        case '\n':
        case '\t':
            os << ' ';
            break;
        default:
            os << c;
            break;
        }
    }
}

inline void D2Renderer::write_quoted_escaped(std::ostream& os, std::string_view s) {
    for (char c : s) {
        switch (c) {
        case '"':
            os << "\\\"";
            break;
        case '\\':
            os << "\\\\";
            break;
        case '\n':
            os << "\\n";
            break;
        case '\r':
            break;
        case '\t':
            os << ' ';
            break;
        default:
            os << c;
            break;
        }
    }
}

inline void D2Renderer::format_id(std::ostream& os, const void* pointer) {
    std::format_to(std::ostreambuf_iterator<char>{os}, "p{:x}",
                   reinterpret_cast<std::uintptr_t>(pointer));
}

// ---- 原生 D2 彩色信号量节点 ----------------------------------------------

inline void D2Renderer::write_sem_pill_native(std::ostream& os, SemReqs reqs,
                                              const char* tag, int radius, int font_size) {
    os << "  " << tag[0]
       << "_bar: \"\" {\n"
          "    style.fill: transparent\n"
          "    style.stroke: transparent\n"
          "    grid-rows: 1\n"
          "    grid-gap: 6\n\n";

    for (std::size_t i = 0; i < reqs.size(); ++i) {
        const auto& req = reqs[i];
        const auto  pal = format_palette(req.sem);

        os << "    " << tag[0] << i << ": \"" << tag << ":[";
        write_quoted_escaped(os, req.sem->name());
        os << "][" << req.count << '/' << req.sem->max_value()
           << "]\" {\n"
              "      shape: rectangle\n"
              "      style.border-radius: "
           << radius << "\n"
                        "      style.fill: \""
           << pal.bg << "\"\n"
                        "      style.font-color: \""
           << pal.fg << "\"\n"
                        "      style.stroke: transparent\n"
                        "      style.font-size: "
           << font_size << "\n"
                           "      height: 20\n"
                           "    }\n";
    }

    os << "  }\n\n";
}

inline void D2Renderer::write_sem_pill_grid(std::ostream& os, SemReqs reqs, const char* tag) {
    // 原矩形节点使用 10px 字号、12px 圆角的 pill。
    write_sem_pill_native(os, reqs, tag, 12, 10);
}

inline void D2Renderer::write_sem_pill_row(std::ostream& os, SemReqs reqs, const char* tag) {
    // 原非矩形节点的 HTML span 使用 9px 字号、8px 圆角。
    write_sem_pill_native(os, reqs, tag, 8, 9);
}

// ---- 原生 D2 节点标题 -----------------------------------------------------

inline void D2Renderer::write_title_grid(std::ostream& os, const Work& w,
                                         std::string_view type_name, const char* font_color,
                                         const char* key, int type_font_size) {
    // 关键：直接输出两个兄弟节点，不包一层 mid/title 网格。
    // 外层 grid-columns: 1 使名称、类型与信号量条使用相同的列宽。
    os << "  " << key << "_name: \"";

    if (const std::string_view raw = w._name(); raw.empty())
        format_id(os, std::addressof(w));
    else
        write_quoted_escaped(os, raw);

    os << "\" {\n"
          "    shape: rectangle\n"
          "    style.fill: transparent\n"
          "    style.stroke: transparent\n"
          "    style.bold: true\n"
          "    style.font-size: 14\n"
          "    style.font-color: \""
       << font_color
       << "\"\n"
          "    height: 24\n"
          "  }\n\n"
          "  "
       << key << "_type: \"[ ";

    write_quoted_escaped(os, type_name);

    os << " ]\" {\n"
          "    shape: rectangle\n"
          "    style.fill: transparent\n"
          "    style.stroke: transparent\n"
          "    style.bold: false\n"
          "    style.font-size: "
       << type_font_size << "\n"
                            "    style.font-color: \"#6b7280\"\n"
                            "    height: 20\n"
                            "  }\n\n";
}

// ---- 公开渲染器 -----------------------------------------------------------

inline void D2Renderer::render_work(std::ostream& os, const Work& w, const char* shape,
                                    const char* fill, const char* stroke, const char* font_color,
                                    const char* border_radius, const char* stroke_dash) {
    const char* type_name = to_string(w.type());
    const auto* sd = w.m_semaphores.get();
    const bool  has_acq = sd && !sd->acquires.empty();
    const bool  has_rel = sd && !sd->releases.empty();
    const bool  is_rect = std::string_view{shape} == "rectangle";

    format_id(os, std::addressof(w));

    // 非矩形且无信号量：使用原生普通双行标签。
    // D2 普通形状的多行标签会以节点为基准居中；|md 默认左对齐。
    // 原生标签无法分别为两行指定颜色/字号，优先保证形状与居中效果。
    if (!is_rect && !has_acq && !has_rel) [[likely]] {
        os << ": \"";
        if (const std::string_view raw = w._name(); raw.empty())
            format_id(os, std::addressof(w));
        else
            write_quoted_escaped(os, raw);
        os << "\\n[ ";
        write_quoted_escaped(os, type_name);
        os << " ]\" {\n"
              "  shape: "
           << shape
           << "\n"
              "  style.fill: \""
           << fill
           << "\"\n"
              "  style.stroke: \""
           << stroke
           << "\"\n"
              "  style.font-color: \""
           << font_color
           << "\"\n"
              "  style.border-radius: "
           << border_radius << "\n";
        if (stroke_dash && stroke_dash[0]) os << "  style.stroke-dash: " << stroke_dash << '\n';
        os << "  style.font-size: 14\n}";
        return;
    }

    // 矩形（包括无信号量）及带信号量的非矩形：使用同一层 Grid。
    // 名称、类型和信号量条是同级子节点，消除内层 Grid 导致的偏左。
    // ACQ/REL 使用真正的带圆角 D2 节点，避免原生 Markdown SVG 的 HTML 错误。
    os << ": \"\" {\n"
          "  shape: "
       << shape
       << "\n"
          "  style.fill: \""
       << fill
       << "\"\n"
          "  style.stroke: \""
       << stroke
       << "\"\n"
          "  style.border-radius: "
       << border_radius << "\n";
    if (stroke_dash && stroke_dash[0]) os << "  style.stroke-dash: " << stroke_dash << '\n';
    os << "  grid-columns: 1\n  grid-gap: 4\n\n";

    if (has_acq) {
        if (is_rect)
            write_sem_pill_grid(os, sd->acquires, "acq");
        else
            write_sem_pill_row(os, sd->acquires, "acq");
    }
    // 原版无信号量矩形节点的灰色副标题继承 14px；有信号量时为 11px。
    write_title_grid(os, w, type_name, font_color, "mid", (has_acq || has_rel) ? 11 : 14);
    if (has_rel) {
        if (is_rect)
            write_sem_pill_grid(os, sd->releases, "rel");
        else
            write_sem_pill_row(os, sd->releases, "rel");
    }

    os << "}";
}

inline void D2Renderer::render_graph(std::ostream& os, const Work& w, const char* type_name,
                                     const Graph& graph) {
    if (graph.empty()) {
        render_work(os, w, "rectangle", "#e8f5e9", "#10b981", "#065f46", "8");
        return;
    }

    const auto* sd = w.m_semaphores.get();
    const bool  has_acq = sd && !sd->acquires.empty();
    const bool  has_rel = sd && !sd->releases.empty();

    format_id(os, std::addressof(w));

    // 无信号量 Graph：使用原生多行标签保证两行居中。
    // 保持 graph.dump(os) 直接位于容器内，子节点引用路径不变。
    if (!has_acq && !has_rel) [[likely]] {
        os << ": \"";
        if (const std::string_view raw = w._name(); raw.empty())
            format_id(os, std::addressof(w));
        else
            write_quoted_escaped(os, raw);
        os << "\\n[ ";
        write_quoted_escaped(os, type_name);
        os << " ]\" {\n"
              "  shape: rectangle\n"
              "  label.near: top-center\n"
              "  style.fill: \"#e8f5e9\"\n"
              "  style.stroke: \"#10b981\"\n"
              "  style.stroke-width: 2\n"
              "  style.border-radius: 14\n"
              "  style.font-color: \"#065f46\"\n"
              "  style.font-size: 14\n\n";
        graph.dump(os);
        os << "}";
        return;
    }

    // 有信号量：标题、类型、内容与信号量栏同为单列 Grid 的直接子节点。
    // 两行分别配色，同时避免二次嵌套网格使标题靠左。
    os << ": \"\" {\n"
          "  shape: rectangle\n"
          "  style.fill: \"#e8f5e9\"\n"
          "  style.stroke: \"#10b981\"\n"
          "  style.stroke-width: 2\n"
          "  style.border-radius: 14\n"
          "  grid-columns: 1\n"
          "  grid-gap: 4\n\n";

    if (has_acq) write_sem_pill_grid(os, sd->acquires, "acq");
    write_title_grid(os, w, type_name, "#065f46", "title");

    os << "  content: \"\" {\n"
          "    style.fill: transparent\n"
          "    style.stroke: transparent\n\n";
    graph.dump(os);
    os << "  }\n\n";

    if (has_rel) write_sem_pill_grid(os, sd->releases, "rel");

    os << "}";
}

} // namespace tfl
