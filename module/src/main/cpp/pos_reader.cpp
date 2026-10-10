//
// pos_reader.cpp — 实时对象采样器（Zygisk-Il2CppDumper 扩展）
//
// 做一件事：**不 hook、不改游戏**，只用 il2cpp 官方 API，每秒枚举一次
// 指定类型的托管对象实例，把字段值写进 <app_data_dir>/files/pos.log。
//
// 目标类型来自对王者荣耀(com.tencent.tmgp.sgame 12.1.1.1)的 dump.cs 分析：
//   AivoiceV2 / Aivoice 命名空间下的 GameInfoDynamic / HeroUpdate / MonsterUpdate /
//   OrganUpdate / SoldierUpdate / CakeUpdate 是"按帧下发的战场坐标快照"，
//   坐标最终落在 VInt3 的 x_(0x10) / y_(0x14) / z_(0x18) 三个 Int32 上。
//   很多字段是 protobuf oneof（Object optXxx_ + OptXxxOneofCase），所以这里按
//   **字段名**解析，不依赖固定偏移。
//
// 输出格式（每行可 grep）：
//   S t=<ms> cls=AivoiceV2.HeroUpdate total=<实例总数> shown=<打印数>
//   H ptr=0x.. rid=1234 loc=(x,y,z)                       <- 紧凑行（所有实例）
//   OBJ ptr=0x..  ... 每个字段 name=value ...             <- 完整行（仅前几个实例）
//
#include "pos_reader.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>
#include <unistd.h>

#include "log.h"
#include "il2cpp-class.h"

// 复用 il2cpp_dump.cpp 里定义好的 API 函数指针
#define DO_API(r, n, p) extern r (*n) p;
#include "il2cpp-api-functions.h"
#undef DO_API

// ============================ 可调参数 ============================
static const int   POS_INTERVAL_MS = 1000;        // 采样间隔
static const int   POS_MAX_OBJ     = 128;         // 每类每轮最多记录多少实例
static const int   POS_FULL_OBJ    = 2;           // 每类每轮打印几个"全字段"
static const char *POS_LOG_REL     = "/files/pos.log";
static const long  POS_LOG_MAX     = 64L * 1024 * 1024;

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

// ============================ 小工具 ============================

