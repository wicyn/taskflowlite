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
///  Low-level Writers
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
    //  Low-level writers
    //=========================================================================

    /// @brief 写入 HTML 转义后的字符串，用于 D2 markdown 块。
    static void write_html_escaped(std::ostream& os, std::string_view s);

    /// @brief 写入双引号字符串转义后的内容，用于 D2 字符串字面量。
    static void write_quoted_escaped(std::ostream& os, std::string_view s);

    /// @brief 将指针地址格式化为合法的 D2 节点 ID。
    static void format_id(std::ostream& os, const void* p);

    //=========================================================================
    //  Semaphore pill writers
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
//  Implementations
//=============================================================================

// ---- Palette ---------------------------------------------------------------

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

// ---- Low-level writers ------------------------------------------------------

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

// ---- Semaphore pill writers -------------------------------------------------

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

// ---- Public renderers -------------------------------------------------------

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






// #pragma once

// #include <expected>
// #include <utility>
// #include <span>
// #include <algorithm>
// #include <memory>
// #include <stack>
// #include <future>
// #include <cmath>
// #include <climits>

// #include "enums.hpp"
// #include "utility.hpp"
// #include "exception.hpp"
// #include "traits.hpp"
// #include "observer.hpp"
// #include "topology.hpp"
// #include "semaphore.hpp"
// #include "unordered_dense.hpp"

// namespace tfl {


// class WorkMixin : public Immovable<WorkMixin> {

// public:
//     /// @brief 静态选项配置位域（构建时确定，执行期只读）。
//     ///
//     struct Implicit {
//         using type = std::uint32_t;
//         static constexpr unsigned BITS = sizeof(type) * char_bits;

//         static constexpr type NONE      = 0;

//         // 高位 flag
//         static constexpr type ANCHORED  = type{1} << (BITS - 1);
//         static constexpr type PREEMPTED = type{1} << (BITS - 2);

//         // Join weight: 本节点对每个后继 join_counter 的贡献量 (0/1/2)
//         // 占用 [BITS-4, BITS-3] 两位
//         static constexpr unsigned WEIGHT_SHIFT = BITS - 4;
//         static constexpr type WEIGHT_MASK  = type{0b11} << WEIGHT_SHIFT;
//         static constexpr type WEIGHT_0     = type{0}    << WEIGHT_SHIFT;  // Jump / MultiJump / None
//         static constexpr type WEIGHT_1     = type{1}    << WEIGHT_SHIFT;  // default (Basic / Runtime / ...)
//         static constexpr type WEIGHT_2     = type{2}    << WEIGHT_SHIFT;  // Branch / MultiBranch
//     };

//     /// @brief 运行期状态位域（执行期通过原子指令并发修饰）。
//     struct Explicit {
//         using type = std::uint32_t;

//         static constexpr unsigned BITS       = sizeof(type) * char_bits;
//         static constexpr type NONE      = 0;

//         /// @brief 标记该节点在执行用户闭包的过程中抛出了异常。
//         static constexpr type EXCEPTION = type{1} << (BITS - 1);
//         /// @brief 标记抛出的异常已被本节点或上游的 ANCHORED 节点成功捕获归档。
//         static constexpr type CAUGHT    = type{1} << (BITS - 2);
//     };

//     /// @brief 信号量请求描述符
//     struct SemaphoreReq {
//         Semaphore* sem;
//         std::size_t count;
//     };

//     /// @brief 节点绑定的信号量集合
//     struct SemaphoreData {
//         std::vector<SemaphoreReq> acquires; ///< 执行前必须获取的约束及配额
//         std::vector<SemaphoreReq> releases; ///< 执行后应当释放的约束及配额

//         [[nodiscard]] bool empty() const noexcept {
//             return acquires.empty() && releases.empty();
//         }
//     };


//     /// @brief 节点挂载的生命周期观察者集合（按需延迟分配）。
//     struct ObserverData {
//         std::vector<std::shared_ptr<TaskObserver>> observers;

//         [[nodiscard]] bool empty() const noexcept {
//             return observers.empty();
//         }
//     };

// public:
//     explicit WorkMixin() = default;

//     virtual ~WorkMixin() noexcept = default;

// protected:

//     /// @brief 获取该节点的底层逻辑类型枚举。
//     [[nodiscard]] virtual TaskType type() const noexcept = 0;

//     /// @brief 生成该节点的可视化 D2 声明代码。
//     [[nodiscard]] virtual std::string dump() const = 0;

//     /// @brief 流式导出该节点的可视化 D2 声明代码。
//     virtual void dump(std::ostream& ostream) const = 0;

