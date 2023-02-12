/*
	CPU によるメモリアクセスのバンド幅を調べます。
*/

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <doslib.h>
#include <iocslib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <basic0.h>


#define FORCE_INLINE	__attribute__((__always_inline__)) inline
#define	COUNT_OF(a)		(sizeof(a) / sizeof(a[0]))


/*
	バンド幅テストで利用するメモリブロックのサイズ
	スプライトスクロールレジスタ領域が 1024 バイトしかなく、
	転送元と転送先を被らないようにするとなると、
	512 バイト以外の選択肢がない。
*/
#define	MEM_BLOCK_SIZE_IN_BYTES	(512)


/*
	以下の定数を 1 にすると、SRAM の書き換えを許可した状態でのテストを行う。
	書き換えられた SRAM の内容は、プログラム終了時に復元されるが、
	リセットやクラッシュ等で強制的に中断した場合、復元されないので注意。
	特に理由がない限り、0 にすることを強く推奨する。

	実機環境（Pro10MHz）で、SRAM の書き換え許可/禁止どちらの状態でも、
	メモリアクセス速度に変化がないことが確認されている。
*/
#define ENABLE_SRAM_WRITE	(0)


/*
	デバッグコードの有効化
*/
#define ENABLE_DEBUG_CODE	(0)


#if ENABLE_SRAM_WRITE
/* SRAM バックアップ */
static uint8_t s_sramBack[0x4000];
#endif


/*
	コンパイラの最適化によりコードが消されることを避けるため利用する global 変数
*/
uint8_t g_dummy;


/* 各テストケースの設定 */
typedef enum {
	CrtHf_15KHz = 0,
	CrtHf_31KHz,
	CrtHf_Count
} CrtHf;
typedef enum {
	DisplaySpBg_Off = 0,
	DisplaySpBg_On,
	DisplaySpBg_Count
} DisplaySpBg;
typedef enum {
	TVramMode_Display = 0,
	TVramMode_Buffer,
	TVramMode_Count
} TVramMode;
typedef enum {
	GVramMode_Display = 0,
	GVramMode_Buffer,
	GVramMode_Count
} GVramMode;
typedef enum {
	MemcpyMethod_CpuU8 = 0,
	MemcpyMethod_CpuMovem,
	MemcpyMethod_DmaMaximumRate,
	MemcpyMethod_DmaLimitedRateLiteCsrPolling,
	MemcpyMethod_DmaLimitedRateFullCsrPolling,
	MemcpyMethod_Count
} MemcpyMethod;
static const char *(s_memcpyMethodDescriptions[]) = {
	"memcpyByU8 (ideal throughput is 454545 bytes/sec @10MHz)\n",
	"memcpyByMovem (ideal throughput is 2081301 bytes/sec @10MHz)\n",
	"memcpyByDma Maximum-rate\n",
	"memcpyByDma Limited-rate BwRatio50%%%% BurstTime16 LiteCsrPolling\n",
	"memcpyByDma Limited-rate BwRatio50%%%% BurstTime16 FullCsrPolling\n",
};

/* 内部時計の状態を取得（上位16bit=秒の1桁目、下位16bit=秒の2桁目）*/
static FORCE_INLINE uint32_t
getRtc(){
	/* ハードウェアに直接アクセスし内部時計の状態を取得する */
	uint32_t rtc;
	do {
		/*
			RTC の各レジスタは非同期に更新されるので、秒の1桁目と2桁目をズレなく同時に取得するには工夫が必要。
			ここでは、同じ値が 2 回読めるまで do～while で繰り返す方法を利用している。
		*/
		rtc = *(volatile uint32_t *)0xE8A000 & 0x000F0007;
	} while (rtc != (*(volatile uint32_t *)0xE8A000 & 0x000F0007));
	return rtc;
}

/* 割り込み禁止 */
static FORCE_INLINE void
di() {
	asm volatile (
		"\n"
		"	ori.w	#$0700,sr\n"
	);
}

/* 割り込み許可 */
static FORCE_INLINE void
ei() {
	asm volatile (
		"\n"
		"	andi.w	#$f8ff,sr\n"
	);
}

/*
	uint8 単位で *dst++ = *src++ するナイーブなコピー
*/
static FORCE_INLINE void
memcpyByU8(
	void *dst,
	const void *src,
	uint16_t sizeInBytes
){
#if ENABLE_DEBUG_CODE
	/* エラーチェック */
	if (sizeInBytes == 0) {
		printf("memcpyByU8 ERROR : sizeInBytes is must be != 0.\n");
		exit(EXIT_FAILURE);
	}
#endif

	/*
		1 バイト単位でコピー

		ループ再内周は 1 バイトあたり 22 cycles のコスト。
		クロック 10MHz なら理論上 10000000 / 22 = 454545 バイトコピーできる。
	*/
	{
		register uint16_t     reg_d0 asm ("d0") = sizeInBytes;
		register void const * reg_a0 asm ("a0") = src;
		register void       * reg_a1 asm ("a1") = dst;
		asm volatile (
			"\n"
			"@@:\n"
			"	move.b	(a0)+, (a1)+\n"		/* 12 */
			"	dbra	d0, @b\n"			/* ループ継続:10, ループ終了:14 */

		:	/* 出力 */	"+r"	(reg_d0),	/* in out %0 (入力＆破壊) */
						"+r"	(reg_a0),	/* in out %1 (入力＆破壊) */
						"+r"	(reg_a1)	/* in out %2 (入力＆破壊) */
		:	/* 入力 */
		:	/* 破壊 */	"memory"			/* メモリバリアを要求 */
		);
	}
}

