# D3D12 Final Project — MMD 角色 × Sponza 延遲渲染器 實驗報告

**NTUST 計算機圖學 — Final Project（D3D Graphics Programming）**

| 項目 | 內容 |
|------|------|
| 平台 | Windows 11 / Visual Studio 2022（Toolset v143）/ DirectX 12 |
| 語言 | C++17 + HLSL（Shader Model 5.1，執行期 `D3DCompileFromFile`） |
| 圖形卡 | NVIDIA GeForce RTX 3060（12 GB，feature level 11_0+） |
| 解析度 | 1280 × 720（vsync） |
| 場景 | Crytek Sponza（Assimp 載入）+ PMX 角色 LeMaline（Saba 載入、VMD 動畫） |

> 本份報告對應 **Final Project 的 `README.md` 技術清單**。Assignment 1（純兩階段 deferred shading 基礎）的細節另見 `Report.md`；本報告聚焦在 final project 在該基礎之上新增的進階項目，以及整體程式架構與使用方法。

---

## 1. 概述

本專案以 Assignment 1 自行搭建的 **DirectX 12 兩階段 Deferred Shading 管線**為基底（不使用任何既成 D3D12 渲染框架），向上擴充成一條涵蓋角色動畫、卡通渲染、多種陰影、Forward+ 大量光源、環境光遮蔽與 HDR 後處理的完整 real-time pipeline。

核心成果：

- 在 Sponza 場景中放入一個 **PMX 人型角色**，由 **VMD motion 驅動全身骨架動畫**，並以 **Cel Shading（梯度漫反射 + 輪廓描邊 + rim light）**著色。
- 角色的**裙擺/頭髮物理**與**表情（facial morph）**皆隨動畫即時運作。
- 光照軸實作了 **directional PCF shadow map**、**point-light cube 距離陰影 + bloom sphere 視覺化**，以及 **Forward+ tiled light culling（128 個點光源，compute shader 逐 tile cull）**。
- **SSAO**（world-space hemisphere kernel + noise + blur，可即時開關與調參）。
- **HDR pipeline**：lighting 寫入 RGBA16F → **Bloom** → **ACES Tone Mapping**（exposure + gamma）→ back buffer。
- 程式以 Console subsystem 啟動，同時開啟**主控台（接受文字指令）**與**渲染視窗（鍵鼠操作 + ImGui 控制面板）**；D3D12 Debug Layer 執行期**零 ERROR**。

依 `README.md` 的計分上限 100pt，本專案實作的項目加總**遠超過 100pt 上限**（詳見 §2）。

---

## 2. README 需求達成對照

`README.md` 為四大軸（Animation / Lighting-Shadows / Ambient Occlusion / Post-processing）的技術選單，總分上限 100pt。下表逐項對照本專案的達成情況。

### 2.1 達成項目

| 軸 | 項目 | 配分 | 狀態 | 本專案的對應實作 |
|----|------|------|------|------------------|
| Animation | **PMX Loader + Cel shading（Basic）** | 20 | ✅（被 Advanced 取代計分） | Saba 載入 LeMaline PMX，材質/貼圖/alpha-test 正確，cel-shaded，置於 Sponza |
| Animation | **PMX + VMD 骨架動畫 + Cel shading（Advanced）** | **70** | ✅ | `dz.vmd`（44025 bone keyframes）驅動 Saba 的 bone hierarchy / skinning，CPU skinning 後串流到 dynamic VB |
| Animation | ＋Facial animation | +10 | ✅ | dz.vmd 的 morph keyframes 對應 LeMaline 的標準日文表情 morph（あいうえお/まばたき…），Saba `UpdateAllAnimation` 直接驅動 |
| Animation | ＋裙擺/頭髮物理 | +25 | ✅ | Saba 內建的 Bullet 物理（`MMDPhysics`）即時模擬頭髮/裙擺 |
| Lighting | **Shadow Mapping — Directional（Basic）** | 5 | ✅ | 2048² D32 shadow map，正交視錐 fit Sponza bounds，光向可即時改變 |
| Lighting | ＋邊緣模糊（PCF） | +5 | ✅ | 3×3 `SampleCmpLevelZero` PCF + slope-scaled bias |
| Lighting | **Shadow Mapping — Point（Basic）** | 5 | ✅ | R32 TextureCube 6 面距離陰影 + **bloom sphere 視覺化點光源位置**（README 對 point light 的額外要求） |
| Lighting | **Forward+ Rendering（Advanced）** | **35** | ✅ | 128 點光源（>96 要求），16×16 tile，compute shader 逐 tile cull（含 depth bounds、overflow 上限處理），heatmap 視覺化 |
| Ambient Occlusion | **SSAO（Basic）** | 15 | ✅ | depth+normal 重建 view-space、hemisphere kernel、4×4 noise rotation、box blur、radius/bias/intensity 可調、可開關 |
| Post-processing | **Tone Mapping（Basic）** | 5 | ✅ | HDR(RGBA16F) 上完成 lighting，Narkowicz ACES + exposure + gamma 1/2.2 |
| Post-processing | **Bloom（Basic）** | 5 | ✅ | luma bright-pass → 半解析度 separable 9-tap Gaussian（H/V）→ tonemap 合成；threshold/intensity 可調 |
| Animation | ＋Camera motion | +5 | ✅ | VMD camera track（`cam.vmd` 與 seele `camera.vmd` 可切換），詳見 §4.12 |