//     [[nodiscard]] std::string d2_work(const Work* w,
//                                       const char* shape, const char* fill, const char* stroke,
//                                       const char* font_color, const char* border_radius,
//                                       const char* stroke_dash = "") const;
//     [[nodiscard]] std::string d2_escape(const std::string& s) const;

//     void d2_work(std::ostream& os, const Work* w,
//                  const char* shape, const char* fill, const char* stroke,
//                  const char* font_color, const char* border_radius,
//                  const char* stroke_dash = "") const;
//     void d2_escape(std::ostream& os, const std::string& s) const;

//     /// @brief 在 |md 块 <center> 内渲染信号量 HTML 行。
//     /// @param tag "acq" 或 "rel"，标识获取或释放。

//     // ============================================================================
//     //  [2] 辅助函数实现（替换旧的颜色函数，新增 HTML 生成 + 引号转义）
//     // ============================================================================

//     // ---- D2 引号字符串转义（grid pill 标签专用） ----

//     /// @brief D2 双引号字符串转义：仅处理 `"` `\`，UTF-8 原样透传。
//     /// @details
//     /// **与 d2_escape 的区别**：d2_escape 面向 |md ... | 块内的 HTML 上下文，
//     /// 将 `<>&"` 转为 HTML 实体。而 pill 标签是 D2 引号字符串，
//     /// HTML 实体会被原样显示，因此必须使用 D2 自身的转义规则。
//     [[nodiscard]] std::string d2_str_escape(const std::string& s) const;

//     void d2_str_escape(std::ostream& os, const std::string& s) const;

//     // ---- 地址 → 唯一颜色 ----

//     [[nodiscard]] std::uint8_t hsl_channel(float p, float q, float t) const noexcept;

//     void hsl_to_hex(float h, float s, float l, char* buf) const noexcept;

//     /// @brief 信号量 pill 配色三元组
//     struct _D2SemPillColor {
//         char bg[8];   ///< 暗底色（grid pill 背景）
//         char fg[8];   ///< 亮字色（grid pill 前景，暗底上）
//         char md[8];   ///< 中彩色（|md 块内文字，浅底上高对比度）
//     };

//     /// @brief 信号量地址 → 唯一配色三元组
//     ///
//     /// @details
//     /// 地址经 Murmur-finish 散列 + 黄金比例旋转，在 360° 色轮上拉开最大间距。
//     /// - bg: S=0.45, L=0.17 — grid pill 暗底
//     /// - fg: S=0.80, L=0.72 — grid pill 亮字
//     /// - md: S=0.65, L=0.40 — |md 块内文字（浅色节点背景上高可读性）
//     _D2SemPillColor sem_addr_color(const Semaphore* sem) const noexcept;

//     void d2_sem_html(std::string& out, std::span<SemaphoreReq const> reqs, const char* tag) const;
//     void d2_sem_html(std::ostream& os, std::span<SemaphoreReq const> reqs, const char* tag) const;


//     // ---- 地址 → 唯一颜色 ----

//     [[nodiscard]]  std::uint8_t hsl_ch(float p, float q, float t) const noexcept;
//     void hsl_hex(float h, float s, float l, char* buf) const noexcept;

//     void d2_pill_bar(std::string& out,
//                      std::span<SemaphoreReq const> reqs, const char* tag) const;
//     void d2_pill_bar(std::ostream& os,
//                      std::span<SemaphoreReq const> reqs, const char* tag) const;

//     /// @brief |md 块内渲染信号量 HTML 行（非 rectangle 节点专用）。
//     void d2_sem_lines(std::string& out,
//                       std::span<SemaphoreReq const> reqs, const char* tag) const;
//     void d2_sem_lines(std::ostream& os,
//                       std::span<SemaphoreReq const> reqs, const char* tag) const;

//     struct _D2SemColor { char bg[8]; char fg[8]; char md[8]; };
//     [[nodiscard]]  _D2SemColor sem_color(const Semaphore* sem) const noexcept;

// };


// inline std::string WorkMixin::d2_escape(const std::string& s) const {
//     std::string out;
//     out.reserve(s.size());
//     for (char c : s) {
//         switch (c) {
//         case '"':  out += "&quot;"; break;
//         case '&':  out += "&amp;";  break;
//         case '<':  out += "&lt;";   break;
//         case '>':  out += "&gt;";   break;
//         case '\\': out += "\\\\";   break;
//         default:   out += c;
//         }
//     }
//     return out;
// }

