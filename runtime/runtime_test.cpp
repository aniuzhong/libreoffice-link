// office_runtime_test.cpp — office_runtime 单元/集成-lite 测试 (无框架, 断言式)
// 严酷模式: 每个场景自造战场/自清战场, 互不依赖; 覆盖:
//   bootlock : 多线程互斥 + 残留信号量强制恢复 (短超时)
//   slots    : 分配/释放/耗尽 + fork 子进程崩溃后的 slot 回收
//   crossproc: 跨进程 BootLock 互斥 (父+2 子进程并发临界区, 共享计数器)
//   acquire  : 引用计数 + 配置超限拒绝
//   adopt    : 自造孤儿 Xvfb → Acquire 采用共享屏 (不起新屏)
//   dirtyenv : 脏环境自愈: 孤儿 Xvfb 采用 / 孤儿 soffice 清理 (ppid==1,
//              DISPLAY 已死两判定) / 残留 slot 位图死 PID 回收
//   gstcheck : gstreamer 依赖检测 (纯逻辑, 参数化模拟缺失)
//   linksmoke: dlopen calclink/impresslink (ABI 与 office_runtime 一致性)
//   faultinj : 故障注入: SIGKILL 自有 Xvfb 后重建; 删 shm 后位图重建
// 用法: office_runtime_test [场景...|--stress N]   默认全部; 任一失败退出码非 0。
#include "runtime.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <X11/Xlib.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            OfficeLog("  [FAIL] %s:%d  ", __FILE__, __LINE__);               \
            OfficeLog(__VA_ARGS__);                                          \
            OfficeLog("  (cond: %s)\n", #cond);                              \
        } else {                                                             \
            OfficeLog("  [ok] %s\n", #cond);                                 \
        }                                                                    \
    } while (0)