**估算總分**：70（Advanced 動畫）+ 10（表情）+ 25（物理）+ 35（Forward+）+ 15（SSAO）+ 10（directional shadow 5 + PCF 5）+ 5（point shadow）+ 5（ToneMap）+ 5（Bloom）= **約 180pt → 受 100pt 上限封頂**。

即使完全不計動畫的兩項加分（表情、物理），主幹仍有 70 + 35 + 15 + 10 + 5 + 5 + 5 = **145pt**，依舊穩穩超過 100pt 上限。

### 2.2 未實作項目（皆非達標所需）

| 軸 | 項目 | 配分 | 說明 |
|----|------|------|------|
| Animation | IK | +15 | Saba `MMDIkSolver` 在 `UpdateAllAnimation` 內會解 IK（足ＩＫ 等），但未作為獨立 demo 賣點強調 |
| Lighting | VSM / ESM | 15 | 與 basic shadow 擇一；已做 basic + PCF，未做 moment-based |
| Lighting | LTC Area Light | 20 | 未實作 |
| Ambient Occlusion | RTAO（DXR） | 40 | 與 SSAO 擇一；已選 SSAO |
| Post-processing | TAA | 30 | 未實作 |

由於四軸總分已封頂於 100pt，上述進階替代項目對最終分數無影響。

### 2.3 共通規範遵守

- **嚴禁既成 D3D12 渲染框架**：✅ 全部 D3D12 device/swapchain/PSO/root signature/barrier 皆自行撰寫；第三方僅用 **Assimp**（Sponza 載入）、**Saba**（PMX/VMD *解析*，README 明列為參考 sample，非 rendering engine）、**DirectXTK12**（貼圖上載 / GraphicsMemory / ScreenGrab 工具）、**ImGui**（控制面板）、**Bullet**（Saba 物理依賴）。
- **45 fps @ 1280×720 全特效**：✅ Release 建置全特效開啟為 **60 fps（vsync 上限）**，超過 45 fps 預算（詳見 §7）。
- **基於 Assignment 1**：✅ 直接沿用 A1 的 deferred 基底。

---

## 3. 程式整體架構

### 3.1 模組切分

程式碼依職責切成數個聚焦的翻譯單元（`DeferredRenderer/src/`）：

| 檔案 | 負責 |
|------|------|
| `Main.cpp` | 進入點、Win32 視窗與 message pump、Raw Input、ImGui WndProc 轉發、Console 執行緒、**指令分派器 `HandleCommand`**、主迴圈（dt 計時 + FPS 標題） |
| `Renderer.h/.cpp` | D3D12 裝置/SwapChain、所有 PSO 與 root signature、資源建立、**整條 frame graph 的 `Render()`**、`Update()`、screenshot、ImGui 整合 |
| `Camera.h/.cpp` | 第一人稱 yaw/pitch 相機，`XMMatrixPerspectiveFovLH` |
| `Scene.h/.cpp` | VB/IB、Submesh 表、材質 SRV heap、world 矩陣、model-space bounds（Sponza 與角色共用此結構） |
| `AssetLoader.h/.cpp` | Assimp 拜訪 Sponza、頂點聚合、DDS 貼圖載入（DirectXTK12） |
| `MmdLoader.h/.cpp` | **`MmdAnimator`**（pimpl 包 Saba 的 PMXModel + VMDAnimation，負責推進動畫與 CPU skinning）、**`BuildSceneFromMmd`**（一次性建立角色 GPU 資源）、`ProbeMmd`（`mmd` 指令的煙霧測試） |
| `Console.h/.cpp` | 背景 `std::cin` 執行緒，mutex 保護的指令佇列 |
| `Input.h` | 鍵盤/滑鼠 delta/look-mode 旗標的共享狀態 |
| `Common.h` | `ComPtr`、`ThrowIfFailed`/`HrException`、`Widen`/`Narrow`、`AlignUp` |

Shader（`DeferredRenderer/shaders/`，執行期編譯）：`Geometry.hlsl`、`Lighting.hlsl`、`Shadow.hlsl`、`PointShadow.hlsl`、`LightCulling.hlsl`（compute）、`Ssao.hlsl`、`PostProcess.hlsl`、`BloomSphere.hlsl`。

### 3.2 兩個視窗、兩條輸入

連結器使用 `/SUBSYSTEM:CONSOLE`，loader 自動接好 stdin/stdout/stderr；渲染視窗額外以 `CreateWindowExW` 建立。`Console` 在背景執行緒以 `std::getline` 讀取指令推入 mutex 佇列，主執行緒每幀 Render 前消化佇列，因此讀指令不會阻塞 render loop。角色/物理載入時的 Saba/Bullet 雜訊由 `ScopedStdSilence`（RAII `_dup2` stdout/stderr → NUL）吞掉，保持主控台乾淨。

### 3.3 場景資料模型

