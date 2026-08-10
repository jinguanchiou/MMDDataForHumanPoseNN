# 《明日方舟：終末地》角色渲染解析

> 來源：[chris0214/Arknights-Endfield-MME-Shader](https://github.com/chris0214/Arknights-Endfield-MME-Shader)（作者 克里斯提亞娜，MIT / 資產另計）。
> 該倉庫是把終末地角色渲染在 **MMD 9.32 + MME 0.37 + DirectX 9 (`ps_3_0`)** 上的重寫實作，
> 其參考鏈為：知乎拆解文（MyZmd）、Unity `Perlica` 官方樣板場景（DanbaidongRP）、Goo Blender 材質圖、
> ray-mmd、HgShadow。**它不是官方原始碼**，但通道語意、Ramp/LUT 用法、SDF 契約都是逐項對照原始資產驗證出來的，
> 對我們的 D3D12 專案而言是目前最可靠的參考。
>
> 本文整理自倉庫的 `EndfieldMME/internal/*.hlsl`、`docs/*.md`、`USER_GUIDE_CN.md` 與 `Source/.../TextureAutoMatcher.cs`。

---

## 1. 整體管線

終末地是 **NPR 與 PBR 混合**：臉是純風格化（SDF 陰影 + Ramp + LUT），衣服是接近標準的 PBR（GGX + IBL + 金屬度），
頭髮和皮膚介於兩者之間。它不是一套 uber shader，而是**按部位分材質**，每個部位一支 shader：

| 材質類型 | 職責 |
|---|---|
| `Face` | 只走 SDF 臉部光照，不接一般場景陰影 |
| `Iris` / `EyeWhite` / `EyeHighlight` | 虹膜視差 + MatCap + 眼透 stencil／眼白柔光／獨立高光面片 |
| `BrowLash` / `Mouth` | 眉睫、唇與口內 |
| `Hair` | 三層漫反射 + Kajiya-Kay 天使環 + 螢幕空間 Rim + 瀏海投臉陰影 |
| `Skin` | 縮減版 PBR-toon（無 IBL），半 Lambert Ramp + LUT 暗部 + NoV SSS |
| `Cloth` | 真 PBR：金屬度 / 粗糙度 / AO / IBL / 各向異性 / 雨水 |
| `FaceProxy` | 只參與深度，不可見 |

另外掛三個場景級 Pass：

```
ZMDshadow  ──►  陰影圖 / 線性深度（給所有材質 + 螢幕空間 Rim 用）
EyeThrough ──►  離屏擷取五官 → 疊回畫面（眼睛透過瀏海可見）
EndfieldPost ─► Bloom → Tonemap → 分級 → Dither（Bloom 必須在 Tonemap 之前）
```

---

## 2. 光照契約（`endfield_lighting.hlsl`）

這是整套風格的核心，四點：

1. **主光去強度、只留色相**：`mainLightColor /= luminance(LightColor)`。亮度由 Ramp 和控制器決定，
   不由光源強度決定 —— 所以轉光不會把角色打爆或打黑。
2. **三層漫反射**：亮部 / 暗部 / 暗中暗（`diffDarkAttn = diffDark * 0.65`），
   由 `min(AO, shadow, ramp.a)` 選擇亮暗，`AO*shadow*rampNoF` 再選暗與暗中暗。
   **AO 不是乘到最終顏色上，而是把像素推向 LUT 的暗色**（這是不變髒、不變黑的關鍵）。
3. **Ramp 的取樣座標不是 half-Lambert**，而是帶逆光補償的二次重映射：
   ```hlsl
   rampN  = 0.5 - 0.5*NoL*NoL;                       // 二次項
   finalN = clamp(rampN*backLight + NoL, -1, 1);     // 相機背對光時把暗面抬起來
   rampNoF = finalN*0.5 + 0.5;                       // 這才是查 RD 的 U
   ```
   `backLight = saturate(-dot(camFwd.xz, lightDir.xz)) * smoothstep(saturate(0.75-|camFwd.y|))`。
   （衣服/皮膚的次要分支才用 `pow(half-Lambert, curve)`，主 toon 路線用上式。）
4. **Ramp 上色帶亮度補償**：
   ```hlsl
   rampSat = max(ramp)-min(ramp);
   rampEff = ramp.rgb*rampSat + 1-rampSat;      // 灰階 Ramp 自動變成中性
   result  = diffuse*rampEff * clamp(lum(diffuse)/lum(diffuse*rampEff), 0, 1.5);
   ```
   Ramp 只提供**色相**，不當第二個亮度乘數。

---

## 3. 貼圖後綴總表

命名規則 `T_actor_<角色>_<部位>_<編號>_<後綴>`；`<角色>` 為 `common` 表示全角色共用資產。

| 後綴 | 色彩空間 | 用途 | 通道語意 |
|---|---|---|---|
| `_D` | sRGB | BaseColor | RGB 顏色；**A 常是 AO**（臉部確定是） |
| `_N` | Linear | 切線空間法線 | RG（DX 綠通道，可翻轉相容 GL） |
| `_HN` | Linear | 頭髮專用法線 | RG＝一般法線；**BA＝高光用的軟化法線** |
| `_P` | Linear | PBR 屬性打包 | 衣服：R=Metallic、G=Reflectivity、B=AO、A=Smoothness（roughness = 1−A）<br>頭髮：R=外/內層法線選擇、G=高光節奏、B=AO、A=暗線區 |
| `_E` | sRGB | 自發光 | |
| `_RD` | sRGB | **漫反射 Ramp**（256×1 條狀） | 以光照量當 U 取樣：RGB=色相偏移、**A=亮暗混合權重** |
| `_RS` | sRGB→Linear | **高光顏色 LUT**（2D） | 衣服 UV=(GGX 分佈項, (1−metallic)*roughness)；頭髮 UV=(dot(view.xz, N.xz), KK 高光範圍) |
| `_ST` | Linear | 區域/參數遮罩 | 臉：**G=眼睛/上臉柔性區域**，用作瀏海投影的接收遮罩（排除耳、嘴 UV 島）<br>頭髮：R=各向異性擾動 |
| `_SDF` | Linear | **臉部角度陰影場** | R=背光通道、G=正面通道（Goo 路線取兩者平均） |
| `_cm_M` | Linear | 臉部控制遮罩 | R=SSS 區域、G=SDF↔幾何 NoL 混合＋非臉區保護、B=相機在後方時的陰影區、**A=Rim 手繪區** |
| `_hl_M` | Linear | 唇高光遮罩 | R 通道，隨視角橫向平移 |
| `hairline_M` | Linear | 髮絲暗線紋理 | 與 `P.a` 相乘成暗線 |
| `_lut_D` | sRGB | **32 格 3D LUT 攤平成 1024×32** | 皮膚/衣服暗部的專用色；B 選片、RG 定位 |
| `matcap_*_D` | sRGB | MatCap | 眼睛環境反射（相機空間法線）與細節層 |
| `PreIntegratedFGD` | Linear | 預積分 DFG | IBL 的 split-sum 第二項 |
| `environment*.dds` | HDR | 環境反射圖 | 衣服 IBL |
| `rain_*` | — | 雨滴 / 相位 / 遮罩 | 濕潤、流水、水花 |

**每個角色都必須用自己的 `_D`／`_SDF`／`_N`／`_P`；只有 `common` 系列（RD/RS/LUT/ST/cm_M/matcap）可以共用。**

---

## 4. 臉部：SDF 陰影（`endfield_face.hlsl`）

臉不能用 `N·L`，會被鼻樑和眼窩打出髒陰影。做法是把**光的水平角**當索引查 SDF：

```hlsl
// 1) 取頭骨基底（MMD 慣例：Front=-row3, Right=-row1）
headFront = -normalize(headBone._31_32_33);
headRight = -normalize(headBone._11_12_13);

// 2) 光向量投影到臉的水平面
lightWS        = -normalize(MmdLightDirection);       // MMD 的 DIRECTION 是行進方向，要取負
projectedLight = normalize(lightWS - dot(lightWS, headUp)*headUp);
side           = dot(projectedLight, headRight);

// 3) 光在哪一側 → 鏡像 U（貼圖只畫了一半）
uv.x = (side >= 0) ? uv.x : 1 - uv.x;
float4 sdf = tex2D(SdfSampler, uv);

// 4-a) 知乎/Unity 路線：正面用 G、背面用 R，各自 smoothstep
// 4-b) Goo 路線（本倉庫正式版）：整個 360° 用同一個連續角度門檻
float angle01 = abs(atan2(side, dot(projectedLight, headFront))) / PI;
float sdfAvg  = 0.5*(sdf.r + sdf.g);
float gooMask = 1 / (1 + pow(100000.0, -3*sharp*(sdfAvg - (angle01 + 0.1))));  // Sigmoid，sharp 控制邊緣銳利度
```

接著：

```hlsl
mask   = lerp(gooMask, saturate(dot(N, L)), cm_M.g);   // 非臉區（脖子）退回幾何光照
rd     = tex2D(RD, float2(mask, 0.5));
weight = min(rd.a, pow(D.a, aoStrength));              // AO 與場景陰影都只是「選暗色」的權重
weight = min(weight, zmdShadowEffect);
color  = lerp(SkinLUT(D.rgb), D.rgb, weight);          // 暗部＝LUT 查表色，不是乘黑
color  = ApplyRdColor(color, rd.rgb);                  // Ramp 色相 + 亮度補償
```

臉部其他項目：

- **SSS**：`(1 - (NoV*0.85+0.15)) * cm_M.r`，只作用在亮部 albedo，暗部保持 LUT 的顏色。
- **唇高光**：`hl_M.r` 依 `dot(viewDir, headRight)` 橫移 UV，主光跑到臉後就淡出。
- **Rim**：`pow(cm_M.a, 1/width)` × 受光側半邊 × `saturate(faceNoV - 門檻)`，顏色可取 PMX 的 `EDGECOLOR`。
- **場景陰影閘門**：`shadowArea = max(cm_M.g, smoothstep(-2*camFrontDot) * cm_M.b)`
  —— 臉正面不吃場景陰影，脖子與後腦才吃。
- **`ST.g`** 在 stencil pass 當接收遮罩：只有眼睛/上臉區域寫入接收位，耳朵與嘴的 UV 島被排除，
  瀏海投影才不會糊到嘴裡。

---

## 5. 頭髮（`docs/hair_final_implementation.md`）

1. 用 `HN.rg` 算漫反射法線（無切線時 `ddx/ddy` 重建 TBN）。
2. **高光法線另外造**：外層 = 75% 頭部球形法線 + 25% `HN.ba`，由 `P.r` 選外層軟法線或內層一般法線
   —— 天使環才會是完整一圈而不是跟著髮絲碎掉。
3. 三層卡通漫反射（RD + AO + ZMD 陰影 + 自陰影 + 逆光補償）。
4. `P.a × hairline_M` 形成暗線，`P.g` 切分高光節奏，避免暗部出現連續塑膠高光。
5. **Kajiya-Kay 各向異性高光**，前髮硬上沿、柔下沿；`ST.r` 提供齒狀擾動；
   顏色是 Goo A/B 分層再疊 `linear(RS) * 0.28 * 0.35`。
   **KK 高光跟相機而不跟光** —— 轉光時天使環才不會亂飄。
6. 頭冠補光：只給 `N.y ∈ [0.45, 0.70]` 的世界向上錐形區補光。
7. **Rim 是獨立第二 Pass**（`ONE/ONE` 加法，讀 ZMD 線性深度做螢幕空間邊緣），
   因為主 PS 已經 504/512 指令逼近 DX9 上限。
8. 瀏海投臉陰影：真實頭髮材質的偏移 Pass，靠 stencil bit 限定接收範圍，深度 `LESSEQUAL + 0.0025`。

---

## 6. 皮膚

臉的縮減版：半 Lambert → body `_RD` → LUT 只作用在暗支 → `NoV*0.85+0.15` 的暖色 SSS 乘進 albedo →
ZMD 陰影 → 克制的 NoV Rim + 光向感知的螢幕空間深度 Rim → 很弱很寬的直接光 GGX（`F0 = 0.04*0.5`、roughness 0.58）。
**沒有 IBL**，暗面禁止出現鏡面感。

---

## 7. 衣服：真 PBR（`endfield_cloth.hlsl`，最大的一支，~89 KB）

```hlsl
metallic     = P.r * ctrl;
reflectivity = P.g * ctrl;
ao           = P.b;
roughness    = max(1 - P.a, 0.04);

// 直接光：GGX + 各向異性，高光顏色查 RS
rsUv  = float2(D_GGX_noPi * (roughness²+ε), (1-metallic)*roughness);
spec *= tex2D(RS, rsUv).rgb;

// 漫反射：半 Lambert^curve → RD ramp → 暗支查 cloth LUT
rd    = tex2D(RD, float2(pow(halfLambert, curve), 0.5));
dark  = lerp(color, ClothLUT(color), lutStrength);
diff  = lerp(dark, color, min(rd.a, ao, shadow));

// 間接光：環境圖 Ld × (PreIntegratedFGD 的 DFG)，金屬吃全額、介電只吃受控比例
```

外加雨水鏈：`P.a` 決定吸水率 → 濕潤壓暗/提亮滑 → 雨滴法線 → 流水 → 水花 → 額外的 clear-coat IBL。

---

## 8. 眼睛

- **虹膜視差**：切線空間 view 方向偏移 UV（depth 0.02、最大偏移 0.035），做出眼球凹陷。
- **MatCap 05**：相機空間法線取樣，當環境反射（`color * (1 + matcap*0.5667)`）。
- **MatCap 07**：UV 固定的發光層。
- **虹膜貼圖的 Alpha 是自發光遮罩** —— 所以不能用 MMD 扁平化過的複製貼圖，要用 `other tex` 的原檔。
- **眼透（EyeThrough）**：離屏 RT 只畫五官（虹膜、眉睫），頭髮在擷取時被深度偏移推開，
  最後以 `SRCALPHA/INVSRCALPHA` 疊回畫面，強度預設 0.38。這是後處理，不改主 Hair Pass。

---

## 9. 描邊（`endfield_outline.hlsl`）

法線外擴背面法，但寬度是**螢幕像素恆定**：

```hlsl
probeCS   = mul(posOS + normalOS*probeDist, WVP);     // 探針點投影
pixelDir  = normalize((probeNdc - baseNdc) * viewport);
offset    = pixelDir * (2*widthPixels/viewport) * posCS.w;   // 乘 w 抵銷透視除法
```

遠距離用 `smoothstep(1,12,w)` 收窄，顏色取 PMX `EDGECOLOR` 或手動色，並用 `pow(shade, contrast)` 讓描邊在暗部加深。

---

## 10. 陰影與後處理

- **ZMDshadow**：基於 針金P 的 HgShadow（`CFSUSM` / `CLSPSM` 兩種級聯/透視陰影圖）。
  輸出 `ViewportMap2`：**R = 陰影量、G = 線性相機深度**。深度那一路同時供頭髮/皮膚的螢幕空間 Rim 使用。
- **EndfieldPost**：
  1. 場景擷取、sRGB→linear；
  2. Karis 加權 13-tap 下採樣建 5 級金字塔（1/2 … 1/32）＋可分離高斯＋tent 上採樣；
  3. **Bloom 在 Tonemap 之前**；
  4. Tonemap 可選 ACES / Gran Turismo / AgX-like / Neutral（ACES 白點 4.0）；
  5. 曝光、對比、飽和、32 格 LUT；
  6. 最後才 dither（藍噪聲），消 8-bit 帶狀。
- 建議初值：曝光 +0.05~+0.10 EV、Bloom 閾值 ≈1.0、強度 0.10~0.25、scatter 0.5~0.65、LUT 佔比 0.10~0.20。

---

## 11. 顏色空間契約

- `_D`、`_RD`、`_RS`、`lut_D` 是 sRGB；`_N`、`_HN`、`_P`、`_ST`、`_SDF`、`_cm_M`、`hairline_M` 是**線性控制資料**，絕不能套 gamma。
- 材質內部用 `pow(x, 2.2)` / `pow(x, 1/2.2)` 進出線性空間；AO、曝光、Rim 疊加都在線性空間做。
- 材質最終輸出目前是 clamp 過的 LDR；真 HDR 需要 `A16B16G16R16F` 場景 RT 並改寫材質輸出契約。

---

## 12. 對照我們的 D3D12 專案

| 終末地做法 | 我們現況 (`Endfield.hlsl` / `RenderProfile::EndfieldPBR`) |
|---|---|
| 部位分材質（Face/Hair/Skin/Cloth/Eye） | 目前單一 forward PBR pass，**需要按 PMX 材質名分流** |
| 臉 SDF + cm_M + ST + RD + skin LUT | 萤石模型的 `other tex/` **全部具備**，尚未接線 |
| 主光去強度只留色相 | 未做；接上後轉光才會穩 |
| AO/陰影 → 選 LUT 暗色，而非乘黑 | 未做；這是「暗部發灰發髒」的根因 |
| Ramp 飽和度自適應 + 亮度補償 | 未做 |
| Bloom→ACES→dither，Bloom 必須在 Tonemap 前 | 已有 HDR→Bloom→ACES 鏈，順序相符 |
| 螢幕空間深度 Rim（讀陰影圖的線性深度 G） | 有 shadow map，未輸出線性深度通道 |
| 像素恆定寬度描邊 | 已有 cel outline（1px），可沿用 `w` 補償寫法 |

建議接線順序（單變數驗證，倉庫本身就是這樣做的）：
**Base → RD ramp → LUT 暗支 → SDF 臉部陰影 → cm_M（SSS/Rim/陰影閘門）→ 頭髮 KK 高光 → 衣服 P/IBL → 後處理**。
每一步只加一個變數，否則分不清是貼圖接錯還是公式錯。
