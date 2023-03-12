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
#include <stat.h>
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
	"memcpyByDma Limited-rate LiteCsrPolling\n",
	"memcpyByDma Limited-rate FullCsrPolling\n",
};

/*
	ログ記録まわり
	ディスクへの書き込み頻度を減らすため、バッファに溜めて一気に書き出す。
*/
static char s_logBuffer[256 * 1024] = {0};
size_t s_logBufferIndex = 0;
static bool printLogFile(
	const char *string
){
	printf("%s", string);
	size_t len = strlen(string) + 1;						/* 終端 \0 を含ませた長さ */
	if (s_logBufferIndex + len > sizeof(s_logBuffer)) {
		return false;
	}
	memcpy(&s_logBuffer[s_logBufferIndex], string, len);	/* 終端 \0 もコピー */
	s_logBufferIndex += len - 1;							/* 終端 \0 分戻す */
}
static void writeLogFile(const char *fileName) {
	FILE *fp = fopen(fileName, "wb");
	fwrite(s_logBuffer, 1, strlen(s_logBuffer), fp);		/* 終端 \0 は書き込まれない */
	fclose(fp);
}

/* メモリ確保した領域にファイルを読み込む */
void *mallocReadFile(
	const char *fileName,
	size_t *sizeRet
){
	FILE *fd = fopen(fileName, "rb");
	if (fd == NULL) return NULL;
	struct stat stbuf;
	if (fstat(fileno(fd), &stbuf) == -1) {
		fclose(fd);
		return NULL;
	}
	size_t sizeInBytes = (uint32_t)stbuf.st_size;
	void *buffer = malloc(sizeInBytes);
	if (buffer == NULL) {
		fclose(fd);
		return NULL;
	}
	fread(buffer, 1, sizeInBytes, fd);
	*sizeRet = sizeInBytes;
	fclose(fd);
	return buffer;
}

