// 让本插件统一使用 LeviLamina 的内存分配运算符。
// 缺少该文件时，LeviLamina 会拒绝加载模组（提示"未使用统一的内存分配运算符"）。

#define LL_MEMORY_OPERATORS

#include "ll/api/memory/MemoryOperators.h" // IWYU pragma: keep