`Scene` 是 Sponza 與角色共用的容器：VB/IB + `Submesh[]`（每個記 index 範圍與 diffuse SRV index）+ 一個 shader-visible SRV heap + `world` 矩陣 + model-space bounds。
- **Sponza**：Assimp 以 `PreTransformVertices` 攤平階層、焙進世界座標，故 `world` 維持 identity。
- **角色**：`MmdLoader` 保留 model-space 頂點與 bounds，placement 由 `Renderer::RecomputeCharacterWorld()` 從 `m_charPos/m_charScale/m_charYawDeg` + bounds 算出 `world`（可即時調整、且為動畫的接點）。

### 3.4 每幀資料流（Frame Graph）

`Render()` 內每幀的 pass 順序（資源狀態以單一 `barrier()` lambda 批次切換）：

```
[1] Shadow Pass（directional）   → m_shadowMap（D32，PSR↔DEPTH_WRITE）
[2] Point-Light Cube Shadow      → m_pointCube（R32 cube 6 面；僅在 m_pointShadowDirty 時重畫）
[3] Geometry Pass                → G-buffer：Normal(RGBA16F) + Albedo(RGBA8) + Depth(D32)
       ├ Sponza   ：CULL_BACK PSO，materialId=0（Blinn-Phong）
       └ 角色      ：CULL_NONE PSO，materialId=1（cel），讀 per-frame skinned VB
[4] SSAO                          → m_ssaoRT（R8）→ box blur → m_ssaoBlurRT
[5] Forward+ Light Culling（CS）  → m_tileLightBuffer（每 tile 的 light index 清單）
[6] Lighting Pass（全螢幕三角形） → m_sceneHDR(RGBA16F)
       讀 depth/normal/albedo/shadow/ssao/tileLights/pointCube，依 viewMode 與 materialId 分支
[7] Bloom Sphere（點光源標記）    → 畫入 m_sceneHDR（depth-test，發光）
[8] Bloom                        → bright → blurH → blurV（半解析度 bloom0/bloom1）
[9] Tone Map（ACES）             → back buffer
[10] ImGui                       → back buffer
[11] Present + DrainInfoQueue
```

幀末所有 RT 與 depth 都回到「下一幀預期的初始狀態」（RENDER_TARGET / DEPTH_WRITE），第一幀不需任何特例。

---

## 4. 各子系統怎麼寫成的

### 4.1 Deferred 基底（沿用 Assignment 1）

- **G-buffer 格式固定**（作業規範）：Depth `R32_TYPELESS`（DSV=`D32_FLOAT`、SRV=`R32_FLOAT`）、Normal `R16G16B16A16_FLOAT`（xyz 存世界法線、**a 存 materialId**）、Albedo `R8G8B8A8_UNORM`。
- **Lighting Pass 用全螢幕三角形**（非 quad）：VS 從 `SV_VertexID` 合成 NDC/UV，三次呼叫覆蓋整個 viewport，無對角線接縫。
- **PerFrame CB**（b0）被多條 pass 共用：`viewProj`、`invViewProj`（lighting 反推世界座標用）、`cameraPos/viewMode`、`lightDirToLight/zNear`、`lightIntensity/zFar`、`lightViewProj`、`shadowBias/shadowTexel`。HLSL 端 `row_major` 與 DirectXMath 直接相容、不需轉置。
- **世界座標重建**：為省一張 position G-buffer，lighting 用 `invViewProj × NDC(含深度)` 反推世界座標供 specular/陰影計算。

### 4.2 PMX 角色載入與材質（Animation Basic 的渲染品質基礎）

- 透過 **Saba**（`third_party/saba` 原始碼納入）載入 LeMaline PMX：153,033 頂點、818,010 索引、17 submesh、10 貼圖、259 bones、80 morph。
- `BuildSceneFromMmd` 一次性建立 index buffer、各材質 PNG diffuse（DirectXTK12 `CreateWICTextureFromFile`）、SRV heap、submesh 表、bind-pose VB；忠實依 PMX 材質表載入（不臆造 sphere/mask/normal）。
- **關鍵 bug 修正（V-flip）**：Saba 為 OpenGL 底-左原點在載入時翻轉 V 座標（`uv = (u, 1-v)`），直接餵給 D3D 頂-左原點 sampler 會讓眼睛/眉毛/睫毛/眼神光全部取樣到錯誤的貼圖列。解法是在 `BuildSceneFromMmd`（靜態 VB）與 `CopySkinnedVertices`（動畫 VB）兩處都把 V 翻回（`uv.y = 1 - uv.y`）。
- **角色 PSO = CULL_NONE + LESS_EQUAL**：LeMaline 所有材質 bothFace=1 且混合 winding，任何背面剔除都會破面，故角色不剔除。
- Geometry PS 對角色做 **alpha test `clip(tex.a - 0.3)`**：MMD 的眉/睫/眼神光在透明背景的平面上，需丟棄透明像素；門檻 0.3（非 0.5）才不會切掉柔邊虹膜。

### 4.3 Cel Shading（Animation 20pt 的著色要求）

在 deferred 管線中以 G-buffer `normal.a` 攜帶的 **materialId 選擇式著色**：0=Sponza 走 Blinn-Phong、1=角色走 `Lighting.hlsl::ShadeCel`。Cel 包含 README 要求的三要素：

