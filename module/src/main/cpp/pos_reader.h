//
// pos_reader.h — 实时对象采样器（Zygisk-Il2CppDumper 扩展）
//

#ifndef ZYGISK_IL2CPPDUMPER_POS_READER_H
#define ZYGISK_IL2CPPDUMPER_POS_READER_H

// 在 il2cpp 初始化完成之后调用；会起一个分离线程，周期性把指定类型
// 的实例字段写进 <game_data_dir>/files/pos.log
void pos_reader_start(const char *game_data_dir);

#endif //ZYGISK_IL2CPPDUMPER_POS_READER_H
