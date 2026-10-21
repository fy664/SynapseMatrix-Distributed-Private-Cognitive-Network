// SynapseMatrix — Device：设备信息（值类型，Month 3 项目对象之一）
// ====================================================================
// 规格书 Month 3 项目对象之一：Device。
// 语义：描述一个参与 SynapseMatrix 网络的设备（本机或远端）。
// V1 只记录静态信息；V2 接入网络层后承载身份/心跳/能力协商
// （Month 4 Network & P2P 用）。
//
// 为什么做成"值类型 + 普通 struct"：
//   - 设备信息是不可变描述（id/name/caps），无资源需管理 → 无 RAII 需求
//   - 值语义（可拷贝）让 Device 能放进容器、跨线程传递
//
// 语法讲解：
//   - struct 默认 public；这里存数据，不需要封装
//   - 默认构造函数：C++ 会生成，所有成员用 = 初始化的默认值
//   - std::vector<std::string> caps：能力列表（"cpu"、"gpu"、"memory"…）

#pragma once

#include <string>
#include <vector>

namespace synapse::runtime {

// 设备能力枚举：能力协商时用（V2 扩展）
enum class Capability {
    Cpu,      // 可执行 CPU 推理
    Gpu,      // 可执行 GPU 推理（本地有独立显卡）
    Memory,   // 可做记忆存储节点
};

struct Device {
    std::string id;                 // 设备唯一标识（V2 用 UUID）
    std::string name;               // 人类可读名（如 "home-pc"）
    std::vector<Capability> caps;   // 能力列表
    bool online = false;            // 当前是否在线（网络层更新）
};

}  // namespace synapse::runtime
