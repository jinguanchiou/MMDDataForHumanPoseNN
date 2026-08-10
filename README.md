# D3D Graphics Programming Final Project 技術候選清單

本清單以 Sponza 場景渲染為基礎，整理可供選擇與組合的進階 real-time rendering 技術。

### Notice
- 所有實作的渲染算法全部開啟的情況下，在 1280x720 的解析度下最少須維持穩定 45 fps。
- **嚴禁使用任何既成之 D3D12 渲染框架**：例如 [Microsoft MiniEngine](https://github.com/microsoft/DirectX-Graphics-Samples/tree/master/MiniEngine)、或是任何第三方封裝好的 Rendering Engine。
- 請基於各位的 assignment 1 進行 final project 的實做。
- 分數上限 100pt

---

## Animation / Interaction

### Basic
- **PMD/PMX Loader + Cel shading (20pt)**
  - 需能讀取並顯示 MMD 角色模型 (PMD/PMX)，模型的 mesh、material、texture 與 alpha-tested 部分需正確渲染。
  - 需正確處理模型的座標系、縮放、正反面、透明材質與貼圖路徑，避免角色出現鏡像、破面或貼圖遺失。
  - Cel shading 須至少包含梯度漫反射、輪廓線描邊與 rim light。
  - 請選擇人型角色作為 Demo case，並將角色放入 Sponza 場景中展示，而不是只在空白背景中顯示模型。
  - Basic 僅要求靜態角色顯示，不要求骨架動畫、IK、表情或物理模擬。

### Advanced
- **PMD/PMX + VMD Skeleton Animation + Cel shading (70pt)**
  - 需能讀取 PMD 或 PMX 模型，並根據 VMD motion data 驅動骨架動畫。
  - 需正確實作 bone hierarchy、local/global transform、skinning matrix 與 vertex bone weight。
  - 動畫播放需連續且穩定，角色不應出現明顯的骨架錯位、關節爆開、模型撕裂或姿勢跳動。
  - Cel shading 須至少包含梯度漫反射、輪廓線描邊與 rim light，並需與動畫角色整合，而非只套用在靜態模型上。
  - 請選擇人型角色作為 Demo case，並展示一段能清楚看出全身骨架動作的 VMD 動畫。
  - 可以忽略 VMD 當中的 camera motion、facial animation、IK 與物理模擬；本項目主要評分重點是 skeleton animation 與 cel-shaded character rendering。
  - 額外支援 facial animation (+10pt)、IK (+15pt)、裙擺/頭髮物理 (+25pt) 或 camera motion (+5pt)，可在 Demo 中說明作為加分亮點。

### Notice
- **計分規則**
  - Advanced 不會跟 Basic 重複計算得分。
- **完成標準**
  - Advanced 的角色渲染品質至少需達到 Basic 要求；也就是說，完成動畫但缺少正確材質、貼圖或 cel shading，不視為完整實作 Advanced。

### Reference
- **Tutorial / sample project**
  - [Saba: OpenGL PMD/PMX viewer](https://github.com/benikabocha/saba)
  - [DirectX11 Draw PMX](https://github.com/kuyuri-iroha/Draw-PMX)
  - [DirectX12でミクさんを躍らせてみよう](https://qiita.com/dpals39/items/f296a458d4905dfa7341)
  - [Cel-Shading: Unity中仿原神的卡通渲染实现](https://x-wflo.github.io/2021/08/14/Cel-shading/)
  - [LearnOpenGL: Skeletal Animation](https://learnopengl.com/Guest-Articles/2020/Skeletal-Animation)
  - [Reze Engine: A Hand-Rolled WebGPU Renderer for MMD Characters](https://www.webgpu.com/showcase/reze-engine-webgpu-mmd-renderer/)

---

## Lighting / Shadows

### Basic
- **Shadow Mapping (5pt)**
  - 需能支援至少一種光源類型：Directional light、Point light 或 Spot light。
  - 需要處理 shadow acne 與 peter panning 問題，例如 depth bias、slope-scaled bias、normal offset bias 等。
  - 若使用 directional light，需能動態改變光照方向，並讓陰影即時更新。
  - 若使用 point light，需使用 cubemap shadow map 或等效方法，並繪製 bloom sphere 視覺化點光源位置。
  - 若使用 spot light，需繪製 light cone 視覺化 spot light 位置及方向。
  - 邊緣模糊 +5pt：可使用 PCF、Poisson disk sampling 或其他 filter 方法，但需避免過度模糊導致陰影漏光。

### Advanced
- **Variance Shadow Maps, VSM (15pt)**
  - Shadow map 需儲存一階與二階 depth moment，並使用 Chebyshev upper bound 估計 visibility。
  - 需展示 VSM 可被 linear filtering、mipmap 或 blur prefilter 的特性。
  - 若支援多種光源類型，每種光源類型可分開計分。
- **Exponential Shadow Maps, ESM / EVSM (15pt)**
  - 需將 depth 轉換到 exponential domain，並說明 exponential constant 對陰影銳利度、漏光與數值穩定性的影響。
  - 需注意浮點精度與 overflow 問題，建議使用高精度 render target。
  - 若實作 EVSM，需說明正負 exponent 或 moment 的設計。
  - 若支援多種光源類型，每種光源類型可分開計分。
- **Forward+ Rendering (35pt)**
  - 畫面中點光源的數量最少須為 96。
  - 需將畫面切成 screen-space tiles，為每個 tile 建立受影響光源列表。
  - Light culling 需使用 compute shader 或等效 GPU-based 方法，並需處理每個 tile 的最大光源數與 buffer overflow。
- **Linearly Transformed Cosines, LTC Area Light (20pt)**
  - 需支援至少一種 polygonal area light，例如 rectangle light 或 disk light。
  - 光源需要能動態改變方向或位置，展現 real-time 渲染的效果。

### Notice
- **VSM / ESM 計分**
  - VSM 跟 ESM 請擇一實現。
  - VSM、ESM 不與 Basic 重複算分。
- **Shadow Mapping 注意事項**
  - 光源需要能動態改變方向或位置，展現 real-time 渲染的效果。
  - Directional light、Point light 和 Spot light 各算一個分數，依照光源類型計算（舉例若完成了 Directional light 與 Point light 的 VSM，即得 15*2=30pt）。
  - 若使用 point light，請額外繪製 bloom sphere 視覺化點光源位置。
  - 若使用 spot light，請額外繪製 light cone 視覺化 spot light 位置及方向。

### Reference
- **Tutorial / sample project**
  - [RasterTek: DirectX 11 Shadow Mapping Tutorial](https://www.rastertek.com/dx11win10tut41.html)
  - [LearnOpenGL: Shadow Mapping](https://learnopengl.com/Advanced-Lighting/Shadows/Shadow-Mapping)
  - [LearnOpenGL: Point Shadows](https://learnopengl.com/Advanced-Lighting/Shadows/Point-Shadows)
  - [GameDev.net: Cubic Shadow Mapping in Direct3D](https://gamedev.net/tutorials/programming/graphics/cubic-shadow-mapping-in-direct3d-r2457/)
  - [MJP: A Sampling of Shadow Techniques + sample project](https://therealmjp.github.io/posts/shadow-maps/)
  - [AMD GPUOpen: ForwardPlus11 DirectX 11 SDK Sample](https://gpuopen.com/learn/forwardplus11-directx-11-sdk-sample/)
  - [3D Game Engine Programming: Forward vs Deferred vs Forward+ Rendering with DirectX 11](https://www.3dgep.com/forward-plus/)
  - [LearnOpenGL: Area Lights with LTC](https://learnopengl.com/Guest-Articles/2022/Area-Lights)
  - [Area Lights](https://alextardif.com/arealights.html)
  - [Self Shadow: LTC sample code](https://github.com/selfshadow/ltc_code)
- **Further reading**
  - [Variance Shadow Maps, Donnelly and Lauritzen](https://www.sciweavers.org/publications/variance-shadow-maps)
  - [Exponential Shadow Maps, Annen et al.](https://discovery.ucl.ac.uk/id/eprint/10001/)


---

## Ambient Occlusion

### Basic
- **Screen Space Ambient Occlusion, SSAO (15pt)**
  - 需使用 depth buffer 與 normal buffer 或 depth reconstruction 取得 view-space position 與 normal。
  - Sampling kernel 應以 surface normal 為中心建立 hemisphere，而非直接使用 sphere kernel。
  - 需使用 random rotation/noise texture 或其他方法減少 banding，並使用 blur 或 bilateral blur 降低 noise。
  - 需提供 radius、bias、intensity 等參數調整，並展示不同參數對 Sponza 牆角、柱子、拱門等區域的影響。
  - 需注意 SSAO 是 screen-space technique，應能說明畫面外幾何、深度不連續與 halo artifact 的限制。

### Advanced
- **Real-time Ray-traced Ambient Occlusion, RTAO (40pt)**
  - 需使用 DXR 或等效 real-time ray tracing API 建立 BLAS/TLAS，並從可見表面發射 AO rays。
  - 需控制 ray length、sample count 與 random direction distribution，避免 AO 過黑或過度 noise。

### Notice
- **AO 計分**
  - SSAO 跟 Ray-traced AO 請擇一實現。
- **效果比較**
  - Ambient occlusion 需要能夠 enable/disable，方便觀察效果。

### Reference
- **Tutorial / sample project**
  - [RasterTek: DirectX 11 Screen Space Ambient Occlusion Tutorial](https://rastertek.com/dx11win10tut51.html)
  - [LearnOpenGL: SSAO](https://learnopengl.com/Advanced-Lighting/SSAO)
  - [NVIDIA: SSAO11 DirectX 11 Sample Documentation](https://developer.download.nvidia.com/assets/gamedev/files/sdk/11/SSAO11.pdf)
  - [AMD FidelityFX CACAO](https://gpuopen.com/fidelityfx-cacao/)
  - [Direct3D 12 Raytracing Samples, Microsoft Learn](https://learn.microsoft.com/en-us/samples/microsoft/directx-graphics-samples/d3d12-raytracing-samples-win32/)
  - [DirectX-Graphics-Samples: Real-Time Denoised Ambient Occlusion overview](https://deepwiki.com/microsoft/DirectX-Graphics-Samples/5.2.3-real-time-denoised-ambient-occlusion)
  - [NVIDIA: DX12 Raytracing Tutorial Part 1](https://developer.nvidia.com/rtx/raytracing/dxr/dx12-raytracing-tutorial-part-1)
- **Further reading**
  - [DirectX Raytracing Functional Spec](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html)
  - [Ray Tracing Gems, free Open Access book](https://www.realtimerendering.com/raytracinggems/)

---

## Post-processing

### Basic
- **Tone Mapping, Reinhard / Filmic / ACES (5pt)**
  - 需在 HDR render target 上完成 lighting，再將 HDR color 轉換到 LDR display color。
  - 至少需實作一種 tone mapping operator，例如 Reinhard、Filmic 或 ACES approximation。
  - 需正確處理 linear color space、gamma correction 與 sRGB output，避免在 gamma space 中做 lighting 或 bloom。
  - 需提供 exposure 參數，並展示高亮區域如何被壓縮而非直接 clamp。
- **Bloom (5pt)**
  - 需從 HDR image 中擷取亮部，經過多次 blur 或 downsample/upsample 後再與原圖合成。
  - 需注意 bloom 應在 tone mapping 前或與 HDR pipeline 一致的位置完成，避免 LDR clamp 後失去高亮資訊。
  - 需提供 threshold、intensity、radius 等參數，避免整個畫面被過度洗白。
  - 建議使用 separable Gaussian blur 或 mip-chain bloom，以兼顧品質與效能。

### Advanced
- **Temporal Anti-Aliasing, TAA (30pt)**
  - 若未實作角色動畫，請於場景中加入至少兩個動態物件並且其有機會遮擋彼此，用以展示 TAA 處理動態物體的結果。
  - 需使用 sub-pixel jitter 改變 projection matrix，並累積 previous frame history。
  - 需輸出或重建 motion vectors，將上一幀 history reproject 到目前畫面。
  - 需要說明如何處理動態物件的 temporal sampling，避免 ghosting effect。
  - 需處理 disocclusion、depth/normal rejection、history clipping 或 neighborhood clamping。
  - 需能夠 enable/disable 觀察 TAA 的效果，並展示靜態畫面抗鋸齒與動態畫面 ghosting 控制。

### Notice
- **TAA 品質檢查**
  - TAA 若無 motion vector 或 history rejection，容易產生明顯拖影，評分時會特別檢查動態鏡頭。

### Reference
- **Tutorial / sample project**
  - [LearnOpenGL: HDR](https://learnopengl.com/Advanced-Lighting/HDR)
  - [LearnOpenGL: Bloom](https://learnopengl.com/Advanced-Lighting/Bloom)
  - [LearnOpenGL: Physically Based Bloom](https://learnopengl.com/Guest-Articles/2022/Phys.-Based-Bloom)
  - [RasterTek: DirectX 11 Glow Tutorial](https://www.rastertek.com/dx11win10tut46.html)
  - [DirectX 11 Image Postprocessing with DirectCompute: Blur and Bloom](https://cosnect.com/techstudies/directx-11-image-postprocessing-with-direct-compute-blur-and-bloom/)
  - [NVIDIA GameWorks: Direct3D Graphics/Compute Samples, including FXAA 3.11](https://docs.nvidia.com/gameworks/content/gameworkslibrary/graphicssamples/d3d_samples/d3d_samples.htm)
  - [Intel GameTechDev: DirectX 12 Temporal Anti-Aliasing sample](https://github.com/GameTechDev/TAA)
  - [Diligent Engine: Temporal Anti-Aliasing component](https://diligentgraphics.github.io/docs/db/d24/DiligentFX_PostProcess_TemporalAntiAliasing_README.html)
  - [What is TAA, with implementation in DirectX 11](https://www.armandyilinkou.com/blog/what-is-temporal-anti-aliasing)
- **Further reading**
  - [Filmic Tonemapping with Piecewise Power Curves, John Hable](https://filmicworlds.com/blog/filmic-tonemapping-with-piecewise-power-curves/)
  - [Tone Mapping Overview](https://64.github.io/tonemapping/)
  - [A Survey of Temporal Antialiasing Techniques](https://behindthepixels.io/assets/files/TemporalAA.pdf)
