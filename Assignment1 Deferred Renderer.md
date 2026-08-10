# Assignment: Foundations of Real-Time Rendering – Deferred Shading

## 1\. 作業概述 (Overview)

在本作業中，你將學習如何使用 **DirectX 12 (D3D12)** API 實作一個完整的 **延遲著色 (Deferred Shading)** 渲染管線。延遲著色的核心思想是將幾何資訊與光照計算分離，這對於處理複雜場景與大量光源至關重要。

你將使用 **Assimp** 載入經典的 **Sponza** 場景，並透過 **G-buffer** 存儲幾何屬性，最後在 Lighting Pass 中應用 **Blinn-Phong** 著色模型。

---

## 2\. 開發限制與規範 (Development Constraints)

*   **嚴禁使用任何既成之 D3D12 渲染框架**：例如 [Microsoft MiniEngine](https://github.com/microsoft/DirectX-Graphics-Samples/tree/master/MiniEngine)、或是任何第三方封裝好的 Rendering Engine。
*   **實作基礎**：請基於\*\*[課程提供之範例程式碼](https://cgv.cs.nthu.edu.tw:8889/ntust_cg/d3d12samples/-/tree/master?ref_type=heads)\*\*或是 **DirectX-Graphics-Samples 中的** [**D3D12HelloWorld**](https://github.com/microsoft/DirectX-Graphics-Samples/tree/master/Samples/Desktop/D3D12HelloWorld) 系列進行擴充與開發。

---

## 3\. 核心需求 (Requirements)

### A. 模型與資產載入 (Model Loading)

*   **Assimp 整合**：使用 [Open Asset Import Library (Assimp)](https://github.com/assimp/assimp) 讀取提供的 `sponza.obj` 模型檔。
*   **貼圖處理**：讀取 `.dds` 格式貼圖。需正確處理 D3D12 的 Texture Resource 與 Shader Resource View (SRV)。
*   **頂點格式**：頂點資訊須包含 Position, Normal, 以及 UV Coordinates。

### B. 延遲著色管線 (Deferred Shading Pipeline)

作業須包含兩個主要階段 (Passes)：

1.  \*\*Geometry Pass (G-Buffer Construction)\*\*：
    *   將場景渲染至多個 Render Target。
    *   **G-buffer 內容**：
        *   **Depth Buffer** (使用 D32\_FLOAT)
        *   **Normal Buffer** (使用 R16G16B16A16\_FLOAT)
        *   **Albedo Buffer** (使用 R8G8B8A8\_UNORM)
2.  **Lighting Pass**：
    *   使用一個全螢幕三角形 (Full-screen Triangle) 進行採樣渲染。
    *   從 G-buffer 中讀取資料並計算最終光照結果。

### C. 光照模型 (Shading Model)

*   **演算法**：實作 **Blinn-Phong Shading**。
*   **光源設定**：
    *   類型：Directional Light (平行光)。
    *   強度 (Intensity)：`(1.0, 1.0, 1.0, 1.0)`。
    *   光照入射之**反方向** (Direction to Light)：`(-0.577f, -0.577f, -0.577f, 1.0f)`。

### D. 互動與視覺化 (Interaction & Debugging)

*   **互動相機**：實作可自由移動的相機（第一人稱 WASD 移動 + 滑鼠左鍵旋轉, 滑鼠右鍵拖移）。
*   **切換功能**：按下鍵盤 `**Z**` 鍵需能在不同的渲染結果間循環切換，顯示順序如下：
    1.  **Depth** (深度圖)
    2.  **Normal** (法線圖)：須將法線值從 `[-1, 1]` 映射至 `[0, 1]` 區間以正確顯示色彩。
    3.  **Albedo** (貼圖顏色)
    4.  **Color** (最終合成之光照結果)

---

## 4 實作建議 (Implementation Suggestions)

為了降低開發難度並確保各個環節正確，建議採取以下開發流程：

*   **階段一：Forward Rendering 驗證**
    *   優先實作基礎的 Forward Rendering。確保 Assimp 模型載入邏輯正確、頂點資訊傳輸無誤、且貼圖能正確綁定並顯示。
    *   在此階段先實作 Blinn-Phong 著色，確保在傳統管線下能正確渲染場景。這有助於排除資源創建 (Resource Creation) 與模型載入的潛在問題。
*   **階段二：轉換至 Deferred Shading**
    *   當 Forward Rendering 正確後，再將渲染邏輯拆分為 Geometry Pass 與 Lighting Pass。
    *   修改渲染目標至 G-buffer，並撰寫全螢幕 Pass 讀取 G-buffer 進行著色。

---

## 5 提供資源 (Provided Assets)

*   **模型檔案**：`sponza.obj`
*   **貼圖檔案**：多張 `.dds` 格式貼圖（含 Diffuse, Normal 等）。

---

## 6. 繳交內容 (Submission Instructions)
請將以下內容打包為一個壓縮檔（`.zip` 或 `.7z`）進行繳交：

1.  **程式原始碼 (Source Code)**：
    * 包含所有 `.cpp`, `.h`, `.hlsl` 檔案。
    * 包含 Visual Studio 專案檔（`.sln`, `.vcxproj`, `.vcxproj.filters`）。
2.  **實驗報告 (Report)**：
    * 格式：PDF。
    * 內容：簡述實作方法、遇到的困難與解決方案，並附上各 G-buffers（Depth, Normal, Albedo）以及渲染結果的截圖。
3.  **檔案清理要求 (Cleanup - 重要)**：
    * **請勿上傳**任何 Visual Studio 自動生成的暫存檔、編譯產物或大型二進位檔案。
    * 繳交前請務必刪除以下資料夾/檔案：
        * `.vs/` (隱藏資料夾)
        * `bin/`, `obj/` 或 `x64/`, `Debug/`, `Release/` 資料夾。
        * `*.user`, `*.pdb`, `*.ilk`, `*.exe` 等非原始碼檔案。

---

## 7 評分標準 (Grading Rubric)

1.  \*\*正確性 (40%)\*\*：
    *   Sponza 模型完整顯示，無破面或貼圖錯誤，Blinn-Phong 光照計算正確。
    *   **嚴格要求**：程式執行期間，**D3D12 Debug Layer 不得出現任何 ERROR 訊息**。若渲染結果正確但後台有 API 使用錯誤 (D3D12 ERROR)，將視情況嚴重程度扣分。
2.  \*\*管線實作 (30%)\*\*：成功實作 Deferred Shading 流程，G-buffer 寫入與讀取邏輯正確。
3.  \*\*功能性 (15%)\*\*：相機操作順暢，且 `Z` 鍵切換各項 Buffer 的功能符合規格要求。
4.  \*\*報告 (5%)\*\*：報告內容須包含 G-buffer 畫面截圖以及操作說明。