- **梯度漫反射**：`N·L`（乘陰影 visibility）量化成 3–4 個固定色階（0.30/0.40/0.65/1.00）。
- **輪廓描邊**：`DetectCharacterEdge` 在螢幕空間以 ±1px taps 偵測「對背景的剪影 / 線性深度斷層 / 法線急轉」，命中即輸出近黑色（thin ~1px 線，整數 Load offset 與解析度無關）。
- **Rim light**：`smoothstep(0.55,1, 1-N·V)` 在側緣補光。
- 另有硬邊 toon specular（`step` 量化），並乘上 SSAO（0.6 權重）。

### 4.4 VMD 骨架動畫 + CPU Skinning（Animation Advanced 70pt 核心）

- 動畫資料 `data/Motion/dz.vmd`（44025 bone + 5982 morph keyframes）綁定 LeMaline 的標準骨架。
- `MmdAnimator`（pimpl 包 Saba）每幀執行 Saba 的標準序列：`BeginAnimation → UpdateAllAnimation(anim, animTime×30, dt) → EndAnimation → Update`（MMD 為 30fps，故時間 ×30 換算 frame）；`dt` clamp 到 ≤1/30 以穩定物理。
- **CPU skinning**：`Renderer::Update` 呼叫 `m_animator->Update(dt)`（推進 morph + node transform + 物理 + skinning），再 `StreamSkinnedVertices()` 把 Saba 的 `GetUpdatePositions/Normals/UVs` 複製進**每幀**從 DirectXTK `GraphicsMemory` 配出的 dynamic VB，並重指 `m_mmd.vbv`。placement 仍留在 PerObject `world` 矩陣（model-space skin → world）。
- 暫停（`m_animPaused`）時跳過 `Update(dt)` 但**仍重新串流** VB（因為它是 per-frame memory）。
- **表情（+10pt）**：dz.vmd 的 morph keyframes 命中 LeMaline 的標準日文 morph 名稱，`UpdateAllAnimation`→`GetUpdatePositions` 已自動變形臉部，**無需額外程式碼**。
- **頭髮/裙擺物理（+25pt）**：Saba 內建 Bullet（`MMDPhysics`）在 `Update` 內模擬，隨骨架即時擺動。

### 4.5 Directional Shadow Map + PCF（Lighting 5+5pt）

- 每幀第一個 pass：從平行光視角把 Sponza + 角色畫進 2048² D32 shadow map（`Shadow.hlsl` 為 depth-only VS）。
- 光矩陣 = 正交視錐 fit 場景 bounds：`XMMatrixLookAtLH(center + dirToLight·2r, center)` × `XMMatrixOrthographicLH(2r, 2r, 0.1, 4r)`。光向可由 `light x y z` 即時改變、陰影即時更新。
- PSO 用 `DepthBias` + `SlopeScaledDepthBias` 對抗 shadow acne。
- Lighting 端 `ShadowVisibility` 做 **3×3 PCF**（`SampleCmpLevelZero` + comparison sampler，LESS_EQUAL，邊界 white=視錐外視為受光），並加上 slope-scaled bias `max(bias·(1-N·L), bias·0.2)`。陰影同時作用於 cel 與 Blinn-Phong 兩條路徑。

### 4.6 Point-Light Cube 距離陰影 + Bloom Sphere（Lighting 5pt）

- 一個獨立於 Forward+ 的**會投影**全向點光源：`PointShadow.hlsl` 把場景畫 6 次（每面 90° FOV）到 R32_FLOAT `TextureCube`（512²/面），PS 存 `saturate(dist/range)`。
- Lighting 端 `PointShadowVisibility` 以「光→表面」方向取樣 cube、與儲存距離比較（0.02 bias）。
- **效能優化**：6 次全場景 pass 是最大開銷，故僅在 `m_pointShadowDirty`（光源/角色移動）時重畫，否則 cube 常駐 —— 避免 60→30fps 掉幀。
- **Bloom sphere 視覺化**（README 對 point light 的要求）：`BloomSphere.hlsl` 在 lighting 後、bloom 前把一顆 emissive 小球（CULL_NONE、depth-test）畫進 sceneHDR，emissive>1 讓 bloom pass 使其發光，標記點光源位置。此 pass 也順便把 depth 還原成 DEPTH_WRITE 供下一幀。

### 4.7 Forward+ Tiled Light Culling（Lighting Advanced 35pt）

- **光源**：128 個點光源（`mt19937` 固定 seed 1337，散佈於 Sponza 中庭，飽和色相，radius 220–430），每幀在 `Update` 中繞 home position 公轉（動態）。每幀上傳到一塊 transient `GraphicsMemory` buffer，**同時供 compute（t0）與 lighting（t5）讀取**。
- **Compute（`LightCulling.hlsl`，`cs_5_1`）**：每個 16×16 tile 一個 thread group（256 threads）。
  1. 各 thread 折入自己像素的深度，`InterlockedMin/Max(asuint(z))` 求出 tile 的 **min/max 深度界**（跳過背景）。
  2. 由 8 個重建的 world-space 角點（`invViewProj`）建出緊緻的 6-plane 視錐。
  3. 256 threads 協同（strided）測試每個光源 sphere vs 6 planes，`InterlockedAdd` 進 groupshared 清單。
  4. 寫出 `g_tileLights`（扁平 uint 陣列；每 tile slot0=count、其後為 index，stride 65）。空 tile 寫 0 並 bail；超過 64 上限的溢位被 clamp 丟棄（README 要求處理 overflow）。
