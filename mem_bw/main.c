/*
	CPU �ɂ�郁�����A�N�Z�X�̃o���h���𒲂ׂ܂��B
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
	�o���h���e�X�g�ŗ��p���郁�����u���b�N�̃T�C�Y
	�X�v���C�g�X�N���[�����W�X�^�̈悪 1024 �o�C�g�����Ȃ��A
	�]�����Ɠ]�������Ȃ��悤�ɂ���ƂȂ�ƁA
	512 �o�C�g�ȊO�̑I�������Ȃ��B
*/
#define	MEM_BLOCK_SIZE_IN_BYTES	(512)


/*
	�ȉ��̒萔�� 1 �ɂ���ƁASRAM �̏�����������������Ԃł̃e�X�g���s���B
	����������ꂽ SRAM �̓��e�́A�v���O�����I�����ɕ�������邪�A
	���Z�b�g��N���b�V�����ŋ����I�ɒ��f�����ꍇ�A��������Ȃ��̂Œ��ӁB
	���ɗ��R���Ȃ�����A0 �ɂ��邱�Ƃ�������������B

	���@���iPro10MHz�j�ŁASRAM �̏�����������/�֎~�ǂ���̏�Ԃł��A
	�������A�N�Z�X���x�ɕω����Ȃ����Ƃ��m�F����Ă���B
*/
#define ENABLE_SRAM_WRITE	(0)


/*
	�f�o�b�O�R�[�h�̗L����
*/
#define ENABLE_DEBUG_CODE	(0)


#if ENABLE_SRAM_WRITE
/* SRAM �o�b�N�A�b�v */
static uint8_t s_sramBack[0x4000];
#endif


/*
	�R���p�C���̍œK���ɂ��R�[�h��������邱�Ƃ�����邽�ߗ��p���� global �ϐ�
*/
uint8_t g_dummy;


/* �e�e�X�g�P�[�X�̐ݒ� */
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

/* �������v�̏�Ԃ��擾�i���16bit=�b��1���ځA����16bit=�b��2���ځj*/
static FORCE_INLINE uint32_t
getRtc(){
	/* �n�[�h�E�F�A�ɒ��ڃA�N�Z�X���������v�̏�Ԃ��擾���� */
	uint32_t rtc;
	do {
		/*
			RTC �̊e���W�X�^�͔񓯊��ɍX�V�����̂ŁA�b��1���ڂ�2���ڂ��Y���Ȃ������Ɏ擾����ɂ͍H�v���K�v�B
			�����ł́A�����l�� 2 ��ǂ߂�܂� do�`while �ŌJ��Ԃ����@�𗘗p���Ă���B
		*/
		rtc = *(volatile uint32_t *)0xE8A000 & 0x000F0007;
	} while (rtc != (*(volatile uint32_t *)0xE8A000 & 0x000F0007));
	return rtc;
}

/* ���荞�݋֎~ */
static FORCE_INLINE void
di() {
	asm volatile (
		"\n"
		"	ori.w	#$0700,sr\n"
	);
}

/* ���荞�݋��� */
static FORCE_INLINE void
ei() {
	asm volatile (
		"\n"
		"	andi.w	#$f8ff,sr\n"
	);
}

/*
	uint8 �P�ʂ� *dst++ = *src++ ����i�C�[�u�ȃR�s�[
*/
static FORCE_INLINE void
memcpyByU8(
	void *dst,
	const void *src,
	uint16_t sizeInBytes
){
#if ENABLE_DEBUG_CODE
	/* �G���[�`�F�b�N */
	if (sizeInBytes == 0) {
		printf("memcpyByU8 ERROR : sizeInBytes is must be != 0.\n");
		exit(EXIT_FAILURE);
	}
#endif

	/*
		1 �o�C�g�P�ʂŃR�s�[

		���[�v�ē����� 1 �o�C�g������ 22 cycles �̃R�X�g�B
		�N���b�N 10MHz �Ȃ痝�_�� 10000000 / 22 = 454545 �o�C�g�R�s�[�ł���B
	*/
	{
		register uint16_t     reg_d0 asm ("d0") = sizeInBytes;
		register void const * reg_a0 asm ("a0") = src;
		register void       * reg_a1 asm ("a1") = dst;
		asm volatile (
			"\n"
			"@@:\n"
			"	move.b	(a0)+, (a1)+\n"		/* 12 */
			"	dbra	d0, @b\n"			/* ���[�v�p��:10, ���[�v�I��:14 */

		:	/* �o�� */	"+r"	(reg_d0),	/* in out %0 (���́��j��) */
						"+r"	(reg_a0),	/* in out %1 (���́��j��) */
						"+r"	(reg_a1)	/* in out %2 (���́��j��) */
		:	/* ���� */
		:	/* �j�� */	"memory"			/* �������o���A��v�� */
		);
	}
}