/*
	movem を使った高速なコピー
*/
static FORCE_INLINE void
memcpyByMovem(
	void *dst,
	const void *src,
	uint16_t sizeInBytes
){
#if ENABLE_DEBUG_CODE
	/* エラーチェック */
	if ((uintptr_t)dst & 1) {
		printf("memcpyByMovem ERROR : dst is not 2 bytes aligned.\n");
		exit(EXIT_FAILURE);
	}
	if ((uintptr_t)src & 1) {
		printf("memcpyByMovem ERROR : src is not 2 bytes aligned.\n");
		exit(EXIT_FAILURE);
	}
	if (sizeInBytes & 255) {
		printf("memcpyByMovem ERROR : sizeInBytes is not 256 bytes aligned.\n");
		exit(EXIT_FAILURE);
	}
	if (sizeInBytes < 256) {
		printf("memcpyByU8 ERROR : sizeInBytes is must be >= 256.\n");
		exit(EXIT_FAILURE);
	}
#endif

	/*
		256 バイト単位でコピー

		ループ再内周は 256 バイトあたり、およそ 76 * 16 - 4 + 8 + 10 = 1230 cycles のコスト
		クロック 10MHz なら理論上 10000000 * 256 / 1230 = 2081301 バイトコピーできる。
	*/
	{
		register uint16_t     reg_d0 asm ("d0") = (sizeInBytes >> 8) - 1;
		register void const * reg_a0 asm ("a0") = src;
		register void       * reg_a1 asm ("a1") = dst;
		asm volatile (
			"\n"
			"@@:\n"
			"	movem.l	(a0)+, d4-d7/a2-a5\n"	/* 12+8n = 76 */
			"	movem.l	d4-d7/a2-a5, (a1)\n"	/*  8+8n = 72 */

			"	movem.l	(a0)+, d4-d7/a2-a5\n"	/* 12+8n = 76 */
			"	movem.l	d4-d7/a2-a5, $20(a1)\n"	/* 12+8n = 76 */

			"	movem.l	(a0)+, d4-d7/a2-a5\n"	/* 12+8n = 76 */
			"	movem.l	d4-d7/a2-a5, $40(a1)\n"	/* 12+8n = 76 */

			"	movem.l	(a0)+, d4-d7/a2-a5\n"	/* 12+8n = 76 */
			"	movem.l	d4-d7/a2-a5, $60(a1)\n"	/* 12+8n = 76 */

			"	movem.l	(a0)+, d4-d7/a2-a5\n"	/* 12+8n = 76 */
			"	movem.l	d4-d7/a2-a5, $80(a1)\n"	/* 12+8n = 76 */

			"	movem.l	(a0)+, d4-d7/a2-a5\n"	/* 12+8n = 76 */
			"	movem.l	d4-d7/a2-a5, $a0(a1)\n"	/* 12+8n = 76 */

			"	movem.l	(a0)+, d4-d7/a2-a5\n"	/* 12+8n = 76 */
			"	movem.l	d4-d7/a2-a5, $c0(a1)\n"	/* 12+8n = 76 */

			"	movem.l	(a0)+, d4-d7/a2-a5\n"	/* 12+8n = 76 */
			"	movem.l	d4-d7/a2-a5, $e0(a1)\n"	/* 12+8n = 76 */

			"	lea.l	$100(a1), a1\n"			/* 8 */

			"	dbra	d0, @b\n"				/* ループ継続:10, ループ終了:14 */

		:	/* 出力 */	"+r"	(reg_d0),	/* in out %0 (入力＆破壊) */
						"+r"	(reg_a0),	/* in out %1 (入力＆破壊) */
						"+r"	(reg_a1)	/* in out %2 (入力＆破壊) */
		:	/* 入力 */
		:	/* 破壊 */	"memory",			/* メモリバリアを要求 */
						"d4","d5","d6","d7","a2","a3","a4","a5"
		);
	}
}

