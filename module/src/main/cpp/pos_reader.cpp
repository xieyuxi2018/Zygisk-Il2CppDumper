//
// pos_reader.cpp — 实时对象采样器（Zygisk-Il2CppDumper 扩展）v2
//
// v1 用 il2cpp_gc_foreach_heap() 扫堆，实测在本作上**会让游戏进程 SIGSEGV**
//   （crash 回溯: #00 libil2cpp.so 0x3b7e7c8  <- #01 il2cpp_gc_foreach_heap+24
//    <- #02/#03 /memfd:jit-cache 即本模块线程），所以 v2 改成**只读内存扫描**：
//
//   1) 用 il2cpp API 解析目标类，拿到 Il2CppClass* （纯查询，安全）；
//   2) 遍历 /proc/self/maps 里"匿名可读写、1MB~420MB、非 dalvik/文件映射"的区域，
//      用 pread(/proc/self/mem) 按 8 字节对齐扫**指向该 klass 的指针** ——
//      托管对象的头 8 字节就是 klass 指针，命中处即对象地址；
//   3) 再用 il2cpp_field_get_value() 读字段（按字段名，不写死偏移）。
//
// 这条路径不碰 GC 内部，纯读，不会像 v1 那样把游戏打崩。
//
// 输出 (<app_data_dir>/files/pos.log)，每行可 grep：
//   S t=<ms> cls=AivoiceV2.HeroUpdate total=<找到的实例数> shown=<打印数>
//   C <cls> ptr=0x.. rid=.. cfg=.. camp=.. hp=.. -> loc=(x,y,z)
//   OBJ <cls> ptr=0x.. （每类前 POS_FULL_OBJ 个实例打全字段）
//
#include "pos_reader.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "log.h"
#include "il2cpp-class.h"

// 复用 il2cpp_dump.cpp 里定义好的 API 函数指针
#define DO_API(r, n, p) extern r (*n) p;
#include "il2cpp-api-functions.h"
#undef DO_API

// ============================ 可调参数 ============================
static const int   POS_INTERVAL_MS = 2000;        // 采样间隔
static const int   POS_MAX_OBJ     = 128;         // 每类每轮最多记录多少实例
static const int   POS_FULL_OBJ    = 2;           // 每类每轮打印几个"全字段"
static const char *POS_LOG_REL     = "/files/pos.log";
static const long  POS_LOG_MAX     = 64L * 1024 * 1024;
static const unsigned long long POS_REGION_MIN = 1ULL << 20;      // 1MB
static const unsigned long long POS_REGION_MAX = 420ULL << 20;    // 420MB

struct Target {
    const char *image;   // 空字符串 = 在所有 image 里按命名空间+类名找
    const char *ns;
    const char *name;
};

static const Target TARGETS[] = {
        {"Scripts.Plugins.dll", "AivoiceV2", "GameInfoDynamic"},
        {"Scripts.Plugins.dll", "AivoiceV2", "HeroUpdate"},
        {"Scripts.Plugins.dll", "AivoiceV2", "Hero"},
        {"Scripts.Plugins.dll", "AivoiceV2", "MonsterUpdate"},
        {"Scripts.Plugins.dll", "AivoiceV2", "Monster"},
        {"Scripts.Plugins.dll", "AivoiceV2", "OrganUpdate"},
        {"Scripts.Plugins.dll", "AivoiceV2", "SoldierUpdate"},
        {"Scripts.Plugins.dll", "AivoiceV2", "CakeUpdate"},
        {"Scripts.Plugins.dll", "AivoiceV2", "TargetInfo"},
        {"",                    "AivoiceV2", "VInt3"},
        {"",                    "Aivoice",   "VInt3"},
};
static const int TARGET_COUNT = (int) (sizeof(TARGETS) / sizeof(TARGETS[0]));

static Il2CppClass *g_klass[TARGET_COUNT];
static bool g_reported[TARGET_COUNT];

struct Region {
    uintptr_t start;
    uintptr_t end;
};

static std::vector<Region> g_regions;

// ============================ 小工具 ============================

