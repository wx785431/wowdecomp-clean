# 64 位 Wow 客户端读取加密补丁（DINPUT8 代理 DLL）

给 **x64** 客户端用的一个替身 `DINPUT8.dll`：磁盘上的 `patch-zhCN-*.mpq` 继续用
`MqpEncrypt.exe` 加密（双击打不开），客户端启动后自己把前 512 字节还原，正常加载补丁。

- 不改客户端、不改登录器、不用注入器
- 不需要 `encrypted_key.bin`（那套 AES 是另一条链路，跟这个加密无关）
- 32 位老客户端继续用原来那个 32 位的 `dinput8.dll`，两边互不影响

---

## 一、加密规则（已实测确认）

`MqpEncrypt.exe` 只改动 MPQ 的**前 512 字节**，文件大小不变：

| 位置 | 处理 |
|---|---|
| 字节 0~2 | 固定写成 `5E 24 26`（抹掉 `MPQ\x1A` 魔数）|
| 字节 3~511 | `密文[i] = 明文[i] ^ PAD[i % 16]` |
| 512 之后 | 与原包**逐字节相同** |

```
PAD = 61 5E D7 FB 15 04 B0 C7 24 A2 96 D7 A6 9E 6C 7B
```

DLL 做的事就是：凡是读到落在这 512 字节范围内的数据，就地还原；其它一律不碰。

---

## 二、云编译（GitHub Actions，不用装任何环境）

1. 把本目录整个推到你的仓库（保持目录结构，`.github/workflows/build.yml` 要在仓库根）；
2. 仓库 → **Actions** → 左边选 `build-dinput8-x64` → **Run workflow**；
3. 跑完点进这次运行，最下面 **Artifacts** 下载 `DINPUT8-x64`，解压出一个 `DINPUT8.dll`。

这个工作流会先跑两个自检，两个都绿才会出产物：

- `selftest.exe` —— 校验 512 字节还原公式（用真实补丁的头 16 字节做向量）
- `hooktest.exe` —— 真的把 `DINPUT8.dll` 加载进进程，再 `CreateFile + ReadFile` 一个加密样本，
  检查读出来的头是不是 `MPQ\x1A`、定位读取对不对、512 之后有没有被动过

---

## 三、部署（3 步）

> ⚠️ 第 1 步最容易漏：客户端根目录里现在那个 `dinput8.dll` 是 **32 位**的，
> 64 位 Wow.exe 静态导入了它，留着必然报 **0xc000007b**（应用程序无法正常启动）。

1. **把旧的 32 位 DLL 改名**（别删，32 位客户端还要用）：
   把 `D:\World of for\dinput8.dll` 改名为 `dinput8_x86.dll.bak`；
2. 把编译出来的 **x64 `DINPUT8.dll`** 放进 `D:\World of for\`；
3. 直接启动 `Wow.exe`。加密补丁照常放 `Data\zhCN\`，不用还原、不用改。

`encrypted_key.bin` 可以留着，也可以删，本方案不读它。

---

## 四、怎么确认它真的生效了

启动一次游戏后看客户端根目录的 **`WowProxy64.log`**：

```
=== DINPUT8 代理已就绪，PID=12345 ===
[read] 已还原头部: \\?\D:\World of for\Data\zhCN\patch-zhCN-Y.mpq
[read] 已还原头部: \\?\D:\World of for\Data\zhCN\patch-zhCN-Z.mpq
```

- 有 `已还原头部` 行 → 生效了，补丁在加载
- 只有"已就绪"一行、没有任何还原记录 → 客户端没走到钩子上（见下面第 2 条）
- 连日志都没有 → DLL 没被加载（名字不对 / 还在用 32 位那个 / 放错目录）

---

## 五、出问题先看这三条

1. **启动就 0xc000007b**
   旧的 32 位 `dinput8.dll` 还在。改名，再放我们的 x64 版。

2. **日志有"已就绪"但补丁不生效**
   说明客户端读 MPQ 没经过 `ReadFile`。把 `WowProxy64.log` 发我，
   我按它的实际读取方式（可能是 `ReadFileEx` / 完整的 mmap / 异步 IO）补钩子。
   `MapViewOfFile` 那条路已经兜了（用写时复制，不落盘、不改文件）。

3. **游戏变慢或卡加载**
   本 DLL 每次 `ReadFile` 会问一次当前文件偏移（一次很轻的调用），
   只有落在前 512 字节的读取才做还原。正常不会有感知。
   真觉得慢告诉我，我改成按句柄缓存偏移。

---

## 六、目录

```
wow64-mpq-proxy/
├─ src/
│  ├─ dinput8_proxy.cpp   代理 DLL 全部逻辑（转发导出 + IAT 钩子 + 还原）
│  └─ dinput8.def         6 个导出，与系统 dinput8.dll 完全一致
├─ tools/
│  └─ hooktest.cpp        端到端钩子测试
├─ CMakeLists.txt
└─ .github/workflows/build.yml
```

## 七、说明一句

这套加密的防护力本来就很低（固定 16 字节密钥流、只改 512 字节），
它能做到的是"**双击 MPQEditor 打不开**、玩家没法随手翻补丁"，
挡不住真想拆的人。要更强的保护得换方案（整包 AES / 关键数据放服务端）。