/*
	DMA を使った高速なコピー
*/
typedef enum {
	DmaOcrReqg_LimitedRate = 0,				/* 限定速度 */
	DmaOcrReqg_MaimumRate,					/* 最大速度 */
	DmaOcrReqg_ExternalRequests,			/* 外部要求転送 */
	DmaOcrReqg_MixedRequestGeneration,		/* 2番目以降は外部要求 */
} DmaOcrReqg;
static FORCE_INLINE void
memcpyByDma(
	void *dst,
	const void *src,
	uint16_t sizeInBytes,
	DmaOcrReqg ocrReqg,
	bool fullCsrPolling		/* CSR を全力でポーリングするか？ */
) {
#if ENABLE_DEBUG_CODE
	/* エラーチェック */
	if ((uintptr_t)dst & 1) {
		printf("memcpyByDma ERROR : dst is not 2 bytes aligned.\n");
		exit(EXIT_FAILURE);
	}
	if ((uintptr_t)src & 1) {
		printf("memcpyByDma ERROR : src is not 2 bytes aligned.\n");
		exit(EXIT_FAILURE);
	}
	if (sizeInBytes & 3) {
		printf("memcpyByDma ERROR : sizeInBytes is not 4 bytes aligned.\n");
		exit(EXIT_FAILURE);
	}
#endif

	/* チャンネル #2 の設定ポート */
	uint8_t *dmaReg = (uint8_t *)0xE84080;

	/* clear flag */
	uint8_t volatile *csr = dmaReg + 0x00;
	*csr = 0xff;

	/* setup */
	/*
		InsideX68000 p.44
		DCR : デバイスコントロールレジスタ
		OCR : オペレーションコントロールレジスタ
	*/
	uint8_t *dcr = dmaReg + 0x04;
	{
		uint8_t xrm		= 0;
		uint8_t dtyp	= 0;
		uint8_t dps		= 1;		/* デバイスポートサイズ 0:8bit, 1:16bit */
		uint8_t pcl		= 0;
		*dcr = ((xrm & 3) << 6) | ((dtyp & 3) << 4) | ((dps & 1) << 3) | (pcl & 3);
	}
	uint8_t *ocr = dmaReg + 0x05;
	{
		uint8_t dir		= 0;		/* 転送方向 0:メモリ->デバイス, 1:デバイス->メモリ */
		uint8_t btd		= 0;
		uint8_t size	= 2;		/* オペランドサイズ 0:8bit, 1:16bit, 2:32bit, 3:パック無し */
		uint8_t chain	= 0;		/* 0:チェインなし, 2:アレイチェイン, 3:リンクアレイチェイン */
		uint8_t reqg	= ocrReqg;	/* 0:限定速度, 1:最大速度, 2:外部要求転送, 3:2番目以降は外部要求 */
		*ocr = ((dir & 1) << 7) | ((btd & 1) << 6) | ((size & 3) << 4) | ((chain & 3) << 2) | (reqg & 3);
	}
	/*
		InsideX68000 p.50
		SCR : シーケンスコントロールレジスタ
		CCR : チャンネルコントロールレジスタ
	*/
	uint8_t *scr = dmaReg + 0x06;
	{
		uint8_t mac = 1;	/* メモリアドレスレジスタ増減 0:変化なし, 1:増加, 2:減少 */
		uint8_t dac = 1;	/* デバイスアドレスレジスタ増減 0:変化なし, 1:増加, 2:減少 */
		*scr = ((mac & 3) << 2) | (dac & 3);
	}
	uint8_t *ccr = dmaReg + 0x07;
	{
		uint8_t str = 0;	/* 1:動作スタート */
		uint8_t cnt = 0;
		uint8_t hlt = 0;
		uint8_t sab = 0;
		uint8_t inte = 0;
		*ccr = ((str & 1) << 7) | ((cnt & 1) << 6) | ((hlt & 1) << 5) | ((sab & 1) << 4) | ((inte & 1) << 3);
	}
	/*
		InsideX68000 p.54
		CPR : チャンネルプライオリティレジスタ
	*/
	uint8_t *cpr = dmaReg + 0x2D;
	{
		uint8_t cp = 3;		/* 0:最も高い優先度 ～ 3:最も低い優先度 */
		*cpr = cp & 3;
	}
	/*
		InsideX68000 p.54
		MFC : メモリファンファンクションコード
		DFC : デバイスファンクションコード
		BFC : ベースファンクションコード
	*/
	uint8_t *mfc = dmaReg + 0x29;
	uint8_t *dfc = dmaReg + 0x31;
	{
		uint8_t fc2 = 1;
		uint8_t fc1 = 0;
		uint8_t fc0 = 1;
		*mfc = ((fc2 & 1) << 2) | ((fc1 & 1) << 1) | (fc0 & 1);
		*dfc = ((fc2 & 1) << 2) | ((fc1 & 1) << 1) | (fc0 & 1);
	}
	/*
		InsideX68000 p.29
		MTC : メモリトランスファカウンタ
	*/
	uint16_t *mtc = (uint16_t *)(dmaReg + 0x0A);
	*mtc = sizeInBytes >> 2;	/* 32bit 単位でコピーするので 4 で割っている */
	/*
		InsideX68000 p.29
		MAR : メモリアドレスレジスタ
	*/
	uint32_t *mar = (uint32_t *)(dmaReg + 0x0C);
	*mar = (uintptr_t)src;
	/*
		InsideX68000 p.29
		DAR : デバイスアドレスレジスタ
	*/
	uint32_t *dar = (uint32_t *)(dmaReg + 0x14);
	*dar = (uintptr_t)dst;

#if 1
	/*
		InsideX68000 p.56
		GCR : ジェネラルコントロールレジスタ (ch#3 にしか存在しない)
	*/
	uint8_t *gcr = (uint8_t *)0xE840FF;
	{
		uint8_t bt = 0;		/* 1バーストあたりのDMAクロックサイクル数 0:16clock, 1:32clock, 2:64clock, 3:128clock */
		uint8_t br = 0;		/* バス占有率 0:50%, 1:25%, 2:12.5%, 3:6.25% */
		*gcr = ((bt & 3) << 2) | (br & 3);
	}
#endif

	/* start */
	*ccr |= 0x80;

	/* wait */
	/*
		このループはコンパイラによって意味がないコードと見なされ除去される。
		意味のあるコードのふりをするため、g_dummy にアクセスする必要がある。
		引数 fullCsrPolling で csr のポーリング頻度を調整する事ができる。
	*/
	if (fullCsrPolling) {
	/* CSR を全力でポーリング */
		uint8_t dummy = 0;
		while ((*csr & 0x90) == 0) {
			dummy++;
		}
		g_dummy = dummy;
	} else {
	/* CSR を軽めにポーリング */
		uint8_t dummy = 0;
		while ((*csr & 0x90) == 0) {
			/* バスを圧迫しない重そうな処理を適当に実行 */
			dummy += dummy * dummy * dummy * dummy + 123;
		}
		g_dummy = dummy;
	}
}