/* CRC32 を求める */
uint32_t crc32(
	const	void		*p,
			size_t		sizeInBytes
){
	static const uint32_t crcTable[] = {
		0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f, 0xe963a535, 0x9e6495a3,
		0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988, 0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91,
		0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
		0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5,
                                                                                                      
		0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172, 0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,
		0x35b5a8fa, 0x42b2986c, 0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
		0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423, 0xcfba9599, 0xb8bda50f,
		0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924, 0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,
                                                                                                      
		0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
		0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d, 0x91646c97, 0xe6635c01,
		0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e, 0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457,
		0x65b0d9c6, 0x12b7e950, 0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
                                                                                                      
		0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb,
		0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0, 0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9,
		0x5005713c, 0x270241aa, 0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
		0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81, 0xb7bd5c3b, 0xc0ba6cad,
                                                                                                      
		0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a, 0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683,
		0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
		0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb, 0x196c3671, 0x6e6b06e7,
		0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc, 0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5,
                                                                                                      
		0xd6d6a3e8, 0xa1d1937e, 0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
		0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55, 0x316e8eef, 0x4669be79,
		0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236, 0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f,
		0xc5ba3bbe, 0xb2bd0b28, 0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
                                                                                                      
		0x9b64c2b0, 0xec63f226, 0x756aa39c,  0x26d930a, 0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713,
		0x95bf4a82, 0xe2b87a14, 0x7bb12bae,  0xcb61b38, 0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21,
		0x86d3d2d4, 0xf1d4e242, 0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
		0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45,
                                                                                                      
		0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2, 0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db,
		0xaed16a4a, 0xd9d65adc, 0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
		0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693, 0x54de5729, 0x23d967bf,
		0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94, 0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d,
	};
	uint32_t crc = 0xFFFFFFFF;
	const uint8_t *pU8 = (const uint8_t *)p;
	const uint8_t *pU8End = pU8 + sizeInBytes;
	for (; pU8 < pU8End; pU8++) {
		crc = crcTable[(uint8_t)crc ^ *pU8] ^ (crc >> 8);
	}
	return crc ^ 0xFFFFFFFF;
}

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
	uint8_t dmaGcrBr,
	uint8_t dmaGcrBt,
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

	/*
		InsideX68000 p.56
		GCR : ジェネラルコントロールレジスタ (ch#3 にしか存在しない)
	*/
	uint8_t *gcr = (uint8_t *)0xE840FF;
	{
		uint8_t bt = dmaGcrBt;		/* 1バーストあたりのDMAクロックサイクル数 0:16clock, 1:32clock, 2:64clock, 3:128clock */
		uint8_t br = dmaGcrBr;		/* バス占有率 0:50%, 1:25%, 2:12.5%, 3:6.25% */
		*gcr = ((bt & 3) << 2) | (br & 3);
	}

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
	uint8_t dmaGcrBr,
	uint8_t dmaGcrBt
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
			printLogFile(buff);
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
			printLogFile(buff);
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
			printLogFile(buff);
		} break;
		case DisplaySpBg_On: {
			*(uint8_t *)0xEB0808 = *(uint8_t *)0xEB0808 |  2;
			sprintf(
				buff,
				"SP BG disp on (by $EB0808)\n"
			);
			printLogFile(buff);
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
			sprintf(
				buff,
				"testCase %d/%d の計測が進行中です。しばらくお待ちください。",
				testCase,
				numTestCases - 1
			);
			int r = 8;
			int g = 8;
			int b = 8;
			uint16_t color = (g << (5+5+1)) | (r << (5+1)) | (b << 1);
			struct SYMBOLPTR param = {
				/* WORD		x1 */				0x10,
				/* WORD		y1 */				0x20,
				/* UBYTE	*string_address */	buff,
				/* UBYTE	mag_x */			1,
				/* UBYTE	mag_y */			1,
				/* UWORD	color */			color,
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
		printLogFile(buff);
	}

	/* MemcpyMethod をログに記録 */
	{
		sprintf(
			buff,
			s_memcpyMethodDescriptions[memcpyMethod]
		);
		printLogFile(buff);
	}

	/* ログの header */
	{
		sprintf(
			buff,
			"\n"
			"throughput (bytes/sec)\n"
			"+------------------------+"
		);
		printLogFile(buff);

		for (int iDst = 0; iDst < COUNT_OF(s_dstMemInfoArray); ++iDst) {
			sprintf(
				buff,
				"----------+"
			);
			printLogFile(buff);
		}

		sprintf(
			buff,
			"\n"
			"|                   dst  |"
		);
		printLogFile(buff);

		for (int iDst = 0; iDst < COUNT_OF(s_dstMemInfoArray); ++iDst) {
			sprintf(
				buff,
				"  %s  |",
				s_dstMemInfoArray[iDst].name
			);
			printLogFile(buff);
		}

		sprintf(
			buff,
			"\n"
			"|                        |"
		);
		printLogFile(buff);

		for (int iDst = 0; iDst < COUNT_OF(s_dstMemInfoArray); ++iDst) {
			sprintf(
				buff,
				" 0x%06x |",
				s_dstMemInfoArray[iDst].p
			);
			printLogFile(buff);
		}

		sprintf(
			buff,
			"\n"
			"|  src                   |"
		);
		printLogFile(buff);

		for (int iDst = 0; iDst < COUNT_OF(s_dstMemInfoArray); ++iDst) {
			sprintf(
				buff,
				"~0x%06x |",
				s_dstMemInfoArray[iDst].p + MEM_BLOCK_SIZE_IN_BYTES - 1
			);
			printLogFile(buff);
		}

		sprintf(
			buff,
			"\n"
			"+------------------------+"
		);
		printLogFile(buff);

		for (int iDst = 0; iDst < COUNT_OF(s_dstMemInfoArray); ++iDst) {
			sprintf(
				buff,
				"----------+"
			);
			printLogFile(buff);
		}

		sprintf(
			buff,
			"\n"
		);
		printLogFile(buff);
	}

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
		printLogFile(buff);

		/* 現在時刻の初期値 */
		uint32_t rtcCur = getRtc();

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
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_MaimumRate, dmaGcrBr, dmaGcrBt, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_MaimumRate, dmaGcrBr, dmaGcrBt, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_MaimumRate, dmaGcrBr, dmaGcrBt, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_MaimumRate, dmaGcrBr, dmaGcrBt, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_MaimumRate, dmaGcrBr, dmaGcrBt, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_MaimumRate, dmaGcrBr, dmaGcrBt, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_MaimumRate, dmaGcrBr, dmaGcrBt, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_MaimumRate, dmaGcrBr, dmaGcrBt, true);
							copiedSizeInBytes += MEM_BLOCK_SIZE_IN_BYTES * 8;
							rtcCur = getRtc();
						}
					} break;

					case MemcpyMethod_DmaLimitedRateLiteCsrPolling: {
						while (rtcPrev == rtcCur) {
							/* 計測オーバーヘッドを隠すため 8 回実行 */
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, dmaGcrBr, dmaGcrBt, false);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, dmaGcrBr, dmaGcrBt, false);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, dmaGcrBr, dmaGcrBt, false);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, dmaGcrBr, dmaGcrBt, false);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, dmaGcrBr, dmaGcrBt, false);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, dmaGcrBr, dmaGcrBt, false);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, dmaGcrBr, dmaGcrBt, false);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, dmaGcrBr, dmaGcrBt, false);
							copiedSizeInBytes += MEM_BLOCK_SIZE_IN_BYTES * 8;
							rtcCur = getRtc();
						}
					} break;

					case MemcpyMethod_DmaLimitedRateFullCsrPolling: {
						while (rtcPrev == rtcCur) {
							/* 計測オーバーヘッドを隠すため 8 回実行 */
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, dmaGcrBr, dmaGcrBt, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, dmaGcrBr, dmaGcrBt, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, dmaGcrBr, dmaGcrBt, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, dmaGcrBr, dmaGcrBt, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, dmaGcrBr, dmaGcrBt, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, dmaGcrBr, dmaGcrBt, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, dmaGcrBr, dmaGcrBt, true);
							memcpyByDma(dst, src, MEM_BLOCK_SIZE_IN_BYTES, DmaOcrReqg_LimitedRate, dmaGcrBr, dmaGcrBt, true);
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
				printLogFile("aborted");
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
							printLogFile(buff);
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
			printLogFile(buff);
		}

		/* ログの改行 */
		sprintf(
			buff,
			"\n"
		);
		printLogFile(buff);
	}

	/* ログの footer */
	{
		sprintf(
			buff,
			"+------------------------+"
		);
		printLogFile(buff);

		for (int iDst = 0; iDst < COUNT_OF(s_dstMemInfoArray); ++iDst) {
			sprintf(
				buff,
				"----------+"
			);
			printLogFile(buff);
		}

		sprintf(
			buff,
			"\n\n"
		);
		printLogFile(buff);
	}

	return true;
}

