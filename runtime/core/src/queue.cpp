// SynapseMatrix — Queue 实现（当前为 header-only，本文件作为编译单元占位）
// =======================================================================
// TaskQueue 全部实现在 queue.hpp 内联完成（模板化/头文件库风格），
// 此 .cpp 只为 CMake 提供一个稳定的编译单元：
//   - 未来若队列需要独立实现（如无锁队列 V2），在此落实现文件
//   - 当前保留，保证 add_library(synapse_core STATIC ...) 有源可编

#include "runtime/queue.hpp"
