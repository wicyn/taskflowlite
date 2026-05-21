/// @file d2_render.hpp
/// @brief D2 可视化渲染器 D2Renderer —— Work / Graph 的图形导出
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-04-19
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
/// @details
/// `D2Renderer` 是框架的可视化后端，把 `Work` 节点和 `Graph` 任务图渲染为
/// **D2** 文本描述（D2 是 Terrastruct 出的现代图描述语言，类似 PlantUML
/// 但语法更现代）。生成的 .d2 文本可用 `d2` 命令行工具一键转 SVG / PNG。
///
/// ============================================================================
///  为什么要把渲染单独拎出来？
/// ============================================================================
/// 渲染逻辑触及 Work / Graph 的几乎所有内部字段（name / type / edges /
/// semaphores / observers）。如果让这些字段的访问方法都 public 化，会泄露
/// 内部 ABI；让 dump() 散落在每个 Work 子类里，又会产生大量重复代码。
///
/// 解法：**单一友元类聚合所有渲染逻辑**：
/// - `D2Renderer` 是 `Work` / `Graph` / `Semaphore` 的 friend；
/// - 内部直读 protected/private 字段；
/// - 用户只需调 `Work::dump(os)` / `Graph::dump(os)`，背后转发到本类。
///
/// 这是"友元类做横切关注点"的经典用法 —— 比把所有访问器 public 化干净得多。
///
/// ============================================================================
///  核心入口 —— 仅两个 public API
/// ============================================================================
/// - **`render_work(...)`** ：渲染单个节点，按 shape 与信号量存在性走两条路径：
///   - 无信号量 / 非 rectangle → `|md ... |` markdown 块（D2 的内嵌 markdown）；
///   - rectangle + 信号量      → grid pill bar（横向药丸条）。
///
/// - **`render_graph(...)`** ：渲染嵌套 Flow 的容器节点（Subflow / DepFlow）。
///
/// ============================================================================
///  Palette —— HSL 黄金比例哈希配色
/// ============================================================================
/// 信号量的可视化"药丸"颜色由 sem 地址决定：
/// 1. 把 sem 指针 hash 到 HSL 色相（hue）；
/// 2. 用黄金比例（0.618...）旋转 hue，让相邻 sem 的颜色尽量分散；
/// 3. 转换 HSL → hex，作为 pill 背景 / 字色。
///
/// 收益：**同一信号量在所有节点上颜色一致**（地址恒定），不同 sem 的颜色尽量
/// 远（金分布），用户能一眼看出"这几个节点占用同一资源"。
///
/// ============================================================================
///  底层写入器
/// ============================================================================
/// - `write_html_escaped`  ：转义 `<>&"` 等字符（节点名出现在 markdown 块里时安全）；
/// - `write_quoted_escaped`：转义 `"` 和 `\`（用作 D2 字符串字面量时）；
/// - `format_id`           ：把指针格式化为 `pXXXX`（D2 标识符需以字母开头，
///                          直接用地址会以数字开头）。
///
/// 这一层小工具看似无聊，实则是渲染 robust 的底座 —— 用户随便取个含 `<` 的
/// 节点名也不会破坏输出。
///
/// ============================================================================
///  设计风格 —— 全 static，无状态
/// ============================================================================
/// `D2Renderer` 不持有任何成员 —— 所有方法都是 static。把它定义为类（而不是
/// namespace 函数集合）的唯一理由是 **friend 声明的便利** ：
/// `friend class D2Renderer;` 一句搞定整个组件的访问授权。
///
/// @see Work::dump / Graph::dump  转发入口
/// @see Flow::dump                顶层用户入口（带 root 容器）
class D2Renderer {
public:
    /// @brief 信号量请求只读视图。
    using SemReqs = std::span<const Work::SemaphoreReq>;

    //=========================================================================
    //  Palette — HSL → hex，hue 由 semaphore 地址黄金比例旋转得到
    //=========================================================================

