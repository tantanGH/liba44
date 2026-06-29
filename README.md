# liba44
A44 Encoding/Decoding C Library for X680x0

Otankonas氏による16bitPCMデータ向けADPCMエンコード・デコードライブラリADPCMLIB 0.02/0.03をCでリライトし、elf2x68k向けのライブラリとしたものです。再コンパイルすれば64bit環境にも対応できる想定です。

オリジナルのアーカイブは配布自由とのことなので、`ADPCMLIB.LZH`としてツリーに含めてあります。

なお、オリジナルにあった以下のバグを修正してあります。

 - atop_make_buffer の初期化コードで不正アクセスしてしまう (0.02)
 - R/Lをコード上取り違えている(データとしては正しく、まーきゅりーゆにっとのLRインターリーブは維持されています)

* 基本的にオリジナルの 0.03 は 0.02 のバグを修正し、さらに高速化を追求したものです。


ADPCMLIBの提供ファンクションに準じた以下の関数が利用可能です。
A44_HANDLE構造体の変数をアプリケーション側で用意し、それを渡す形になります。(FILEハンドル構造体に近いイメージです)

```
#include <a44.h>

void a44_ptoa_make_buffer(A44_HANDLE* a44);
void a44_ptoa_init(A44_HANDLE* a44, int16_t mode);
void a44_ptoa_exec(A44_HANDLE* a44, const uint8_t* pcm_addr, uint32_t pcm_bytes, uint8_t* adpcm_addr);

void a44_atop_make_buffer(A44_HANDLE* a44);
void a44_atop_init(A44_HANDLE* a44, int16_t mode);
void a44_atop_exec(A44_HANDLE* a44, const uint8_t* adpcm_addr, uint32_t adpcm_bytes, uint8_t* pcm_addr);

void a44_atop_mem(A44_HANDLE* a44, uint8_t* save_addr);
void a44_atop_set(A44_HANDLE* a44, const uint8_t* load_addr);
void a44_atop_null_exec(A44_HANDLE* a44, const uint8_t* adpcm_addr, uint32_t adpcm_bytes);
void a44_ad_set_panpot(A44_HANDLE* a44, int16_t mode);
```

オリジナルとは以下のAPIの出力するバイナリ構造については0.02とも0.03とも互換性はありません。

 - a44_atop_mem
 - a44_atop_set

これらはシークを伴うようなアプリケーションでの利用が想定されたものですが、オリジナルの 0.02 と 0.03 との間でも互換性はありません。liba44 はそのいずれとも互換性はありませんが、実用上は問題ないと思われます。

使う時は、サブモジュールとして組み込むのが簡単です。例えばプロジェクト直下にて以下を実行します。

```
git submodule add https://github.com/tantanGH/liba44.git libs/liba44
```

以下のようなツリーとなります。

```
my_app/
├── .git/
├── .gitmodules
├── libs/
│   └── liba44/
│       ├── include/a44.h
│       └── lib/liba44.a
└── src/
    ├── main.c
    └── Makefile
```

ヘッダー検索パスとライブラリ検索パスをMakefile内で
```
-I../libs/liba44/include
-L../libs/liba44/lib
```
のように指定し、`-la44` でリンクできます。