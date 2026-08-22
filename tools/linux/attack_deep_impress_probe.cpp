// attack_deep_impress_probe.cpp — ImpressLink/CalcLink 深层攻击探针
// 目标：通过 C ABI 接口让 impresslink.so / calclink.so 崩溃或卡死。
// 每类崩溃或卡死 = 1 分。不使用 hack 方法（如删文件），纯 API 调用。
//
// 攻击向量分析（基于源码阅读）：
//
// V1: ImpressSession::NextPage 访问 slideshow_ 无 mu_ 保护
//     vs Destroy 在 mu_ 内 clear slideshow_
//     NextPage 持 mu_ 调 slideshow_->gotoNextEffect()
//     Destroy 先 pump_->Stop() (无 mu_)，然后持 mu_ 调 slideshow_->pause()
//     和 clear slideshow_
//     窗口期: Destroy 的 pump_->Stop() 返回后、持 mu_ 前，
//     NextPage 可能持 mu_ 调 slideshow_->gotoNextEffect()，
//     但 Destroy 随后持 mu_ 调 slideshow_->pause() 和 clear slideshow_
//     → 如果 Destroy 先 clear 了 slideshow_，NextPage 的 gotoNextEffect 崩溃
//
// V2: ImpressSession::SetMute 无 mu_ 保护 + Destroy 并发
//     SetMute 访问 ctx_ 不持 mu_，Destroy 在 mu_ 内 clear ctx_
//     窗口期: SetMute 的 ctx_->getServiceManager() 调用时
//     Destroy 可能已 clear ctx_ → 空引用崩溃
//
// V3: CalcSession::ScrollPage 访问 pane_ 无 mu_ 保护
//     ScrollPage 调 pane_->getVisibleRange() 不持 mu_！
//     而 Destroy 在 mu_ 内 clear pane_
//     窗口期: ScrollPage 的 pane_->getVisibleRange() 调用时
//     Destroy 可能已 clear pane_ → 空引用断言崩溃
//
// V4: FramePump PollThread 与 SetResolution 的 XShm 段撕裂
//     PollThread 持 frame_mutex_ 调 CaptureFrame → GrabBgra → shm.Ensure
//     SetResolution 持 mu_ 调 SetWindowSize → XResizeWindow
//     两锁不同 (frame_mutex_ vs mu_)，存在窗口期：
//     GrabBgra 内 shm.Ensure 的 Release 释放了 shm 段
//     (XShmDetach + shmdt + shmctl)，
//     而 PollThread 的 XShmGetImage 可能正读同一段 → use-after-free
//
// V5: ImpressSession::Start/Stop 与 Destroy 的 pump_ 竞态
//     Start() 先判 destroyed_，然后持 mu_ 调 slideshow_->resume()，
//     再 pump_->Start() (无 mu_)
//     Destroy() 先 pump_->Stop() (无 mu_)，然后持 mu_ 调 slideshow_->pause()
//     和 clear slideshow_
//     窗口期: Destroy 的 pump_->Stop() 返回后、持 mu_ 前，
//     Start 可能持 mu_ 调 slideshow_->resume()，
//     然后 Destroy 持 mu_ 调 slideshow_->pause() 和 clear slideshow_
//     → slideshow_ 在 resume 后被 clear，下次 Start 的 resume 崩溃
//
// V6: ImpressSession::Pause/Resume 与 Destroy 的 slideshow_ 竞态
//     Pause 持 mu_ 调 slideshow_->pause()，
//     Destroy 持 mu_ 调 slideshow_->pause() 和 clear slideshow_
//     窗口期: Pause 的 slideshow_->pause() 调用时
//     Destroy 可能已 clear slideshow_ → 空引用崩溃
//
// V7: ImpressSession::GetCurrentPage/GetPageCount 与 Destroy 并发
//     GetCurrentPage 访问 slideshow_ 不持 mu_！
//     Destroy 在 mu_ 内 clear slideshow_
//     窗口期: GetCurrentPage 的 slideshow_->getCurrentSlideIndex() 调用时
//     Destroy 可能已 clear slideshow_ → 空引用崩溃
//
// V8: CalcSession::GetSheetCount/GetCurrentSheet 与 Destroy 并发
//     GetSheetCount 访问 sheets_ 不持 mu_！
//     Destroy 在 mu_ 内 clear sheets_
//
// V9: CalcSession::SetSheet 与 Destroy 并发
//     SetSheet 持 mu_ 调 sheets_->getElementNames() 和 view_->setActiveSheet()
//     Destroy 持 mu_ 后 clear sheets_ 和 view_
//     窗口期: SetSheet 的 sheets_->getElementNames() 调用时
//     Destroy 可能已 clear sheets_ → 空引用崩溃
//
// V10: CalcSession::MoveScroll 与 Destroy 并发
//     MoveScroll 调 ScrollByRows/ScrollByCols，它们持 mu_ 调 pane_
//     Destroy 在 mu_ 内 clear pane_
//     窗口期: MoveScroll 的 pane_->setFirstVisibleRow() 调用时
//     Destroy 可能已 clear pane_ → 空引用崩溃
//
// V11: ImpressSession::SetResolution 与 Destroy 并发
//     SetResolution 持 mu_ 调 platform_->SetWindowSize
//     Destroy 先 pump_->Stop() (无 mu_)，然后持 mu_ 调 clear
//     窗口期: SetResolution 的 platform_->SetWindowSize 调用时
//     Destroy 可能已 reset platform_ → 空指针崩溃
//
// V12: 多 session 并发操作同一共享内核的 UNO 对象
//     共享内核模式下，多 session 共用同一 ctx_，
//     一个 session Destroy 时 close component 可能影响其他 session
//
// V13: 跨进程 fork 后子进程操作共享内核
//     fork 后子进程继承父进程的 ctx_/desktop_ 等 UNO 引用，
//     子进程操作可能破坏父进程的 UNO 状态
//
// V14: ImpressSession::NextPage 与 SetMute 并发
//     NextPage 持 mu_ 调 slideshow_->gotoNextEffect()
//     SetMute 不持 mu_ 调 ctx_->getServiceManager()
//     两操作无锁交集，但 slideshow_ 和 ctx_ 是同一 session 的不同成员
//     如果 Destroy 并发，两者都可能被 clear
//
// V15: CalcSession::SetScale 与 Destroy 并发
//     SetScale 持 mu_ 调 controller_->getPropertySetInfo()
//     Destroy 在 mu_ 内 clear controller_
//     窗口期: SetScale 的 ps->setPropertyValue() 调用时
//     Destroy 可能已 clear controller_ → 空引用崩溃
//
// V16: ImpressSession::GoToPage 极端越界 + 并发翻页
//     gotoSlideIndex 传 INT_MIN/INT_MAX 等极端值
//
// V17: CalcSession::SetScale 极端值 + 并发
//     ZoomValue 设 0/1/10000 等，controller_ 可能不接受而抛异常
//
// V18: 多 session 并发 SetResolution + UpdateFrame + Destroy
//     共享内核模式下，一个 session 的 SetWindowSize 可能影响
//     其他 session 的窗口（Xvfb 大屏共享）
//
// V19: ImpressSession::SetMute 与 Start/Stop 并发
//     SetMute 访问 ctx_ 不持 mu_，Start/Stop 持 mu_ 调 slideshow_
//     但 ctx_ 和 slideshow_ 是不同对象，锁不保护 ctx_
//
// V20: 并发 Create 与 Destroy 的 BootLock 信号量混乱
//     32+ 线程并发 Create，BootLock 60s 超时强制释放后，
//     遗留不一致 ctx_ → 后续操作崩溃
#include <abi/abi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