- **深度界是關鍵**：沒有它，走廊上每個 tile 都會吃到全部 128 燈（heatmap 全紅）；有了它每 tile 降到約 8–24 盞。
- **Lighting 端**：`AccumPointLights` 只加總該像素 tile 的清單（quadratic falloff × N·L），疊加在 directional 結果上；`fpDebugHeat` 改輸出藍→紅的 tile 燈數 heatmap。
- **資源狀態機**：tile buffer 每幀 UAV→(dispatch)→PSR（也是 cull 寫入的可見性 barrier）→(lighting 讀 t6)→UAV；depth 過渡為 `PIXEL|NON_PIXEL_SHADER_RESOURCE`（compute 以 non-pixel 讀）。

### 4.8 SSAO（Ambient Occlusion 15pt）

- `Ssao.hlsl::SsaoPS`：用 depth 重建 view/world-space position、讀 normal G-buffer，以 **surface normal 為中心的 16-sample hemisphere kernel**（非 sphere），用 4×4 noise tile 逐像素旋轉 TBN 減少 banding；範圍檢查 `smoothstep` 抑制 halo。輸出 full-res R8。
- `BlurPS`：4×4 box blur 抹掉 noise 旋轉花紋 → `m_ssaoBlurRT`。
- **參數可調**（radius/bias/intensity）且**可開關**（README 要求 enable/disable）：GUI checkbox、`ssao on|off`、`SsaoEnabledRef`。停用時 SsaoPS 直接回傳 1.0（pass 照跑以維持資源狀態一致）。
- SSAO RS 刻意用**三個各 1-SRV 的 table（t0/t1/t2）**：兩描述子的 table 會跨到 blur 輸出 RTV 槽而觸發 debug layer（曾踩過此坑）。

### 4.9 HDR / Bloom / Tone Mapping（Post-processing 5+5pt）

- Lighting 不再直接寫 back buffer，而是寫 **`m_sceneHDR`（RGBA16F）**，使高亮資訊在 LDR clamp 前保留。
- **Bloom**：`BrightPS` 以 luma 門檻（保留 hue）抽亮部 → 半解析度 → `BlurPS` separable 9-tap Gaussian（先 H 後 V，bloom0↔bloom1 ping-pong）。
- **Tone Map**：`TonemapPS` 把 `(hdr + bloomStrength·bloom) × exposure` 經 **Narkowicz ACES** → `pow(1/2.2)` gamma → 全域 vibrance 飽和度微推 → back buffer。
- **debug view 直通**：viewMode≠3（Depth/Normal/Albedo）時 tonemap 原樣輸出，確保 G-buffer 截圖顏色準確。
- 三個 PS 共用一條 `m_postRS`（兩個 1-SRV table + CBV + linear-clamp sampler），三顆 PSO 切換。

### 4.10 相機與輸入

- 第一人稱 yaw/pitch 相機，pitch 夾在 ±π/2−0.01 防 gimbal flip；移動 800 u/s、Shift ×4。
- **Alt 切換式 look mode**：按 Alt 進/出鎖定；鎖定中以 **`WM_INPUT` Raw HID delta** 轉相機（與游標位置無關，避免 `SetCursorPos`/`WM_MOUSEMOVE` 的 race），`ClipCursor`+置中+隱藏游標，`WM_KILLFOCUS` 失焦強制解鎖。RMB+滑鼠在 look mode 中改為平移。
- Z 鍵與 `cycle`/`view` 依規範循環 **Depth → Normal → Albedo → Color**。

### 4.11 D3D12 Debug Layer 與健全性

- Debug 組態啟用 `ID3D12Debug` + DXGI debug factory；`ID3D12InfoQueue` 對 CORRUPTION break-on，並透過 storage filter 濾掉 INFO。
- 本次新增 **`Renderer::DrainInfoQueue()`**：每幀 Present 後把 InfoQueue 內的 WARNING/ERROR/CORRUPTION 抽出、去重後印到 stderr（ERROR 不再無 debugger 時靜默崩潰，而是可觀測）。實測完整操作序列（cycle/四 view/fplus/plight/ssao/shot/quit）**無任何 D3D12 WARNING/ERROR**。

### 4.12 可選擇的動作與相機（多 VMD 切換）

- **動作 clip 清單**：`Renderer::AddMotionClip(name, vmd, bgm)` 註冊一個 clip（VMD + 可選 BGM）。Clip 0 = 預設舞蹈（`dz.vmd` + `bgm.wav`），Clip 1 = `兔子洞修 fit for seele`（215,887 bone / 3,671 morph keyframes）+ 其專屬舞曲。GUI「Dance」下拉選單與主控台 `motion [list|<index>]` 可即時切換。
- **執行緒安全的切換**：動畫更新在 worker thread（見 §4.13），由它獨佔 `MmdAnimator`；切換時 GUI/console 只把 index 存進 atomic，真正的重載（停 worker → `LoadMotion` → 換 BGM → seek 0 → 重啟 worker）延到 render thread 的 `Update()` 執行，避免 data race。
- **相機 track 清單**：同樣以 `AddCameraClip(name, vmd)` 註冊。Clip 0 = `data/Motion/cam.vmd`，Clip 1 = seele 附的 `camera.vmd`（171 keyframes，比預設的 3 keyframes 豐富許多）。GUI「Cam track」下拉選單 + 主控台 `cam on|off|list|<index>`，同一時間只有一條 track 生效，故**不與其他相機動作衝突**。
- 相機 track 以 Saba `VMDCameraAnimation`+`MMDLookAtCamera` 求值（FOV 為弧度，eye/center/up 在模型自身空間），再用角色的 world 矩陣轉到世界座標，故鏡頭會正確框住放好的角色。**這同時補上了 README 的 Camera motion（+5pt）項目**（原報告 §2.2 列為未實作）。