    /// @brief D2 渲染时使用的信号量配色。
    struct Palette {
        char bg[8];   ///< pill 暗底色
        char fg[8];   ///< pill 亮字色
        char md[8];   ///< |md 块内文字色
    };

    /// @brief 计算 HSL 转 RGB 过程中的单个颜色通道。
    [[nodiscard]] static std::uint8_t format_hsl_component(float p, float q, float t) noexcept;

    /// @brief 将 HSL 颜色格式化为 `#rrggbb` 十六进制字符串。
    static void format_hsl_hex(float h, float s, float l, char* out) noexcept;

    /// @brief 根据 Semaphore 地址生成稳定配色。
    [[nodiscard]] static Palette format_palette(const Semaphore* sem) noexcept;

    //=========================================================================
    //  底层写入器
    //=========================================================================

    /// @brief 写入 HTML 转义后的字符串，用于 D2 markdown 块。
    static void write_html_escaped(std::ostream& os, std::string_view s);

    /// @brief 写入双引号字符串转义后的内容，用于 D2 字符串字面量。
    static void write_quoted_escaped(std::ostream& os, std::string_view s);

    /// @brief 将指针地址格式化为合法的 D2 节点 ID。
    static void format_id(std::ostream& os, const void* p);

    //=========================================================================
    //  信号量药丸写入器
    //=========================================================================

    /// @brief 写入 grid 布局的信号量 pill bar，供 rectangle 外壳节点使用。
    static void write_sem_pill_grid(std::ostream& os, SemReqs reqs, const char* tag);

    /// @brief 写入 markdown 内联信号量 pill 行，供 diamond / hexagon 等节点使用。
    static void write_sem_pill_row(std::ostream& os, SemReqs reqs, const char* tag);

    /// @brief 渲染单个 Work 节点。
    ///
    /// @details 依据 shape 与信号量存在性走两条路径：
    ///   - 无信号量 / 非 rectangle → `|md ... |` 块；
    ///   - rectangle + 信号量      → grid pill bar。
    static void render_work(std::ostream& os, const Work* w,
                            const char* shape,
                            const char* fill, const char* stroke,
                            const char* font_color, const char* border_radius,
                            const char* stroke_dash = "");

    /// @brief 渲染内嵌 Flow 的容器节点，Subflow / DepFlow 通用。
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
    constexpr float PHI_INV = 0.6180339887498949f;
    auto v = reinterpret_cast<std::uintptr_t>(sem);
    v ^= v >> 16; v *= 0x45d9f3bu; v ^= v >> 16;
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

    char sid[32];
    for (std::size_t i = 0; i < reqs.size(); ++i) {
        const auto& req = reqs[i];
        std::snprintf(sid, sizeof(sid), "sem_%zx",
                      reinterpret_cast<std::uintptr_t>(req.sem));
        const auto pal = format_palette(req.sem);

        os << "    " << tag[0] << i << ": \""
           << tag << ":[" << sid
           << "] [" << req.count << '/' << req.sem->max_value()
           << "] [";
        write_quoted_escaped(os, req.sem->name());
        os << "]\" {\n"
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
    char sid[32];
    for (const auto& req : reqs) {
        std::snprintf(sid, sizeof(sid), "sem_%zx",
                      reinterpret_cast<std::uintptr_t>(req.sem));
        const auto pal = format_palette(req.sem);

        os << "  <span style=\"background-color:" << pal.bg
           << "; color:"           << pal.fg
           << "; border-radius:8px"
              "; padding:1px 6px"
              "; font-size:9px;\">"
           << tag << ":[" << sid
           << "] [" << req.count << '/' << req.sem->max_value()
           << "] [";
        write_html_escaped(os, req.sem->name());
        os << "]</span> ";
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

    // Path A — |md 块（无信号量 或 非矩形）
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

    // Path B — rectangle + 信号量：grid pill bar
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

    // Path A — 无信号量：|md 块内嵌子图
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

    // Path B — 有信号量：grid(title + content)
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