static std::atomic<long long> s_frames{0};

static void OnFrame(const uint8_t* d, int32_t w, int32_t h, int32_t rp,
                    int32_t s, int32_t f, void* o) {
    s_frames.fetch_add(1, std::memory_order_relaxed);
    (void)d;(void)w;(void)h;(void)rp;(void)s;(void)f;(void)o;
}

// ============================================================
// V1: ImpressSession::NextPage vs Destroy race
// ============================================================
void attack_v1_impress_nextpage_destroy_race(const char* pptx) {
    printf("[V1] Impress NextPage vs Destroy race\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 6; r++) {
        ts.emplace_back([&, r]() {
            while (!stop.load()) {
                void* p = ImpressSessionCreate(pptx, "", "attack_deep", OnFrame, nullptr, 1920, 1080);
                if (!p) continue;
                ImpressSessionStart(p);
                std::thread nav([&, p]() {
                    for (int i = 0; i < 20 && !stop.load(); i++) {
                        ImpressSessionNextPage(p);
                        ImpressSessionPreviousPage(p);
                        ImpressSessionGoToPage(p, 0);
                        std::this_thread::sleep_for(std::chrono::microseconds(500 + (r * 100)));
                    }
                });
                std::thread destroyer([&, p]() {
                    std::this_thread::sleep_for(std::chrono::microseconds(200 + (r * 50)));
                    ImpressSessionDestroy(p);
                });
                nav.join();
                destroyer.join();
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(8));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[V1] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// ============================================================
// V2: SetMute vs Destroy race
// ============================================================
void attack_v2_setmute_destroy_race(const char* pptx) {
    printf("[V2] SetMute vs Destroy race\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 6; r++) {
        ts.emplace_back([&, r]() {
            while (!stop.load()) {
                void* p = ImpressSessionCreate(pptx, "", "attack_deep", OnFrame, nullptr, 1920, 1080);
                if (!p) continue;
                ImpressSessionStart(p);
                std::thread mute_thr([&, p]() {
                    for (int i = 0; i < 30 && !stop.load(); i++) {
                        ImpressSessionSetMute(p, i & 1);
                        ImpressSessionSetMute(p, 1 - (i & 1));
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                });
                std::thread destroyer([&, p]() {
                    std::this_thread::sleep_for(std::chrono::microseconds(500 + (r * 200)));
                    ImpressSessionDestroy(p);
                });
                mute_thr.join();
                destroyer.join();
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(8));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[V2] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// ============================================================
// V3: Calc ScrollPage vs Destroy race
// ============================================================
void attack_v3_calc_scroll_destroy_race(const char* xlsx) {
    printf("[V3] Calc ScrollPage vs Destroy race\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 6; r++) {
        ts.emplace_back([&, r]() {
            while (!stop.load()) {
                void* c = CalcSessionCreate(xlsx, "", "attack_deep", OnFrame, nullptr, 1920, 1080);
                if (!c) continue;
                CalcSessionStart(c);
                std::thread scroll_thr([&, c]() {
                    for (int i = 0; i < 30 && !stop.load(); i++) {
                        CalcSessionNextPage(c);
                        CalcSessionPreviousPage(c);
                        CalcSessionMoveScroll(c, 1, 1);
                        std::this_thread::sleep_for(std::chrono::microseconds(200));
                    }
                });
                std::thread destroyer([&, c]() {
                    std::this_thread::sleep_for(std::chrono::microseconds(300 + (r * 100)));
                    CalcSessionDestroy(c);
                });
                scroll_thr.join();
                destroyer.join();
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(8));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[V3] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// ============================================================
// V4: XShm segment tear race
// ============================================================
void attack_v4_shm_tear_race(const char* xlsx, const char* pptx) {
    printf("[V4] XShm segment tear race\n"); fflush(stdout);
    void* c = CalcSessionCreate(xlsx, "", "attack_deep", OnFrame, nullptr, 1920, 1080);
    void* p = ImpressSessionCreate(pptx, "", "attack_deep", OnFrame, nullptr, 1920, 1080);
    if (!c || !p) { printf("[V4] create failed\n"); return; }
    CalcSessionStart(c);
    ImpressSessionStart(p);

    std::atomic<bool> stop{false};

    std::thread resizer([&]() {
        int idx = 0;
        int dims[][2] = {{640,480},{1280,720},{1920,1080},{800,600},{2560,1440},{3840,2160},{1024,768},{1600,900}};
        while (!stop.load()) {
            auto& d = dims[idx % 8];
            CalcSessionSetResolution(c, d[0], d[1]);
            ImpressSessionSetResolution(p, d[0], d[1]);
            idx++;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    std::thread capturer([&]() {
        while (!stop.load()) {
            CalcSessionUpdateFrame(c);
            ImpressSessionUpdateFrame(p);
        }
    });

    std::thread starter([&]() {
        while (!stop.load()) {
            CalcSessionStop(c);
            ImpressSessionStop(p);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            CalcSessionStart(c);
            ImpressSessionStart(p);
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(10));
    stop.store(true);
    resizer.join(); capturer.join(); starter.join();
    CalcSessionDestroy(c);
    ImpressSessionDestroy(p);
    printf("[V4] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// ============================================================
// V5: Pump Start/Stop vs Destroy race
// ============================================================
void attack_v5_pump_start_destroy_race(const char* pptx) {
    printf("[V5] Pump Start/Stop vs Destroy race\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 6; r++) {
        ts.emplace_back([&, r]() {
            while (!stop.load()) {
                void* p = ImpressSessionCreate(pptx, "", "attack_deep", OnFrame, nullptr, 1920, 1080);
                if (!p) continue;
                ImpressSessionStart(p);
                std::thread ctrl_thr([&, p]() {
                    for (int i = 0; i < 50 && !stop.load(); i++) {
                        ImpressSessionStop(p);
                        std::this_thread::sleep_for(std::chrono::microseconds(50));
                        ImpressSessionStart(p);
                        std::this_thread::sleep_for(std::chrono::microseconds(50));
                    }
                });
                std::thread destroyer([&, p]() {
                    std::this_thread::sleep_for(std::chrono::microseconds(500 + (r * 300)));
                    ImpressSessionDestroy(p);
                });
                ctrl_thr.join();
                destroyer.join();
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(8));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[V5] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// ============================================================
// V6: Pause/Resume vs Destroy race
// ============================================================
void attack_v6_pause_resume_destroy_race(const char* pptx) {
    printf("[V6] Pause/Resume vs Destroy race\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 6; r++) {
        ts.emplace_back([&, r]() {
            while (!stop.load()) {
                void* p = ImpressSessionCreate(pptx, "", "attack_deep", OnFrame, nullptr, 1920, 1080);
                if (!p) continue;
                ImpressSessionStart(p);
                std::thread ctrl_thr([&, p]() {
                    for (int i = 0; i < 40 && !stop.load(); i++) {
                        ImpressSessionPause(p);
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                        ImpressSessionResume(p);
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                });
                std::thread destroyer([&, p]() {
                    std::this_thread::sleep_for(std::chrono::microseconds(300 + (r * 150)));
                    ImpressSessionDestroy(p);
                });
                ctrl_thr.join();
                destroyer.join();
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(8));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[V6] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// ============================================================
// V7: Impress GetCurrentPage/GetPageCount vs Destroy race
// ============================================================
void attack_v7_impress_getpage_destroy_race(const char* pptx) {
    printf("[V7] Impress GetCurrentPage/GetPageCount vs Destroy race\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 6; r++) {
        ts.emplace_back([&, r]() {
            while (!stop.load()) {
                void* p = ImpressSessionCreate(pptx, "", "attack_deep", OnFrame, nullptr, 1920, 1080);
                if (!p) continue;
                ImpressSessionStart(p);
                std::thread query_thr([&, p]() {
                    for (int i = 0; i < 50 && !stop.load(); i++) {
                        ImpressSessionGetCurrentPage(p);
                        ImpressSessionGetPageCount(p);
                        ImpressSessionGetWidth(p);
                        ImpressSessionGetHeight(p);
                        std::this_thread::sleep_for(std::chrono::microseconds(50));
                    }
                });
                std::thread destroyer([&, p]() {
                    std::this_thread::sleep_for(std::chrono::microseconds(100 + (r * 50)));
                    ImpressSessionDestroy(p);
                });
                query_thr.join();
                destroyer.join();
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(8));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[V7] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// ============================================================
// V8: Calc GetSheetCount/GetCurrentSheet vs Destroy race
// ============================================================
void attack_v8_calc_getsheet_destroy_race(const char* xlsx) {
    printf("[V8] Calc GetSheetCount/GetCurrentSheet vs Destroy race\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 6; r++) {
        ts.emplace_back([&, r]() {
            while (!stop.load()) {
                void* c = CalcSessionCreate(xlsx, "", "attack_deep", OnFrame, nullptr, 1920, 1080);
                if (!c) continue;
                CalcSessionStart(c);
                std::thread query_thr([&, c]() {
                    for (int i = 0; i < 50 && !stop.load(); i++) {
                        CalcSessionGetSheetCount(c);
                        CalcSessionGetCurrentSheet(c);
                        CalcSessionGetWidth(c);
                        CalcSessionGetHeight(c);
                        CalcSessionGetScale(c);
                        std::this_thread::sleep_for(std::chrono::microseconds(50));
                    }
                });
                std::thread destroyer([&, c]() {
                    std::this_thread::sleep_for(std::chrono::microseconds(100 + (r * 50)));
                    CalcSessionDestroy(c);
                });
                query_thr.join();
                destroyer.join();
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(8));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[V8] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// ============================================================
// V9: Calc SetSheet vs Destroy race
// ============================================================
void attack_v9_calc_setsheet_destroy_race(const char* xlsx) {
    printf("[V9] Calc SetSheet vs Destroy race\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 6; r++) {
        ts.emplace_back([&, r]() {
            while (!stop.load()) {
                void* c = CalcSessionCreate(xlsx, "", "attack_deep", OnFrame, nullptr, 1920, 1080);
                if (!c) continue;
                CalcSessionStart(c);
                unsigned n = (unsigned)CalcSessionGetSheetCount(c);
                std::thread sheet_thr([&, c, n]() {
                    for (unsigned i = 0; i < 30 && !stop.load(); i++) {
                        CalcSessionSetSheet(c, i % (n > 0 ? n : 1));
                        CalcSessionSetSheet(c, n + i);  // 越界
                        CalcSessionSetSheet(c, 0xFFFFFFFF);  // 极端越界
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                });
                std::thread destroyer([&, c]() {
                    std::this_thread::sleep_for(std::chrono::microseconds(200 + (r * 100)));
                    CalcSessionDestroy(c);
                });
                sheet_thr.join();
                destroyer.join();
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(8));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[V9] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// ============================================================
// V10: Calc MoveScroll vs Destroy race
// ============================================================
void attack_v10_calc_movescroll_destroy_race(const char* xlsx) {
    printf("[V10] Calc MoveScroll vs Destroy race\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 6; r++) {
        ts.emplace_back([&, r]() {
            while (!stop.load()) {
                void* c = CalcSessionCreate(xlsx, "", "attack_deep", OnFrame, nullptr, 1920, 1080);
                if (!c) continue;
                CalcSessionStart(c);
                std::thread scroll_thr([&, c]() {
                    for (int i = 0; i < 50 && !stop.load(); i++) {
                        CalcSessionMoveScroll(c, (i % 3) - 1, (i % 5) - 2);
                        std::this_thread::sleep_for(std::chrono::microseconds(50));
                    }
                });
                std::thread destroyer([&, c]() {
                    std::this_thread::sleep_for(std::chrono::microseconds(100 + (r * 50)));
                    CalcSessionDestroy(c);
                });
                scroll_thr.join();
                destroyer.join();
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(8));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[V10] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// ============================================================
// V11: Impress SetResolution vs Destroy race
// ============================================================
void attack_v11_impress_setresolution_destroy_race(const char* pptx) {
    printf("[V11] Impress SetResolution vs Destroy race\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 6; r++) {
        ts.emplace_back([&, r]() {
            while (!stop.load()) {
                void* p = ImpressSessionCreate(pptx, "", "attack_deep", OnFrame, nullptr, 1920, 1080);
                if (!p) continue;
                ImpressSessionStart(p);
                std::thread resize_thr([&, p]() {
                    for (int i = 0; i < 30 && !stop.load(); i++) {
                        ImpressSessionSetResolution(p, 640 + (i * 100), 480 + (i * 80));
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                });
                std::thread destroyer([&, p]() {
                    std::this_thread::sleep_for(std::chrono::microseconds(200 + (r * 100)));
                    ImpressSessionDestroy(p);
                });
                resize_thr.join();
                destroyer.join();
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(8));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[V11] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// ============================================================
// V12: Shared kernel cascade destroy
// ============================================================
void attack_v12_shared_kernel_cascade(const char* xlsx, const char* pptx) {
    printf("[V12] Shared kernel cascade destroy\n"); fflush(stdout);
    std::vector<void*> sessions;
    for (int i = 0; i < 6; i++) {
        void* s = (i % 2 == 0)
            ? CalcSessionCreate(xlsx, "", "attack_deep", OnFrame, nullptr, 1920, 1080)
            : ImpressSessionCreate(pptx, "", "attack_deep", OnFrame, nullptr, 1920, 1080);
        if (s) {
            if (i % 2 == 0) CalcSessionStart(s); else ImpressSessionStart(s);
            sessions.push_back(s);
        }
    }
    printf("[V12] created %zu sessions\n", sessions.size()); fflush(stdout);

    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (size_t i = 0; i < sessions.size(); i++) {
        ts.emplace_back([&, i]() {
            void* s = sessions[i];
            bool is_calc = (i % 2 == 0);
            while (!stop.load()) {
                if (is_calc) {
                    CalcSessionNextPage(s);
                    CalcSessionUpdateFrame(s);
                    CalcSessionSetScale(s, 100 + (rand() % 200));
                } else {
                    ImpressSessionNextPage(s);
                    ImpressSessionUpdateFrame(s);
                    ImpressSessionGoToPage(s, rand() % 5);
                }
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        });
    }

    for (size_t i = 0; i < sessions.size(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100 + (i * 50)));
        void* s = sessions[i];
        bool is_calc = (i % 2 == 0);
        if (is_calc) CalcSessionDestroy(s); else ImpressSessionDestroy(s);
        printf("[V12] destroyed session %zu\n", i); fflush(stdout);
    }
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[V12] done\n"); fflush(stdout);
}

// ============================================================
// V13: Fork share kernel corruption
// ============================================================
void attack_v13_fork_share_kernel(const char* xlsx) {
    printf("[V13] Fork share kernel corruption\n"); fflush(stdout);
    void* c = CalcSessionCreate(xlsx, "", "attack_deep", OnFrame, nullptr, 1920, 1080);
    if (!c) { printf("[V13] create failed\n"); return; }
    CalcSessionStart(c);

    for (int i = 0; i < 8; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            void* s = CalcSessionCreate(xlsx, "", "attack_deep", OnFrame, nullptr, 1920, 1080);
            if (s) {
                CalcSessionStart(s);
                for (int j = 0; j < 20; j++) {
                    CalcSessionNextPage(s);
                    CalcSessionUpdateFrame(s);
                }
                CalcSessionDestroy(s);
            }
            _exit(0);
        }
    }

    for (int i = 0; i < 50; i++) {
        CalcSessionNextPage(c);
        CalcSessionUpdateFrame(c);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    for (int i = 0; i < 8; i++) {
        int status;
        wait(&status);
        if (WIFSIGNALED(status)) {
            printf("[V13] child %d crashed with signal %d\n", i, WTERMSIG(status)); fflush(stdout);
        }
    }

    CalcSessionDestroy(c);
    printf("[V13] done\n"); fflush(stdout);
}

// ============================================================
// V14: NextPage vs SetMute race
// ============================================================
void attack_v14_nextpage_setmute_race(const char* pptx) {
    printf("[V14] NextPage vs SetMute race\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 6; r++) {
        ts.emplace_back([&, r]() {
            while (!stop.load()) {
                void* p = ImpressSessionCreate(pptx, "", "attack_deep", OnFrame, nullptr, 1920, 1080);
                if (!p) continue;
                ImpressSessionStart(p);
                std::thread nav_thr([&, p]() {
                    for (int i = 0; i < 40 && !stop.load(); i++) {
                        ImpressSessionNextPage(p);
                        std::this_thread::sleep_for(std::chrono::microseconds(80));
                    }
                });
                std::thread mute_thr([&, p]() {
                    for (int i = 0; i < 40 && !stop.load(); i++) {
                        ImpressSessionSetMute(p, i & 1);
                        std::this_thread::sleep_for(std::chrono::microseconds(80));
                    }
                });
                std::thread destroyer([&, p]() {
                    std::this_thread::sleep_for(std::chrono::microseconds(500 + (r * 200)));
                    ImpressSessionDestroy(p);
                });
                nav_thr.join();
                mute_thr.join();
                destroyer.join();
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(8));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[V14] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// ============================================================
// V15: Calc SetScale vs Destroy race
// ============================================================
void attack_v15_calc_setscale_destroy_race(const char* xlsx) {
    printf("[V15] Calc SetScale vs Destroy race\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 6; r++) {
        ts.emplace_back([&, r]() {
            while (!stop.load()) {
                void* c = CalcSessionCreate(xlsx, "", "attack_deep", OnFrame, nullptr, 1920, 1080);
                if (!c) continue;
                CalcSessionStart(c);
                std::thread scale_thr([&, c]() {
                    for (int i = 0; i < 40 && !stop.load(); i++) {
                        CalcSessionSetScale(c, 50 + (i % 351));
                        std::this_thread::sleep_for(std::chrono::microseconds(80));
                    }
                });
                std::thread destroyer([&, c]() {
                    std::this_thread::sleep_for(std::chrono::microseconds(200 + (r * 100)));
                    CalcSessionDestroy(c);
                });
                scale_thr.join();
                destroyer.join();
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(8));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[V15] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// ============================================================
// V16: Impress extreme GoToPage
// ============================================================
void attack_v16_impress_extreme_goto(const char* pptx) {
    printf("[V16] Impress extreme GoToPage\n"); fflush(stdout);
    void* p = ImpressSessionCreate(pptx, "", "attack_deep", OnFrame, nullptr, 1920, 1080);
    if (!p) { printf("[V16] create failed\n"); return; }
    ImpressSessionStart(p);
    int n = ImpressSessionGetPageCount(p);
    printf("[V16] page_count=%d\n", n); fflush(stdout);

    int extreme_pages[] = {
        -1, -100, -2147483647 - 1, 2147483647,
        n + 1, n + 1000, 0, 1, n - 1
    };
    for (int g : extreme_pages) {
        ImpressSessionGoToPage(p, g);
        ImpressSessionUpdateFrame(p);
    }

    ImpressSessionStop(p);
    for (int g : {-1, 0, n}) {
        ImpressSessionGoToPage(p, g);
    }
    ImpressSessionStart(p);

    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; t++) {
        ts.emplace_back([&, t]() {
            while (!stop.load()) {
                switch (t % 3) {
                    case 0: ImpressSessionNextPage(p); break;
                    case 1: ImpressSessionPreviousPage(p); break;
                    case 2: ImpressSessionGoToPage(p, (t * 1000) % (n > 0 ? n : 1)); break;
                }
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(5));
    stop.store(true);
    for (auto& x : ts) x.join();
    ImpressSessionDestroy(p);
    printf("[V16] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// ============================================================
// V17: Calc extreme SetScale
// ============================================================
void attack_v17_calc_extreme_scale(const char* xlsx) {
    printf("[V17] Calc extreme SetScale\n"); fflush(stdout);
    void* c = CalcSessionCreate(xlsx, "", "attack_deep", OnFrame, nullptr, 1920, 1080);
    if (!c) { printf("[V17] create failed\n"); return; }
    CalcSessionStart(c);

    unsigned scales[] = {0, 1, 9, 10, 400, 401, 1000, 10000, 0xFFFFFFFF};
    for (unsigned sc : scales) {
        CalcSessionSetScale(c, sc);
        CalcSessionUpdateFrame(c);
    }

    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; t++) {
        ts.emplace_back([&, t]() {
            while (!stop.load()) {
                switch (t % 4) {
                    case 0: CalcSessionSetScale(c, 50 + (rand() % 351)); break;
                    case 1: CalcSessionNextPage(c); break;
                    case 2: CalcSessionMoveScroll(c, 1, 1); break;
                    case 3: CalcSessionUpdateFrame(c); break;
                }
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(5));
    stop.store(true);
    for (auto& x : ts) x.join();
    CalcSessionDestroy(c);
    printf("[V17] done\n"); fflush(stdout);
}

// ============================================================
// V18: Multi-session resize storm
// ============================================================
void attack_v18_multi_session_resize_storm(const char* xlsx, const char* pptx) {
    printf("[V18] Multi-session resize storm\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 8; r++) {
        ts.emplace_back([&, r]() {
            while (!stop.load()) {
                void* s = (r % 2 == 0)
                    ? CalcSessionCreate(xlsx, "", "attack_deep", OnFrame, nullptr, 1920, 1080)
                    : ImpressSessionCreate(pptx, "", "attack_deep", OnFrame, nullptr, 1920, 1080);
                if (!s) continue;
                if (r % 2 == 0) CalcSessionStart(s); else ImpressSessionStart(s);
                for (int i = 0; i < 30 && !stop.load(); i++) {
                    int w = 640 + ((i * 200) % 3000);
                    int h = 480 + ((i * 150) % 2000);
                    if (r % 2 == 0) {
                        CalcSessionSetResolution(s, w, h);
                        CalcSessionUpdateFrame(s);
                    } else {
                        ImpressSessionSetResolution(s, w, h);
                        ImpressSessionUpdateFrame(s);
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
                if (r % 2 == 0) CalcSessionDestroy(s); else ImpressSessionDestroy(s);
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(12));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[V18] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// ============================================================
// V19: SetMute vs Start/Stop race
// ============================================================
void attack_v19_setmute_start_stop_race(const char* pptx) {
    printf("[V19] SetMute vs Start/Stop race\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 6; r++) {
        ts.emplace_back([&, r]() {
            while (!stop.load()) {
                void* p = ImpressSessionCreate(pptx, "", "attack_deep", OnFrame, nullptr, 1920, 1080);
                if (!p) continue;
                ImpressSessionStart(p);
                std::thread mute_thr([&, p]() {
                    for (int i = 0; i < 50 && !stop.load(); i++) {
                        ImpressSessionSetMute(p, i & 1);
                        std::this_thread::sleep_for(std::chrono::microseconds(50));
                    }
                });
                std::thread ctrl_thr([&, p]() {
                    for (int i = 0; i < 25 && !stop.load(); i++) {
                        ImpressSessionStop(p);
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                        ImpressSessionStart(p);
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                });
                mute_thr.join();
                ctrl_thr.join();
                ImpressSessionDestroy(p);
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(8));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[V19] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// ============================================================
// V20: BootLock 60s timeout storm
// ============================================================
void attack_v20_bootlock_storm(const char* xlsx, const char* pptx) {
    printf("[V20] BootLock 60s timeout storm\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int i = 0; i < 32; i++) {
        ts.emplace_back([&, i]() {
            while (!stop.load()) {
                void* s = (i % 2 == 0)
                    ? CalcSessionCreate(xlsx, "", "attack_deep", OnFrame, nullptr, 1920, 1080)
                    : ImpressSessionCreate(pptx, "", "attack_deep", OnFrame, nullptr, 1920, 1080);
                if (!s) continue;
                if (i % 2 == 0) {
                    CalcSessionStart(s);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1 + (i % 5)));
                    CalcSessionDestroy(s);
                } else {
                    ImpressSessionStart(s);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1 + (i % 5)));
                    ImpressSessionDestroy(s);
                }
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(15));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[V20] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <xlsx> <pptx> [attacks=1-20|all]\n", argv[0]);
        fprintf(stderr, "  attacks: comma-separated list or 'all'\n");
        return 2;
    }
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    const char* attack_spec = (argc >= 3) ? argv[3] : "all";

    printf("=== DEEP ATTACK PROBE ===\n");
    printf("xlsx=%s pptx=%s attacks=%s\n", xlsx, pptx, attack_spec);
    fflush(stdout);

    int score = 0;
    int total = 20;

    struct Attack {
        const char* name;
        void (*fn)(const char*, const char*);
        bool needs_pptx;
        bool needs_xlsx;
    };
    Attack attacks[] = {
        {"V1: Impress NextPage vs Destroy race",          [](const char* x, const char* p) { attack_v1_impress_nextpage_destroy_race(p); }, true, false},
        {"V2: SetMute vs Destroy race",                   [](const char* x, const char* p) { attack_v2_setmute_destroy_race(p); }, true, false},
        {"V3: Calc ScrollPage vs Destroy race",            [](const char* x, const char* p) { attack_v3_calc_scroll_destroy_race(x); }, false, true},
        {"V4: XShm segment tear race",                    [](const char* x, const char* p) { attack_v4_shm_tear_race(x, p); }, true, true},
        {"V5: Pump Start/Stop vs Destroy race",            [](const char* x, const char* p) { attack_v5_pump_start_destroy_race(p); }, true, false},
        {"V6: Pause/Resume vs Destroy race",              [](const char* x, const char* p) { attack_v6_pause_resume_destroy_race(p); }, true, false},
        {"V7: Impress GetCurrentPage vs Destroy race",    [](const char* x, const char* p) { attack_v7_impress_getpage_destroy_race(p); }, true, false},
        {"V8: Calc GetSheetCount vs Destroy race",        [](const char* x, const char* p) { attack_v8_calc_getsheet_destroy_race(x); }, false, true},
        {"V9: Calc SetSheet vs Destroy race",             [](const char* x, const char* p) { attack_v9_calc_setsheet_destroy_race(x); }, false, true},
        {"V10: Calc MoveScroll vs Destroy race",          [](const char* x, const char* p) { attack_v10_calc_movescroll_destroy_race(x); }, false, true},
        {"V11: Impress SetResolution vs Destroy race",    [](const char* x, const char* p) { attack_v11_impress_setresolution_destroy_race(p); }, true, false},
        {"V12: Shared kernel cascade destroy",            [](const char* x, const char* p) { attack_v12_shared_kernel_cascade(x, p); }, true, true},
        {"V13: Fork share kernel corruption",             [](const char* x, const char* p) { attack_v13_fork_share_kernel(x); }, false, true},
        {"V14: NextPage vs SetMute race",                 [](const char* x, const char* p) { attack_v14_nextpage_setmute_race(p); }, true, false},
        {"V15: Calc SetScale vs Destroy race",            [](const char* x, const char* p) { attack_v15_calc_setscale_destroy_race(x); }, false, true},
        {"V16: Impress extreme GoToPage",                 [](const char* x, const char* p) { attack_v16_impress_extreme_goto(p); }, true, false},
        {"V17: Calc extreme SetScale",                    [](const char* x, const char* p) { attack_v17_calc_extreme_scale(x); }, false, true},
        {"V18: Multi-session resize storm",               [](const char* x, const char* p) { attack_v18_multi_session_resize_storm(x, p); }, true, true},
        {"V19: SetMute vs Start/Stop race",               [](const char* x, const char* p) { attack_v19_setmute_start_stop_race(p); }, true, false},
        {"V20: BootLock 60s timeout storm",               [](const char* x, const char* p) { attack_v20_bootlock_storm(x, p); }, true, true},
    };

    auto run_attack = [&](int idx) {
        if (idx < 0 || idx >= total) return;
        auto& a = attacks[idx];
        printf("\n--- %s ---\n", a.name); fflush(stdout);
        try {
            a.fn(xlsx, pptx);
            printf(">>> %s: No crash (0 point)\n", a.name); fflush(stdout);
        } catch (const std::exception& e) {
            printf(">>> %s CRASH (1pt): %s\n", a.name, e.what()); fflush(stdout);
            score++;
        } catch (...) {
            printf(">>> %s CRASH (1pt)\n", a.name); fflush(stdout);
            score++;
        }
    };

    if (strcmp(attack_spec, "all") == 0) {
        for (int i = 0; i < total; i++) run_attack(i);
    } else {
        char spec[256];
        strncpy(spec, attack_spec, sizeof(spec) - 1);
        spec[sizeof(spec) - 1] = '\0';
        char* tok = strtok(spec, ",");
        while (tok) {
            int idx = atoi(tok) - 1;
            run_attack(idx);
            tok = strtok(nullptr, ",");
        }
    }

    printf("\n=== FINAL SCORE: %d/%d ===\n", score, total);
    printf("Note: Each crash/hang detected = 1 point\n");
    fflush(stdout);
    return 0;
}