### 4.13 舞曲 BGM 與「音畫同步」（XAudio2 + Media Foundation）

- **BGM 播放**：`src/Audio.{h,cpp}`（pimpl 包 XAudio2）。`.wav` 走自寫的 RIFF chunk 掃描器；**`.mp3`／其他壓縮格式則用 Media Foundation Source Reader 解碼成 16-bit PCM** 後走同一條 XAudio2 路徑。兩個踩過的坑：(1) 同步 Source Reader 在 **STA 執行緒會卡死**（主執行緒因 WIC 貼圖載入而處於 STA），故解碼放在一條專用的 **MTA 執行緒**執行再 join；(2) `Load` 必須**先銷毀舊的 source voice 再解碼進 `pcm` buffer**——正在播放的 voice 仍引用該 buffer，解碼時 realloc 會造成 use-after-free（`0xC0000005`）。
- **音畫同步（音訊為主時鐘）**：worker thread 每幀以 `audioPos = Audio::PositionSeconds()` 設定動畫姿勢（`UpdateTo`），故舞蹈與音樂**永不漂移**、一個循環同時回到起點。pause/replay 也讓音樂與動畫同步停/重起。
- **動作速度時間縮放**：seele 舞蹈（160.3 s）比舞曲（176.4 s）短，直接 1:1 播會「比音樂快、提早結束又重頭」。解法是把舞蹈**時間縮放**去填滿整首歌：`poseTime = audioPos × m_animSpeed`，`m_animSpeed` 預設 = `motionLen/musicLen` 並微調，最終定在使用者實測的 **0.92**（GUI「Dance speed」滑桿 + 主控台 `animspeed <v>` 可即時微調）。預設 `dz` clip 兩者等長故維持 1.0。
- **相機微調**：seele 的 `camera.vmd` 是為較高的模型編的，套到較矮的 LeMaline 會把角色框在偏下方；新增 `m_camYOffset`（GUI「Cam height」滑桿 + `camy <v>`）對求值後的 eye+target 做垂直位移把整體鏡頭往下帶，重新框住角色。

### 4.14 角色透視遮蔽物（X-ray reveal）

VMD 相機會繞到 Sponza 柱子/牆後，使角色被擋住看不到。需求是：**在角色周圍一個螢幕圓形範圍內，逐像素偵測角色與鏡頭之間是否有物體遮擋，有的話就讓該物體「透視」露出角色**（而非單純把整個角色貼在最上層）。

實作刻意**不另開 scene-depth 複製或額外 buffer**（效能考量），只多一個 depth-only PSO：
- `m_charXrayDepthPSO`（複製角色 no-cull geometry PSO，但 `DepthFunc=ALWAYS`、寫深度、無 render target）。其 PS `PSXrayReveal` 算出像素到角色投影中心的距離，套 **4×4 Bayer dither** 對 `edge×strength`，命中就把深度重設為遠（`SV_Depth=1`），否則 `discard`。
- 在角色繪製前先跑這個 pass，於是「圓形範圍內」角色像素的場景深度被清掉，接著正常的角色繪製（`LESS_EQUAL`）就會贏過遮擋牆而顯示，且**仍正確自我遮蔽、寫入真實深度**，故 deferred 光照/陰影維持正確；範圍外或無遮擋處則維持正常遮蔽。
- dither 配合主繪製的深度測試自動產生半透明效果：被遮擋處露出的角色與牆面以 dither 交錯（看起來像透視牆），非遮擋處則為實心角色。GUI「X-ray reveal」開關 + 「X-ray window」(範圍) /「X-ray opacity」(濃度) 滑桿，主控台 `xray on|off`。角色投影中心/半徑由 CPU 端用角色 world×viewProj 投影其包圍盒中心與頂點算出。

### 4.15 視窗縮放穩定性

拖曳視窗邊框放大時 Windows 會進入它自己的 modal 訊息迴圈、卡住主 render loop。直接在 `WM_SIZE` 內重繪會在 resize 後立刻觸發 bloom 的 `Bloom0 DATA_STATIC` debug-layer 錯誤 → GPU fault → 下一幀 fence 等待卡死。最終採**最穩的做法：拖曳期間不重繪、把 swap-chain／G-buffer 的重建延到拖曳結束（`WM_EXITSIZEMOVE`）**才做一次（以 `AppState::sizing` 由 `WM_ENTERSIZEMOVE/EXITSIZEMOVE` 標記；maximize/snap 等非拖曳變更則立即 resize、由主迴圈重繪下一幀）。視窗 class 背景設為黑色 brush 讓拖曳中放大的區域乾淨。如此正常 render 路徑全程乾淨、**無任何 D3D12 ERROR、不再卡死**。