static long now_ms() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long) (ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

static bool in_region(uintptr_t p) {
    for (const auto &r : g_regions) {
        if (p >= r.start && p < r.end) return true;
    }
    return false;
}

static bool plausible_ptr(void *p) {
    auto v = (uintptr_t) p;
    return v > 0x10000 && (v & 7) == 0;
}

// 收集 /proc/self/maps 里适合扫描的匿名 rw 区
static void load_regions() {
    g_regions.clear();
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned long long s = 0, e = 0;
        char perms[8] = {0};
        char rest[400] = {0};
        int n = sscanf(line, "%llx-%llx %7s %*s %*s %*s %399[^\n]", &s, &e, perms, rest);
        if (n < 3) continue;
        if (perms[0] != 'r' || perms[1] != 'w') continue;      // 只读可写的
        unsigned long long sz = e - s;
        if (sz < POS_REGION_MIN || sz > POS_REGION_MAX) continue;
        if (strstr(rest, "dalvik")) continue;
        if (strstr(rest, ".art")) continue;
        if (strstr(rest, ".so")) continue;                     // 文件映射, 不是托管堆
        if (strstr(rest, ".dat")) continue;
        if (strstr(rest, "/dev/")) continue;
        if (strstr(rest, "/memfd:")) continue;
        g_regions.push_back({(uintptr_t) s, (uintptr_t) e});
    }
    fclose(f);
    LOGI("pos_reader: scan regions=%zu", g_regions.size());
}

static Il2CppClass *find_class(const Target &t) {
    Il2CppDomain *domain = il2cpp_domain_get ? il2cpp_domain_get() : nullptr;
    if (!domain) return nullptr;
    size_t size = 0;
    const Il2CppAssembly **assemblies = il2cpp_domain_get_assemblies(domain, &size);
    if (!assemblies) return nullptr;
    for (size_t i = 0; i < size; ++i) {
        const Il2CppImage *image = il2cpp_assembly_get_image(assemblies[i]);
        if (!image) continue;
        if (t.image[0] != '\0') {
            const char *in = il2cpp_image_get_name(image);
            if (!in || strcmp(in, t.image) != 0) continue;
        }
        Il2CppClass *k = il2cpp_class_from_name(image, t.ns, t.name);
        if (k) return k;
    }
    return nullptr;
}

static bool read_int_field(Il2CppObject *obj, Il2CppClass *klass, const char *name, int32_t *out) {
    if (!obj || !klass || !name) return false;
    FieldInfo *f = il2cpp_class_get_field_from_name(klass, name);
    if (!f) return false;
    int32_t v = 0;
    il2cpp_field_get_value(obj, f, &v);
    *out = v;
    return true;
}

static Il2CppObject *read_ref_field(Il2CppObject *obj, Il2CppClass *klass, const char *name) {
    if (!obj || !klass || !name) return nullptr;
    FieldInfo *f = il2cpp_class_get_field_from_name(klass, name);
    if (!f) return nullptr;
    void *p = nullptr;
    il2cpp_field_get_value(obj, f, &p);
    if (!plausible_ptr(p) || !in_region((uintptr_t) p)) return nullptr;
    return (Il2CppObject *) p;
}

static bool print_vint3(FILE *fp, const char *label, Il2CppObject *p) {
    if (!p || !plausible_ptr(p) || !in_region((uintptr_t) p)) return false;
    Il2CppClass *k = p->klass;
    if (!k || !plausible_ptr(k)) return false;
    const char *cn = il2cpp_class_get_name(k);
    if (!cn || strcmp(cn, "VInt3") != 0) return false;
    int32_t x = 0, y = 0, z = 0;
    read_int_field(p, k, "x_", &x);
    read_int_field(p, k, "y_", &y);
    read_int_field(p, k, "z_", &z);
    fprintf(fp, " -> %s=(%d,%d,%d)\n", label, (int) x, (int) y, (int) z);
    return true;
}

