# elf2x68k インストールパス
ifndef ELF2X68K_DIR
	ELF2X68K_DIR=/opt/homebrew/Cellar/elf2x68k/20260602
endif

# デフォルトサフィックスを削除
.SUFFIXES:

# 各種コマンド短縮名
CC = m68k-xelf-gcc
AS = m68k-xelf-as
AR = m68k-xelf-ar

# 出力アーカイブファイル名
TARGET_FILE = ../lib/liba44.a

# ヘッダ検索パス
INCLUDE_FLAGS = -I${ELF2X68K_DIR}/lib/gcc/m68k-elf/13.4.0/include \
				-I${ELF2X68K_DIR}/m68k-elf/include \
				-I../include

# コンパイルフラグ
COMMON_FLAGS = -m68000 -O2 -msoft-float $(INCLUDE_FLAGS)
CFLAGS = $(COMMON_FLAGS)

# *.c ソースファイル
C_SRCS = a44.c

# *.s ソースファイル
ASM_SRCS =

# *.h ヘッダーファイル
HEADER_SRCS = ../include/a44.h

# リンク対象のライブラリファイル
LIBS =

# 中間ファイル生成用ディレクトリ
INTERMEDIATE_DIR = _build/elf2x68k

# オブジェクトファイル
OBJS =	$(addprefix $(INTERMEDIATE_DIR)/,$(patsubst %.c,%.o,$(C_SRCS))) \
	    $(addprefix $(INTERMEDIATE_DIR)/,$(patsubst %.s,%.o,$(ASM_SRCS)))

# デフォルトのターゲット
all : $(TARGET_FILE)

# 中間生成物の削除
clean : 
	rm -f $(TARGET_FILE)
	rm -rf $(INTERMEDIATE_DIR)

# アーカイブファイルの生成
$(TARGET_FILE) : $(OBJS)
	$(AR) -rs ${TARGET_FILE} $(OBJS)

# *.c ソースのコンパイル
$(INTERMEDIATE_DIR)/%.o : %.c Makefile $(HEADER_SRCS)
	mkdir -p $(INTERMEDIATE_DIR)
	$(CC) $(CFLAGS) -c $*.c -o $(INTERMEDIATE_DIR)/$*.o