---

## 5. 關鍵資料結構與 Constant Buffer 配置

| CB | register | 內容（重點） |
|----|----------|-------------|
| `PerFrameCB`（b0，多 pass 共用） | b0 | viewProj、invViewProj、cameraPos/viewMode、lightDirToLight/zNear、lightIntensity/zFar、lightViewProj、shadowBias/shadowTexel |
| `PerObjectCB` | b1 | world、materialId（0/1）、useFaceMask/useNormalMap/satBoost、view、useSphere、faceMaskStrength/sphereStrength、contrast |
| `ForwardPlusCB` | b1（lighting） | tileCount、numLights、enabled、debugHeat、pointEnabled、dirEnabled、點光源 pos/range/color/intensity |
| `SsaoCB` | b1（ssao） | radius、bias、intensity、enabled、screen size |
| `CullCB` | b0（compute） | invViewProj、cameraPos、numLights、screenSize、tileCount |
| `PostCB` | b0（post） | exposure、bloomStrength、threshold、vertical、invTexel、viewMode、vibrance |
| `PointShadowCB` / `SphereCB` | b0 | 各面 viewProj / 點光源位置範圍；球心半徑 emissive |

Heap 佈局（`Renderer.h` 常數）：
- **RTV heap**（kFrameCount+7）：2 back buffers、normal、albedo、sceneHDR、bloom0、bloom1、ssao、ssaoBlur。
- **DSV heap**：slot0 主深度、slot1 directional shadow、slot2 point cube depth。
- **Shader-visible SRV heap**（11 槽）：depth、normal、albedo、sceneHDR、bloom0、bloom1、shadow、ssao、ssaoBlur、noise、pointCube。

---

## 6. 使用方法

### 6.1 建置

依賴以 **vcpkg manifest mode** 管理（`vcpkg.json` 宣告 assimp / directxtk12 / directx-headers / glm / bullet3 / fmt / imgui）。**重要陷阱**：專案根目錄含中文（`工作\課`），vcpkg 的 post-build pkgconf 步驟無法 round-trip 非 ASCII 路徑；且 VS 內附的 vcpkg 是唯讀（artifacts-only）。因此：

```bat
:: 1. 另外 clone 一份 vcpkg 到純 ASCII 路徑
git clone https://github.com/microsoft/vcpkg.git C:\dev\vcpkg
C:\dev\vcpkg\bootstrap-vcpkg.bat

:: 2. 安裝相依（buildtrees / packages / install root 全部用 ASCII 路徑）
C:\dev\vcpkg\vcpkg.exe install ^
    --triplet=x64-windows ^
    --x-manifest-root="<專案根>" ^
    --x-install-root=C:\vcpkg-cache\installed ^
    --x-buildtrees-root=C:\vcpkg-cache\bld ^
    --x-packages-root=C:\vcpkg-cache\pkg ^
    --disable-metrics
```

接著用 VS2022 開 `DeferredRenderer.sln` 建置，或命令列：

```bat
msbuild DeferredRenderer.sln /m /p:Configuration=Release /p:Platform=x64
```

輸出在 `build/x64/{Debug,Release}/DeferredRenderer.exe`，相依 DLL 由 post-build target 複製到 exe 旁。**評分請用 Release**（Debug 受 Saba+Bullet 物理 CPU 成本拖慢）。

### 6.2 執行

從**專案根目錄**啟動（讓相對路徑 `sponza/sponza.obj` 與 `data/…/LeMaline v1.0.pmx`、`data/Motion/dz.vmd` 可被解析）：

```bat
build\x64\Release\DeferredRenderer.exe
```

啟動後同時出現**主控台**與**渲染視窗 + ImGui 控制面板**。

### 6.3 渲染視窗操作（鍵鼠）

| 輸入 | 動作 |
|------|------|
| **Alt** | 切換 look mode（進入時游標隱藏、鎖定視窗中心） |
| W/A/S/D | 前後左右移動（永遠生效） |
| Q/Ctrl、E/Space | 下降 / 上升 |
| Shift | 移動加速 ×4 |
| 滑鼠（look mode） | 旋轉相機 |
| RMB+滑鼠（look mode） | 沿 right/up 平移 |
| Space | 動畫播放 / 暫停 |
| Z | 循環 view：Depth → Normal → Albedo → Color |
| ESC | 結束 |

### 6.4 主控台指令

