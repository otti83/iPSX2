# iPSX2 — ライセンスと第三者コンポーネントの表示

iPSX2 は **[PCSX2](https://github.com/PCSX2/pcsx2)** (オープンソースの PlayStation 2 エミュレータ) を
**ARMSX2 (PCSX2 の ARM64 / Android ポート) 経由**で iOS / macOS ARM64 向けに移植した派生物である。
本プロジェクトは PCSX2 および ARMSX2 の公式リリースではない。

## 本体のライセンス

**GNU General Public License, version 3 or later (GPL-3.0-or-later)**

上流 PCSX2 の全ソースが `SPDX-License-Identifier: GPL-3.0+` を宣言しており、
iPSX2 はその改変物なので、**結合著作物である iPSX2 全体が GPL-3.0-or-later の条件で配布される**。
全文は同梱の `LICENSE` (= 上流の `COPYING.GPLv3` と同一) を参照。

- iPSX2 が新規に追加したファイルにも `SPDX-License-Identifier: GPL-3.0-or-later` を用いる。
- 上流の SPDX ヘッダおよび著作権表示は削除しない。

### ソースコードの提供

公開用ソースコードの入手先: **https://github.com/otti83/iPSX2**。
このツリーには市販ゲーム由来のオフライン変換済み VU1 マイクロプログラムは含めていない。対応ソースの完全性は出荷バイナリの内容との照合が必要。

### 改変の告知 (GPL-3.0 §5(a))

**ルートの [`NOTICE`](NOTICE) が改変告知にあたる。** iPSX2 が PCSX2 の改変物であること、
改変者、改変時期、主な改変内容を記載してある。
配布物には必ず同梱すること。

---

## 同梱している第三者コンポーネント

以下は **ビルド構成 (`cmake/SearchForStuff.cmake`) から機械的に抽出**した、
iOS / macOS ARM64 ビルドで実際にコンパイル対象となるコンポーネント。
これは**出荷バイナリのシンボル検査ではなくビルド構成に基づく**。
最終バイナリへの実リンクは未検証 (Unknown)。

### 常に構築されるもの

| コンポーネント | ライセンス | 用途 |
|---|---|---|
| **PCSX2** (+ ARMSX2 の ARM64 ポート) | GPL-3.0-or-later | エミュレータ本体 |
| **fmt** | MIT | 文字列整形 |
| **fast_float** | Apache-2.0 / MIT / BSL-1.0 | 浮動小数パース |
| **rapidyaml** | MIT | YAML (ゲームデータベース) |
| **rapidjson** | MIT (msinttypes 部は BSD-3-Clause) | JSON |
| **LZMA SDK** | パブリックドメイン | 圧縮 |
| **libchdr** | BSD-3-Clause | CHD ディスクイメージ |
| **libzip** | BSD-3-Clause | ZIP |
| **SoundTouch** | LGPL-2.1-or-later | オーディオのテンポ / ピッチ変換 |
| **FreeSurround** | GPL-2.0-or-later | サラウンドデコード |
| **cubeb** | ISC | オーディオ出力の抽象化 |
| **SimpleIni** | MIT | 設定ファイル |
| **Dear ImGui** | MIT | オンスクリーン UI |
| **cpuinfo** | BSD-2-Clause | CPU 機能検出 |
| **rcheevos** | MIT | RetroAchievements |
| **discord-rpc** | MIT | Discord Rich Presence |
| **LLVM demangler** | Apache-2.0 with LLVM Exception (同梱の `DemanglerLicenseGPL.txt` / `DemanglerLicenseLGPL.txt` も参照) | デバッガのシンボル復元 |
| **ccc** | MIT | シンボルテーブル解析 |

### Apple (iOS / macOS) 分岐で構築されるもの

| コンポーネント | ライセンス | 用途 |
|---|---|---|
| **zlib** | Zlib | 圧縮 |
| **Zstandard (zstd)** | BSD-3-Clause / GPL-2.0 デュアル | 圧縮 (セーブステート等) |
| **LZ4** | BSD-2-Clause (ライブラリ) / GPL-2.0 (付属ツール) | 圧縮 |
| **libwebp** | BSD-3-Clause | 画像 |
| **SDL3** | Zlib | 入力 / ウィンドウ |
| **HarfBuzz** | "Old MIT" | テキストシェーピング |
| **FreeType** | FTL または GPL-2.0 のデュアル | フォントラスタライズ |
| **plutosvg** | MIT | SVG |

### ARM64 分岐で構築されるもの

| コンポーネント | ライセンス | 用途 |
|---|---|---|
| **glslang** | BSD-3-Clause / Apache-2.0 | シェーダのコンパイル |
| **VIXL** | BSD-3-Clause | ARM64 コード生成 (JIT 経路。既定の iv2 インタプリタ構成では未使用) |

### 条件付き

| コンポーネント | 条件 | ライセンス |
|---|---|---|
| **Vulkan-Headers** | `USE_VULKAN` | Apache-2.0 / MIT |
| **glad** | `USE_OPENGL` | (同梱ディレクトリにライセンス表示なし。上流 glad は MIT) |

### 同梱しているが、この構成では使わないもの

- **FFmpeg** — `3rdparty/ffmpeg` は **ヘッダのみ**で、実装は実行時に動的ロードされる。
  静的リンクはしない。`COPYING.GPLv2` / `COPYING.LGPLv2.1` 等が同梱されている。
- **oboe / libadrenotools / shaderc の arm64-v8a prebuilt** — ARMSX2-Android からの継承。
  Android 専用で、iOS / macOS のビルド構成からは参照されない。
- **googletest, zydis, D3D12MemAlloc, winwil, winpixeventruntime, rainterface, promptfont,
  xbyak, discord_social_sdk** — Windows / x86 / テスト専用。

### ライセンス表示の復元 (対応済)

vendored ディレクトリのうち 5 件にライセンス全文が入っていなかったため、上流から復元した。
BSD / MIT / Zlib 系はいずれも「ライセンス文の添付」を再配布の条件にしているため、
これがないまま配布することはできなかった。

| ディレクトリ | 追加したファイル | 取得元 |
|---|---|---|
| `imgui` | `LICENSE.txt` | ocornut/imgui タグ `v1.91.9b` (同梱しているのがこの版) |
| `rapidyaml` | `LICENSE.txt` | biojppm/rapidyaml |
| `ccc` | `License.txt` | chaoticgd/ccc (上流のファイル名に合わせた) |
| `glad` | `LICENSE` | Dav1dde/glad — glad 本体の MIT に加え Khronos 仕様の Apache-2.0 / MIT を含む |
| `zlib` | `LICENSE` | 上流 zlib は独立したライセンスファイルを配布しないため、同梱している zlib 1.2.11 の `zlib.h` 冒頭の条項をそのまま書き写した |

`zstd` (`LICENSE`, `COPYING`)、`lz4` (`LICENSE`)、`glslang` (`LICENSE.txt`)、
`freesurround` (`GPL.txt`) は同梱済みだった。

---

## PlayStation 2 のシステムソフトウェア・ゲームについて

iPSX2 は PS2 の BIOS / ゲームイメージを**同梱していない** (ツリー内に存在しないことを確認済み)。
利用者は自身が所有する実機・ソフトから吸い出したものを用意する必要がある。
PlayStation は株式会社ソニー・インタラクティブエンタテインメントの商標であり、
iPSX2 は同社と一切関係がない。

市販タイトルから生成された `src/cpp/pcsx2/iv2/iv2_t1_*.inc` は、この公開用リポジトリには含まれない。
これらを含まないビルドでは T1 最適化を使わず、インタプリタで実行する。

---

公開用調整: 2026-11-06
