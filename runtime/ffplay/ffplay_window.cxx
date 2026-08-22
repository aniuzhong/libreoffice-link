// ffplay_window.cxx — XPlayerWindow no-op 壳 (经验 34 起: 渲染由 ffplay 引擎
// SDL_CreateWindowFrom(LO 媒体子窗口) 接管, LO 侧 XPlayerWindow 仅承接接口调用)
#include "ffplay_window.hxx"
#include "ffplay_log.h"  // FFLOG_*

PlayerWindowShell::PlayerWindowShell(const char* dpy_name, sal_IntPtr parent_xw,
                         const css::awt::Rectangle& rect)
    : m_dpy_name(dpy_name), m_parent(static_cast<::Window>(parent_xw)), m_rect(rect) {
    (void)m_dpy_name; (void)m_parent;
    FFLOG_INFO("[FFPLAY] PlayerWindow shell parent=0x%lx rect=%dx%d@(%d,%d)",
               (unsigned long)m_parent,
               (int)rect.Width, (int)rect.Height, (int)rect.X, (int)rect.Y);
}

PlayerWindowShell::~PlayerWindowShell() {
    // 引擎窗口 (SDL_CreateWindowFrom) 随引擎销毁, 无需额外清理
}

void PlayerWindowShell::setPosSize(sal_Int32 X, sal_Int32 Y, sal_Int32 Width, sal_Int32 Height, sal_Int16 Flags) {
    (void)X; (void)Y; (void)Width; (void)Height; (void)Flags;
    // no-op: 引擎在 LO 子窗口内自渲染, LO 的 resize 由 X 窗口链传导
}
css::awt::Rectangle PlayerWindowShell::getPosSize() { return m_rect; }
void PlayerWindowShell::setVisible(sal_Bool bVisible) { (void)bVisible; }
void PlayerWindowShell::setEnable(sal_Bool) {}
void PlayerWindowShell::setFocus() {}
void PlayerWindowShell::addWindowListener(const css::uno::Reference<css::awt::XWindowListener>&) {}
void PlayerWindowShell::removeWindowListener(const css::uno::Reference<css::awt::XWindowListener>&) {}
void PlayerWindowShell::addFocusListener(const css::uno::Reference<css::awt::XFocusListener>&) {}
void PlayerWindowShell::removeFocusListener(const css::uno::Reference<css::awt::XFocusListener>&) {}
void PlayerWindowShell::addKeyListener(const css::uno::Reference<css::awt::XKeyListener>&) {}
void PlayerWindowShell::removeKeyListener(const css::uno::Reference<css::awt::XKeyListener>&) {}
void PlayerWindowShell::addMouseListener(const css::uno::Reference<css::awt::XMouseListener>&) {}
void PlayerWindowShell::removeMouseListener(const css::uno::Reference<css::awt::XMouseListener>&) {}
void PlayerWindowShell::addMouseMotionListener(const css::uno::Reference<css::awt::XMouseMotionListener>&) {}
void PlayerWindowShell::removeMouseMotionListener(const css::uno::Reference<css::awt::XMouseMotionListener>&) {}
void PlayerWindowShell::addPaintListener(const css::uno::Reference<css::awt::XPaintListener>&) {}
void PlayerWindowShell::removePaintListener(const css::uno::Reference<css::awt::XPaintListener>&) {}
void PlayerWindowShell::update() {}
sal_Bool PlayerWindowShell::setZoomLevel(css::media::ZoomLevel) { return true; }
css::media::ZoomLevel PlayerWindowShell::getZoomLevel() { return css::media::ZoomLevel_ZOOM_1_TO_2; }
void PlayerWindowShell::setPointerType(sal_Int32) {}
void PlayerWindowShell::dispose() {}
void PlayerWindowShell::addEventListener(const css::uno::Reference<css::lang::XEventListener>&) {}
void PlayerWindowShell::removeEventListener(const css::uno::Reference<css::lang::XEventListener>&) {}