int main(int argc, char *argv[])
{
	SUPER(0);

	/* ヒープサイズを拡張 */
	allmem();

	/* 引数取得結果 */
	typedef enum {
		TestMode_Undefined = 0,
		TestMode_Basic,
		TestMode_Full,
		TestMode_Count,
	} TestMode;
	struct {
		TestMode	testMode;
		int			testCase;
		uint8_t		dmaGcrBr;
		uint8_t		dmaGcrBt;
		const char	*outputFilename;
	} arg = {
		/* testMode */			TestMode_Undefined,
		/* testCase */			-1,	/* 全テストケースを実行 */
		/* dmaGcrBr */			0,	/* Band-width ratio 50% */
		/* dmaGcrBt */			0,	/* Burst time 16 cycles */
		/* outputFilename */	"mem_bw.log"
	};

	/* 引数解析 */
	if (argc == 1) {
		printf(
			"Memory bandwidth measurement tool for X680x0.\n"
			"https://github.com/yosshin4004/x68k_test/tree/main/mem_bw\n"
			"\n"
			"usage:\n"
			"	MEM_BW.X [options]\n"
			"\n"
			"option:\n"
			"	-mode <n>\n"
			"		Select a test mode.\n"
			"		1: Basic (40 test cases)\n"
			"		2: Full (80 test cases)\n"
			"\n"
			"	-case <n>\n"
			"		Execute specified test case.\n"
			"\n"
			"	-br <n>\n"
			"		Specify a limited-rate DMA band-width ratio. (0~3)\n"
			"		0: 50%%\n"
			"		1: 25%%\n"
			"		2: 12.5%%\n"
			"		3: 6.25%%\n"
			"\n"
			"	-bt <n>\n"
			"		Specify a limited-rate DMA burst time. (0~3)\n"
			"		0: 16 sycles\n"
			"		1: 32 sycles\n"
			"		2: 64 sycles\n"
			"		3: 128 sycles\n"
			"\n"
			"	-o <filename>\n"
			"		Specify a output filename.\n"
			"		default filename is 'mem_bw.log'\n"
			"\n"
			"example:\n"
			"	MEM_BW.X -mode 1 -case 1 -bw 3 -bt 3 -o measurement.log\n"
		);
		return EXIT_SUCCESS;
	} else {
		int i = 1;
		while (i < argc) {
			if (argv[i][0] == '-') {
				if (strcmp(argv[i], "-mode") == 0) {
					i++;
					if (i >= argc) {
						printf("ERROR : No arg for '%s'.\n", argv[i - 1]);
						return EXIT_FAILURE;
					}
					arg.testMode = atoi(argv[i]);
					if (arg.testMode <= TestMode_Undefined || TestMode_Count <= arg.testMode) {
						printf("ERROR : Invalid arg for '%s'.\n", argv[i - 1]);
						return EXIT_FAILURE;
					}
				} else
				if (strcmp(argv[i], "-case") == 0) {
					i++;
					if (i >= argc) {
						printf("ERROR : No arg for '%s'.\n", argv[i - 1]);
						return EXIT_FAILURE;
					}
					arg.testCase = atoi(argv[i]);
					if (arg.testCase < 0) {
						printf("ERROR : Invalid arg for '%s'.\n", argv[i - 1]);
						return EXIT_FAILURE;
					}
				} else
				if (strcmp(argv[i], "-br") == 0) {
					i++;
					if (i >= argc) {
						printf("ERROR : No arg for '%s'.\n", argv[i - 1]);
						return EXIT_FAILURE;
					}
					arg.dmaGcrBr = atoi(argv[i]);
					if (arg.dmaGcrBr < 0 || 3 < arg.dmaGcrBr) {
						printf("ERROR : Invalid arg for '%s'.\n", argv[i - 1]);
						return EXIT_FAILURE;
					}
				} else
				if (strcmp(argv[i], "-bt") == 0) {
					i++;
					if (i >= argc) {
						printf("ERROR : No arg for '%s'.\n", argv[i - 1]);
						return EXIT_FAILURE;
					}
					arg.dmaGcrBt = atoi(argv[i]);
					if (arg.dmaGcrBt < 0 || 3 < arg.dmaGcrBt) {
						printf("ERROR : Invalid arg for '%s'.\n", argv[i - 1]);
						return EXIT_FAILURE;
					}
				} else
				if (strcmp(argv[i], "-o") == 0) {
					i++;
					if (i >= argc) {
						printf("ERROR : No arg for '%s'.\n", argv[i - 1]);
						return EXIT_FAILURE;
					}
					arg.outputFilename = argv[i];
				} else {
					printf("ERROR : Invalid arg '%s'\n", argv[i]);
					return EXIT_FAILURE;
				}
			} else {
				printf("ERROR : Invalid arg '%s'\n", argv[i]);
				return EXIT_FAILURE;
			}
			i++;
		}
	}

	/* 引数チェック */
	if (arg.testMode == TestMode_Undefined) {
		printf("ERROR : Test mode (-mode option) is not specified.\n");
		return EXIT_FAILURE;
	}

	/* 実行ファイルの crc32 を求める */
	uint32_t crc = 0;
	{
		size_t sizeInBytes;
		void *p = mallocReadFile(argv[0], &sizeInBytes);
		if (p == NULL) {
			printf("ERROR : can not read %s.\n", argv[0]);
			return EXIT_FAILURE;
		}
		crc = crc32(p, sizeInBytes);
		free(p);
	}

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

	/* ビルド設定と引数を記録 */
	{
		char buff1[0x1000];
		if (arg.testCase != -1) {
			sprintf(buff1, "%d", arg.testCase);
		} else {
			sprintf(buff1, "%s", "(not specified)");
		}
		char buff2[0x1000];
		const char *s_brTbl[] = {"50", "25", "12.5", "6.25"};
		const int s_btTbl[] = {16, 32, 64, 128};
		sprintf(
			buff2,
			"build settings\n"
			"	ENABLE_SRAM_WRITE %d\n"
			"	BUILD " __DATE__ ", CRC32 %08X\n"
			"	ARG\n"
			"		-mode %d\n"
			"		-case %s\n"
			"		-br %d (Limited-rate DMA band-width ratio %s%%)\n"
			"		-bt %d (Limited-rate DMA burst time %d cycles)\n"
			"\n",
			ENABLE_SRAM_WRITE,
			crc,
			arg.testMode,
			buff1,
			arg.dmaGcrBr,
			s_brTbl[arg.dmaGcrBr & 3],
			arg.dmaGcrBt,
			s_btTbl[arg.dmaGcrBt & 3]
		);
		printLogFile(buff2);
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

	/* テストモード毎のテストセットを実行 */
	switch (arg.testMode) {
		case TestMode_Basic: {
			/*
				表示 on off 設定を 4 通りに減らすことで、
				組み合わせ爆発を多少軽減したセット。
			*/
			int numTestCases = 4 * CrtHf_Count * MemcpyMethod_Count;
			int testCase = 0;
			for (CrtHf crtHf = 0; crtHf < CrtHf_Count; ++crtHf) {
				for (MemcpyMethod memcpyMethod = 0; memcpyMethod < MemcpyMethod_Count; ++memcpyMethod) {
					for (int displayMode = 0; displayMode < 4; ++displayMode) {
						if (arg.testCase == -1 || arg.testCase == testCase) {
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
								arg.dmaGcrBr,
								arg.dmaGcrBt
							);
							if (ret == false) {
								goto aborted;
							}
						}
						testCase++;
					}
				}
			}
		} break;

		case TestMode_Full: {
			/*
				全組み合わせパターンを計測対象とする。
				組み合わせ爆発が起きるので利用は推奨しない。
			*/
			int numTestCases = CrtHf_Count * DisplaySpBg_Count * TVramMode_Count * GVramMode_Count * MemcpyMethod_Count;
			int testCase = 0;
			for (CrtHf crtHf = 0; crtHf < CrtHf_Count; ++crtHf) {
				for (DisplaySpBg displaySpBg = 0; displaySpBg < DisplaySpBg_Count; ++displaySpBg) {
					for (TVramMode tvramMode = 0; tvramMode < TVramMode_Count; ++tvramMode) {
						for (GVramMode gvramMode = 0; gvramMode < GVramMode_Count; ++gvramMode) {
							for (MemcpyMethod memcpyMethod = 0; memcpyMethod < MemcpyMethod_Count; ++memcpyMethod) {
								if (arg.testCase == -1 || arg.testCase == testCase) {
									/* 計測 */
									bool ret = measurement(
										testCase,
										numTestCases,
										crtHf,
										displaySpBg,
										tvramMode,
										gvramMode,
										memcpyMethod,
										arg.dmaGcrBr,
										arg.dmaGcrBt
									);
									if (ret == false) {
										goto aborted;
									}
								}
								testCase++;
							}
						}
					}
				}
			}
		} break;
	}


aborted:
#if ENABLE_SRAM_WRITE
	/* SRAM バックアップから復元 */
	memcpy((void *)0xED0000, s_sramBack, 0x4000);

	/* SRAM を書き込み禁止モードに戻す */
	B_BPOKE((UBYTE *)0xE8E00D, 0x00);
#endif

	/* ログファイルを書き出す */
	writeLogFile(arg.outputFilename);

	/* 画面モードを戻す */
	CRTMOD(0x10);

	return EXIT_SUCCESS;
}