| 指令 | 說明 |
|------|------|
| `help` | 列出所有指令 |
| `view depth\|normal\|albedo\|color` / `cycle` | 切換 / 循環 view mode |
| `pos` | 印相機位置與 yaw/pitch |
| `campos x y z` / `camaim yawDeg pitchDeg` | 設定相機位置 / 朝向（截圖取景用，+pitch 往下看） |
| `char [pos X Y Z\|scale S\|yaw D]` | 即時調整角色 placement（無參數=列印） |
| `anim <seconds>` | 跳到指定動畫時間並重新 skin（截圖用） |
| `pause` / `play` / `replay` | 動畫控制（replay = seek 0 並播放） |
| `light <x> <y> <z>` / `light on\|off` | 設定平行光方向 / 開關平行光（陰影即時更新） |
| `lights on\|off` | 主開關（平行光 + 128 點光 + 點光陰影一次切換） |
| `fplus on\|off\|heat`（別名 `forwardplus`） | 開關 128 彩色點光 / tile 燈數 heatmap |
| `plight on\|off` / `plight pos X Y Z` | 開關會投影的點光源（cube 陰影 + bloom sphere）/ 設位置 |
| `ssao on\|off` | 開關 SSAO |
| `contrast <v>` / `sat <v>` | 角色 albedo 對比 / 飽和度 |
| `shot [name]` | 存當前 view 截圖（無參數→`screenshots/shot.png`；給純名稱自動補 `.png` 並放進 `screenshots/`；給含目錄的路徑照原樣） |
| `shots` | 一次存 Depth/Normal/Albedo/Color 四張（自動隱藏面板） |
| `mmd [pmx] [vmd]` | 以 Saba 煙霧測試載入 PMX(+VMD) 並印統計 |
| `quit` / `exit` | 結束 |

> **預設行為**：彩色 128 點光（`fplus`）與會投影的點光源（`plight`）**預設關閉**，預設場景是乾淨的陽光照明 Sponza + cel-shaded 角色跳舞；要展示 Forward+/點光陰影時再用指令開啟。

### 6.5 ImGui 控制面板

面板頂端顯示 **FPS（≥45 綠 / <45 紅）**；下方含 Play/Pause/Replay、SSAO 開關與 radius/intensity、Bloom、Exposure、平行光方向、Scene lights（All ON/OFF + 各光源開關）、Forward+ heatmap 與 tile 燈數、角色 Saturation/Contrast、View mode 等。

---

## 7. 效能

- **Release，全特效開啟（角色動畫 + 物理 + directional shadow + SSAO + bloom + tonemap，128 燈與點光陰影視需要開）**：**60 fps（vsync 上限）**，超過 README 的 45 fps @ 1280×720 預算。
- Point-light cube 的 6 次全場景 pass 原本會 60→30 掉幀，以 `m_pointShadowDirty`（僅光源/角色移動時重畫）解決。
- **Debug** 約 27 fps（動畫中）/ 60 fps（暫停）——瓶頸是未最佳化的 Saba+Bullet `UpdateAllAnimation`（CPU 物理 + skinning），非 GPU；故效能驗收一律以 Release 為準。

---

## 8. 健全性驗證

- **編譯**：Debug 與 Release 完整重建皆 **0 警告 / 0 錯誤**（主專案 `/W4 /utf-8`；Saba 22 個 `.cpp` 以 `/W0` 並 per-file scoped defines）。
- **執行期 D3D12 Debug Layer**：經 geometry/lighting/Forward+ compute/point cube shadow/SSAO/bloom/tonemap/screenshot 全路徑，**無 WARNING/ERROR/CORRUPTION**（`DrainInfoQueue` 每幀抽取、去重印出，確認乾淨）。
- **主控台**：正常操作無非預期警告，`quit` 乾淨退出（exit code 0）。

---

## 9. 第三方元件與授權邊界

| 元件 | 用途 | 是否屬「既成渲染框架」 |
|------|------|----------------------|
| Assimp | 載入 Sponza `.obj`/`.mtl` | 否（資產載入器） |
| Saba | 解析 PMX/PMD/VMD、骨架/morph/物理求值 | 否（PMX/VMD loader，README 明列為參考 sample；所有 D3D12 繪製為自寫） |
| Bullet | Saba 的物理依賴（頭髮/裙擺） | 否（物理函式庫） |
| DirectXTK12 | 貼圖上載（DDS/WIC）、`GraphicsMemory`、`ScreenGrab` | 否（工具函式庫，非渲染引擎） |
| Dear ImGui | 控制面板 GUI | 否（UI） |
| glm / fmt | Saba 的數學/格式化依賴 | 否 |

所有 D3D12 device / swapchain / root signature / PSO / barrier / descriptor heap / 兩階段管線 / 所有 HLSL 皆自行撰寫，符合「嚴禁既成 D3D12 渲染框架」。

---

## 10. 檔案清單（繳交相關）

| 路徑 | 說明 |
|------|------|
| `DeferredRenderer.sln` / `DeferredRenderer/DeferredRenderer.vcxproj(.filters)` | VS2022 方案與專案檔 |
| `vcpkg.json` | 相依 manifest |
| `DeferredRenderer/src/*.{h,cpp}` | 全部 C++ 原始碼 |
| `DeferredRenderer/shaders/*.hlsl` | 8 個 shader（含 compute） |
| `third_party/saba/` | Saba PMX/VMD loader（原始碼納入，Log 已 patch 成 fmt） |
| `FinalProject_Report.md` | 本份 final-project 報告 |
| `Report.md` | Assignment 1（deferred 基底）報告 |
| `CLAUDE.md` | 專案備忘（建置陷阱、規範、操作要求） |
| `sponza/`、`data/` | Sponza 與 LeMaline/dz.vmd 資產（依繳交規定附帶或另行提供） |
| `screenshots/` | 報告截圖 |
</content>
</invoke>