// inline void WorkMixin::d2_escape(std::ostream& os, const std::string& s) const {
//     for (char c : s) {
//         switch (c) {
//         case '"':  os << "&quot;"; break;
//         case '&':  os << "&amp;";  break;
//         case '<':  os << "&lt;";   break;
//         case '>':  os << "&gt;";   break;
//         case '\\': os << "\\\\";   break;
//         default:   os << c;
//         }
//     }
// }

// /// @brief 在 |md 块 <center> 内渲染信号量 HTML 行。
// /// @param tag "acq" 或 "rel"，标识获取或释放。

// // ============================================================================
// //  [2] 辅助函数实现（替换旧的颜色函数，新增 HTML 生成 + 引号转义）
// // ============================================================================

// // ---- D2 引号字符串转义（grid pill 标签专用） ----

// /// @brief D2 双引号字符串转义：仅处理 `"` `\`，UTF-8 原样透传。
// /// @details
// /// **与 d2_escape 的区别**：d2_escape 面向 |md ... | 块内的 HTML 上下文，
// /// 将 `<>&"` 转为 HTML 实体。而 pill 标签是 D2 引号字符串，
// /// HTML 实体会被原样显示，因此必须使用 D2 自身的转义规则。
// inline std::string WorkMixin::d2_str_escape(const std::string& s) const {
//     std::string out;
//     out.reserve(s.size());
//     for (char c : s) {
//         switch (c) {
//         case '"':  out += "\\\""; break;
//         case '\\': out += "\\\\"; break;
//         default:   out += c;
//         }
//     }
//     return out;
// }

// inline void WorkMixin::d2_str_escape(std::ostream& os, const std::string& s) const {
//     for (char c : s) {
//         switch (c) {
//         case '"':  os << "\\\""; break;
//         case '\\': os << "\\\\"; break;
//         default:   os << c;
//         }
//     }
// }


// // ---- 地址 → 唯一颜色 ----

// inline std::uint8_t WorkMixin::hsl_channel(float p, float q, float t) const noexcept {
//     if (t < 0.f) t += 1.f;
//     if (t > 1.f) t -= 1.f;
//     float v;
//     if (t < 1.f / 6.f)      v = p + (q - p) * 6.f * t;
//     else if (t < 1.f / 2.f) v = q;
//     else if (t < 2.f / 3.f) v = p + (q - p) * (2.f / 3.f - t) * 6.f;
//     else                     v = p;
//     return static_cast<std::uint8_t>(v * 255.f + 0.5f);
// }

// inline void WorkMixin::hsl_to_hex(float h, float s, float l, char* buf) const noexcept {
//     float q = (l < 0.5f) ? l * (1.f + s) : l + s - l * s;
//     float p = 2.f * l - q;
//     auto r = hsl_channel(p, q, h + 1.f / 3.f);
//     auto g = hsl_channel(p, q, h);
//     auto b = hsl_channel(p, q, h - 1.f / 3.f);
//     std::snprintf(buf, 8, "#%02x%02x%02x", r, g, b);
// }

// /// @brief 信号量地址 → 唯一配色三元组
// ///
// /// @details
// /// 地址经 Murmur-finish 散列 + 黄金比例旋转，在 360° 色轮上拉开最大间距。
// /// - bg: S=0.45, L=0.17 — grid pill 暗底
// /// - fg: S=0.80, L=0.72 — grid pill 亮字
// /// - md: S=0.65, L=0.40 — |md 块内文字（浅色节点背景上高可读性）
// inline WorkMixin::_D2SemPillColor WorkMixin::sem_addr_color(const Semaphore* sem) const noexcept {
//     constexpr float PHI_INV = 0.6180339887498949f;
//     auto h = reinterpret_cast<std::uintptr_t>(sem);
//     h ^= h >> 16;
//     h *= 0x45d9f3bu;
//     h ^= h >> 16;

//     float hue = static_cast<float>(h & 0xFFFFu) / 65536.f;
//     hue = std::fmod(hue * PHI_INV, 1.f);

//     _D2SemPillColor c{};
//     hsl_to_hex(hue, 0.45f, 0.17f, c.bg);
//     hsl_to_hex(hue, 0.80f, 0.72f, c.fg);
//     hsl_to_hex(hue, 0.65f, 0.40f, c.md);
//     return c;
// }


// // ---- |md 块内信号量 HTML 行生成 ----