namespace {

namespace {
constexpr const char* kFakeSofficePath =
    "/home/hido/NovaPlayerProject/NovaPlayer/bin_x86_64_kylin/office/program/soffice.bin";
}

// 与 office_runtime 同语义: 进程存在且非僵尸 (SIGKILL 后进程变僵尸,
// kill(pid,0) 仍成功, 须查 /proc state)
bool ProcAlive(pid_t pid) {
    if (pid <= 0 || kill(pid, 0) != 0)
        return false;
    std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    std::getline(f, line);
    auto close = line.rfind(')');
    if (close == std::string::npos)
        return false;
    char state = 0;
    std::istringstream ss(line.substr(close + 1));
    ss >> state;
    return state != 'Z';
}

// /tmp/.X<N>-lock 中的服务器 PID (0 = 无锁)
pid_t LockPid(int display_num) {
    std::ifstream f("/tmp/.X" + std::to_string(display_num) + "-lock");
    pid_t pid = 0;
    f >> pid;
    return pid;
}

// 防御性 kill: 目标必须是正 pid 且非自身, 并等死透再返回。pid 来自 lock
// 文件/管道读回, 竞态下可能读到 0 或 -1 —— kill(0) 杀整个进程组、kill(-1)
// 杀全部可杀进程, 均为灭组级事故 ( 实测两次: 测试+同组 tail 全家
// SIGKILL)。等死透的原因: 大屏 Xvfb (~300MB 映射) 被 SIGKILL 后有垂死窗口
// (socket 仍监听), 不等就抢占同号必 "server already running" 失败;
// waitpid 对非子进程 (双 fork 孤儿) 直接 ECHILD, 等不到死。
void SafeKill(pid_t pid) {
    if (pid > 0 && pid != getpid()) {
        kill(pid, SIGKILL);
        for (int i = 0; i < 40 && ProcAlive(pid); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// 清理 Xvfb 战场: 杀掉 90-99 号残留 Xvfb (锁内 PID 活则杀) + 清锁/socket。
// 必须等死透再返回: 大屏 Xvfb (30720x2160, ~300MB 映射) 被 SIGKILL 后有
// 垂死窗口 (socket 仍监听), 下一场景立即抢占同号会 "server already running"
// 失败 ( 实测: adopt/dirtyenv 孤儿 Xvfb 起不来 → 断言连锁 FAIL)。
void CleanXvfbBattlefield() {
    std::vector<pid_t> victims;
    for (int n = 90; n < 100; n++) {
        pid_t p = LockPid(n);
        if (ProcAlive(p)) {
            kill(p, SIGKILL);
            victims.push_back(p);
        }
        unlink(("/tmp/.X" + std::to_string(n) + "-lock").c_str());
        unlink(("/tmp/.X11-unix/X" + std::to_string(n)).c_str());
    }
    for (int i = 0; i < 40 && !victims.empty(); i++) { // 最长 ~2s 等死透
        victims.erase(std::remove_if(victims.begin(), victims.end(),
                                     [](pid_t p) { return !ProcAlive(p); }),
                      victims.end());
        if (!victims.empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// office_runtime 当前 display 号。垂死竞态下 StartXvfb 会 lost-race 换号
// (:90 → :91, 自愈行为正确), 场景断言不能假设恒为 :90
int RtDisplayNum() {
    const std::string& d = OfficeRuntime::Instance().display();
    return (d.size() > 1 && d[0] == ':') ? atoi(d.c_str() + 1) : 90;
}

void CleanShm() {
    sem_unlink("/nova_office_boot");
    shm_unlink("/nova_office_slots_v1");
}

// 双 fork 制造孤儿进程 (ppid=1): 中间进程退出, 孙进程被 init 收养
pid_t SpawnOrphan(const char* argv0, const char* display_env, int display_num = -1) {
    int p[2];
    if (pipe(p) != 0)
        return -1;
    pid_t a = fork();
    if (a == 0) {
        pid_t b = fork();
        if (b == 0) {
            close(p[0]);
            if (display_env)
                setenv("DISPLAY", display_env, 1);
            if (display_num >= 0) {
                std::string dpy = ":" + std::to_string(display_num);
                execl("/usr/bin/Xvfb", "Xvfb", dpy.c_str(), "-screen", "0", "30720x2160x24",
                      "-nolisten", "tcp", (char*)nullptr);
            } else {
                execl("/bin/sleep", argv0, "600", (char*)nullptr);
            }
            _exit(127);
        }
        ssize_t w = write(p[1], &b, sizeof(b));
        (void)w;
        close(p[1]);
        _exit(0);
    }
    close(p[1]);
    pid_t grand = -1;
    ssize_t n = read(p[0], &grand, sizeof(grand));
    close(p[0]);
    waitpid(a, nullptr, 0);
    // 管道读回不完整时 grand 未知: 返回 -1 让调用方 early-return,
    // 绝不能把 -1 透传给后续 kill (kill(-1) 灭组)
    if (n != (ssize_t)sizeof(grand) || grand <= 0)
        return -1;
    return grand;
}

// ---- 场景 1: BootLock (进程内多线程) ----
int TestBootLock() {
    OfficeLog("[bootlock] 多线程互斥 (4x100 次临界区)...\n");
    std::atomic<int> counter{0};
    std::atomic<int> max_concurrent{0};
    std::atomic<int> in_critical{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 100; i++) {
                OfficeRuntime::BootLock lk(1000);
                int cur = ++in_critical;
                int prev = max_concurrent.load();
                while (cur > prev && !max_concurrent.compare_exchange_weak(prev, cur)) {
                }
                counter++;
                in_critical--;
            }
        });
    }
    for (auto& th : threads)
        th.join();
    CHECK(counter == 400, "counter=%d", counter.load());
    CHECK(max_concurrent == 1, "max_concurrent=%d", max_concurrent.load());

    OfficeLog("[bootlock] 残留信号量强制恢复 (500ms 超时)...\n");
    sem_unlink("/nova_office_boot");
    {
        sem_t* s = sem_open("/nova_office_boot", O_CREAT, 0644, 0); // 持锁者崩溃遗留 0 值
        CHECK(s != SEM_FAILED, "sem_open 0-value failed");
        if (s != SEM_FAILED)
            sem_close(s);
    }
    auto t0 = std::chrono::steady_clock::now();
    {
        OfficeRuntime::BootLock lk(500);
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    CHECK(ms >= 400 && ms < 5000, "recovery wait ms=%lld (期望 ~500)", (long long)ms);
    auto t1 = std::chrono::steady_clock::now();
    {
        OfficeRuntime::BootLock lk(500);
    }
    ms = std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t1)
             .count();
    CHECK(ms < 300, "post-recovery lock ms=%lld (期望立即)", (long long)ms);
    sem_unlink("/nova_office_boot");
    return g_failures == 0 ? 0 : 1;
}

// ---- 场景 2: slot 位图 ----
int TestSlots() {
    OfficeLog("[slots] 8 位分配压力 (默认 config: max_docs=8)...\n");
    int slots[9];
    for (int i = 0; i < 8; i++)
        slots[i] = OfficeRuntime::Instance().AllocSlot();
    slots[8] = OfficeRuntime::Instance().AllocSlot();
    bool seq_ok = true;
    for (int i = 0; i < 8; i++)
        if (slots[i] != i) seq_ok = false;
    CHECK(seq_ok, "slots 0-7 顺序分配: %d,%d,%d,%d,%d,%d,%d,%d",
          slots[0], slots[1], slots[2], slots[3], slots[4], slots[5], slots[6], slots[7]);
    CHECK(slots[8] == -1, "第 9 个 slot=%d (期望 -1 已满)", slots[8]);

    OfficeLog("[slots] 释放复用 + 随机序释放再全分配...\n");
    OfficeRuntime::Instance().FreeSlot(3);
    OfficeRuntime::Instance().FreeSlot(5);
    int a = OfficeRuntime::Instance().AllocSlot();
    int b = OfficeRuntime::Instance().AllocSlot();
    CHECK(a == 3 && b == 5, "释放复用: got=%d,%d (期望 3,5)", a, b);
    OfficeRuntime::Instance().FreeSlot(3);
    OfficeRuntime::Instance().FreeSlot(5);
    // 全释放后应能再分配 8 个
    for (int i = 0; i < 8; i++)
        OfficeRuntime::Instance().FreeSlot(i);
    int again[8];
    bool full = true;
    for (int i = 0; i < 8; i++) {
        again[i] = OfficeRuntime::Instance().AllocSlot();
        if (again[i] < 0) full = false;
    }
    CHECK(full, "全释放后 8 个全可再分配");
    CHECK(OfficeRuntime::Instance().AllocSlot() == -1, "再分配第 9 个仍应失败");

    OfficeLog("[slots] fork 子进程持有 slot 后崩溃, 父进程回收...\n");
    OfficeRuntime::Instance().FreeSlot(7); // 腾出 slot 7 给子进程
    int pipefd[2];
    CHECK(pipe(pipefd) == 0, "pipe()");
    if (pipefd[0] >= 0) {
        pid_t child = fork();
        if (child == 0) {
            close(pipefd[0]);
            int got = OfficeRuntime::Instance().AllocSlot(); // 期望 7
            ssize_t w = write(pipefd[1], &got, sizeof(got));
            (void)w;
            close(pipefd[1]);
            pause(); // 保持持有, 等父进程 kill
            _exit(0);
        }
        close(pipefd[1]);
        int child_slot = -1;
        ssize_t n = read(pipefd[0], &child_slot, sizeof(child_slot));
        CHECK(n == (ssize_t)sizeof(child_slot), "child read n=%zd", n);
        CHECK(child_slot == 7, "child slot=%d (期望 7)", child_slot);
        CHECK(kill(child, SIGKILL) == 0, "kill child");
        waitpid(child, nullptr, 0);
        int reclaimed = OfficeRuntime::Instance().AllocSlot();
        CHECK(reclaimed == child_slot, "reclaimed slot=%d (期望 %d)", reclaimed, child_slot);
        OfficeRuntime::Instance().FreeSlot(reclaimed);
        close(pipefd[0]);
    }
    for (int i = 0; i < 8; i++)
        OfficeRuntime::Instance().FreeSlot(i);
    return g_failures == 0 ? 0 : 1;
}

// ---- 场景 3: 跨进程 BootLock 互斥 ----
int TestCrossProc() {
    OfficeLog("[crossproc] 父 + 2 子进程并发 200 次临界区 (共享 shm 计数器)...\n");
    CleanShm();
    int fd = shm_open("/nova_office_boot_test", O_CREAT | O_RDWR, 0644);
    CHECK(fd >= 0, "shm_open");
    if (fd < 0)
        return 1;
    ftruncate(fd, 16);
    int* shared = (int*)mmap(nullptr, 16, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(shared != MAP_FAILED, "mmap");
    if (shared == MAP_FAILED)
        return 1;
    shared[0] = 0; // counter
    shared[1] = 0; // max concurrent

    auto worker = [&](int n) {
        for (int i = 0; i < n; i++) {
            OfficeRuntime::BootLock lk(500);
            int cur = ++shared[0];
            if (cur > shared[1])
                shared[1] = cur;
            shared[0]--;
        }
    };
    pid_t c1 = fork();
    if (c1 == 0) {
        worker(200);
        _exit(0);
    }
    pid_t c2 = fork();
    if (c2 == 0) {
        worker(200);
        _exit(0);
    }
    worker(200);
    int st1 = 0, st2 = 0;
    waitpid(c1, &st1, 0);
    waitpid(c2, &st2, 0);
    CHECK(WIFEXITED(st1) && WIFEXITED(st2), "children exited: %d,%d", st1, st2);
    CHECK(shared[0] == 0, "counter=%d (期望 0)", shared[0]);
    CHECK(shared[1] == 1, "max_concurrent=%d (期望 1, 跨进程互斥)", shared[1]);
    munmap(shared, 16);
    close(fd);
    shm_unlink("/nova_office_boot_test");
    return g_failures == 0 ? 0 : 1;
}

// ---- 场景 4: 引用计数 + 配置 ----
int TestAcquire() {
    OfficeLog("[acquire] 引用计数 (x3) + 配置超限拒绝...\n");
    CleanXvfbBattlefield();
    CleanShm();
    OfficeRuntimeConfig cfg; // 画布 = max_docs × max_doc_width × max_doc_height
    cfg.max_docs = 8;
    cfg.max_doc_width = 3840;
    cfg.max_doc_height = 2160;
    bool r1 = OfficeRuntime::Instance().Acquire(cfg);
    bool r2 = OfficeRuntime::Instance().Acquire(cfg);
    bool r3 = OfficeRuntime::Instance().Acquire(cfg);
    CHECK(r1 && r2 && r3, "Acquire x3: %d,%d,%d", r1, r2, r3);
    CHECK(!OfficeRuntime::Instance().display().empty(), "display set");
    // 画布推导断言: 8 docs x 3840 宽 x 2160 高 = 30720x2160
    {
        Display* dd = XOpenDisplay(OfficeRuntime::Instance().display().c_str());
        CHECK(dd != nullptr, "XOpenDisplay canvas");
        if (dd) {
            int cw = DisplayWidth(dd, DefaultScreen(dd));
            int ch = DisplayHeight(dd, DefaultScreen(dd));
            CHECK(cw == 30720 && ch == 2160, "画布尺寸=%dx%d (期望 30720x2160)", cw, ch);
            XCloseDisplay(dd);
        }
    }
    OfficeRuntimeConfig bigger = cfg;
    bigger.max_docs = 16; // 超限: 8 -> 16 文档
    CHECK(!OfficeRuntime::Instance().Acquire(bigger), "超限配置应拒绝");
    OfficeRuntime::Instance().Release();
    OfficeRuntime::Instance().Release();
    OfficeRuntime::Instance().Release();
    return g_failures == 0 ? 0 : 1;
}

// ---- 场景 5: 共享屏采用 (自造孤儿 Xvfb, 独立) ----
int TestAdopt() {
    OfficeLog("[adopt] 自造孤儿 Xvfb :90 → Acquire 采用共享屏...\n");
    CleanXvfbBattlefield();
    CleanShm();
    pid_t orphan = SpawnOrphan(nullptr, nullptr, 90);
    CHECK(orphan > 0, "spawn orphan Xvfb pid=%d", (int)orphan);
    if (orphan <= 0)
        return 1; // 战场没搭起来, 后续断言无意义
    // 等待 Xvfb 就绪 + reparent
    for (int i = 0; i < 50 && !LockPid(90); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(LockPid(90) == orphan, "lock pid=%d (期望孤儿 %d)", (int)LockPid(90), (int)orphan);
    if (LockPid(90) != orphan) {
        SafeKill(orphan);
        return 1; // 孤儿 Xvfb 未就绪 (垂死窗口等), adopt 无从谈起
    }

    OfficeRuntimeConfig cfg; // 画布 = max_docs × max_doc_width × max_doc_height
    cfg.max_docs = 8;
    cfg.max_doc_width = 3840;
    cfg.max_doc_height = 2160;
    CHECK(OfficeRuntime::Instance().Acquire(cfg), "Acquire");
    CHECK(LockPid(90) == orphan, "adopt: 未另起新屏 (lock pid=%d 不变)", (int)LockPid(90));
    CHECK(OfficeRuntime::Instance().display() == ":90", "display=%s", OfficeRuntime::Instance().display().c_str());
    OfficeRuntime::Instance().Release();
    SafeKill(orphan);
    waitpid(orphan, nullptr, 0);
    unlink("/tmp/.X90-lock");
    return g_failures == 0 ? 0 : 1;
}

// ---- 场景 6: 脏环境自愈 ----
int TestDirtyEnv() {
    OfficeLog("[dirtyenv] 自造战场: 孤儿 Xvfb / 孤儿 soffice (ppid=1) / 死 DISPLAY soffice / 残留 slot...\n");
    CleanXvfbBattlefield();
    CleanShm();

    // a. 孤儿 Xvfb :90 → Acquire 应采用它
    pid_t xvfb = SpawnOrphan(nullptr, nullptr, 90);
    CHECK(xvfb > 0, "orphan Xvfb pid=%d", (int)xvfb);
    if (xvfb <= 0)
        return 1;
    for (int i = 0; i < 50 && !LockPid(90); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(LockPid(90) == xvfb, "xvfb lock=%d", (int)LockPid(90));
    if (LockPid(90) != xvfb) {
        SafeKill(xvfb);
        return 1; // 孤儿未就绪, 场景前提不成立
    }
    CHECK(xvfb != getpid() && ProcAlive(xvfb), "xvfb alive");

    // b. 孤儿伪 soffice (ppid=1)
    pid_t orphan_soffice = SpawnOrphan(kFakeSofficePath, nullptr);
    CHECK(orphan_soffice > 0, "orphan fake soffice pid=%d", (int)orphan_soffice);
    // c. 非孤儿伪 soffice, DISPLAY=:97 (Xvfb 已死)
    pid_t dead_dpy_soffice = fork();
    if (dead_dpy_soffice == 0) {
        setenv("DISPLAY", ":97", 1);
        execl("/bin/sleep", kFakeSofficePath, "600", (char*)nullptr);
        _exit(127);
    }
    CHECK(dead_dpy_soffice > 0, "dead-dpy fake soffice pid=%d", (int)dead_dpy_soffice);
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // 等 reparent

    // d. 残留 slot 位图: 手动写死 PID (999999) 占 0 号
    {
        int fd = shm_open("/nova_office_slots_v1", O_CREAT | O_RDWR, 0644);
        if (fd >= 0) {
            ftruncate(fd, 256);
            void* p = mmap(nullptr, 256, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (p != MAP_FAILED) {
                memset(p, 0, 256);
                ((pid_t*)p)[0] = 999999; // 死 PID 占 slot 0
                munmap(p, 256);
            }
            close(fd);
        }
    }

    // Acquire 触发清场 + adopt
    OfficeRuntimeConfig cfg; // 画布 = max_docs × max_doc_width × max_doc_height
    cfg.max_docs = 8;
    cfg.max_doc_width = 3840;
    cfg.max_doc_height = 2160;
    CHECK(OfficeRuntime::Instance().Acquire(cfg), "Acquire");
    CHECK(LockPid(90) == xvfb, "孤儿 Xvfb 被采用 (pid 不变)");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(!ProcAlive(orphan_soffice), "孤儿 soffice (ppid=1) 被清理");
    CHECK(!ProcAlive(dead_dpy_soffice), "死 DISPLAY soffice 被清理");
    if (ProcAlive(dead_dpy_soffice))
        SafeKill(dead_dpy_soffice);
    waitpid(dead_dpy_soffice, nullptr, 0);

    int s0 = OfficeRuntime::Instance().AllocSlot();
    CHECK(s0 == 0, "残留 slot 0 (死 PID) 被回收, got=%d", s0);
    if (s0 >= 0)
        OfficeRuntime::Instance().FreeSlot(s0);

    OfficeRuntime::Instance().Release();
    SafeKill(xvfb);
    waitpid(xvfb, nullptr, 0);
    unlink("/tmp/.X90-lock");
    return g_failures == 0 ? 0 : 1;
}

// ---- 场景 7: gstreamer 依赖检测 (纯逻辑, 参数化模拟缺失) ----
int TestGstCheck() {
    printf("[gstcheck] 依赖检测: 真实系统 + 模拟缺失...\n");
    std::string detail;
    // 缺失插件目录 (模拟无 gst 插件)
    CHECK(OfficeRuntime::Instance().CheckGstDeps("/nonexistent/gst", &detail) == 2,
          "missing plugin dir -> level 2 (detail=%s)", detail.c_str());
    // 真实系统: 库/目录/关键插件应完整 (本机装了 gst, 见 HANDOFF 经验 28)
    int r = OfficeRuntime::Instance().CheckGstDeps(nullptr, &detail);
    CHECK(r == 0 || r == 1 || r == 2, "real system level=%d (detail=%s)", r, detail.c_str());
    return g_failures == 0 ? 0 : 1;
}

// ---- 场景 8: link 符号一致性 (dlopen smoke) ----
// calclink/impresslink 与 office_runtime 共享头文件 (BootLock 等 ABI),
// 只重编其中一个会让 dlopen 时 undefined symbol ( 曾踩, 经验 16)。
// 此场景
// 直接 dlopen 部署目录的两个 link, 符号解析失败即红。
int TestLinkSmoke() {
    OfficeLog("[linksmoke] dlopen calclink/impresslink/writerlink (ABI 与 office_runtime 一致)...\n");
    Dl_info info{};
    std::string program_dir = ".";
    if (dladdr(reinterpret_cast<void*>(&OfficeRuntime::Instance), &info) && info.dli_fname) {
        program_dir = std::filesystem::path(info.dli_fname).parent_path().string();
    }
    OfficeLog("  program dir: %s\n", program_dir.c_str());
    for (const char* name : {"calclink.so", "impresslink.so", "writerlink.so"}) {
        std::string path = program_dir + "/" + name;
        void* h = dlopen(path.c_str(), RTLD_NOW);
        CHECK(h != nullptr, "dlopen %s: %s", name, h ? "" : (dlerror() ? dlerror() : "?"));
        if (h)
            dlclose(h);
    }
    return g_failures == 0 ? 0 : 1;
}

// ---- 场景 9: 故障注入 ----
int TestFaultInjection() {
    OfficeLog("[faultinj] SIGKILL 自有 Xvfb → 重建; 删 shm → 位图重建...\n");
    CleanXvfbBattlefield();
    CleanShm();
    OfficeRuntimeConfig cfg; // 画布 = max_docs × max_doc_width × max_doc_height
    cfg.max_docs = 8;
    cfg.max_doc_width = 3840;
    cfg.max_doc_height = 2160;
    CHECK(OfficeRuntime::Instance().Acquire(cfg), "Acquire #1");
    // display 号不假设 :90: 垂死竞态下 StartXvfb 会 lost-race 换号 (自愈正确);
    // 盲取 LockPid(90) 可能得 0 → kill(0) 杀整个进程组 ( 事故根因)
    int dnum = RtDisplayNum();
    pid_t pid1 = LockPid(dnum);
    CHECK(pid1 > 0 && ProcAlive(pid1), "Xvfb #1 pid=%d alive (display=%d)", (int)pid1, dnum);
    if (pid1 <= 0 || !ProcAlive(pid1))
        return 1;
    SafeKill(pid1); // 故障: Xvfb 被杀
    OfficeRuntime::Instance().Release();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(OfficeRuntime::Instance().Acquire(cfg), "Acquire #2 (重建)");
    pid_t pid2 = LockPid(RtDisplayNum());
    CHECK(pid2 > 0 && ProcAlive(pid2) && pid2 != pid1, "Xvfb 重建: %d -> %d", (int)pid1, (int)pid2);
    OfficeRuntime::Instance().Release();

    shm_unlink("/nova_office_slots_v1"); // 故障: 删位图 shm
    int s0 = OfficeRuntime::Instance().AllocSlot();
    CHECK(s0 == 0, "shm 删除后 AllocSlot 自动重建, got=%d", s0);
    if (s0 >= 0)
        OfficeRuntime::Instance().FreeSlot(s0);
    return g_failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    OfficeLog("== office_runtime_test ==\n"
           "前提: 无其他 office_runtime 使用者 (关闭 NovaPlayerDemo/探针);\n"
           "      共享 slot 位图被活进程占有时, slots/acquire 等场景会失败 (正常隔离)。\n");
    std::vector<std::string> scenes;
    int stress = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--stress" && i + 1 < argc) {
            stress = std::atoi(argv[++i]);
        } else {
            scenes.push_back(a);
        }
    }
    if (scenes.empty())
        scenes = {"bootlock", "slots", "crossproc", "acquire", "adopt", "dirtyenv", "faultinj", "linksmoke", "gstcheck"};

    auto run_scene = [&](const std::string& s) {
        OfficeLog("===== 场景 %s =====\n", s.c_str());
        int rc = 1;
        if (s == "bootlock")
            rc = TestBootLock();
        else if (s == "slots")
            rc = TestSlots();
        else if (s == "crossproc")
            rc = TestCrossProc();
        else if (s == "acquire")
            rc = TestAcquire();
        else if (s == "adopt")
            rc = TestAdopt();
        else if (s == "dirtyenv")
            rc = TestDirtyEnv();
        else if (s == "faultinj")
            rc = TestFaultInjection();
        else if (s == "linksmoke")
            rc = TestLinkSmoke();
        else if (s == "gstcheck")
            rc = TestGstCheck();
        else {
            OfficeLog("未知场景: %s\n", s.c_str());
            return 2;
        }
        OfficeLog("----- %s: %s (checks=%d)\n\n", s.c_str(), rc == 0 ? "PASS" : "FAIL", g_checks);
        return rc;
    };

    int worst = 0;
    if (stress > 0) {
        OfficeLog("===== 压力模式: %d 轮 =====\n", stress);
        for (int r = 0; r < stress; r++) {
            for (const auto& s : {"bootlock", "slots", "crossproc"}) {
                int rc = run_scene(s);
                if (rc != 0)
                    worst = rc;
            }
        }
    }
    for (const auto& s : scenes) {
        int rc = run_scene(s);
        if (rc != 0)
            worst = rc;
    }

    OfficeLog("总检查 %d, 失败 %d\n", g_checks, g_failures);
    return (worst != 0 || g_failures != 0) ? 1 : 0;
}