static void print_compact(FILE *fp, Il2CppObject *obj, const char *cls) {
    Il2CppClass *k = obj->klass;
    int32_t rid = 0, cfg = 0, camp = 0, hp = 0;
    bool has_rid = read_int_field(obj, k, "runtimeId_", &rid);
    read_int_field(obj, k, "configId_", &cfg);
    read_int_field(obj, k, "camp_", &camp);
    read_int_field(obj, k, "hpRatio_", &hp);
    fprintf(fp, "C %s ptr=%p", cls, (void *) obj);
    if (has_rid) fprintf(fp, " rid=%d", (int) rid);
    fprintf(fp, " cfg=%d camp=%d hp=%d", (int) cfg, (int) camp, (int) hp);

    Il2CppObject *loc = read_ref_field(obj, k, "optLocation_");
    if (!loc) loc = read_ref_field(obj, k, "location_");
    if (loc) {
        if (!print_vint3(fp, "loc", loc)) fprintf(fp, " loc=?\n");
    } else {
        fprintf(fp, " loc=none\n");
    }
    Il2CppObject *lv = read_ref_field(obj, k, "optLastVisibleLocation_");
    if (!lv) lv = read_ref_field(obj, k, "lastVisibleLocation_");
    if (lv) print_vint3(fp, "lastvis", lv);

    int32_t tmp = 0;
    if (read_int_field(obj, k, "frameNo_", &tmp)) fprintf(fp, " frameNo=%d\n", (int) tmp);
    if (read_int_field(obj, k, "lastVisibleFrameNo_", &tmp))
        fprintf(fp, " lastVisibleFrameNo=%d\n", (int) tmp);
}

static void print_full(FILE *fp, Il2CppObject *obj, const char *cls) {
    Il2CppClass *k = obj->klass;
    fprintf(fp, "OBJ %s ptr=%p", cls, (void *) obj);
    void *iter = nullptr;
    FieldInfo *f;
    while ((f = il2cpp_class_get_fields(k, &iter)) != nullptr) {
        const char *name = il2cpp_field_get_name(f);
        const Il2CppType *t = il2cpp_field_get_type(f);
        size_t off = il2cpp_field_get_offset(f);
        auto *base = (uint8_t *) obj + off;
        if (!name || !t) continue;
        switch (t->type) {
            case IL2CPP_TYPE_BOOLEAN:
            case IL2CPP_TYPE_I1:
            case IL2CPP_TYPE_U1:
                fprintf(fp, "\n  %s=%d", name, (int) *(int8_t *) base);
                break;
            case IL2CPP_TYPE_I2:
            case IL2CPP_TYPE_U2:
            case IL2CPP_TYPE_CHAR:
                fprintf(fp, "\n  %s=%d", name, (int) *(int16_t *) base);
                break;
            case IL2CPP_TYPE_I4:
            case IL2CPP_TYPE_U4:
            case IL2CPP_TYPE_ENUM:
                fprintf(fp, "\n  %s=%d", name, (int) *(int32_t *) base);
                break;
            case IL2CPP_TYPE_I8:
            case IL2CPP_TYPE_U8:
                fprintf(fp, "\n  %s=%lld", name, (long long) *(int64_t *) base);
                break;
            case IL2CPP_TYPE_R4:
                fprintf(fp, "\n  %s=%.4f", name, (double) *(float *) base);
                break;
            case IL2CPP_TYPE_R8:
                fprintf(fp, "\n  %s=%.4f", name, *(double *) base);
                break;
            case IL2CPP_TYPE_CLASS:
            case IL2CPP_TYPE_OBJECT:
            case IL2CPP_TYPE_STRING:
            case IL2CPP_TYPE_SZARRAY:
            case IL2CPP_TYPE_ARRAY:
            case IL2CPP_TYPE_GENERICINST: {
                void *p = *(void **) base;
                fprintf(fp, "\n  %s=%p", name, p);
                if (plausible_ptr(p) && in_region((uintptr_t) p)) {
                    auto *po = (Il2CppObject *) p;
                    Il2CppClass *pk = po->klass;
                    if (plausible_ptr(pk)) {
                        const char *pcn = il2cpp_class_get_name(pk);
                        if (pcn) fprintf(fp, "(%s)", pcn);
                    }
                    print_vint3(fp, name, po);
                }
                break;
            }
            default:
                break;
        }
    }
    fprintf(fp, "\n");
}