// /// @brief 为 |md 块 <center> 内生成信号量彩色标记行。
// /// @details 每个 SemaphoreReq 生成一行：
// /// `<span style="color:#MDCOLOR; font-size:9px;">● name · max:N · tag:M</span><br/>`
// /// 用于非 rectangle 节点（diamond/hexagon）和 Subflow 容器标题，
// /// 这些场景不能使用 grid 布局，只能在 md 块内嵌 HTML。
// inline void WorkMixin::d2_sem_html(std::string& out, std::span<SemaphoreReq const> reqs, const char* tag) const {
//     for (const auto& req : reqs) {
//         auto c = sem_addr_color(req.sem);
//         out += "  <span style=\"color:";
//         out += c.md;
//         out += "; font-size:9px;\">&#x25CF; ";
//         out += d2_escape(std::string(req.sem->name()));
//         out += " \xC2\xB7 max:";
//         out += std::to_string(req.sem->max_value());
//         out += " \xC2\xB7 ";
//         out += tag;
//         out += ":";
//         out += std::to_string(req.count);
//         out += "</span><br/>\n";
//     }
// }

// inline void WorkMixin::d2_sem_html(std::ostream& os, std::span<SemaphoreReq const> reqs, const char* tag) const {
//     for (const auto& req : reqs) {
//         auto c = sem_addr_color(req.sem);
//         os << "  <span style=\"color:" << c.md
//            << "; font-size:9px;\">&#x25CF; ";
//         d2_escape(os, std::string(req.sem->name()));
//         os << " \xC2\xB7 max:" << req.sem->max_value()
//            << " \xC2\xB7 " << tag << ":" << req.count
//            << "</span><br/>\n";
//     }
// }


// // ---- 地址 → 唯一颜色 ----

// inline std::uint8_t WorkMixin::hsl_ch(float p, float q, float t) const noexcept {
//     if (t < 0.f) t += 1.f;
//     if (t > 1.f) t -= 1.f;
//     float v;
//     if (t < 1.f / 6.f)      v = p + (q - p) * 6.f * t;
//     else if (t < 1.f / 2.f) v = q;
//     else if (t < 2.f / 3.f) v = p + (q - p) * (2.f / 3.f - t) * 6.f;
//     else                     v = p;
//     return static_cast<std::uint8_t>(v * 255.f + 0.5f);
// }

// inline void WorkMixin::hsl_hex(float h, float s, float l, char* buf) const noexcept {
//     float q = (l < 0.5f) ? l * (1.f + s) : l + s - l * s;
//     float p = 2.f * l - q;
//     std::snprintf(buf, 8, "#%02x%02x%02x",
//                   hsl_ch(p, q, h + 1.f / 3.f),
//                   hsl_ch(p, q, h),
//                   hsl_ch(p, q, h - 1.f / 3.f));
// }

// inline WorkMixin::_D2SemColor WorkMixin::sem_color(const Semaphore* sem) const noexcept {
//     constexpr float PHI_INV = 0.6180339887498949f;
//     auto v = reinterpret_cast<std::uintptr_t>(sem);
//     v ^= v >> 16; v *= 0x45d9f3bu; v ^= v >> 16;
//     float hue = std::fmod(static_cast<float>(v & 0xFFFFu) / 65536.f * PHI_INV, 1.f);

//     _D2SemColor c{};
//     hsl_hex(hue, 0.45f, 0.17f, c.bg);   // grid pill 暗底
//     hsl_hex(hue, 0.80f, 0.72f, c.fg);   // grid pill 亮字
//     hsl_hex(hue, 0.70f, 0.38f, c.md);   // |md 块内文字（浅底上高对比度）
//     return c;
// }


// // ============================================================================
// //  标签格式：acq:[sem_XXXX] [count/max] [name]
// // ============================================================================


// // ---- grid pill bar（rectangle 节点 + Subflow 外壳专用）----

// inline void WorkMixin::d2_pill_bar(std::string& out,
//                                    std::span<SemaphoreReq const> reqs, const char* tag) const
// {
//     char bar[8];
//     std::snprintf(bar, sizeof(bar), "%c_bar", tag[0]);

//     out += "  "; out += bar; out += ": \"\" {\n";
//     out += "    style.fill: transparent\n";
//     out += "    style.stroke: transparent\n";
//     out += "    grid-rows: 1\n";
//     out += "    grid-gap: 6\n\n";

//     char pid[24];
//     char sid[32];
//     for (std::size_t i = 0; i < reqs.size(); ++i) {
//         std::snprintf(pid, sizeof(pid), "%c%zu", tag[0], i);
//         std::snprintf(sid, sizeof(sid), "sem_%zx", reinterpret_cast<std::uintptr_t>(reqs[i].sem));
//         auto [bg, fg, md] = sem_color(reqs[i].sem);