static long now_ms() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long) (ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

static bool plausible_ptr(void *p) {
    auto v = (uintptr_t) p;
    return v > 0x10000 && (v & 7) == 0;
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

// 读一个 int32 字段（按名字找，找不到返回 false）
static bool read_int_field(Il2CppObject *obj, Il2CppClass *klass, const char *name, int32_t *out) {
    if (!obj || !klass || !name) return false;
    FieldInfo *f = il2cpp_class_get_field_from_name(klass, name);
    if (!f) return false;
    int32_t v = 0;
    il2cpp_field_get_value(obj, f, &v);
    *out = v;
    return true;
}

// 读一个引用字段（按名字找），返回对象指针
static Il2CppObject *read_ref_field(Il2CppObject *obj, Il2CppClass *klass, const char *name) {
    if (!obj || !klass || !name) return nullptr;
    FieldInfo *f = il2cpp_class_get_field_from_name(klass, name);
    if (!f) return nullptr;
    void *p = nullptr;
    il2cpp_field_get_value(obj, f, &p);
    return plausible_ptr(p) ? (Il2CppObject *) p : nullptr;
}

// 若 p 指向一个 VInt3 实例，则打印它的 x_/y_/z_
static bool print_vint3(FILE *fp, const char *label, Il2CppObject *p) {
    if (!p || !plausible_ptr(p)) return false;
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

// 一行的紧凑信息：runtimeId_ + 坐标
static void print_compact(FILE *fp, Il2CppObject *obj, const char *cls) {
    Il2CppClass *k = obj->klass;
    int32_t rid = 0, cfg = 0, camp = 0, hp = 0;
    bool has_rid = read_int_field(obj, k, "runtimeId_", &rid);
    read_int_field(obj, k, "configId_", &cfg);
    read_int_field(obj, k, "camp_", &camp);
    read_int_field(obj, k, "hpRatio_", &hp);
    fprintf(fp, "C %s ptr=%p", cls, obj);
    if (has_rid) fprintf(fp, " rid=%d", (int) rid);
    fprintf(fp, " cfg=%d camp=%d hp=%d", (int) cfg, (int) camp, (int) hp);

    // 坐标：oneof 名字优先，其次直连名字
    Il2CppObject *loc = read_ref_field(obj, k, "optLocation_");
    if (!loc) loc = read_ref_field(obj, k, "location_");
    if (loc) print_vint3(fp, "loc", loc);
    else fprintf(fp, " loc=none\n");

    Il2CppObject *lv = read_ref_field(obj, k, "optLastVisibleLocation_");
    if (!lv) lv = read_ref_field(obj, k, "lastVisibleLocation_");
    if (lv) print_vint3(fp, "lastvis", lv);

    int32_t frame_no = 0;
    if (read_int_field(obj, k, "frameNo_", &frame_no)) fprintf(fp, " frameNo=%d\n", (int) frame_no);
    int32_t lvframe = 0;
    if (read_int_field(obj, k, "lastVisibleFrameNo_", &lvframe))
        fprintf(fp, " lastVisibleFrameNo=%d\n", (int) lvframe);
}

// 完整字段打印（前几个实例）
static void print_full(FILE *fp, Il2CppObject *obj, const char *cls) {
    Il2CppClass *k = obj->klass;
    fprintf(fp, "OBJ %s ptr=%p", cls, obj);
    void *iter = nullptr;
    FieldInfo *f;
    while ((f = il2cpp_class_get_fields(k, &iter)) != nullptr) {
        const char *name = il2cpp_field_get_name(f);
        const Il2CppType *t = il2cpp_field_get_type(f);
        size_t off = il2cpp_field_get_offset(f);
        auto *base = (uint8_t *) obj + off;
        if (!name || !t) continue;
        // 只打标量；引用类型单独处理
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
                if (plausible_ptr(p)) print_vint3(fp, name, (Il2CppObject *) p);
                break;
            }
            default:
                break;
        }
    }
    fprintf(fp, "\n");
}

// ============================ 采样 ============================

struct WalkState {
    Il2CppClass *klass;
    Il2CppObject *objs[POS_MAX_OBJ];
    int n;
    int total;
};

static void heap_cb(void *data, void *user_data) {
    auto *st = (WalkState *) user_data;
    auto *obj = (Il2CppObject *) data;
    if (!obj) return;
    if (obj->klass != st->klass) return;   // 只比较对象头, 回调里不做任何分配/IO
    st->total++;
    if (st->n < POS_MAX_OBJ) st->objs[st->n++] = obj;
}

static void reader_main(const char *game_data_dir) {
    Il2CppDomain *domain = il2cpp_domain_get ? il2cpp_domain_get() : nullptr;
    if (domain && il2cpp_thread_attach) il2cpp_thread_attach(domain);
    LOGI("pos_reader: started, dir=%s", game_data_dir ? game_data_dir : "(null)");
    if (!game_data_dir) return;

    char path[512];
    snprintf(path, sizeof(path), "%s%s", game_data_dir, POS_LOG_REL);

    // 解析目标类（VM 刚起来时可能还没就绪，重试一会儿）
    for (int attempt = 0; attempt < 120; ++attempt) {
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
            break;
        }
        usleep(500 * 1000);
    }

    for (;;) {
        FILE *fp = fopen(path, "a");
        if (!fp) {
            LOGE("pos_reader: cannot open %s", path);
            return;
        }
        fseek(fp, 0, SEEK_END);
        if (ftell(fp) > POS_LOG_MAX) {          // 太大就重开
            fclose(fp);
            fp = fopen(path, "w");
            if (!fp) return;
            fprintf(fp, "# rotated at t=%ld\n", now_ms());
        }
        long t = now_ms();
        int written = 0;
        for (int i = 0; i < TARGET_COUNT; ++i) {
            if (!g_klass[i]) continue;
            WalkState st{};
            st.klass = g_klass[i];
            st.n = 0;
            st.total = 0;
            if (il2cpp_gc_foreach_heap) il2cpp_gc_foreach_heap(heap_cb, &st);
            if (st.total <= 0) continue;
            fprintf(fp, "S t=%ld cls=%s.%s total=%d shown=%d\n", t, TARGETS[i].ns, TARGETS[i].name,
                    st.total, st.n);
            for (int j = 0; j < st.n; ++j) {
                if (j < POS_FULL_OBJ) print_full(fp, st.objs[j], TARGETS[i].name);
                print_compact(fp, st.objs[j], TARGETS[i].name);
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
    if (!il2cpp_gc_foreach_heap) {
        LOGW("pos_reader: il2cpp_gc_foreach_heap not available, skip");
        return;
    }
    char *dir = nullptr;
    if (game_data_dir) {
        size_t n = strlen(game_data_dir) + 1;
        dir = new char[n];
        memcpy(dir, game_data_dir, n);
    }
    std::thread reader(reader_main, dir);
    reader.detach();
}