/*
	指定の条件で計測を実行する
*/
static bool 
measurement(
	int testCase,
	int numTestCases,
	CrtHf crtHf,
	DisplaySpBg displaySpBg,
	TVramMode tvramMode,
	GVramMode gvramMode,
	MemcpyMethod memcpyMethod,
	FILE *fp
){
	char buff[0x1000];

	/* メインメモリのコピーテストで扱う領域 */
	static uint32_t s_mainMemSrc[MEM_BLOCK_SIZE_IN_BYTES / sizeof(uint32_t)];
	static uint32_t s_mainMemDst[MEM_BLOCK_SIZE_IN_BYTES / sizeof(uint32_t)];

	/* コピー元コピー先の情報 */
	typedef struct tag_MemInfo MemInfo;
	struct tag_MemInfo {
		void *p;
		const char *name;
		bool verify;	/* ベリファイ可能か？（書き込んだ通りに読み出せるか？）*/
	};
	static const MemInfo s_srcMemInfoArray[] = {
		{
			(void *)s_mainMemSrc,
			" MAIN ",
			true
		},
		{
			(void *)0xC00000,
			"G-VRAM",
			true
		},
		{
			(void *)0xE00000,
			"T-VRAM",
			true
		},
		{
			(void *)0xEB0000,
			" SPR  ",
			false		/* 全ビットが有効ではないため false */
		},
		{
			(void *)0xEB8000,
			" PCG  ",
			false		/* uint8_t 単位のアクセスがサポートされていないため false */
		},
		{
			(void *)0xED3800,
			" SRAM ",
#if ENABLE_SRAM_WRITE
			true
#else
			false
#endif
		},
		{
			(void *)0xF00000,
			"CGROM ",
			false		/* read-only なので false */
		},
		{
			(void *)0xFE0000,
			"IPLROM",
			false		/* read-only なので false */
		},
	};
	MemInfo s_dstMemInfoArray[] = {
		{
			(void *)s_mainMemDst,
			" MAIN ",
			true
		},
		{
			(void *)(0xC00000 + MEM_BLOCK_SIZE_IN_BYTES),
			"G-VRAM",
			true
		},
		{
			(void *)(0xE00000 + MEM_BLOCK_SIZE_IN_BYTES),
			"T-VRAM",
			true
		},
		{
			(void *)(0xEB0000 + MEM_BLOCK_SIZE_IN_BYTES),
			" SPR  ",
			false		/* 全ビットが有効ではないため false */
		},
		{
			(void *)(0xEB8000 + MEM_BLOCK_SIZE_IN_BYTES),
			" PCG  ",
			false		/* uint8_t 単位のアクセスがサポートされていないため false */
		},
		{
			(void *)(0xED3800 + MEM_BLOCK_SIZE_IN_BYTES),	/* 0xED3FFF までアクセスしようとするとバスエラーになるので注意 */
			" SRAM ",
#if ENABLE_SRAM_WRITE
			true
#else
			false
#endif
		},
	};

	/* CRT モード */
	switch (crtHf) {
		case CrtHf_15KHz: {
			CRTMOD(13);
			G_CLR_ON();
			sprintf(
				buff,
				"\n"
				"testCase %d/%d\n"
				"\n"
				"CRTMOD(13) 512x512 65536color 1plane 15kHz\n",
				testCase,
				numTestCases - 1
			);
			printf(buff); fprintf(fp, buff);
		} break;
		case CrtHf_31KHz: {
			CRTMOD(12);
			G_CLR_ON();
			sprintf(
				buff,
				"\n"
				"testCase %d/%d\n"
				"\n"
				"CRTMOD(12) 512x512 65536color 1plane 31kHz\n",
				testCase,
				numTestCases - 1
			);
			printf(buff); fprintf(fp, buff);
		} break;
	}

	/*
		テキストパレット設定
		15hKz インターレース表示は液晶モニタに焼き付きを起こす。
		偶数ラインと奇数ラインの輝度差を抑えるためテキストパレットを補正する。
	*/
	{
		/*
			InsideX68000 p.215
			スプライトパレット #0 の定義開始アドレス
		*/
		uint16_t *spPalette = (uint16_t *)0xE82200;
		/* 文字の色 */
		{
			int r = 12;
			int g = 12;
			int b = 12;
			for (int iColor = 1; iColor < 16; iColor++) {
				spPalette[iColor] = (g << (5+5+1)) | (r << (5+1)) | (b << 1);
			}
		}
		/* 背景色 */
		{
			int r = 4;
			int g = 4;
			int b = 4;
			for (int iColor = 1; iColor < 16; iColor++) {
				spPalette[0] = (g << (5+5+1)) | (r << (5+1)) | (b << 1);
			}
		}
	}

	/*
		スプライト BG の表示 on/off
		$EB0808 については Inside X68000 p.190 を参照。
	*/
	switch (displaySpBg) {
		case DisplaySpBg_Off: {
			*(uint8_t *)0xEB0808 = *(uint8_t *)0xEB0808 & ~2;
			sprintf(
				buff,
				"SP BG disp off (by $EB0808)\n"
			);
			printf(buff); fprintf(fp, buff);
		} break;
		case DisplaySpBg_On: {
			*(uint8_t *)0xEB0808 = *(uint8_t *)0xEB0808 |  2;
			sprintf(
				buff,
				"SP BG disp on (by $EB0808)\n"
			);
			printf(buff); fprintf(fp, buff);
		} break;
	}

	/*
		T-VRAM/G-VRAM モード
		buffer モードに切り替えると画面がグリッチ状になるので、
		ビデオコントロールレジスタで表示 off にする必要がある。
	*/
	{
		/*
			CRTC R20
			Inside X68000 p.233
		*/
		{
			uint16_t *crtcR20 = (uint16_t *)0xE80028;
			uint16_t val = *crtcR20;
			val &= ~((1 << 12) | (1 << 11));
			val |= ((tvramMode == TVramMode_Display)? 0: 1) << 12;
			val |= ((gvramMode == GVramMode_Display)? 0: 1) << 11;
			*crtcR20 = val;
		}

		/*
			表示コントロール
			Inside X68000 p.210
		*/
		{
			uint16_t *videoCtrlR2 = (uint16_t *)0xE82600;
			uint16_t val = *videoCtrlR2;
			val &= ~((1 << 5) | 15);
			val |= ((tvramMode == TVramMode_Display)?  1: 0) << 5;
			val |= ((gvramMode == GVramMode_Display)? 15: 0);
			*videoCtrlR2 = val;
		}

		/* 途中画面が真っ黒になる旨の説明 */
		if (tvramMode == TVramMode_Buffer) {
			struct SYMBOLPTR param = {
				/* WORD		x1 */				0x10,
				/* WORD		y1 */				0x10,
				/* UBYTE	*string_address */	"計測進行中です。しばらくお待ちください・・・。",
				/* UBYTE	mag_x */			1,
				/* UBYTE	mag_y */			1,
				/* UWORD	color */			0x5555,
				/* UBYTE	font_type */		1,
				/* UBYTE	angle */			0
			};
			SYMBOL(&param);
		}

		sprintf(
			buff,
			"T-VRAM %s\n"
			"G-VRAM %s\n",
			(tvramMode == TVramMode_Display)? "display": "buffer",
			(gvramMode == GVramMode_Display)? "display": "buffer"
		);
		printf(buff); fprintf(fp, buff);
	}

	/* MemcpyMethod をログに記録 */
	{
		sprintf(
			buff,
			s_memcpyMethodDescriptions[memcpyMethod]
		);
		printf(buff); fprintf(fp, buff);
	}

	/* ログの header */
	{
		sprintf(
			buff,
			"\n"
			"throughput (bytes/sec)\n"
			"+------------------------+"
		);
		printf(buff); fprintf(fp, buff);

		for (int iDst = 0; iDst < COUNT_OF(s_dstMemInfoArray); ++iDst) {
			sprintf(
				buff,
				"----------+"
			);
			printf(buff); fprintf(fp, buff);
		}

		sprintf(
			buff,
			"\n"
			"|                   dst  |"
		);
		printf(buff); fprintf(fp, buff);

		for (int iDst = 0; iDst < COUNT_OF(s_dstMemInfoArray); ++iDst) {
			sprintf(
				buff,
				"  %s  |",
				s_dstMemInfoArray[iDst].name
			);
			printf(buff); fprintf(fp, buff);
		}

		sprintf(
			buff,
			"\n"
			"|                        |"
		);
		printf(buff); fprintf(fp, buff);

		for (int iDst = 0; iDst < COUNT_OF(s_dstMemInfoArray); ++iDst) {
			sprintf(
				buff,
				" 0x%06x |",
				s_dstMemInfoArray[iDst].p
			);
			printf(buff); fprintf(fp, buff);
		}

		sprintf(
			buff,
			"\n"
			"|  src                   |"
		);
		printf(buff); fprintf(fp, buff);

		for (int iDst = 0; iDst < COUNT_OF(s_dstMemInfoArray); ++iDst) {
			sprintf(
				buff,
				"~0x%06x |",
				s_dstMemInfoArray[iDst].p + MEM_BLOCK_SIZE_IN_BYTES - 1
			);
			printf(buff); fprintf(fp, buff);
		}

		sprintf(
			buff,
			"\n"
			"+------------------------+"
		);
		printf(buff); fprintf(fp, buff);

		for (int iDst = 0; iDst < COUNT_OF(s_dstMemInfoArray); ++iDst) {
			sprintf(
				buff,
				"----------+"
			);
			printf(buff); fprintf(fp, buff);
		}

		sprintf(
			buff,
			"\n"
		);
		printf(buff); fprintf(fp, buff);
	}

	/* 現在時刻の初期値 */
	uint32_t rtcCur = getRtc();

	/* src 側のループ */
	for (int iSrc = 0; iSrc < COUNT_OF(s_srcMemInfoArray); ++iSrc) {
		/* 計測値の一時保存バッファ */
		int results[COUNT_OF(s_dstMemInfoArray)];

		/* src 側がベリファイ可能なメモリならデフォルト値で埋める */
		if (s_srcMemInfoArray[iSrc].verify) {
			uint8_t *u8Mem = (uint8_t *)s_srcMemInfoArray[iSrc].p;
			for (int i = 0; i < MEM_BLOCK_SIZE_IN_BYTES; ++i) {
//				u8Mem[i] = (uint8_t)(rand() >> 4);
				u8Mem[i] = (uint8_t)i;
			}
		}

		/* src 側の情報をログに記録 */
		sprintf(
			buff,
			"|%s 0x%06x~0x%06x|",
			s_srcMemInfoArray[iSrc].name,
			s_srcMemInfoArray[iSrc].p,
			s_srcMemInfoArray[iSrc].p + MEM_BLOCK_SIZE_IN_BYTES - 1
		);
		printf(buff); fprintf(fp, buff);

		/* 秒の頭を検出 */
		{
			uint32_t rtcPrev = rtcCur;
			while (rtcPrev == rtcCur) {
				rtcCur = getRtc();
			}
		}

		/* dst 側のループ */
		for (int iDst = 0; iDst < COUNT_OF(s_dstMemInfoArray); ++iDst) {
			/* 1 秒間の計測 */
			uint32_t copiedSizeInBytes = 0;
			{
				di();
				void       *dst = s_dstMemInfoArray[iDst].p;
				void const *src = s_srcMemInfoArray[iSrc].p;
				uint32_t rtcPrev = rtcCur;
				switch (memcpyMethod) {
					case MemcpyMethod_CpuU8: {
						while (rtcPrev == rtcCur) {
							/* 計測オーバーヘッドを隠すため 8 回実行 */
							memcpyByU8(dst, src, MEM_BLOCK_SIZE_IN_BYTES);
							memcpyByU8(dst, src, MEM_BLOCK_SIZE_IN_BYTES);
							memcpyByU8(dst, src, MEM_BLOCK_SIZE_IN_BYTES);
							memcpyByU8(dst, src, MEM_BLOCK_SIZE_IN_BYTES);
							memcpyByU8(dst, src, MEM_BLOCK_SIZE_IN_BYTES);
							memcpyByU8(dst, src, MEM_BLOCK_SIZE_IN_BYTES);
							memcpyByU8(dst, src, MEM_BLOCK_SIZE_IN_BYTES);
							memcpyByU8(dst, src, MEM_BLOCK_SIZE_IN_BYTES);
							copiedSizeInBytes += MEM_BLOCK_SIZE_IN_BYTES * 8;
							rtcCur = getRtc();
						}
					} break;

					case MemcpyMethod_CpuMovem: {
						while (rtcPrev == rtcCur) {
							/* 計測オーバーヘッドを隠すため 8 回実行 */
							memcpyByMovem(dst, src, MEM_BLOCK_SIZE_IN_BYTES);
							memcpyByMovem(dst, src, MEM_BLOCK_SIZE_IN_BYTES);
							memcpyByMovem(dst, src, MEM_BLOCK_SIZE_IN_BYTES);
							memcpyByMovem(dst, src, MEM_BLOCK_SIZE_IN_BYTES);
							memcpyByMovem(dst, src, MEM_BLOCK_SIZE_IN_BYTES);
							memcpyByMovem(dst, src, MEM_BLOCK_SIZE_IN_BYTES);
							memcpyByMovem(dst, src, MEM_BLOCK_SIZE_IN_BYTES);
							memcpyByMovem(dst, src, MEM_BLOCK_SIZE_IN_BYTES);
							copiedSizeInBytes += MEM_BLOCK_SIZE_IN_BYTES * 8;
							rtcCur = getRtc();
						}
					} break;

					case MemcpyMethod_DmaMaximumRate: {
						while (rtcPrev == rtcCur) {
							/* 計測オーバーヘッドを隠すため 8 回実行 */
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_MaimumRate, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_MaimumRate, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_MaimumRate, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_MaimumRate, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_MaimumRate, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_MaimumRate, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_MaimumRate, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_MaimumRate, true);
							copiedSizeInBytes += MEM_BLOCK_SIZE_IN_BYTES * 8;
							rtcCur = getRtc();
						}
					} break;

					case MemcpyMethod_DmaLimitedRateLiteCsrPolling: {
						while (rtcPrev == rtcCur) {
							/* 計測オーバーヘッドを隠すため 8 回実行 */
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, false);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, false);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, false);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, false);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, false);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, false);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, false);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, false);
							copiedSizeInBytes += MEM_BLOCK_SIZE_IN_BYTES * 8;
							rtcCur = getRtc();
						}
					} break;

					case MemcpyMethod_DmaLimitedRateFullCsrPolling: {
						while (rtcPrev == rtcCur) {
							/* 計測オーバーヘッドを隠すため 8 回実行 */
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, true);
							copiedSizeInBytes += MEM_BLOCK_SIZE_IN_BYTES * 8;
							rtcCur = getRtc();
						}
					} break;
				}
				ei();
			}

			/* 計測値の保存 */
			results[iDst] = copiedSizeInBytes;

			/* 何かキーが押されていたら中断 */
			if (INPOUT(0xFF)) {
				printf("aborted"); fprintf(fp, "aborted");
				return false;
			}
		}

		/* src dst ともにベリファイ可能なメモリならベリファイを実行 */
		if (s_srcMemInfoArray[iSrc].verify) {
			for (int iDst = 0; iDst < COUNT_OF(s_dstMemInfoArray); ++iDst) {
				if (s_dstMemInfoArray[iDst].verify) {
					uint8_t const *u8Src = (uint8_t const *)s_srcMemInfoArray[iSrc].p;
					uint8_t const *u8Dst = (uint8_t const *)s_dstMemInfoArray[iDst].p;
					for (int i = 0; i < MEM_BLOCK_SIZE_IN_BYTES; ++i) {
						if (u8Src[i] != u8Dst[i]) {
							/* 不一致箇所を表示 */
							sprintf(
								buff,
								"VERIFY ERROR AT src(%s) dst(%s) index(%d)\n",
								s_srcMemInfoArray[iSrc].name,
								s_dstMemInfoArray[iDst].name,
								i
							);
							printf(buff); fprintf(fp, buff);

							/* メモリブロックをログにダンプする */
							fprintf(fp, "src:\n");
							for (int i = 0; i < MEM_BLOCK_SIZE_IN_BYTES; ++i) {
								fprintf(
									fp,
									"%02x%s",
									u8Src[i],
									(i & 15) == 15? "\n": " ",
									i
								);
							}
							fprintf(fp, "dst:\n");
							for (int i = 0; i < MEM_BLOCK_SIZE_IN_BYTES; ++i) {
								fprintf(
									fp,
									"%02x%s",
									u8Dst[i],
									(i & 15) == 15? "\n": " ",
									i
								);
							}

							return false;
						}
					}
				}
			}
		}

		/* 計測値の記録 */
		for (int iDst = 0; iDst < COUNT_OF(s_dstMemInfoArray); ++iDst) {
			sprintf(
				buff,
				" %8d |",
				results[iDst]
			);
			printf(buff); fprintf(fp, buff);
		}

		/* ログの改行 */
		sprintf(
			buff,
			"\n"
		);
		printf(buff); fprintf(fp, buff);
	}

	/* ログの footer */
	{
		sprintf(
			buff,
			"+------------------------+"
		);
		printf(buff); fprintf(fp, buff);

		for (int iDst = 0; iDst < COUNT_OF(s_dstMemInfoArray); ++iDst) {
			sprintf(
				buff,
				"----------+"
			);
			printf(buff); fprintf(fp, buff);
		}

		sprintf(
			buff,
			"\n\n"
		);
		printf(buff); fprintf(fp, buff);
	}

	return true;
}