// 在 [s,e) 里找"头 8 字节 == klass"的对象
static int scan_region(Il2CppClass *klass, const Region &r, Il2CppObject **out, int maxout, int *total) {
    static uint8_t *buf = nullptr;
    const size_t CHUNK = 1u << 20;
    if (!buf) {
        buf = (uint8_t *) malloc(CHUNK);
        if (!buf) return 0;
    }
    int fd = open("/proc/self/mem", O_RDONLY);
    if (fd < 0) return 0;
    const uint64_t key = (uint64_t) (uintptr_t) klass;
    int found = 0;
    for (uintptr_t p = r.start; p < r.end; p += CHUNK) {
        size_t want = (r.end - p) < CHUNK ? (size_t) (r.end - p) : CHUNK;
        ssize_t got = pread(fd, buf, want, (off_t) p);
        if (got <= 0) continue;
        for (ssize_t i = 0; i + 8 <= got; i += 8) {
            if (*(uint64_t *) (buf + i) == key) {
                (*total)++;
                if (found < maxout) {
                    out[found] = (Il2CppObject *) (p + (uintptr_t) i);
                    found++;
                }
            }
        }
    }
    close(fd);
    return found;
}

// ============================ 采样主循环 ============================

static void reader_main(const char *game_data_dir) {
    Il2CppDomain *domain = il2cpp_domain_get ? il2cpp_domain_get() : nullptr;
    if (domain && il2cpp_thread_attach) il2cpp_thread_attach(domain);
    LOGI("pos_reader v2: started, dir=%s", game_data_dir ? game_data_dir : "(null)");
    if (!game_data_dir) return;

    char path[512];
    snprintf(path, sizeof(path), "%s%s", game_data_dir, POS_LOG_REL);

    // 解析目标类（VM 刚起来时可能还没就绪，重试一会儿）
    for (int attempt = 0; attempt < 60; ++attempt) {
        int found = 0;
        for (int i = 0; i < TARGET_COUNT; ++i) {
            if (!g_klass[i]) g_klass[i] = find_class(TARGETS[i]);
            if (g_klass[i]) found++;
        }
        if (found > 0) {
            for (int i = 0; i < TARGET_COUNT; ++i) {
                if (!g_klass[i] && !g_reported[i]) {
                    g_reported[i] = true;
                    LOGW("pos_reader: class not found %s.%s", TARGETS[i].ns, TARGETS[i].name);
                }
            }
            LOGI("pos_reader: resolved %d/%d classes", found, TARGET_COUNT);
            break;
        }
        usleep(500 * 1000);
    }
    load_regions();

    for (;;) {
        FILE *fp = fopen(path, "a");
        if (!fp) {
            LOGE("pos_reader: cannot open %s", path);
            return;
        }
        fseek(fp, 0, SEEK_END);
        if (ftell(fp) > POS_LOG_MAX) {
            fclose(fp);
            fp = fopen(path, "w");
            if (!fp) return;
            fprintf(fp, "# rotated at t=%ld\n", now_ms());
        }
        long t = now_ms();
        int written = 0;
        for (int i = 0; i < TARGET_COUNT; ++i) {
            if (!g_klass[i]) continue;
            Il2CppObject *objs[POS_MAX_OBJ];
            int total = 0;
            int n = 0;
            for (const auto &r : g_regions) {
                n += scan_region(g_klass[i], r, objs + n, POS_MAX_OBJ - n, &total);
                if (n >= POS_MAX_OBJ) break;
            }
            if (total <= 0) continue;
            fprintf(fp, "S t=%ld cls=%s.%s total=%d shown=%d\n", t, TARGETS[i].ns, TARGETS[i].name,
                    total, n);
            for (int j = 0; j < n; ++j) {
                if (j < POS_FULL_OBJ) print_full(fp, objs[j], TARGETS[i].name);
                print_compact(fp, objs[j], TARGETS[i].name);
                written++;
            }
        }
        fflush(fp);
        fclose(fp);
        if (written > 0) LOGI("pos_reader: t=%ld samples=%d", t, written);
        usleep(POS_INTERVAL_MS * 1000);
    }
}

void pos_reader_start(const char *game_data_dir) {
    char *dir = nullptr;
    if (game_data_dir) {
        size_t n = strlen(game_data_dir) + 1;
        dir = new char[n];
        memcpy(dir, game_data_dir, n);
    }
    std::thread reader(reader_main, dir);
    reader.detach();
}
