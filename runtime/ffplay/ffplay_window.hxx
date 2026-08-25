#pragma once
#include <com/sun/star/awt/XWindow.hpp>
#include <com/sun/star/awt/Rectangle.hpp>
#include <com/sun/star/media/XPlayerWindow.hpp>
#include <com/sun/star/media/ZoomLevel.hpp>
#include <cppuhelper/implbase1.hxx>
#include <sal/types.h>
#include <X11/Xlib.h>

// PlayerWindowShell — XPlayerWindow no-op 壳 (渲染由 ffplay 引擎接管, 见 doc/ffplay-embed.md §4):
// setPosSize/setVisible/update 等均为 no-op。窗口句柄 + 尺寸来自 createPlayerWindow 的
// sequence<any> (mediawindow_impl.cxx:436-442, [0]=sal_IntPtr 父窗口句柄, [1]=awt::Rectangle)。
class PlayerWindowShell final : public cppu::WeakImplHelper1<css::media::XPlayerWindow> {
public:
    PlayerWindowShell(const char* dpy_name, sal_IntPtr parent_xw, const css::awt::Rectangle& rect);
    ~PlayerWindowShell() override;
    // XWindow 接口 (实现见 ffplay_window.cxx)
    void SAL_CALL setPosSize(sal_Int32 X, sal_Int32 Y, sal_Int32 Width, sal_Int32 Height, sal_Int16 Flags) override;
    css::awt::Rectangle SAL_CALL getPosSize() override;
    void SAL_CALL setVisible(sal_Bool) override;
    void SAL_CALL setEnable(sal_Bool) override;
    void SAL_CALL setFocus() override;
    void SAL_CALL addWindowListener(const css::uno::Reference<css::awt::XWindowListener>&) override;
    void SAL_CALL removeWindowListener(const css::uno::Reference<css::awt::XWindowListener>&) override;
    void SAL_CALL addFocusListener(const css::uno::Reference<css::awt::XFocusListener>&) override;
    void SAL_CALL removeFocusListener(const css::uno::Reference<css::awt::XFocusListener>&) override;
    void SAL_CALL addKeyListener(const css::uno::Reference<css::awt::XKeyListener>&) override;
    void SAL_CALL removeKeyListener(const css::uno::Reference<css::awt::XKeyListener>&) override;
    void SAL_CALL addMouseListener(const css::uno::Reference<css::awt::XMouseListener>&) override;
    void SAL_CALL removeMouseListener(const css::uno::Reference<css::awt::XMouseListener>&) override;
    void SAL_CALL addMouseMotionListener(const css::uno::Reference<css::awt::XMouseMotionListener>&) override;
    void SAL_CALL removeMouseMotionListener(const css::uno::Reference<css::awt::XMouseMotionListener>&) override;
    void SAL_CALL addPaintListener(const css::uno::Reference<css::awt::XPaintListener>&) override;
    void SAL_CALL removePaintListener(const css::uno::Reference<css::awt::XPaintListener>&) override;
    // XPlayerWindow 特有
    void SAL_CALL update() override;
    sal_Bool SAL_CALL setZoomLevel(css::media::ZoomLevel) override;
    css::media::ZoomLevel SAL_CALL getZoomLevel() override;
    void SAL_CALL setPointerType(sal_Int32) override;
    // XComponent (XWindow 继承链)
    void SAL_CALL dispose() override;
    void SAL_CALL addEventListener(const css::uno::Reference<css::lang::XEventListener>&) override;
    void SAL_CALL removeEventListener(const css::uno::Reference<css::lang::XEventListener>&) override;
private:
    const char* m_dpy_name = nullptr; // 诊断用 (getenv("DISPLAY"))
    ::Window m_parent = 0;            // LO 媒体子窗口 (aArgs[0], 诊断用)
    css::awt::Rectangle m_rect;
};