void main()
{
	SUPER(0);

	/* ヒープサイズを拡張 */
	allmem();

	/* 512x512 256color 2plane 31kHz に設定 */
	CRTMOD(8);			/* X68000 EnvironmentHandBook p.296 */

	/* グラフィッククリアと表示 on */
	G_CLR_ON();

	/* スプライトが使える状態にする */
	SP_INIT();			/* X68000 EnvironmentHandBook p.415 */
	SP_ON();			/* X68000 EnvironmentHandBook p.415 */

	/* BG 0 1 のプレーン割り当てと表示 on off */
	BGCTRLST(0, 0, 1);	/* X68000 EnvironmentHandBook p.422 */
	BGCTRLST(1, 1, 1);	/* X68000 EnvironmentHandBook p.422 */

	/* カーソル消去 */
	B_CUROFF();			/* X68000 EnvironmentHandBook p.312 */

	/* ログファイルを開く */
	FILE *fp = fopen("mem_bw.log", "wb");

	/* ビルド設定を記録 */
	{
		char buff[0x1000];
		sprintf(
			buff,
			"build settings\n"
			"	ENABLE_SRAM_WRITE %d\n"
			"\n",
			ENABLE_SRAM_WRITE
		);
		printf(buff); fprintf(fp, buff);
	}

#if ENABLE_SRAM_WRITE
	/* SRAM を書き込み可能モードに設定する */
	B_BPOKE((UBYTE *)0xE8E00D, 0x31);

	/* バックアップを取る */
	memcpy(s_sramBack, (const void *)0xED0000, 0x4000);
#else
	/* SRAM を書き込み禁止モードに設定する */
	B_BPOKE((UBYTE *)0xE8E00D, 0x00);
#endif

	/* テストケースのループ */
	int testCase = 0;
#if 0
/*
	全組み合わせパターンを計測対象とする。
	組み合わせ爆発が起きるので利用は推奨しない。
*/
	int numTestCases = CrtHf_Count * DisplaySpBg_Count * TVramMode_Count * GVramMode_Count * MemcpyMethod_Count;
	for (CrtHf crtHf = 0; crtHf < CrtHf_Count; ++crtHf) {
		for (DisplaySpBg displaySpBg = 0; displaySpBg < DisplaySpBg_Count; ++displaySpBg) {
			for (TVramMode tvramMode = 0; tvramMode < TVramMode_Count; ++tvramMode) {
				for (GVramMode gvramMode = 0; gvramMode < GVramMode_Count; ++gvramMode) {
					for (MemcpyMethod memcpyMethod = 0; memcpyMethod < MemcpyMethod_Count; ++memcpyMethod) {
						/* 計測 */
						bool ret = measurement(
							testCase,
							numTestCases,
							crtHf,
							displaySpBg,
							tvramMode,
							gvramMode,
							memcpyMethod,
							fp
						);
						if (ret == false) {
							goto aborted;
						}
						testCase++;
					}
				}
			}
		}
	}
#else
/*
	表示 on off 設定を 4 通りに減らすことで、
	組み合わせ爆発を多少軽減した実装。
*/
	int numTestCases = 4 * CrtHf_Count * MemcpyMethod_Count;
	for (CrtHf crtHf = 0; crtHf < CrtHf_Count; ++crtHf) {
		for (MemcpyMethod memcpyMethod = 0; memcpyMethod < MemcpyMethod_Count; ++memcpyMethod) {
			for (int displayMode = 0; displayMode < 4; ++displayMode) {
				DisplaySpBg displaySpBg = 0;
				TVramMode tvramMode = 0;
				GVramMode gvramMode = 0;
				switch (displayMode) {
					case 0: {
						displaySpBg	= DisplaySpBg_On;
						tvramMode	= TVramMode_Display;
						gvramMode	= GVramMode_Display;
					} break;
					case 1: {
						displaySpBg	= DisplaySpBg_Off;
						tvramMode	= TVramMode_Display;
						gvramMode	= GVramMode_Display;
					} break;
					case 2: {
						displaySpBg	= DisplaySpBg_On;
						tvramMode	= TVramMode_Buffer;
						gvramMode	= GVramMode_Display;
					} break;
					case 3: {
						displaySpBg	= DisplaySpBg_On;
						tvramMode	= TVramMode_Display;
						gvramMode	= GVramMode_Buffer;
					} break;
				}
				/* 計測 */
				bool ret = measurement(
					testCase,
					numTestCases,
					crtHf,
					displaySpBg,
					tvramMode,
					gvramMode,
					memcpyMethod,
					fp
				);
				if (ret == false) {
					goto aborted;
				}
				testCase++;
			}
		}
	}
#endif


aborted:
#if ENABLE_SRAM_WRITE
	/* SRAM バックアップから復元 */
	memcpy((void *)0xED0000, s_sramBack, 0x4000);

	/* SRAM を書き込み禁止モードに戻す */
	B_BPOKE((UBYTE *)0xE8E00D, 0x00);
#endif

	/* ログファイルを閉じる */
	fclose(fp);

	/* 画面モードを戻す */
	CRTMOD(0x10);
}