//         out += "    "; out += pid;
//         out += ": \""; out += tag;
//         out += ":[";   out += sid;
//         out += "] [";  out += std::to_string(reqs[i].count);
//         out += "/";    out += std::to_string(reqs[i].sem->max_value());
//         out += "] [";  out += d2_str_escape(std::string(reqs[i].sem->name()));
//         out += "]\" {\n";
//         out += "      shape: rectangle\n";
//         out += "      style.border-radius: 12\n";
//         out += "      style.fill: \"";       out += bg; out += "\"\n";
//         out += "      style.font-color: \""; out += fg; out += "\"\n";
//         out += "      style.stroke: transparent\n";
//         out += "      style.font-size: 10\n";
//         out += "      height: 20\n";
//         out += "    }\n";
//     }
//     out += "  }\n\n";
// }

// inline void WorkMixin::d2_pill_bar(std::ostream& os,
//                                    std::span<SemaphoreReq const> reqs, const char* tag) const
// {
//     char bar[8];
//     std::snprintf(bar, sizeof(bar), "%c_bar", tag[0]);

//     os << "  " << bar << ": \"\" {\n";
//     os << "    style.fill: transparent\n";
//     os << "    style.stroke: transparent\n";
//     os << "    grid-rows: 1\n";
//     os << "    grid-gap: 6\n\n";

//     char pid[24];
//     char sid[32];
//     for (std::size_t i = 0; i < reqs.size(); ++i) {
//         std::snprintf(pid, sizeof(pid), "%c%zu", tag[0], i);
//         std::snprintf(sid, sizeof(sid), "sem_%zx", reinterpret_cast<std::uintptr_t>(reqs[i].sem));
//         auto [bg, fg, md] = sem_color(reqs[i].sem);

//         os << "    " << pid << ": \""
//            << tag << ":[" << sid
//            << "] [" << reqs[i].count
//            << "/" << reqs[i].sem->max_value()
//            << "] [";
//         d2_str_escape(os, std::string(reqs[i].sem->name()));
//         os << "]\" {\n";
//         os << "      shape: rectangle\n";
//         os << "      style.border-radius: 12\n";
//         os << "      style.fill: \"" << bg << "\"\n";
//         os << "      style.font-color: \"" << fg << "\"\n";
//         os << "      style.stroke: transparent\n";
//         os << "      style.font-size: 10\n";
//         os << "      height: 20\n";
//         os << "    }\n";
//     }
//     os << "  }\n\n";
// }


// // ---- |md 块内信号量 pill 行（diamond/hexagon 节点专用，水平排列）----

// inline void WorkMixin::d2_sem_lines(std::string& out,
//                                     std::span<SemaphoreReq const> reqs, const char* tag) const
// {
//     char sid[32];
//     for (std::size_t i = 0; i < reqs.size(); ++i) {
//         std::snprintf(sid, sizeof(sid), "sem_%zx", reinterpret_cast<std::uintptr_t>(reqs[i].sem));
//         auto [bg, fg, md] = sem_color(reqs[i].sem);

//         out += "  <span style=\"";
//         out += "background-color:"; out += bg;
//         out += "; color:";          out += fg;
//         out += "; border-radius:8px";
//         out += "; padding:1px 6px";
//         out += "; font-size:9px";
//         out += ";\">";
//         out += tag;
//         out += ":[";
//         out += sid;
//         out += "] [";
//         out += std::to_string(reqs[i].count);
//         out += "/";
//         out += std::to_string(reqs[i].sem->max_value());
//         out += "] [";
//         out += d2_escape(std::string(reqs[i].sem->name()));
//         out += "]</span> ";
//     }
//     out += "<br/>\n";
// }

// inline void WorkMixin::d2_sem_lines(std::ostream& os,
//                                     std::span<SemaphoreReq const> reqs, const char* tag) const
// {
//     char sid[32];
//     for (std::size_t i = 0; i < reqs.size(); ++i) {
//         std::snprintf(sid, sizeof(sid), "sem_%zx", reinterpret_cast<std::uintptr_t>(reqs[i].sem));
//         auto [bg, fg, md] = sem_color(reqs[i].sem);

//         os << "  <span style=\""
//            << "background-color:" << bg
//            << "; color:"          << fg
//            << "; border-radius:8px"
//            << "; padding:1px 6px"
//            << "; font-size:9px"
//            << ";\">"
//            << tag << ":[" << sid
//            << "] [" << reqs[i].count
//            << "/" << reqs[i].sem->max_value()
//            << "] [";
//         d2_escape(os, std::string(reqs[i].sem->name()));
//         os << "]</span> ";
//     }
//     os << "<br/>\n";
// }


// }  // namespace tfl