/*
	movem ���g���������ȃR�s�[
*/
static FORCE_INLINE void
memcpyByMovem(
	void *dst,
	const void *src,
	uint16_t sizeInBytes
){
#if ENABLE_DEBUG_CODE
	/* �G���[�`�F�b�N */
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
		256 �o�C�g�P�ʂŃR�s�[

		���[�v�ē����� 256 �o�C�g������A���悻 76 * 16 - 4 + 8 + 10 = 1230 cycles �̃R�X�g
		�N���b�N 10MHz �Ȃ痝�_�� 10000000 * 256 / 1230 = 2081301 �o�C�g�R�s�[�ł���B
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

			"	dbra	d0, @b\n"				/* ���[�v�p��:10, ���[�v�I��:14 */

		:	/* �o�� */	"+r"	(reg_d0),	/* in out %0 (���́��j��) */
						"+r"	(reg_a0),	/* in out %1 (���́��j��) */
						"+r"	(reg_a1)	/* in out %2 (���́��j��) */
		:	/* ���� */
		:	/* �j�� */	"memory",			/* �������o���A��v�� */
						"d4","d5","d6","d7","a2","a3","a4","a5"
		);
	}
}

/*
	DMA ���g���������ȃR�s�[
*/
typedef enum {
	DmaOcrReqg_LimitedRate = 0,				/* ���葬�x */
	DmaOcrReqg_MaimumRate,					/* �ő呬�x */
	DmaOcrReqg_ExternalRequests,			/* �O���v���]�� */
	DmaOcrReqg_MixedRequestGeneration,		/* 2�Ԗڈȍ~�͊O���v�� */
} DmaOcrReqg;
static FORCE_INLINE void
memcpyByDma(
	void *dst,
	const void *src,
	uint16_t sizeInBytes,
	DmaOcrReqg ocrReqg,
	bool fullCsrPolling		/* CSR ��S�͂Ń|�[�����O���邩�H */
) {
#if ENABLE_DEBUG_CODE
	/* �G���[�`�F�b�N */
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

	/* �`�����l�� #2 �̐ݒ�|�[�g */
	uint8_t *dmaReg = (uint8_t *)0xE84080;

	/* clear flag */
	uint8_t volatile *csr = dmaReg + 0x00;
	*csr = 0xff;

	/* setup */
	/*
		InsideX68000 p.44
		DCR : �f�o�C�X�R���g���[�����W�X�^
		OCR : �I�y���[�V�����R���g���[�����W�X�^
	*/
	uint8_t *dcr = dmaReg + 0x04;
	{
		uint8_t xrm		= 0;
		uint8_t dtyp	= 0;
		uint8_t dps		= 1;		/* �f�o�C�X�|�[�g�T�C�Y 0:8bit, 1:16bit */
		uint8_t pcl		= 0;
		*dcr = ((xrm & 3) << 6) | ((dtyp & 3) << 4) | ((dps & 1) << 3) | (pcl & 3);
	}
	uint8_t *ocr = dmaReg + 0x05;
	{
		uint8_t dir		= 0;		/* �]������ 0:������->�f�o�C�X, 1:�f�o�C�X->������ */
		uint8_t btd		= 0;
		uint8_t size	= 2;		/* �I�y�����h�T�C�Y 0:8bit, 1:16bit, 2:32bit, 3:�p�b�N���� */
		uint8_t chain	= 0;		/* 0:�`�F�C���Ȃ�, 2:�A���C�`�F�C��, 3:�����N�A���C�`�F�C�� */
		uint8_t reqg	= ocrReqg;	/* 0:���葬�x, 1:�ő呬�x, 2:�O���v���]��, 3:2�Ԗڈȍ~�͊O���v�� */
		*ocr = ((dir & 1) << 7) | ((btd & 1) << 6) | ((size & 3) << 4) | ((chain & 3) << 2) | (reqg & 3);
	}
	/*
		InsideX68000 p.50
		SCR : �V�[�P���X�R���g���[�����W�X�^
		CCR : �`�����l���R���g���[�����W�X�^
	*/
	uint8_t *scr = dmaReg + 0x06;
	{
		uint8_t mac = 1;	/* �������A�h���X���W�X�^���� 0:�ω��Ȃ�, 1:����, 2:���� */
		uint8_t dac = 1;	/* �f�o�C�X�A�h���X���W�X�^���� 0:�ω��Ȃ�, 1:����, 2:���� */
		*scr = ((mac & 3) << 2) | (dac & 3);
	}
	uint8_t *ccr = dmaReg + 0x07;
	{
		uint8_t str = 0;	/* 1:����X�^�[�g */
		uint8_t cnt = 0;
		uint8_t hlt = 0;
		uint8_t sab = 0;
		uint8_t inte = 0;
		*ccr = ((str & 1) << 7) | ((cnt & 1) << 6) | ((hlt & 1) << 5) | ((sab & 1) << 4) | ((inte & 1) << 3);
	}
	/*
		InsideX68000 p.54
		CPR : �`�����l���v���C�I���e�B���W�X�^
	*/
	uint8_t *cpr = dmaReg + 0x2D;
	{
		uint8_t cp = 3;		/* 0:�ł������D��x �` 3:�ł��Ⴂ�D��x */
		*cpr = cp & 3;
	}
	/*
		InsideX68000 p.54
		MFC : �������t�@���t�@���N�V�����R�[�h
		DFC : �f�o�C�X�t�@���N�V�����R�[�h
		BFC : �x�[�X�t�@���N�V�����R�[�h
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
		MTC : �������g�����X�t�@�J�E���^
	*/
	uint16_t *mtc = (uint16_t *)(dmaReg + 0x0A);
	*mtc = sizeInBytes >> 2;	/* 32bit �P�ʂŃR�s�[����̂� 4 �Ŋ����Ă��� */
	/*
		InsideX68000 p.29
		MAR : �������A�h���X���W�X�^
	*/
	uint32_t *mar = (uint32_t *)(dmaReg + 0x0C);
	*mar = (uintptr_t)src;
	/*
		InsideX68000 p.29
		DAR : �f�o�C�X�A�h���X���W�X�^
	*/
	uint32_t *dar = (uint32_t *)(dmaReg + 0x14);
	*dar = (uintptr_t)dst;

#if 1
	/*
		InsideX68000 p.56
		GCR : �W�F�l�����R���g���[�����W�X�^ (ch#3 �ɂ������݂��Ȃ�)
	*/
	uint8_t *gcr = (uint8_t *)0xE840FF;
	{
		uint8_t bt = 0;		/* 1�o�[�X�g�������DMA�N���b�N�T�C�N���� 0:16clock, 1:32clock, 2:64clock, 3:128clock */
		uint8_t br = 0;		/* �o�X��L�� 0:50%, 1:25%, 2:12.5%, 3:6.25% */
		*gcr = ((bt & 3) << 2) | (br & 3);
	}
#endif

	/* start */
	*ccr |= 0x80;

	/* wait */
	/*
		���̃��[�v�̓R���p�C���ɂ���ĈӖ����Ȃ��R�[�h�ƌ��Ȃ��ꏜ�������B
		�Ӗ��̂���R�[�h�̂ӂ�����邽�߁Ag_dummy �ɃA�N�Z�X����K�v������B
		���� fullCsrPolling �� csr �̃|�[�����O�p�x�𒲐����鎖���ł���B
	*/
	if (fullCsrPolling) {
	/* CSR ��S�͂Ń|�[�����O */
		uint8_t dummy = 0;
		while ((*csr & 0x90) == 0) {
			dummy++;
		}
		g_dummy = dummy;
	} else {
	/* CSR ���y�߂Ƀ|�[�����O */
		uint8_t dummy = 0;
		while ((*csr & 0x90) == 0) {
			/* �o�X���������Ȃ��d�����ȏ�����K���Ɏ��s */
			dummy += dummy * dummy * dummy * dummy + 123;
		}
		g_dummy = dummy;
	}
}

/*
	�w��̏����Ōv�������s����
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

	/* ���C���������̃R�s�[�e�X�g�ň����̈� */
	static uint32_t s_mainMemSrc[MEM_BLOCK_SIZE_IN_BYTES / sizeof(uint32_t)];
	static uint32_t s_mainMemDst[MEM_BLOCK_SIZE_IN_BYTES / sizeof(uint32_t)];

	/* �R�s�[���R�s�[��̏�� */
	typedef struct tag_MemInfo MemInfo;
	struct tag_MemInfo {
		void *p;
		const char *name;
		bool verify;	/* �x���t�@�C�\���H�i�������񂾒ʂ�ɓǂݏo���邩�H�j*/
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
			false		/* �S�r�b�g���L���ł͂Ȃ����� false */
		},
		{
			(void *)0xEB8000,
			" PCG  ",
			false		/* uint8_t �P�ʂ̃A�N�Z�X���T�|�[�g����Ă��Ȃ����� false */
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
			false		/* read-only �Ȃ̂� false */
		},
		{
			(void *)0xFE0000,
			"IPLROM",
			false		/* read-only �Ȃ̂� false */
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
			false		/* �S�r�b�g���L���ł͂Ȃ����� false */
		},
		{
			(void *)(0xEB8000 + MEM_BLOCK_SIZE_IN_BYTES),
			" PCG  ",
			false		/* uint8_t �P�ʂ̃A�N�Z�X���T�|�[�g����Ă��Ȃ����� false */
		},
		{
			(void *)(0xED3800 + MEM_BLOCK_SIZE_IN_BYTES),	/* 0xED3FFF �܂ŃA�N�Z�X���悤�Ƃ���ƃo�X�G���[�ɂȂ�̂Œ��� */
			" SRAM ",
#if ENABLE_SRAM_WRITE
			true
#else
			false
#endif
		},
	};

	/* CRT ���[�h */
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
		�e�L�X�g�p���b�g�ݒ�
		15hKz �C���^�[���[�X�\���͉t�����j�^�ɏĂ��t�����N�����B
		�������C���Ɗ���C���̋P�x����}���邽�߃e�L�X�g�p���b�g��␳����B
	*/
	{
		/*
			InsideX68000 p.215
			�X�v���C�g�p���b�g #0 �̒�`�J�n�A�h���X
		*/
		uint16_t *spPalette = (uint16_t *)0xE82200;
		/* �����̐F */
		{
			int r = 12;
			int g = 12;
			int b = 12;
			for (int iColor = 1; iColor < 16; iColor++) {
				spPalette[iColor] = (g << (5+5+1)) | (r << (5+1)) | (b << 1);
			}
		}
		/* �w�i�F */
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
		�X�v���C�g BG �̕\�� on/off
		$EB0808 �ɂ��Ă� Inside X68000 p.190 ���Q�ƁB
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
		T-VRAM/G-VRAM ���[�h
		buffer ���[�h�ɐ؂�ւ���Ɖ�ʂ��O���b�`��ɂȂ�̂ŁA
		�r�f�I�R���g���[�����W�X�^�ŕ\�� off �ɂ���K�v������B
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
			�\���R���g���[��
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

		/* �r����ʂ��^�����ɂȂ�|�̐��� */
		if (tvramMode == TVramMode_Buffer) {
			struct SYMBOLPTR param = {
				/* WORD		x1 */				0x10,
				/* WORD		y1 */				0x10,
				/* UBYTE	*string_address */	"�v���i�s���ł��B���΂炭���҂����������E�E�E�B",
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

	/* MemcpyMethod �����O�ɋL�^ */
	{
		sprintf(
			buff,
			s_memcpyMethodDescriptions[memcpyMethod]
		);
		printf(buff); fprintf(fp, buff);
	}

	/* ���O�� header */
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

	/* ���ݎ����̏����l */
	uint32_t rtcCur = getRtc();

	/* src ���̃��[�v */
	for (int iSrc = 0; iSrc < COUNT_OF(s_srcMemInfoArray); ++iSrc) {
		/* �v���l�̈ꎞ�ۑ��o�b�t�@ */
		int results[COUNT_OF(s_dstMemInfoArray)];

		/* src �����x���t�@�C�\�ȃ������Ȃ�f�t�H���g�l�Ŗ��߂� */
		if (s_srcMemInfoArray[iSrc].verify) {
			uint8_t *u8Mem = (uint8_t *)s_srcMemInfoArray[iSrc].p;
			for (int i = 0; i < MEM_BLOCK_SIZE_IN_BYTES; ++i) {
//				u8Mem[i] = (uint8_t)(rand() >> 4);
				u8Mem[i] = (uint8_t)i;
			}
		}

		/* src ���̏������O�ɋL�^ */
		sprintf(
			buff,
			"|%s 0x%06x~0x%06x|",
			s_srcMemInfoArray[iSrc].name,
			s_srcMemInfoArray[iSrc].p,
			s_srcMemInfoArray[iSrc].p + MEM_BLOCK_SIZE_IN_BYTES - 1
		);
		printf(buff); fprintf(fp, buff);

		/* �b�̓������o */
		{
			uint32_t rtcPrev = rtcCur;
			while (rtcPrev == rtcCur) {
				rtcCur = getRtc();
			}
		}

		/* dst ���̃��[�v */
		for (int iDst = 0; iDst < COUNT_OF(s_dstMemInfoArray); ++iDst) {
			/* 1 �b�Ԃ̌v�� */
			uint32_t copiedSizeInBytes = 0;
			{
				di();
				void       *dst = s_dstMemInfoArray[iDst].p;
				void const *src = s_srcMemInfoArray[iSrc].p;
				uint32_t rtcPrev = rtcCur;
				switch (memcpyMethod) {
					case MemcpyMethod_CpuU8: {
						while (rtcPrev == rtcCur) {
							/* �v���I�[�o�[�w�b�h���B������ 8 ����s */
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
							/* �v���I�[�o�[�w�b�h���B������ 8 ����s */
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
							/* �v���I�[�o�[�w�b�h���B������ 8 ����s */
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
							/* �v���I�[�o�[�w�b�h���B������ 8 ����s */
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
							/* �v���I�[�o�[�w�b�h���B������ 8 ����s */
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

			/* �v���l�̕ۑ� */
			results[iDst] = copiedSizeInBytes;

			/* �����L�[��������Ă����璆�f */
			if (INPOUT(0xFF)) {
				printf("aborted"); fprintf(fp, "aborted");
				return false;
			}
		}

		/* src dst �Ƃ��Ƀx���t�@�C�\�ȃ������Ȃ�x���t�@�C�����s */
		if (s_srcMemInfoArray[iSrc].verify) {
			for (int iDst = 0; iDst < COUNT_OF(s_dstMemInfoArray); ++iDst) {
				if (s_dstMemInfoArray[iDst].verify) {
					uint8_t const *u8Src = (uint8_t const *)s_srcMemInfoArray[iSrc].p;
					uint8_t const *u8Dst = (uint8_t const *)s_dstMemInfoArray[iDst].p;
					for (int i = 0; i < MEM_BLOCK_SIZE_IN_BYTES; ++i) {
						if (u8Src[i] != u8Dst[i]) {
							/* �s��v�ӏ���\�� */
							sprintf(
								buff,
								"VERIFY ERROR AT src(%s) dst(%s) index(%d)\n",
								s_srcMemInfoArray[iSrc].name,
								s_dstMemInfoArray[iDst].name,
								i
							);
							printf(buff); fprintf(fp, buff);

							/* �������u���b�N�����O�Ƀ_���v���� */
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

		/* �v���l�̋L�^ */
		for (int iDst = 0; iDst < COUNT_OF(s_dstMemInfoArray); ++iDst) {
			sprintf(
				buff,
				" %8d |",
				results[iDst]
			);
			printf(buff); fprintf(fp, buff);
		}

		/* ���O�̉��s */
		sprintf(
			buff,
			"\n"
		);
		printf(buff); fprintf(fp, buff);
	}

	/* ���O�� footer */
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

	/* �q�[�v�T�C�Y���g�� */
	allmem();

	/* 512x512 256color 2plane 31kHz �ɐݒ� */
	CRTMOD(8);			/* X68000 EnvironmentHandBook p.296 */

	/* �O���t�B�b�N�N���A�ƕ\�� on */
	G_CLR_ON();

	/* �X�v���C�g���g�����Ԃɂ��� */
	SP_INIT();			/* X68000 EnvironmentHandBook p.415 */
	SP_ON();			/* X68000 EnvironmentHandBook p.415 */

	/* BG 0 1 �̃v���[�����蓖�Ăƕ\�� on off */
	BGCTRLST(0, 0, 1);	/* X68000 EnvironmentHandBook p.422 */
	BGCTRLST(1, 1, 1);	/* X68000 EnvironmentHandBook p.422 */

	/* �J�[�\������ */
	B_CUROFF();			/* X68000 EnvironmentHandBook p.312 */

	/* ���O�t�@�C�����J�� */
	FILE *fp = fopen("mem_bw.log", "wb");

	/* �r���h�ݒ���L�^ */
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
	/* SRAM ���������݉\���[�h�ɐݒ肷�� */
	B_BPOKE((UBYTE *)0xE8E00D, 0x31);

	/* �o�b�N�A�b�v����� */
	memcpy(s_sramBack, (const void *)0xED0000, 0x4000);
#else
	/* SRAM ���������݋֎~���[�h�ɐݒ肷�� */
	B_BPOKE((UBYTE *)0xE8E00D, 0x00);
#endif

	/* �e�X�g�P�[�X�̃��[�v */
	int testCase = 0;
#if 0
/*
	�S�g�ݍ��킹�p�^�[�����v���ΏۂƂ���B
	�g�ݍ��킹�������N����̂ŗ��p�͐������Ȃ��B
*/
	int numTestCases = CrtHf_Count * DisplaySpBg_Count * TVramMode_Count * GVramMode_Count * MemcpyMethod_Count;
	for (CrtHf crtHf = 0; crtHf < CrtHf_Count; ++crtHf) {
		for (DisplaySpBg displaySpBg = 0; displaySpBg < DisplaySpBg_Count; ++displaySpBg) {
			for (TVramMode tvramMode = 0; tvramMode < TVramMode_Count; ++tvramMode) {
				for (GVramMode gvramMode = 0; gvramMode < GVramMode_Count; ++gvramMode) {
					for (MemcpyMethod memcpyMethod = 0; memcpyMethod < MemcpyMethod_Count; ++memcpyMethod) {
						/* �v�� */
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
	�\�� on off �ݒ�� 4 �ʂ�Ɍ��炷���ƂŁA
	�g�ݍ��킹�����𑽏��y�����������B
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
				/* �v�� */
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
	/* SRAM �o�b�N�A�b�v���畜�� */
	memcpy((void *)0xED0000, s_sramBack, 0x4000);

	/* SRAM ���������݋֎~���[�h�ɖ߂� */
	B_BPOKE((UBYTE *)0xE8E00D, 0x00);
#endif

	/* ���O�t�@�C������� */
	fclose(fp);

	/* ��ʃ��[�h��߂� */
	CRTMOD(0x10);
}
